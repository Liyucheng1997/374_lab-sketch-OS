/*
 * console.c — 往“控制台”写字：开机横幅、提示框、关机提示
 *
 * 内核命令行 "console=tty1 console=ttyS0" 让内核自己的消息 (printk) 同时出现在
 * 屏幕 (tty1) 和串口 (ttyS0) 上；但用户态打开的 /dev/console 只对应最后一个 console=，
 * 也就是串口。所以“屏幕和串口都要看得到”的内容（横幅、关机提示），要自己分别写到
 * 每一个活动控制台。活动控制台列表在 /sys/class/tty/console/active 里，例如 "tty1 ttyS0"，
 * 其中最后一项就是 /dev/console 指向的那个。
 *
 * 两种控制台能显示的字不一样：
 *   串口 (ttyS0)：内核只是把字节原样送出去，由电脑上的终端程序来显示，中文没问题；
 *   屏幕上的虚拟终端 (tty1…)：由内核自己用点阵字体画字，这种字体最多 512 个字形，
 *                            根本装不下汉字，中文会显示成一个个小方块。
 * 所以给虚拟终端写的是英文版，给串口写的是中文版（CON_SAY 宏同时提供两种文字）。
 *
 * 另外，1 号进程绝不能被一个“卡住”的终端拖住（例如有人在串口上按了 Ctrl+S 暂停输出），
 * 所以这里用非阻塞方式写，写不进去就短暂等待，等太久就放弃。
 */
#define _GNU_SOURCE
#include "sinit.h"
#include "skutil.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define CON_MAX 4

static int g_con[CON_MAX];
static int g_con_vt[CON_MAX];    /* 1 = 屏幕上的虚拟终端（显示不了中文） */
static int g_ncon;

static int open_con(const char *path)
{
    return open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
}

/* "tty1"、"tty0" 这样 tty 后面直接跟数字的是虚拟终端；"ttyS0" 是串口 */
static int is_vt_name(const char *name)
{
    return strncmp(name, "tty", 3) == 0 && isdigit((unsigned char)name[3]);
}

static void add_con(int fd, int vt)
{
    if (fd < 0 || g_ncon >= CON_MAX)
        return;
    g_con[g_ncon] = fd;
    g_con_vt[g_ncon] = vt;
    g_ncon++;
}

void con_init(void)
{
    for (int i = 0; i < g_ncon; i++)
        close(g_con[i]);
    g_ncon = 0;

    char active[256] = "";
    char *names[8];
    int n = 0;
    if (sk_read_line("/sys/class/tty/console/active", active, sizeof active) == 0)
        n = sk_split_args(active, names, (int)SK_ARRAY_LEN(names));

    int fd = open_con("/dev/console");
    int have_console = fd >= 0;
    add_con(fd, n > 0 && is_vt_name(names[n - 1]));
    /* 最后一项已经通过 /dev/console 打开了；如果 /dev/console 打不开，就全部单独打开 */
    int upto = have_console ? n - 1 : n;
    for (int i = 0; i < upto; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/%s", names[i]);
        add_con(open_con(path), is_vt_name(names[i]));
    }
}

/* 把 buf 全部写进 fd；终端暂时写不进去 (EAGAIN) 时最多等大约 1 秒 */
static void write_all(int fd, const char *buf, size_t len)
{
    int waited_ms = 0;
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w > 0) {
            buf += w;
            len -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno == EAGAIN && waited_ms < 1000) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            poll(&p, 1, 100);
            waited_ms += 100;
            continue;
        }
        return;   /* 其它错误或等太久：放弃，别让 1 号进程卡死 */
    }
}

/* 终端可能被程序设成了“原始模式”（不会把 \n 自动变成 \r\n），所以自己转换。
 * which：0 = 写给串口等能显示中文的控制台，1 = 写给虚拟终端 */
static void write_kind(const char *s, int which)
{
    size_t n = strlen(s);
    char *out = malloc(n * 2 + 1);
    if (!out)
        return;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r'))
            out[o++] = '\r';
        out[o++] = s[i];
    }
    for (int i = 0; i < g_ncon; i++)
        if (g_con_vt[i] == which)
            write_all(g_con[i], out, o);
    free(out);
}

void con_write_split(const char *zh, const char *en)
{
    if (g_ncon == 0)
        con_init();
    write_kind(zh, 0);
    write_kind(en, 1);
}

void con_printf(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    con_write_split(buf, buf);
}

/* ---------- 计算一行文字在终端上占几列 ----------
 * 汉字等“全角”字符占 2 列，ASCII 占 1 列；ANSI 颜色控制序列不占位置。
 * 画框时需要知道每行有多宽，才能把右边框对齐。 */
static int is_wide(uint32_t c)
{
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
           (c >= 0xFFE0 && c <= 0xFFE6);
}

static int display_width(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    int w = 0;
    while (*p) {
        if (*p == 0x1b && p[1] == '[') {          /* ESC [ ... 字母：颜色等控制序列 */
            p += 2;
            while (*p && !(*p >= 0x40 && *p <= 0x7e))
                p++;
            if (*p)
                p++;
            continue;
        }
        uint32_t c;
        int len;
        if (*p < 0x80)                { c = *p;        len = 1; }
        else if ((*p & 0xE0) == 0xC0) { c = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { c = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { c = *p & 0x07; len = 4; }
        else { p++; w++; continue; }
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { len = i; break; }
            c = (c << 6) | (p[i] & 0x3F);
        }
        p += len;
        w += is_wide(c) ? 2 : 1;
    }
    return w;
}

/* ---------- 先把整段文字拼好，再一次写出去 ---------- */
typedef struct {
    char   buf[8192];
    size_t len;
} textbuf;

static void tb_add(textbuf *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tb_add(textbuf *t, const char *fmt, ...)
{
    if (t->len >= sizeof t->buf - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t->buf + t->len, sizeof t->buf - t->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        t->len = SK_MIN(t->len + (size_t)n, sizeof t->buf - 1);
}

/* “手绘”边框：上下边是波浪线，左右边交替用 ( 和 )，像铅笔随手画的，有点歪 */
static void tb_edge(textbuf *t, int inner, char corner_l, char corner_r, int period)
{
    tb_add(t, "    %c", corner_l);
    for (int i = 0; i < inner + 1; i++)
        tb_add(t, "%c", (i % period == period / 2) ? '-' : '~');
    tb_add(t, "%c\n", corner_r);
}

static void tb_row(textbuf *t, int row, const char *content, int inner)
{
    int pad = inner - display_width(content);
    if (pad < 0)
        pad = 0;
    if (row % 2 == 0)
        tb_add(t, "   ( %s%*s )\n", content, pad, "");
    else
        tb_add(t, "    )%s%*s(\n", content, pad, "");
}

static void tb_box(textbuf *t, const char *title, const char *const *lines)
{
    int inner = display_width(title) + 4;
    for (int i = 0; lines[i]; i++)
        inner = SK_MAX(inner, display_width(lines[i]) + 4);
    char buf[512];
    int row = 0;
    tb_add(t, "\n");
    tb_edge(t, inner, '.', '.', 7);
    snprintf(buf, sizeof buf, "  \033[1m%s\033[0m", title);
    tb_row(t, row++, buf, inner);
    tb_row(t, row++, "", inner);
    for (int i = 0; lines[i]; i++) {
        snprintf(buf, sizeof buf, "  %s", lines[i]);
        tb_row(t, row++, buf, inner);
    }
    tb_edge(t, inner, '\'', '\'', 5);
    tb_add(t, "\n");
}

void con_box(const char *title_zh, const char *const *zh, const char *title_en, const char *const *en)
{
    static textbuf a, b;
    a.len = b.len = 0;
    tb_box(&a, title_zh, zh);
    tb_box(&b, title_en, en);
    con_write_split(a.buf, b.buf);
}

/* figlet 风格的 "SketchOS" 字样 */
static const char *const k_logo[] = {
    "  ____  _        _       _      ___  ____  ",
    " / ___|| | _____| |_ ___| |__  / _ \\/ ___| ",
    " \\___ \\| |/ / _ \\ __/ __| '_ \\| | | \\___ \\ ",
    "  ___) |   <  __/ || (__| | | | |_| |___) |",
    " |____/|_|\\_\\___|\\__\\___|_| |_|\\___/|____/ ",
};

static void tb_banner(textbuf *t, const char *slogan)
{
    const int inner = 54;
    char buf[512];
    int row = 0;
    tb_add(t, "\n");
    tb_edge(t, inner, '.', '.', 7);
    tb_row(t, row++, "", inner);
    for (size_t i = 0; i < SK_ARRAY_LEN(k_logo); i++) {
        snprintf(buf, sizeof buf, "   \033[1;36m%s\033[0m", k_logo[i]);
        tb_row(t, row++, buf, inner);
    }
    tb_row(t, row++, "", inner);
    tb_row(t, row++, slogan, inner);
    tb_row(t, row++, "", inner);
    tb_edge(t, inner, '\'', '\'', 5);
    /* 一支铅笔：橡皮头 | 笔杆 | 削尖的笔头 */
    tb_add(t, "       _______________________________________________\n"
              "      |_|_______________________________________________|`-._\n"
              "      |_|_______________________________________________|_.-'\n\n");
}

void con_banner(const char *hostname, const char *tz, long boot_centis)
{
    static textbuf zh, en;
    zh.len = en.len = 0;

    char slogan[256];
    snprintf(slogan, sizeof slogan, "     手绘操作系统 %s · 用铅笔画出来的 Linux 用户态", SKETCHOS_VERSION);
    tb_banner(&zh, slogan);
    snprintf(slogan, sizeof slogan, "     SketchOS %s - a Linux userland drawn in pencil", SKETCHOS_VERSION);
    tb_banner(&en, slogan);

    struct utsname u;
    if (uname(&u) != 0) {
        snprintf(u.release, sizeof u.release, "?");
        snprintf(u.machine, sizeof u.machine, "?");
    }
    char when[64] = "?";
    time_t t = time(NULL);
    struct tm tm;
    if (localtime_r(&t, &tm))
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);

    tb_add(&zh, "       内核   Linux %s (%s)\n", u.release, u.machine);
    tb_add(&zh, "       主机   %s\n", hostname);
    tb_add(&zh, "       时间   %s（时区 %s）\n", when, tz);
    tb_add(&en, "       Kernel   Linux %s (%s)\n", u.release, u.machine);
    tb_add(&en, "       Host     %s\n", hostname);
    tb_add(&en, "       Time     %s (TZ %s)\n", when, tz);
    if (boot_centis >= 0) {
        tb_add(&zh, "       开机   内核用了 %ld.%02ld 秒，把控制权交给了 1 号进程 sinit\n",
               boot_centis / 100, boot_centis % 100);
        tb_add(&en, "       Boot     the kernel handed over to PID 1 (sinit) after %ld.%02ld s\n",
               boot_centis / 100, boot_centis % 100);
    }
    tb_add(&zh, "\n");
    tb_add(&en, "\n       Note: this text console has no Chinese font. Chinese text shows up\n"
                "       on the serial terminal and in the desktop's terminal (skterm).\n\n");
    con_write_split(zh.buf, en.buf);
}
