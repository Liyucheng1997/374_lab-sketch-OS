/*
 * taskbar.c — 任务栏：屏幕底部一条“撕下来的纸”
 *
 *   [开始]  [便利贴：终端] [便利贴：记事本] ...            CPU 12%  内存 35%   14:05
 *
 * 和桌面一样，任务栏预渲染在自己的图层画布里（顶部撕纸边以上是透明的），
 * 合成时“抠像”拷贝。变化时只重画变化的那一块：
 *   - 时钟：用 timerfd 在每个整分钟（:00 秒）叫醒一次，只重画时钟那一小块；
 *   - CPU/内存：每 2 秒采样一次 /proc/stat、/proc/meminfo，数值变了才重画；
 *   - 窗口按钮：窗口增删、焦点、标题、最小化变化时重画按钮区。
 *
 * timerfd 是 Linux 把“定时器”做成文件描述符的接口：到时间后这个 fd 变成可读，
 * 正好可以和套接字、输入设备一起放进同一个 poll() 里等待，不需要信号或线程。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define TB_SEED     0x7A5CBA12u
#define BTN_H       32
#define START_W     96
#define TASK_X0     (8 + START_W + 12)
#define TASK_MAX_W  176
#define TASK_GAP    8
#define METER_W     78
#define CLOCK_W     64
#define MAX_TASKS   32

enum { T_NONE = -1, T_START = -2, T_CLOCK = -3 };

static sk_canvas *g_tb;          /* 图层：宽 = 屏幕宽，高 = 任务栏 + 撕纸边 */
static int        g_w, g_h;      /* 屏幕尺寸 */
static int        g_y0;          /* 图层在屏幕上的 y */
static int        g_body;        /* 图层里任务栏主体开始的 y（= SKD_TEAR_H） */
static int        g_hover = T_NONE, g_pressed = T_NONE;
static int        g_start_down;
static int        g_buttons_dirty = 1;
static int        g_clock_fd = -1, g_meter_fd = -1;

typedef struct { uint32_t win; sk_rect r; } task_btn;
static task_btn   g_tasks[MAX_TASKS];
static int        g_ntasks;

static int        g_cpu = -1, g_mem = -1;   /* 千分比；-1 = 还没采样 */
static sk_cpustat g_prev_cpu;
static int        g_have_prev;

/* ---------- 几何（图层坐标） ---------- */

static int btn_y(void) { return g_body + (SKD_TASKBAR_H - BTN_H) / 2; }
static sk_rect start_rect(void) { return sk_rect_make(8, btn_y(), START_W, BTN_H); }
static sk_rect clock_rect(void) { return sk_rect_make(g_w - CLOCK_W - 8, btn_y(), CLOCK_W, BTN_H); }
static sk_rect meter_rect(int which)
{
    return sk_rect_make(g_w - CLOCK_W - 16 - (2 - which) * (METER_W + 6), btn_y(), METER_W, BTN_H);
}
static int tasks_right(void) { return meter_rect(0).x - 14; }
static sk_rect tasks_area(void) { return sk_rect_make(TASK_X0 - 4, g_body, tasks_right() - TASK_X0 + 8, SKD_TASKBAR_H); }

static sk_rect to_screen(sk_rect r) { return sk_rect_offset(r, 0, g_y0); }
static int intersects(sk_rect a, sk_rect b) { return !sk_rect_empty(sk_rect_intersect(a, b)); }

sk_rect taskbar_start_rect(void) { return to_screen(start_rect()); }

int taskbar_contains(int x, int y)
{
    (void)x;
    return y >= g_y0 + g_body && y < g_h;
}

int taskbar_covers(sk_rect r) { return r.y >= g_y0 + g_body && r.y + r.h <= g_h; }

/* ---------- 绘制各部分 ---------- */

/* 撕纸边：沿顶边每隔约 9px 取一个高度随机的点，连成锯齿多边形 */
static void paint_paper(sk_canvas *c, sk_rect r)
{
    sk_rect body = sk_rect_make(0, g_body, g_w, SKD_TASKBAR_H);
    sk_rect b = sk_rect_intersect(body, r);
    if (!sk_rect_empty(b))
        sk_paper_fill(c, b, SK_PAPER_DARK, 8, TB_SEED);
    if (r.y >= g_body + 2)
        return;
    sk_rng rng;
    sk_rng_seed(&rng, TB_SEED);
    sk_pointf edge[512];
    int n = 0;
    float x = 0;
    while (n < 508) {
        float y = (float)g_body - sk_rng_range(&rng, 0.5f, (float)SKD_TEAR_H - 0.5f);
        edge[n].x = x;
        edge[n].y = y;
        n++;
        if (x >= (float)g_w)
            break;
        x += sk_rng_range(&rng, 6.0f, 12.0f);
        if (x > (float)g_w)
            x = (float)g_w;
    }
    /* 锯齿带：多边形 = 锯齿顶边 + 主体顶边 */
    sk_pointf poly[512];
    int m = 0;
    for (int i = 0; i < n; i++)
        poly[m++] = edge[i];
    poly[m].x = (float)g_w;
    poly[m].y = (float)g_body + 2;
    m++;
    poly[m].x = 0;
    poly[m].y = (float)g_body + 2;
    m++;
    sk_fill_polygon(c, poly, m, SK_PAPER_DARK);
    /* 撕口的铅笔线：深一点的折线，下面再画一道很淡的“纸纤维”线 */
    for (int i = 0; i + 1 < n; i++) {
        sk_line_aa(c, edge[i].x, edge[i].y, edge[i + 1].x, edge[i + 1].y, 1.2f,
                   SK_RGBA(0x5C, 0x5C, 0x5C, 0xB0));
        sk_line_aa(c, edge[i].x, edge[i].y + 2.2f, edge[i + 1].x, edge[i + 1].y + 2.2f, 1.0f,
                   SK_RGBA(0xFF, 0xFF, 0xFF, 0x70));
    }
}

/* 便利贴：手绘方框 + 淡淡的底色，左上角一小段透明胶带 */
static void paint_note(sk_canvas *c, sk_rect r, sk_color tint, int focused, int hover, int faded,
                       uint32_t seed)
{
    sk_rough_style s;
    sk_rough_default(&s);
    s.seed = seed;
    s.roughness = 0.8f;
    s.stroke_width = focused ? 1.8f : 1.4f;
    s.stroke = faded ? SK_INK_FAINT : (focused ? SK_INK : SK_INK_SOFT);
    s.fill_style = SK_FILL_SOLID;
    int amount = faded ? 30 : (focused ? 150 : (hover ? 110 : 75));
    s.fill = sk_mix(SK_PAPER, tint, amount);
    /* 下边和右边的一道阴影，像贴在纸上微微翘起 */
    sk_line_aa(c, (float)r.x + 3, (float)(r.y + r.h) + 1.5f, (float)(r.x + r.w) + 1, (float)(r.y + r.h) + 1.5f,
               1.6f, SK_RGBA(0x5C, 0x5C, 0x5C, 0x50));
    sk_rough_rect(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, &s);
    if (focused)
        sk_marker_highlight(c, sk_rect_make(r.x + 26, r.y + r.h - 11, r.w - 34, 8), SK_HIGHLIGHT, seed + 9);
    if (!faded) {
        sk_rect tape = sk_rect_make(r.x - 4, r.y - 3, 16, 7);
        sk_fill_rect(c, tape, SK_RGBA(0xFF, 0xFF, 0xF0, 0xA0));
        sk_rect_outline(c, tape, SK_RGBA(0xA0, 0xA0, 0xA0, 0x60));
    }
}

static void paint_start(sk_canvas *c)
{
    sk_rect r = start_rect();
    int on = g_start_down || g_pressed == T_START;
    int hover = g_hover == T_START;
    sk_rough_style s;
    sk_rough_default(&s);
    s.seed = TB_SEED + 1;
    s.stroke_width = 1.7f;
    s.fill_style = SK_FILL_HACHURE;
    s.fill = on ? SK_ORANGE : SK_YELLOW;
    s.hachure_gap = on ? 3.5f : (hover ? 4.0f : 5.0f);
    s.hachure_angle = -41;
    s.fill_weight = 1.3f;
    sk_rough_round_rect(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 8, &s);
    sk_draw_icon(c, SK_ICON_SKETCH_LOGO, r.x + 8, r.y + 4, 24, TB_SEED + 2);
    sk_font *f = sk_font_get(SK_FONT_TITLE);
    sk_rect tr = sk_rect_make(r.x + 36, r.y, r.w - 40, r.h);
    sk_draw_text_in_rect(c, f, tr, "开始", SK_INK, SK_ALIGN_CENTER);
}

static void paint_tasks(sk_canvas *c, sk_rect clip)
{
    sk_font *f = sk_font_get(SK_FONT_UI);
    for (int i = 0; i < g_ntasks; i++) {
        sk_rect r = g_tasks[i].r;
        if (!intersects(sk_rect_inset(r, -6, -6), clip))
            continue;
        skd_window *w = wm_find(g_tasks[i].win);
        if (!w)
            continue;
        int focused = (w == wm_focused()) && !w->minimized;
        int hover = g_hover == i || g_pressed == i;
        paint_note(c, r, w->accent, focused, hover, w->minimized, w->seed ^ TB_SEED);
        int isz = 20;
        sk_draw_icon(c, w->icon, r.x + 6, r.y + (r.h - isz) / 2, isz, w->seed);
        sk_rect tr = sk_rect_make(r.x + 6 + isz + 6, r.y, r.w - isz - 18, r.h);
        sk_draw_text_in_rect(c, f, tr, w->title[0] ? w->title : w->app,
                             w->minimized ? SK_INK_FAINT : SK_INK, SK_ALIGN_LEFT);
    }
}

static void paint_meter(sk_canvas *c, int which)
{
    sk_rect r = meter_rect(which);
    int v = which == 0 ? g_cpu : g_mem;
    sk_font *f = sk_font_get(SK_FONT_UI);
    char txt[32];
    if (v < 0)
        snprintf(txt, sizeof txt, "%s --", which == 0 ? "CPU" : "内存");
    else
        snprintf(txt, sizeof txt, "%s %d%%", which == 0 ? "CPU" : "内存", (v + 5) / 10);
    int base = r.y + sk_font_ascent(f) + 1;
    sk_draw_text(c, f, r.x + 2, base, txt, SK_INK_SOFT);
    sk_rect bar = sk_rect_make(r.x + 2, r.y + r.h - 9, r.w - 4, 7);
    sk_color col = which == 0 ? (v > 800 ? SK_RED : SK_GREEN) : (v > 850 ? SK_RED : SK_BLUE);
    sk_draw_meter(c, bar, v < 0 ? 0 : v, col, TB_SEED + 20 + (uint32_t)which);
}

static void paint_clock(sk_canvas *c)
{
    sk_rect r = clock_rect();
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char txt[16];
    snprintf(txt, sizeof txt, "%02d:%02d", tm.tm_hour, tm.tm_min);
    sk_font *f = sk_font_get(SK_FONT_TITLE);
    sk_draw_text_in_rect(c, f, r, txt, SK_INK, SK_ALIGN_CENTER);
    if (g_hover == T_CLOCK)
        sk_rough_underline(c, (float)r.x + 8, (float)(r.x + r.w - 8), (float)(r.y + r.h - 4),
                           SK_RGBA(0x4A, 0x7B, 0xD0, 0xA0), 1.6f, TB_SEED + 30);
}

static void layout_tasks(void)
{
    skd_window *wins[MAX_TASKS];
    int n = wm_task_list(wins, MAX_TASKS);
    int avail = tasks_right() - TASK_X0;
    int w = n ? (avail - (n - 1) * TASK_GAP) / n : TASK_MAX_W;
    if (w > TASK_MAX_W)
        w = TASK_MAX_W;
    if (w < 40)
        w = 40;
    g_ntasks = 0;
    for (int i = 0; i < n; i++) {
        int x = TASK_X0 + i * (w + TASK_GAP);
        if (x + w > tasks_right())
            break;
        g_tasks[g_ntasks].win = wins[i]->id;
        g_tasks[g_ntasks].r = sk_rect_make(x, btn_y(), w, BTN_H);
        g_ntasks++;
    }
}

/* 在图层上重画 r（图层坐标）这一块 */
static void render(sk_rect r)
{
    r = sk_rect_intersect(r, sk_rect_make(0, 0, g_tb->w, g_tb->h));
    if (sk_rect_empty(r))
        return;
    sk_set_clip(g_tb, r);
    sk_clear(g_tb, SK_WITH_ALPHA(SK_DESK_PAPER, 0));
    paint_paper(g_tb, r);
    if (intersects(sk_rect_inset(start_rect(), -6, -6), r))
        paint_start(g_tb);
    if (intersects(tasks_area(), r))
        paint_tasks(g_tb, r);
    for (int i = 0; i < 2; i++)
        if (intersects(sk_rect_inset(meter_rect(i), -4, -4), r))
            paint_meter(g_tb, i);
    if (intersects(sk_rect_inset(clock_rect(), -4, -4), r))
        paint_clock(g_tb);
    sk_reset_clip(g_tb);
}

static void refresh(sk_rect layer_r)
{
    render(layer_r);
    comp_damage(to_screen(layer_r));
}

void taskbar_paint(sk_canvas *dst, sk_rect r)
{
    if (!g_tb)
        return;
    if (g_buttons_dirty) {
        /* 窗口列表在这一帧里可能变了好几次，统一在这里重新布局、重画一次 */
        g_buttons_dirty = 0;
        layout_tasks();
        render(tasks_area());
    }
    sk_rect lr = sk_rect_make(0, g_y0, g_tb->w, g_tb->h);
    sk_rect ir = sk_rect_intersect(lr, r);
    if (sk_rect_empty(ir))
        return;
    sk_blit_keyed(dst, ir.x, ir.y, g_tb, sk_rect_make(ir.x, ir.y - g_y0, ir.w, ir.h));
}

void taskbar_windows_changed(void)
{
    g_buttons_dirty = 1;
    comp_damage(to_screen(tasks_area()));
}

void taskbar_set_start_pressed(int on)
{
    if (g_start_down == on)
        return;
    g_start_down = on;
    refresh(sk_rect_inset(start_rect(), -6, -6));
}

/* ---------- 鼠标 ---------- */

static int target_at(int x, int y)
{
    if (!taskbar_contains(x, y))
        return T_NONE;
    int ly = y - g_y0;
    if (sk_rect_contains(start_rect(), x, ly))
        return T_START;
    if (sk_rect_contains(clock_rect(), x, ly))
        return T_CLOCK;
    for (int i = 0; i < g_ntasks; i++)
        if (sk_rect_contains(g_tasks[i].r, x, ly))
            return i;
    return T_NONE;
}

static sk_rect target_rect(int t)
{
    if (t == T_START)
        return sk_rect_inset(start_rect(), -6, -6);
    if (t == T_CLOCK)
        return sk_rect_inset(clock_rect(), -4, -4);
    if (t >= 0 && t < g_ntasks)
        return sk_rect_inset(g_tasks[t].r, -6, -6);
    return sk_rect_make(0, 0, 0, 0);
}

static void set_hover(int t)
{
    if (t == g_hover)
        return;
    int old = g_hover;
    g_hover = t;
    if (old != T_NONE)
        refresh(target_rect(old));
    if (t != T_NONE)
        refresh(target_rect(t));
}

void taskbar_pointer_motion(int x, int y)
{
    set_hover(x < 0 ? T_NONE : target_at(x, y));
}

void taskbar_pointer_down(int x, int y, uint32_t button)
{
    if (button != SK_BUTTON_LEFT)
        return;
    g_pressed = target_at(x, y);
    if (g_pressed != T_NONE)
        refresh(target_rect(g_pressed));
}

void taskbar_pointer_up(int x, int y, uint32_t button)
{
    if (button != SK_BUTTON_LEFT)
        return;
    int t = target_at(x, y), was = g_pressed;
    g_pressed = T_NONE;
    if (was != T_NONE)
        refresh(target_rect(was));
    if (t != was || t == T_NONE)
        return;
    if (t == T_START) {
        menu_toggle_start();
    } else if (t == T_CLOCK) {
        /* 点时钟：显示完整日期 */
        static const char *const wd[] = { "日", "一", "二", "三", "四", "五", "六" };
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char msg[64];
        snprintf(msg, sizeof msg, "%d 年 %d 月 %d 日 星期%s", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, wd[tm.tm_wday % 7]);
        menu_toast(msg, 2500);
    } else if (t >= 0) {
        skd_window *w = wm_find(g_tasks[t].win);
        if (!w)
            return;
        /* 点击当前窗口的按钮 = 最小化；点击其它的 = 切换过去（最小化的会恢复） */
        if (w == wm_focused() && !w->minimized)
            wm_minimize(w);
        else
            wm_activate(w);
    }
}

/* ---------- 定时器 ---------- */

/* 把时钟定时器设到“下一个整分钟”。用绝对时间 + CANCEL_ON_SET：
 * 如果有人修改了系统时间，read() 会返回 ECANCELED，我们重新对齐即可。 */
static void arm_clock(void)
{
    if (g_clock_fd < 0)
        return;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec = now.tv_sec - now.tv_sec % 60 + 60;
    its.it_interval.tv_sec = 60;
    if (timerfd_settime(g_clock_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL) < 0)
        sk_log("taskbar: 设置时钟定时器失败: %s", strerror(errno));
}

static void sample_meters(void)
{
    sk_cpustat now;
    int cpu = g_cpu, mem = g_mem;
    if (sk_read_cpustat(&now) == 0) {
        /* 第一次采样没有“上一次”：和全零比较，得到的是开机以来的平均占用率 */
        sk_cpustat zero;
        memset(&zero, 0, sizeof zero);
        cpu = sk_cpu_usage_permille(g_have_prev ? &g_prev_cpu : &zero, &now);
        g_prev_cpu = now;
        g_have_prev = 1;
    }
    sk_meminfo mi;
    if (sk_read_meminfo(&mi) == 0 && mi.total_kb > 0)
        mem = (int)((mi.total_kb - mi.available_kb) * 1000 / mi.total_kb);
    /* 显示的是百分比：变化不到 1% 就不重画 */
    if ((cpu + 5) / 10 != (g_cpu + 5) / 10 || g_cpu < 0) {
        g_cpu = cpu;
        refresh(sk_rect_inset(meter_rect(0), -4, -4));
    }
    if ((mem + 5) / 10 != (g_mem + 5) / 10 || g_mem < 0) {
        g_mem = mem;
        refresh(sk_rect_inset(meter_rect(1), -4, -4));
    }
}

int taskbar_clock_fd(void) { return g_clock_fd; }
int taskbar_meter_fd(void) { return g_meter_fd; }

void taskbar_clock_tick(void)
{
    uint64_t n;
    if (read(g_clock_fd, &n, sizeof n) < 0 && errno == ECANCELED)
        arm_clock();
    refresh(sk_rect_inset(clock_rect(), -4, -4));
}

void taskbar_meter_tick(void)
{
    uint64_t n;
    if (read(g_meter_fd, &n, sizeof n) < 0)
        return;
    sample_meters();
}

void taskbar_init(int w, int h)
{
    g_w = w;
    g_h = h;
    g_body = SKD_TEAR_H;
    g_y0 = h - SKD_TASKBAR_H - SKD_TEAR_H;
    g_tb = sk_canvas_new(w, SKD_TASKBAR_H + SKD_TEAR_H);
    if (!g_tb) {
        sk_log("taskbar: 图层分配失败");
        return;
    }
    g_clock_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_clock_fd < 0)
        sk_log("taskbar: timerfd_create 失败: %s", strerror(errno));
    arm_clock();
    g_meter_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_meter_fd >= 0) {
        struct itimerspec its;
        memset(&its, 0, sizeof its);
        its.it_value.tv_sec = 2;
        its.it_interval.tv_sec = 2;
        timerfd_settime(g_meter_fd, 0, &its, NULL);
    }
    sample_meters();
    layout_tasks();
    g_buttons_dirty = 0;
    render(sk_rect_make(0, 0, g_tb->w, g_tb->h));
}

void taskbar_shutdown(void)
{
    if (g_clock_fd >= 0)
        close(g_clock_fd);
    if (g_meter_fd >= 0)
        close(g_meter_fd);
    g_clock_fd = g_meter_fd = -1;
    if (g_tb)
        sk_canvas_free(g_tb);
    g_tb = NULL;
}
