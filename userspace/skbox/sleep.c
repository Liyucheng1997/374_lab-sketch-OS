/*
 * sleep — 暂停一段时间
 *
 * nanosleep() 让内核把进程从“可运行队列”里拿出来，挂到一个定时器上；
 * 时间到了内核再把它唤醒。睡眠期间这个进程完全不占用 CPU（不是忙等循环）。
 */
#include "skbox.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

SKBOX_APPLET(sleep, "暂停指定的时间（可以是小数）")

static const char usage[] =
    "用法: sleep 数字[后缀]...\n"
    "暂停指定的时间，多个参数会加在一起。数字可以是小数，例如 sleep 0.5。\n"
    "后缀: s 秒（默认）  m 分钟  h 小时  d 天\n";

int applet_sleep(int argc, char **argv)
{
    int c;
    while ((c = sb_getopt(argc, argv, "+", usage)) != -1)
        return 1;
    if (optind >= argc) {
        sb_err("缺少操作数");
        sb_hint();
        return 1;
    }
    double total = 0;
    for (int i = optind; i < argc; i++) {
        char *end;
        errno = 0;
        double v = strtod(argv[i], &end);
        double mul = 1;
        if (end != argv[i] && *end && !end[1]) {
            switch (*end) {
            case 's': mul = 1; end++; break;
            case 'm': mul = 60; end++; break;
            case 'h': mul = 3600; end++; break;
            case 'd': mul = 86400; end++; break;
            }
        }
        if (end == argv[i] || *end || errno || v < 0 || isnan(v)) {
            sb_err("无效的时间间隔 '%s'", argv[i]);
            sb_hint();
            return 1;
        }
        total += v * mul;
    }
    if (total > 2147483647.0)
        total = 2147483647.0;
    struct timespec ts;
    ts.tv_sec = (time_t)total;
    ts.tv_nsec = (long)((total - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec > 999999999)
        ts.tv_nsec = 999999999;
    /* 被信号打断时，内核把剩余的时间写回 ts，接着睡就行 */
    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR) {
            sb_perr("nanosleep 失败");
            return 1;
        }
    }
    return 0;
}
