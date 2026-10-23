/*
 * lib_time.c — 时间格式化与解析（date、ls、stat、touch、ps 共用）
 *
 * Linux 内核只知道“从 1970-01-01 00:00:00 UTC 起过了多少秒”（Unix 时间戳）。
 * 年月日、星期、时区这些都是用户态 C 库根据环境变量 TZ 换算出来的：
 * SketchOS 的 sinit 给所有进程设置 TZ=CST-8，意思是“时区名 CST，比 UTC 早 8 小时”。
 */
#include "skbox.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const g_wday[7] = { "日", "一", "二", "三", "四", "五", "六" };
static const char *const g_month_full[12] = {
    "一月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "十一月", "十二月",
};

/* 往输出缓冲区追加，保证不越界、总是 NUL 结尾 */
typedef struct {
    char *buf;
    size_t cap, len;
} outbuf;

static void ob_put(outbuf *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n && o->len + 1 < o->cap; i++)
        o->buf[o->len++] = s[i];
    if (o->cap)
        o->buf[o->len] = 0;
}

static int is_numeric_conv(char c)
{
    return strchr("CdegGHIjklmMsSuUVwWyYN", c) != NULL;
}

size_t sb_strftime(char *buf, size_t n, const char *fmt, const struct tm *tm, long nsec)
{
    outbuf o = { buf, n, 0 };
    if (n)
        buf[0] = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            ob_put(&o, p, 1);
            continue;
        }
        const char *start = p++;
        /* GNU 扩展的修饰符：- 不补齐，_ 用空格补齐，0 用 0 补齐，^ 转大写；然后是可选宽度 */
        char flag = 0;
        int upper = 0;
        while (*p == '-' || *p == '_' || *p == '0' || *p == '^' || *p == '#') {
            if (*p == '^')
                upper = 1;
            else if (*p != '#')
                flag = *p;
            p++;
        }
        int width = 0;
        while (isdigit((unsigned char)*p))
            width = width * 10 + (*p++ - '0');
        int colons = 0;
        while (*p == ':')
            colons++, p++;
        char mod = 0;
        if (*p == 'E' || *p == 'O')
            mod = *p++;
        char conv = *p;
        if (!conv) {                         /* 格式串以 % 结尾：原样输出 */
            ob_put(&o, start, strlen(start));
            break;
        }

        char tmp[160];
        tmp[0] = 0;
        switch (conv) {
        case 'a':
            sk_strlcpy(tmp, g_wday[tm->tm_wday % 7], sizeof tmp);
            break;
        case 'A':
            snprintf(tmp, sizeof tmp, "星期%s", g_wday[tm->tm_wday % 7]);
            break;
        case 'b':
        case 'h':
            snprintf(tmp, sizeof tmp, "%d月", tm->tm_mon % 12 + 1);
            break;
        case 'B':
            sk_strlcpy(tmp, g_month_full[tm->tm_mon % 12], sizeof tmp);
            break;
        case 'p':
        case 'P':
            sk_strlcpy(tmp, tm->tm_hour < 12 ? "上午" : "下午", sizeof tmp);
            break;
        case 'c':
            sb_strftime(tmp, sizeof tmp, "%Y年%m月%d日 %A %H时%M分%S秒", tm, nsec);
            break;
        case 'x':
            sb_strftime(tmp, sizeof tmp, "%Y年%m月%d日", tm, nsec);
            break;
        case 'X':
            sb_strftime(tmp, sizeof tmp, "%H时%M分%S秒", tm, nsec);
            break;
        case 'r':
            sb_strftime(tmp, sizeof tmp, "%p %I时%M分%S秒", tm, nsec);
            break;
        case 'k':
            snprintf(tmp, sizeof tmp, "%2d", tm->tm_hour);
            break;
        case 'l':
            snprintf(tmp, sizeof tmp, "%2d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12);
            break;
        case 'N': {
            /* 纳秒，9 位；%3N 表示只要前 3 位（毫秒） */
            snprintf(tmp, sizeof tmp, "%09ld", nsec < 0 ? 0 : nsec % 1000000000L);
            if (width > 0 && width < 9)
                tmp[width] = 0;
            width = 0;
            break;
        }
        case 'z':
            if (colons) {
                long off = tm->tm_gmtoff;
                char sign = off < 0 ? '-' : '+';
                if (off < 0)
                    off = -off;
                if (colons == 1)
                    snprintf(tmp, sizeof tmp, "%c%02ld:%02ld", sign, off / 3600, off / 60 % 60);
                else
                    snprintf(tmp, sizeof tmp, "%c%02ld:%02ld:%02ld", sign, off / 3600, off / 60 % 60, off % 60);
            } else {
                strftime(tmp, sizeof tmp, "%z", tm);
            }
            break;
        case '%':
            sk_strlcpy(tmp, "%", sizeof tmp);
            break;
        case 'n':
            sk_strlcpy(tmp, "\n", sizeof tmp);
            break;
        case 't':
            sk_strlcpy(tmp, "\t", sizeof tmp);
            break;
        default: {
            char one[4] = { '%', 0, 0, 0 };
            if (mod) {
                one[1] = mod;
                one[2] = conv;
            } else {
                one[1] = conv;
            }
            if (strftime(tmp, sizeof tmp, one, tm) == 0)
                tmp[0] = 0;
            break;
        }
        }

        /* 应用修饰符 */
        size_t len = strlen(tmp);
        int numeric = is_numeric_conv(conv);
        if (numeric && flag == '-') {
            size_t k = 0;
            while (k + 1 < len && (tmp[k] == '0' || tmp[k] == ' '))
                k++;
            memmove(tmp, tmp + k, len - k + 1);
            len -= k;
        } else if (numeric && flag == '_') {
            for (size_t k = 0; k + 1 < len && tmp[k] == '0'; k++)
                tmp[k] = ' ';
        } else if (numeric && flag == '0') {
            for (size_t k = 0; k + 1 < len && tmp[k] == ' '; k++)
                tmp[k] = '0';
        }
        if (upper)
            for (size_t k = 0; k < len; k++)
                tmp[k] = (char)toupper((unsigned char)tmp[k]);
        if (flag != '-' && width > 0 && (size_t)width > len) {
            char padc = (numeric && flag != '_') ? '0' : ' ';
            for (size_t k = len; k < (size_t)width; k++)
                ob_put(&o, &padc, 1);
        }
        ob_put(&o, tmp, len);
    }
    return o.len;
}

void sb_fmt_time_full(char *buf, size_t n, const struct timespec *ts)
{
    struct tm tm;
    time_t t = ts->tv_sec;
    localtime_r(&t, &tm);
    sb_strftime(buf, n, "%Y-%m-%d %H:%M:%S.%N %z", &tm, ts->tv_nsec);
}

/* 解析 "HH:MM[:SS[.frac]]"，返回消耗的字符数，失败 0 */
static int parse_hms(const char *s, struct tm *tm, long *nsec)
{
    int h, m, sec = 0, k = 0, k2 = 0;
    if (sscanf(s, "%2d:%2d%n", &h, &m, &k) != 2)
        return 0;
    if (s[k] == ':') {
        if (sscanf(s + k + 1, "%2d%n", &sec, &k2) != 1)
            return 0;
        k += 1 + k2;
        if (s[k] == '.' && isdigit((unsigned char)s[k + 1])) {
            long frac = 0;
            int digits = 0;
            k++;
            while (isdigit((unsigned char)s[k])) {
                if (digits < 9) {
                    frac = frac * 10 + (s[k] - '0');
                    digits++;
                }
                k++;
            }
            while (digits++ < 9)
                frac *= 10;
            *nsec = frac;
        }
    }
    if (h > 23 || m > 59 || sec > 60)
        return 0;
    tm->tm_hour = h;
    tm->tm_min = m;
    tm->tm_sec = sec;
    return k;
}

int sb_parse_date(const char *s, int utc, struct timespec *out)
{
    while (isspace((unsigned char)*s))
        s++;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    if (strcmp(s, "now") == 0 || *s == 0) {
        *out = now;
        return 0;
    }
    if (s[0] == '@') {
        /* "@1700000000.5"：直接给出 Unix 时间戳 */
        char *end;
        long long sec = strtoll(s + 1, &end, 10);
        long nsec = 0;
        if (end == s + 1)
            return -1;
        if (*end == '.') {
            const char *q = end + 1;
            int digits = 0;
            while (isdigit((unsigned char)*q)) {
                if (digits < 9) {
                    nsec = nsec * 10 + (*q - '0');
                    digits++;
                }
                q++;
            }
            while (digits++ < 9)
                nsec *= 10;
            end = (char *)q;
        }
        if (*end)
            return -1;
        out->tv_sec = (time_t)sec;
        out->tv_nsec = nsec;
        return 0;
    }

    struct tm tm;
    time_t base = now.tv_sec;
    if (utc)
        gmtime_r(&base, &tm);
    else
        localtime_r(&base, &tm);
    tm.tm_isdst = -1;
    long nsec = 0;

    int y, mo, d, k = 0;
    if (sscanf(s, "%4d-%2d-%2d%n", &y, &mo, &d, &k) == 3 ||
        sscanf(s, "%4d/%2d/%2d%n", &y, &mo, &d, &k) == 3) {
        if (mo < 1 || mo > 12 || d < 1 || d > 31)
            return -1;
        tm.tm_year = y - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = d;
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        const char *rest = s + k;
        if (*rest == ' ' || *rest == 'T') {
            rest++;
            while (*rest == ' ')
                rest++;
            int used = parse_hms(rest, &tm, &nsec);
            if (!used)
                return -1;
            rest += used;
        }
        while (isspace((unsigned char)*rest))
            rest++;
        if (*rest)
            return -1;
    } else {
        int used = parse_hms(s, &tm, &nsec);
        if (!used)
            return -1;
        const char *rest = s + used;
        while (isspace((unsigned char)*rest))
            rest++;
        if (*rest)
            return -1;
    }
    time_t t = utc ? timegm(&tm) : mktime(&tm);
    if (t == (time_t)-1)
        return -1;
    out->tv_sec = t;
    out->tv_nsec = nsec;
    return 0;
}
