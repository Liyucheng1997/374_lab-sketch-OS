/*
 * uptime — 系统运行了多久、平均负载
 *
 * “平均负载”是内核每 5 秒统计一次的“正在运行 + 等着运行（+ 不可中断地等待磁盘）的进程数”，
 * 再做 1 分钟、5 分钟、15 分钟的指数平均。单核 CPU 上负载 1.00 表示 CPU 刚好一直忙着；
 * 大于 1 说明有进程在排队。
 */
#include "skbox.h"

SKBOX_APPLET(uptime, "显示系统运行时间和平均负载")

static const char usage[] =
    "用法: uptime [-p | -s]\n"
    "显示当前时间、系统已经运行了多久，以及最近 1、5、15 分钟的平均负载。\n\n"
    "  -p    只显示运行了多久\n"
    "  -s    显示系统启动的时刻（年-月-日 时:分:秒）\n";

int applet_uptime(int argc, char **argv)
{
    int pretty = 0, since = 0, c;
    while ((c = sb_getopt(argc, argv, "ps", usage)) != -1) {
        switch (c) {
        case 'p': pretty = 1; break;
        case 's': since = 1; break;
        default:  return 1;
        }
    }
    double up = 0;
    if (sk_read_uptime(&up, NULL) != 0) {
        sb_perr("无法读取 /proc/uptime（/proc 挂载了吗？）");
        return 1;
    }
    char dur[64];
    sk_fmt_duration(dur, sizeof dur, (uint64_t)up);
    if (pretty) {
        printf("已运行 %s\n", dur);
        return 0;
    }
    time_t now = time(NULL);
    if (since) {
        time_t boot = now - (time_t)up;
        struct tm tm;
        localtime_r(&boot, &tm);
        char buf[64];
        sb_strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm, 0);
        puts(buf);
        return 0;
    }
    double load[3] = { 0, 0, 0 };
    int running = 0, total = 0;
    sk_read_loadavg(load, &running, &total);
    struct tm tm;
    localtime_r(&now, &tm);
    printf(" %02d:%02d:%02d 已运行 %s,  %d 个进程,  负载均值: %.2f, %.2f, %.2f\n", tm.tm_hour, tm.tm_min,
           tm.tm_sec, dur, total, load[0], load[1], load[2]);
    return 0;
}
