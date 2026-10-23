/*
 * input.c — 读取键盘和鼠标：Linux evdev 接口 (/dev/input/event*)
 *
 * 内核的输入子系统把所有输入设备（键盘、鼠标、触摸板、手柄……）统一成“事件设备”：
 * 每个设备一个 /dev/input/eventN，read() 出来的是一串固定格式的事件
 *     { 时间, type, code, value }
 * 例如按下 A 键：{EV_KEY, KEY_A, 1}，然后 {EV_SYN, SYN_REPORT, 0} 表示“这一组事件结束”。
 * 鼠标向右下移动：{EV_REL, REL_X, 3} {EV_REL, REL_Y, 1} {EV_SYN, SYN_REPORT, 0}。
 *
 * 设备分类：用 ioctl(EVIOCGBIT) 问内核“这个设备能产生哪些事件/按键”：
 *   能产生 KEY_A..KEY_Z 的是键盘；能产生 REL_X/REL_Y 的是（相对）鼠标；
 *   能产生 ABS_X/ABS_Y 的是绝对坐标设备（数位板、触摸屏、虚拟机的 tablet）。
 *
 * 鼠标的三种来源（按优先级）：
 *   1. vmmouse（见 vmmouse.c）：虚拟机直接给出绝对坐标，指针和宿主机完全同步；
 *      此时 PS/2 鼠标设备的事件只用来“叫醒”我们去读 vmmouse 队列，数据本身丢弃。
 *   2. EV_ABS 绝对坐标设备：按设备报告的范围 (EVIOCGABS) 换算到屏幕坐标；
 *   3. EV_REL 相对鼠标：累加移动量，加一点“加速”（移动快时走得更远），限制在屏幕内。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * 32 位程序读到的事件结构：内核按 { u32 秒, u32 微秒, u16 type, u16 code, s32 value }
 * 共 16 字节写出。注意不能直接用 <linux/input.h> 的 struct input_event：musl 在 32 位
 * 平台上用 64 位 time_t（为了解决 2038 年问题），那个结构里的 struct timeval 会变成
 * 16 字节，整个结构 24 字节，和内核对不上，读出来的事件就全乱了。
 */
typedef struct {
    uint32_t sec, usec;
    uint16_t type, code;
    int32_t  value;
} skd_input_event;
_Static_assert(sizeof(skd_input_event) == 16, "evdev 事件在 32 位内核上是 16 字节");

#define MAX_DEVS 16
#define LONG_BITS (8 * sizeof(unsigned long))
#define NLONGS(n) (((n) + LONG_BITS - 1) / LONG_BITS)
#define TEST_BIT(arr, bit) (((arr)[(bit) / LONG_BITS] >> ((bit) % LONG_BITS)) & 1UL)

typedef struct {
    int  fd;
    char path[64];
    char name[80];
    int  is_kbd, is_rel, is_abs;
    struct input_absinfo ax, ay;      /* 绝对坐标设备的取值范围 */
    /* 本组事件（到 SYN_REPORT 为止）累积的数据 */
    int  dx, dy, wheel;
    int  abs_x, abs_y, abs_dirty;
    uint32_t btn_down, btn_up;
} indev;

static indev    g_dev[MAX_DEVS];
static int      g_ndev;
static int      g_sw, g_sh;
static int      g_px, g_py;                /* 指针位置（屏幕坐标） */
static uint32_t g_buttons;                 /* 当前按住的按钮 SK_BUTTON_* */
static int      g_motion_pending;          /* 位置变了但还没告诉 wm（合并多次移动） */
static int      g_vmmouse;                 /* vmmouse 已启用 */
static int      g_vm_failures;
static skd_keymap_state g_keys;

/* ---------- 设备枚举 ---------- */

static void set_leds(void)
{
    skd_input_event ev[3];
    memset(ev, 0, sizeof ev);
    ev[0].type = EV_LED; ev[0].code = LED_CAPSL; ev[0].value = g_keys.caps_lock;
    ev[1].type = EV_LED; ev[1].code = LED_NUML;  ev[1].value = g_keys.num_lock;
    ev[2].type = EV_SYN; ev[2].code = SYN_REPORT;
    /* 往键盘设备“写”一个 EV_LED 事件，内核驱动就会把命令发给键盘去点亮指示灯 */
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].is_kbd && write(g_dev[i].fd, ev, sizeof ev) < 0)
            SKD_DBG("input: 设置 %s 的指示灯失败: %s", g_dev[i].name, strerror(errno));
}

static void probe_device(const char *path)
{
    if (g_ndev >= MAX_DEVS)
        return;
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        sk_log("input: 打开 %s 失败: %s", path, strerror(errno));
        return;
    }
    unsigned long evbits[NLONGS(EV_MAX + 1)];
    unsigned long keybits[NLONGS(KEY_MAX + 1)];
    unsigned long relbits[NLONGS(REL_MAX + 1)];
    unsigned long absbits[NLONGS(ABS_MAX + 1)];
    memset(evbits, 0, sizeof evbits);
    memset(keybits, 0, sizeof keybits);
    memset(relbits, 0, sizeof relbits);
    memset(absbits, 0, sizeof absbits);
    if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0) {
        close(fd);
        return;
    }
    if (TEST_BIT(evbits, EV_KEY))
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
    if (TEST_BIT(evbits, EV_REL))
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits);
    if (TEST_BIT(evbits, EV_ABS))
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);

    indev *d = &g_dev[g_ndev];
    memset(d, 0, sizeof *d);
    d->fd = fd;
    sk_strlcpy(d->path, path, sizeof d->path);
    if (ioctl(fd, EVIOCGNAME(sizeof d->name), d->name) < 0)
        sk_strlcpy(d->name, "?", sizeof d->name);

    /* 能打出全部 26 个字母的才算键盘（电源按钮、多媒体键设备也有 EV_KEY） */
    int letters = 1;
    static const int keys[] = { KEY_A, KEY_E, KEY_Q, KEY_Z, KEY_SPACE, KEY_ENTER };
    for (size_t i = 0; i < SK_ARRAY_LEN(keys); i++)
        if (!TEST_BIT(keybits, keys[i]))
            letters = 0;
    for (int k = KEY_Q; k <= KEY_P && letters; k++)
        if (!TEST_BIT(keybits, k))
            letters = 0;
    d->is_kbd = letters;
    d->is_rel = TEST_BIT(evbits, EV_REL) && TEST_BIT(relbits, REL_X) && TEST_BIT(relbits, REL_Y);
    d->is_abs = TEST_BIT(evbits, EV_ABS) && TEST_BIT(absbits, ABS_X) && TEST_BIT(absbits, ABS_Y);
    if (d->is_abs) {
        if (ioctl(fd, EVIOCGABS(ABS_X), &d->ax) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), &d->ay) < 0 ||
            d->ax.maximum <= d->ax.minimum || d->ay.maximum <= d->ay.minimum)
            d->is_abs = 0;
    }
    if (!d->is_kbd && !d->is_rel && !d->is_abs) {
        SKD_DBG("input: 忽略 %s「%s」", path, d->name);
        close(fd);
        return;
    }
    if (d->is_kbd) {
        /* “独占”这个键盘：内核不再把它的事件交给其它读者（包括控制台的键盘处理程序） */
        if (ioctl(fd, EVIOCGRAB, (void *)1) < 0)
            sk_log("input: 独占键盘 %s 失败: %s", d->name, strerror(errno));
    }
    sk_log("input: %s「%s」%s%s%s", path, d->name, d->is_kbd ? " 键盘" : "",
           d->is_rel ? " 相对鼠标" : "", d->is_abs ? " 绝对坐标" : "");
    g_ndev++;
}

int input_init(int screen_w, int screen_h)
{
    g_sw = screen_w;
    g_sh = screen_h;
    g_px = screen_w / 2;
    g_py = screen_h / 2;
    g_keys.num_lock = 1;     /* 小键盘默认输入数字 */

    DIR *dir = opendir("/dev/input");
    if (dir) {
        /* 先收集名字再排序，让 event0、event1 …… 的顺序稳定 */
        char names[MAX_DEVS * 2][64];
        int n = 0;
        struct dirent *de;
        while ((de = readdir(dir)) && n < (int)SK_ARRAY_LEN(names)) {
            if (strncmp(de->d_name, "event", 5) == 0 && strlen(de->d_name) < 40)
                snprintf(names[n++], sizeof names[0], "/dev/input/%s", de->d_name);
        }
        closedir(dir);
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
                char t[64];
                memcpy(t, names[j], sizeof t);
                memcpy(names[j], names[j - 1], sizeof t);
                memcpy(names[j - 1], t, sizeof t);
            }
        for (int i = 0; i < n; i++)
            probe_device(names[i]);
    } else {
        sk_log("input: 打开 /dev/input 失败: %s", strerror(errno));
    }

    int have_rel = 0;
    for (int i = 0; i < g_ndev; i++)
        have_rel |= g_dev[i].is_rel;
    /* vmmouse 需要 PS/2 鼠标设备的中断作为“闹钟”，没有相对鼠标就不启用 */
    if (have_rel && vmmouse_init() == 0)
        g_vmmouse = 1;
    set_leds();
    return g_ndev;
}

void input_shutdown(void)
{
    if (g_vmmouse)
        vmmouse_shutdown();
    g_vmmouse = 0;
    for (int i = 0; i < g_ndev; i++) {
        if (g_dev[i].is_kbd)
            ioctl(g_dev[i].fd, EVIOCGRAB, (void *)0);
        close(g_dev[i].fd);
    }
    g_ndev = 0;
}

const char *input_mouse_kind(void)
{
    if (g_vmmouse)
        return "vmmouse";
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].is_abs)
            return "evdev-abs";
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].is_rel)
            return "evdev-rel";
    return "none";
}

uint32_t input_modifiers(void) { return keymap_modifiers(&g_keys); }
uint32_t input_buttons(void) { return g_buttons; }

int input_pollfds(struct pollfd *pfd, int max)
{
    int n = 0;
    for (int i = 0; i < g_ndev && n < max; i++) {
        pfd[n].fd = g_dev[i].fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
    }
    return n;
}

/* ---------- 指针 ---------- */

static void flush_motion(void)
{
    if (g_motion_pending) {
        g_motion_pending = 0;
        wm_pointer_motion(g_px, g_py);
    }
}

static void move_to(int x, int y)
{
    x = SK_CLAMP(x, 0, g_sw - 1);
    y = SK_CLAMP(y, 0, g_sh - 1);
    if (x != g_px || y != g_py) {
        g_px = x;
        g_py = y;
        g_motion_pending = 1;
    }
}

/* 按钮状态变化：先把位置同步给 wm（在新位置按下），再逐个报告变化的按钮 */
static void set_buttons(uint32_t now)
{
    uint32_t changed = now ^ g_buttons;
    if (!changed)
        return;
    flush_motion();
    static const uint32_t order[] = { SK_BUTTON_LEFT, SK_BUTTON_RIGHT, SK_BUTTON_MIDDLE };
    for (size_t i = 0; i < SK_ARRAY_LEN(order); i++) {
        uint32_t b = order[i];
        if (!(changed & b))
            continue;
        if (now & b)
            g_buttons |= b;
        else
            g_buttons &= ~b;
        wm_pointer_button(b, (now & b) != 0);
    }
}

static void do_wheel(int delta)
{
    if (!delta)
        return;
    flush_motion();
    wm_pointer_wheel(delta);
}

/* 相对鼠标的“加速”：慢慢移动时 1:1 精确定位，快速甩动时成倍放大 */
static int accel(int d, int speed)
{
    if (speed > 12)
        return d * 3;
    if (speed > 4)
        return d * 2;
    return d;
}

static uint32_t evdev_button(uint16_t code)
{
    switch (code) {
    case BTN_LEFT:
    case BTN_TOUCH:  return SK_BUTTON_LEFT;
    case BTN_RIGHT:  return SK_BUTTON_RIGHT;
    case BTN_MIDDLE: return SK_BUTTON_MIDDLE;
    default:         return 0;
    }
}

static int scale_abs(int v, const struct input_absinfo *a, int size)
{
    long range = (long)a->maximum - a->minimum;
    long pos = (long)v - a->minimum;
    if (pos < 0) pos = 0;
    if (pos > range) pos = range;
    return (int)((pos * (size - 1) + range / 2) / range);
}

static void drain_vmmouse(void)
{
    skd_vmmouse_packet pk[64];
    for (int round = 0; round < 8; round++) {
        int n = vmmouse_read(pk, (int)SK_ARRAY_LEN(pk));
        if (n < 0) {
            /* 连续出错太多次：放弃 vmmouse，退回普通 PS/2 相对鼠标 */
            if (++g_vm_failures > 50) {
                sk_log("input: vmmouse 持续出错，改用 PS/2 相对鼠标");
                vmmouse_shutdown();
                g_vmmouse = 0;
            }
            return;
        }
        if (n == 0)
            return;
        g_vm_failures = 0;
        for (int i = 0; i < n; i++) {
            if (pk[i].relative) {
                move_to(g_px + pk[i].x, g_py + pk[i].y);
            } else {
                /* 0..0xFFFF → 0..宽-1，四舍五入 */
                int x = (int)(((int64_t)pk[i].x * (g_sw - 1) + 0x7FFF) / 0xFFFF);
                int y = (int)(((int64_t)pk[i].y * (g_sh - 1) + 0x7FFF) / 0xFFFF);
                move_to(x, y);
            }
            set_buttons(pk[i].buttons);
            do_wheel(pk[i].wheel);
        }
        if (n < (int)SK_ARRAY_LEN(pk))
            return;
    }
}

/* ---------- 键盘 ---------- */

static void key_event(uint16_t code, int value)
{
    if (keymap_update(&g_keys, code, value))
        set_leds();
    uint32_t keycode = code;
    uint32_t cp = keymap_translate(&g_keys, &keycode);
    flush_motion();
    wm_key(keycode, (uint32_t)value, keymap_modifiers(&g_keys), cp);
}

/* ---------- 读取一个设备的所有事件 ---------- */

static void close_device(int i)
{
    sk_log("input: 设备 %s 已移除", g_dev[i].path);
    close(g_dev[i].fd);
    g_dev[i] = g_dev[--g_ndev];
}

static void sync_frame(indev *d)
{
    if (d->is_rel && g_vmmouse) {
        /* vmmouse 模式：PS/2 数据作废，只是提醒我们“队列里有新包了” */
        d->dx = d->dy = d->wheel = 0;
        d->btn_down = d->btn_up = 0;
        drain_vmmouse();
        return;
    }
    if (d->dx || d->dy) {
        int speed = abs(d->dx) + abs(d->dy);
        move_to(g_px + accel(d->dx, speed), g_py + accel(d->dy, speed));
        d->dx = d->dy = 0;
    }
    if (d->abs_dirty) {
        move_to(scale_abs(d->abs_x, &d->ax, g_sw), scale_abs(d->abs_y, &d->ay, g_sh));
        d->abs_dirty = 0;
    }
    if (d->btn_down || d->btn_up) {
        uint32_t now = (g_buttons | d->btn_down) & ~d->btn_up;
        /* 同一帧里先按下又松开（很快的点击）：分两步报告，不能丢掉这次点击 */
        if (d->btn_down & d->btn_up) {
            set_buttons(g_buttons | d->btn_down);
            now = g_buttons & ~d->btn_up;
        }
        set_buttons(now);
        d->btn_down = d->btn_up = 0;
    }
    if (d->wheel) {
        do_wheel(d->wheel);
        d->wheel = 0;
    }
}

static int read_device(int i)
{
    indev *d = &g_dev[i];
    skd_input_event ev[64];
    for (;;) {
        ssize_t r = read(d->fd, ev, sizeof ev);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                return 0;
            if (errno == ENODEV) {
                close_device(i);
                return -1;
            }
            sk_log("input: 读取 %s 失败: %s", d->path, strerror(errno));
            return 0;
        }
        if (r == 0)
            return 0;
        int n = (int)(r / (ssize_t)sizeof ev[0]);
        for (int k = 0; k < n; k++) {
            const skd_input_event *e = &ev[k];
            switch (e->type) {
            case EV_KEY:
                if (e->code < BTN_MISC || (e->code >= KEY_OK && e->code < BTN_TRIGGER_HAPPY)) {
                    if (d->is_kbd)
                        key_event(e->code, e->value);
                } else {
                    uint32_t b = evdev_button(e->code);
                    if (b && e->value == 1)
                        d->btn_down |= b;
                    else if (b && e->value == 0)
                        d->btn_up |= b;
                }
                break;
            case EV_REL:
                if (e->code == REL_X)
                    d->dx += e->value;
                else if (e->code == REL_Y)
                    d->dy += e->value;
                else if (e->code == REL_WHEEL)
                    d->wheel += e->value;
                break;
            case EV_ABS:
                if (e->code == ABS_X) {
                    d->abs_x = e->value;
                    d->abs_dirty = 1;
                } else if (e->code == ABS_Y) {
                    d->abs_y = e->value;
                    d->abs_dirty = 1;
                }
                break;
            case EV_SYN:
                if (e->code == SYN_REPORT)
                    sync_frame(d);
                else if (e->code == SYN_DROPPED)
                    /* 内核缓冲区溢出丢了事件：丢弃这一帧的半截数据 */
                    d->dx = d->dy = d->wheel = 0;
                break;
            default:
                break;
            }
        }
        if (r < (ssize_t)sizeof ev)
            return 0;
    }
}

void input_dispatch(const struct pollfd *pfd, int n)
{
    for (int k = 0; k < n; k++) {
        if (!(pfd[k].revents & (POLLIN | POLLERR | POLLHUP)))
            continue;
        for (int i = 0; i < g_ndev; i++)
            if (g_dev[i].fd == pfd[k].fd) {
                read_device(i);
                break;
            }
    }
    /* 所有设备的事件都处理完以后，只把“最后的位置”告诉 wm 一次：
     * 窗口拖动、光标重绘都按这一次来，不会因为事件多而越拖越慢 */
    flush_motion();
}

int input_timeout_ms(void)
{
    /* 安全网：按住按钮拖动时每 30ms 主动查一次 vmmouse 队列，万一 PS/2 闹钟丢了也不会卡住 */
    return (g_vmmouse && g_buttons) ? 30 : -1;
}

void input_tick(void)
{
    if (g_vmmouse) {
        drain_vmmouse();
        flush_motion();
    }
}
