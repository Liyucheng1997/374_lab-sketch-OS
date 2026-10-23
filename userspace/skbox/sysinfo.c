/*
 * sysinfo — 用一幅铅笔涂鸦和一张信息卡片介绍这台机器（类似 neofetch）
 *
 * 所有信息都来自内核提供的接口：uname()、/proc/uptime、/proc/meminfo、/proc/cpuinfo、
 * /sys/class/drm（显卡的分辨率）……这正好是一次“用户态如何了解系统”的小巡礼。
 */
#include "skbox.h"

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

SKBOX_APPLET(sysinfo, "以涂鸦风格显示系统信息概览")

static const char usage[] =
    "用法: sysinfo [--no-color]\n"
    "在左边画一支铅笔，右边列出系统信息：操作系统、内核、运行时间、Shell、终端、\n"
    "分辨率、CPU、内存、进程数，最后是终端的 16 色调色板。\n";

/* 铅笔涂鸦。每行开头的控制字符选择颜色：\1 橡皮 \2 金属箍 \3 笔杆 \4 木头 \5 笔芯 \6 涂鸦线 */
static const char *const g_logo[] = {
    "\1     .------.     ",
    "\1     |######|     ",
    "\2     |======|     ",
    "\2     |======|     ",
    "\3     |  ||  |     ",
    "\3     |  ||  |     ",
    "\3     |  ||  |     ",
    "\3     |  ||  |     ",
    "\4     \\  ||  /     ",
    "\4      \\ || /      ",
    "\4       \\||/       ",
    "\5        \\/        ",
    "\6   ~~~~~~'~~~~~~  ",
    "\6  ~ SketchOS ~    ",
};
static const char *const g_logo_colors[] = {
    "", "\033[1;35m", "\033[1;37m", "\033[1;33m", "\033[33m", "\033[1;30m", "\033[1;36m",
};

static void os_name(char *buf, size_t n)
{
    snprintf(buf, n, "%s %s", SKETCHOS_NAME, SKETCHOS_VERSION);
    char *s = sk_read_file("/etc/os-release", NULL);
    if (!s)
        return;
    /* os-release 是 KEY="值" 形式的文本 */
    for (char *line = s; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
            char *v = line + 12;
            if (*v == '"')
                v++;
            size_t l = strlen(v);
            if (l && v[l - 1] == '"')
                v[l - 1] = 0;
            sk_strlcpy(buf, v, n);
            break;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(s);
}

static void resolution(char *buf, size_t n)
{
    buf[0] = 0;
    /* DRM（内核的显卡框架）为每个显示输出建一个目录，modes 文件第一行是当前/首选分辨率 */
    DIR *d = opendir("/sys/class/drm");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && !buf[0]) {
            if (strncmp(e->d_name, "card", 4) != 0 || !strchr(e->d_name, '-'))
                continue;
            char path[PATH_MAX], line[64];
            snprintf(path, sizeof path, "/sys/class/drm/%s/modes", e->d_name);
            if (sk_read_line(path, line, sizeof line) == 0 && line[0])
                snprintf(buf, n, "%s（%s）", line, e->d_name);
        }
        closedir(d);
    }
    if (!buf[0]) {
        char line[64];
        if (sk_read_line("/sys/class/graphics/fb0/virtual_size", line, sizeof line) == 0 && line[0]) {
            char *comma = strchr(line, ',');
            if (comma)
                *comma = 'x';
            snprintf(buf, n, "%s（fb0）", line);
        }
    }
}

static void cpu_model(char *buf, size_t n)
{
    sk_strlcpy(buf, "未知", n);
    char *s = sk_read_file("/proc/cpuinfo", NULL);
    if (!s)
        return;
    char *p = strstr(s, "model name");
    if (p) {
        char *colon = strchr(p, ':'), *nl = strchr(p, '\n');
        if (colon && (!nl || colon < nl)) {
            if (nl)
                *nl = 0;
            colon++;
            while (*colon == ' ' || *colon == '\t')
                colon++;
            sk_strlcpy(buf, colon, n);
        }
    }
    free(s);
}

int applet_sysinfo(int argc, char **argv)
{
    static const struct option lo[] = { { "no-color", no_argument, NULL, 'N' }, { 0, 0, 0, 0 } };
    int color = sb_color_enabled(STDOUT_FILENO), c;
    while ((c = sb_getopt_long(argc, argv, "", lo, usage)) != -1) {
        if (c != 'N')
            return 1;
        color = 0;
    }
    char info[16][640];
    int ni = 0;
    struct utsname u;
    uname(&u);
    char user[64], buf[400], buf2[400];
    sk_user_name(geteuid(), user, sizeof user);

    /* 第一行 用户@主机，第二行同样长的分隔线 */
    char title[200];
    snprintf(title, sizeof title, "%s@%s", user, u.nodename);
    if (color)
        snprintf(info[ni++], 256, "\033[1;36m%s\033[0m@\033[1;36m%s\033[0m", user, u.nodename);
    else
        sk_strlcpy(info[ni++], title, sizeof info[0]);
    int tw = sk_utf8_width(title);
    for (int i = 0; i < tw && i < 200; i++)
        info[ni][i] = '-';
    info[ni][tw < 200 ? tw : 199] = 0;
    ni++;

#define ITEM(label, ...)                                                                     \
    do {                                                                                     \
        char v_[512];                                                                        \
        snprintf(v_, sizeof v_, __VA_ARGS__);                                                \
        snprintf(info[ni++], sizeof info[0], "%s%s%s %s", color ? "\033[1;33m" : "", label, color ? "\033[0m" : "", v_); \
    } while (0)

    os_name(buf, sizeof buf);
    ITEM("系统:", "%s（手绘风格的教学操作系统）", buf);
    ITEM("内核:", "%s %s", u.sysname, u.release);
    ITEM("架构:", "%s", u.machine);
    double up = 0;
    sk_read_uptime(&up, NULL);
    sk_fmt_duration(buf, sizeof buf, (uint64_t)up);
    ITEM("运行时间:", "%s", buf);
    int nproc = 0;
    sk_procinfo *procs = sk_list_procs(&nproc);
    free(procs);
    ITEM("进程:", "%d 个", nproc);
    const char *sh = getenv("SHELL");
    ITEM("Shell:", "%s", sh ? sb_basename(sh, buf2, sizeof buf2) : "sksh");
    const char *term = getenv("TERM");
    ITEM("终端:", "%s", term ? term : "未知");
    resolution(buf, sizeof buf);
    if (buf[0])
        ITEM("分辨率:", "%s", buf);
    cpu_model(buf, sizeof buf);
    ITEM("CPU:", "%s", buf);
    sk_meminfo mi;
    if (sk_read_meminfo(&mi) == 0 && mi.total_kb) {
        uint64_t used = mi.total_kb - mi.available_kb;
        sk_fmt_size_short(buf, sizeof buf, used * 1024);
        sk_fmt_size_short(buf2, sizeof buf2, mi.total_kb * 1024);
        ITEM("内存:", "%s / %s（%d%%）", buf, buf2, (int)(used * 100 / mi.total_kb));
    }
#undef ITEM
    info[ni++][0] = 0;
    /* 调色板：8 个普通色 + 8 个亮色，用背景色画小方块 */
    if (color) {
        char *p = info[ni];
        size_t k = 0;
        for (int i = 0; i < 8; i++)
            k += (size_t)snprintf(p + k, sizeof info[0] - k, "\033[4%dm   ", i);
        snprintf(p + k, 256 - k, "\033[0m");
        ni++;
        p = info[ni];
        k = 0;
        for (int i = 0; i < 8; i++)
            k += (size_t)snprintf(p + k, sizeof info[0] - k, "\033[10%dm   ", i);
        snprintf(p + k, 256 - k, "\033[0m");
        ni++;
    }

    int nlogo = (int)SK_ARRAY_LEN(g_logo);
    int rows = SK_MAX(nlogo, ni);
    for (int r = 0; r < rows; r++) {
        if (r < nlogo) {
            const char *l = g_logo[r];
            int ci = (unsigned char)l[0];
            if (color && ci < (int)SK_ARRAY_LEN(g_logo_colors))
                fputs(g_logo_colors[ci], stdout);
            fputs(l + 1, stdout);
            if (color)
                fputs("\033[0m", stdout);
        } else {
            printf("%*s", (int)strlen(g_logo[0] + 1), "");
        }
        if (r < ni)
            printf("  %s", info[r]);
        putchar('\n');
    }
    return 0;
}
