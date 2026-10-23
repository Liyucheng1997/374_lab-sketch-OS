/*
 * touch — 修改文件的时间戳；文件不存在时创建一个空文件
 *
 * 每个 inode 记着三个时间：
 *   atime 最后一次读取    mtime 最后一次修改内容    ctime 最后一次修改 inode 本身（权限、链接数……）
 * utimensat() 可以设置 atime 和 mtime；ctime 由内核维护，谁也改不了（改时间这件事本身就会更新它）。
 */
#include "skbox.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(touch, "修改文件时间或创建空文件")

static const char usage[] =
    "用法: touch [选项] 文件...\n"
    "把文件的访问时间和修改时间设为当前时间；文件不存在时创建空文件。\n\n"
    "  -a          只修改访问时间（atime）\n"
    "  -m          只修改修改时间（mtime）\n"
    "  -c          不创建不存在的文件\n"
    "  -d 日期     使用指定时间，如 \"2026-10-23 13:55:00\"、\"2026-10-23\"、\"@1700000000\"\n"
    "  -t 时间戳   使用 [[CC]YY]MMDDhhmm[.ss] 格式的时间\n"
    "  -r 文件     使用参考文件的时间\n";

/* 解析 -t 的 [[CC]YY]MMDDhhmm[.ss] */
static int parse_t(const char *s, struct timespec *out)
{
    char digits[16];
    int sec = 0;
    const char *dot = strchr(s, '.');
    size_t n = dot ? (size_t)(dot - s) : strlen(s);
    if (n != 8 && n != 10 && n != 12)
        return -1;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i]))
            return -1;
    if (dot) {
        if (strlen(dot + 1) != 2 || !isdigit((unsigned char)dot[1]) || !isdigit((unsigned char)dot[2]))
            return -1;
        sec = (dot[1] - '0') * 10 + (dot[2] - '0');
    }
    memcpy(digits, s, n);
    digits[n] = 0;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    const char *p = digits;
#define TWO(q) (((q)[0] - '0') * 10 + ((q)[1] - '0'))
    if (n == 12) {
        tm.tm_year = TWO(p) * 100 + TWO(p + 2) - 1900;
        p += 4;
    } else if (n == 10) {
        int yy = TWO(p);
        tm.tm_year = (yy < 69 ? 2000 + yy : 1900 + yy) - 1900;   /* POSIX：69~99 是 19xx 年 */
        p += 2;
    }
    tm.tm_mon = TWO(p) - 1;
    tm.tm_mday = TWO(p + 2);
    tm.tm_hour = TWO(p + 4);
    tm.tm_min = TWO(p + 6);
#undef TWO
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 ||
        tm.tm_hour > 23 || tm.tm_min > 59 || sec > 60)
        return -1;
    out->tv_sec = mktime(&tm);
    out->tv_nsec = 0;
    return out->tv_sec == (time_t)-1 ? -1 : 0;
}

int applet_touch(int argc, char **argv)
{
    int only_a = 0, only_m = 0, no_create = 0, have_time = 0, c;
    struct timespec ts[2];
    while ((c = sb_getopt(argc, argv, "amcd:t:r:f", usage)) != -1) {
        switch (c) {
        case 'a': only_a = 1; break;
        case 'm': only_m = 1; break;
        case 'c': no_create = 1; break;
        case 'f': break;
        case 'd':
            if (sb_parse_date(optarg, 0, &ts[0]) != 0) {
                sb_err("无效的日期格式 '%s'", optarg);
                return 1;
            }
            ts[1] = ts[0];
            have_time = 1;
            break;
        case 't':
            if (parse_t(optarg, &ts[0]) != 0) {
                sb_err("无效的时间戳 '%s'", optarg);
                return 1;
            }
            ts[1] = ts[0];
            have_time = 1;
            break;
        case 'r': {
            struct stat st;
            if (stat(optarg, &st) != 0) {
                sb_perr("无法读取参考文件 '%s' 的时间", optarg);
                return 1;
            }
            ts[0] = st.st_atim;
            ts[1] = st.st_mtim;
            have_time = 1;
            break;
        }
        default:
            return 1;
        }
    }
    if (optind >= argc) {
        sb_err("缺少文件操作数");
        sb_hint();
        return 1;
    }
    if (!have_time) {
        /* UTIME_NOW：让内核填当前时间（这样不需要权限检查以外的东西） */
        ts[0].tv_sec = ts[1].tv_sec = 0;
        ts[0].tv_nsec = ts[1].tv_nsec = UTIME_NOW;
    }
    if (only_a && !only_m)
        ts[1].tv_nsec = UTIME_OMIT;   /* UTIME_OMIT：这个时间保持不变 */
    if (only_m && !only_a)
        ts[0].tv_nsec = UTIME_OMIT;

    int rc = 0;
    for (int i = optind; i < argc; i++) {
        const char *f = argv[i];
        if (strcmp(f, "-") == 0) {
            if (futimens(STDOUT_FILENO, ts) != 0) {
                sb_perr("无法修改标准输出的时间");
                rc = 1;
            }
            continue;
        }
        if (utimensat(AT_FDCWD, f, ts, 0) == 0)
            continue;
        if (errno != ENOENT) {
            sb_perr("无法修改 '%s' 的时间", f);
            rc = 1;
            continue;
        }
        if (no_create)
            continue;
        int fd = open(f, O_WRONLY | O_CREAT | O_NONBLOCK | O_NOCTTY | O_CLOEXEC, 0666);
        if (fd < 0) {
            sb_perr("无法创建 '%s'", f);
            rc = 1;
            continue;
        }
        if (have_time && futimens(fd, ts) != 0) {
            sb_perr("无法修改 '%s' 的时间", f);
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
