/*
 * date — 显示或设置系统时间
 *
 * 系统时钟由内核维护：开机时从主板上的实时时钟（RTC，CMOS 里那块带电池的芯片）读出初值，
 * 之后靠定时器中断往前走。clock_gettime() 读它，clock_settime() 改它（需要 root）。
 * 内核里存的永远是 UTC 时间戳，“北京时间”是 C 库按环境变量 TZ 换算出来的。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(date, "显示或设置日期和时间")

static const char usage[] =
    "用法: date [选项] [+格式]\n"
    "按指定格式显示时间（默认格式如 2026年10月23日 星期五 13:55:00 CST）。\n\n"
    "  -u          使用 UTC（世界协调时）而不是本地时区\n"
    "  -d 日期     显示指定的时间而不是现在：\"2026-10-23 13:55\"、\"2026-10-23\"、\n"
    "              \"13:55:00\"（今天）、\"@1700000000\"（Unix 时间戳）\n"
    "  -r 文件     显示文件的最后修改时间\n"
    "  -s 日期     把系统时间设置为这个时间（需要 root）\n"
    "  -I[精度]    ISO 8601 格式，精度可以是 date（默认）、hours、minutes、seconds、ns\n"
    "  -R          RFC 5322 格式（邮件里用的英文格式）\n\n"
    "常用格式符：\n"
    "  %Y 年  %m 月  %d 日  %H 时  %M 分  %S 秒  %N 纳秒  %j 一年中的第几天\n"
    "  %a 星期（一）  %A 星期（星期一）  %b 月份（10月）  %B 月份（十月）  %p 上午/下午\n"
    "  %F 即 %Y-%m-%d   %T 即 %H:%M:%S   %s Unix 时间戳   %Z 时区名  %z 时区偏移（+0800）\n"
    "  %u 星期几（1~7）  %w 星期几（0~6，0 是星期日）  %e 日（空格补齐）  %% 百分号\n"
    "  修饰：%-d 不补齐   %_H 用空格补齐   %3N 毫秒   %:z 带冒号的时区（+08:00）\n"
    "例如: date '+%Y-%m-%d %H:%M:%S'      date -d @0 -u\n";

int applet_date(int argc, char **argv)
{
    static const struct option lo[] = {
        { "utc", no_argument, NULL, 'u' },
        { "date", required_argument, NULL, 'd' },
        { "reference", required_argument, NULL, 'r' },
        { "set", required_argument, NULL, 's' },
        { "iso-8601", optional_argument, NULL, 'I' },
        { "rfc-email", no_argument, NULL, 'R' },
        { 0, 0, 0, 0 },
    };
    int utc = 0, rfc = 0, c;
    const char *dstr = NULL, *ref = NULL, *set = NULL, *iso = NULL;
    while ((c = sb_getopt_long(argc, argv, "ud:r:s:I::R", lo, usage)) != -1) {
        switch (c) {
        case 'u': utc = 1; break;
        case 'd': dstr = optarg; break;
        case 'r': ref = optarg; break;
        case 's': set = optarg; break;
        case 'I': iso = optarg ? optarg : "date"; break;
        case 'R': rfc = 1; break;
        default:  return 1;
        }
    }
    const char *fmt = "%Y年%m月%d日 %A %H:%M:%S %Z";
    if (optind < argc) {
        if (argv[optind][0] != '+') {
            sb_err("无效的参数 '%s'（格式要以 + 开头）", argv[optind]);
            sb_hint();
            return 1;
        }
        if (optind + 1 < argc) {
            sb_err("多余的操作数 '%s'", argv[optind + 1]);
            return 1;
        }
        fmt = argv[optind] + 1;
    }
    if (utc) {
        /* 最简单可靠的办法：把本进程的时区设成 UTC，后面所有的换算就都是 UTC 了 */
        setenv("TZ", "UTC0", 1);
    }
    tzset();

    struct timespec ts;
    if (set) {
        if (sb_parse_date(set, utc, &ts) != 0) {
            sb_err("无效的日期 '%s'", set);
            return 1;
        }
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            sb_perr("无法设置时间");
            return 1;
        }
    } else if (ref) {
        struct stat st;
        if (stat(ref, &st) != 0) {
            sb_perr("%s", ref);
            return 1;
        }
        ts = st.st_mtim;
    } else if (dstr) {
        if (sb_parse_date(dstr, utc, &ts) != 0) {
            sb_err("无效的日期 '%s'", dstr);
            return 1;
        }
    } else {
        clock_gettime(CLOCK_REALTIME, &ts);
    }

    struct tm tm;
    time_t t = ts.tv_sec;
    localtime_r(&t, &tm);
    char out[1024];
    if (rfc) {
        /* RFC 5322 规定必须用英文的星期和月份缩写，所以这里用 C 库自己的 strftime */
        strftime(out, sizeof out, "%a, %d %b %Y %H:%M:%S %z", &tm);
    } else if (iso) {
        const char *f;
        if (!strcmp(iso, "date"))
            f = "%Y-%m-%d";
        else if (!strcmp(iso, "hours"))
            f = "%Y-%m-%dT%H%:z";
        else if (!strcmp(iso, "minutes"))
            f = "%Y-%m-%dT%H:%M%:z";
        else if (!strcmp(iso, "seconds"))
            f = "%Y-%m-%dT%H:%M:%S%:z";
        else if (!strcmp(iso, "ns"))
            f = "%Y-%m-%dT%H:%M:%S,%N%:z";
        else {
            sb_err("-I 的参数无效: '%s'（可用 date、hours、minutes、seconds、ns）", iso);
            return 1;
        }
        sb_strftime(out, sizeof out, f, &tm, ts.tv_nsec);
    } else {
        sb_strftime(out, sizeof out, fmt, &tm, ts.tv_nsec);
    }
    puts(out);
    return 0;
}
