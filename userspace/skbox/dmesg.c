/*
 * dmesg — 显示内核日志
 *
 * 内核里有一块环形缓冲区（log buffer），printk() 写进去的消息都在这里：
 * 开机时识别到了哪些硬件、驱动加载情况、出错信息……用户程序也可以写 /dev/kmsg 往里加消息
 * （skutil 的 sk_log 就是这样做的）。syslog() 系统调用（C 库里叫 klogctl）可以读出或清空它。
 * 每条消息前面有 <N> 表示级别：0 紧急 … 3 错误、4 警告 … 6 信息、7 调试。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <unistd.h>

SKBOX_APPLET(dmesg, "显示内核日志")

static const char usage[] =
    "用法: dmesg [选项]\n"
    "显示内核环形缓冲区里的日志（开机信息、驱动消息、错误……）。\n\n"
    "  -c        显示后清空日志\n"
    "  -C        只清空，不显示\n"
    "  -r        原样输出（保留每行开头的 <级别>）\n"
    "  -t        不显示时间戳\n"
    "  -l 级别   只显示不低于这个级别的消息（0 紧急 ~ 7 调试；如 -l 4 只看警告和错误）\n"
    "  -n 级别   设置打印到控制台的级别（例如 -n 1 让内核不再往屏幕上打印消息）\n";

/* klogctl 的命令号（见 man 2 syslog） */
enum { SYSLOG_READ_ALL = 3, SYSLOG_READ_CLEAR = 4, SYSLOG_CLEAR = 5, SYSLOG_CONSOLE_LEVEL = 8, SYSLOG_SIZE = 10 };

int applet_dmesg(int argc, char **argv)
{
    int clear_after = 0, clear_only = 0, raw = 0, no_time = 0, max_level = 7, c;
    long long v;
    while ((c = sb_getopt(argc, argv, "cCrtl:n:", usage)) != -1) {
        switch (c) {
        case 'c': clear_after = 1; break;
        case 'C': clear_only = 1; break;
        case 'r': raw = 1; break;
        case 't': no_time = 1; break;
        case 'l':
        case 'n':
            if (sb_parse_ll(optarg, &v) != 0 || v < 0 || v > 8) {
                sb_err("无效的级别 '%s'（0~7）", optarg);
                return 1;
            }
            if (c == 'n') {
                if (klogctl(SYSLOG_CONSOLE_LEVEL, NULL, (int)v) != 0) {
                    sb_perr("无法设置控制台日志级别");
                    return 1;
                }
                return 0;
            }
            max_level = (int)v;
            break;
        default:
            return 1;
        }
    }
    if (clear_only) {
        if (klogctl(SYSLOG_CLEAR, NULL, 0) != 0) {
            sb_perr("无法清空内核日志");
            return 1;
        }
        return 0;
    }
    int size = klogctl(SYSLOG_SIZE, NULL, 0);
    if (size <= 0)
        size = 1 << 17;
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        sb_err("内存不足");
        return 1;
    }
    int n = klogctl(clear_after ? SYSLOG_READ_CLEAR : SYSLOG_READ_ALL, buf, size);
    if (n < 0) {
        sb_perr("无法读取内核日志（需要 root 权限，或内核禁止了 dmesg）");
        free(buf);
        return 1;
    }
    buf[n] = 0;
    int color = sb_color_enabled(STDOUT_FILENO);
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *text = line;
        int level = 6;
        /* 每行开头是 <级别>，有时级别里还混着日志设施号（facility*8 + level） */
        if (text[0] == '<') {
            char *e = strchr(text, '>');
            if (e) {
                level = atoi(text + 1) & 7;
                if (!raw)
                    text = e + 1;
            }
        }
        if (level <= max_level) {
            char *msg = text;
            char *ts_end = NULL;
            if (text[0] == '[' && (ts_end = strchr(text, ']')) != NULL) {
                msg = ts_end + 1;
                if (*msg == ' ')
                    msg++;
            }
            if (no_time && ts_end) {
                text = msg;
                ts_end = NULL;
            }
            const char *lc = level <= 3 ? SB_RED SB_BOLD : level == 4 ? SB_YELLOW : NULL;
            if (color && ts_end && !raw) {
                /* 时间戳（开机后的秒数）用绿色，错误红色，警告黄色 */
                printf(SB_GREEN "%.*s" SB_RESET " ", (int)(ts_end + 1 - text), text);
                if (lc)
                    printf("%s%s" SB_RESET "\n", lc, msg);
                else
                    puts(msg);
            } else if (color && lc && !raw) {
                printf("%s%s" SB_RESET "\n", lc, text);
            } else {
                puts(text);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);
    return 0;
}
