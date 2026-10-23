/*
 * cal — 显示日历
 *
 * 算星期几用的是“蔡勒公式”的一个变体（Sakamoto 算法）：把一月、二月看作上一年的
 * 13、14 月，闰年的影响就只落在“年份/4 - 年份/100 + 年份/400”这一项里。
 * 这里一律按公历（格里高利历）计算，不处理 1582/1752 年的历法切换。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(cal, "显示月历或年历")

static const char usage[] =
    "用法: cal [选项] [[月] 年]\n"
    "显示日历。不带参数时显示本月；只给年份时显示全年；给出月和年时显示那个月。\n"
    "输出到终端时，今天会用反色标出。\n\n"
    "  -y    显示今年全年\n"
    "  -3    显示上个月、本月和下个月\n"
    "  -m    星期一作为一周的第一天（默认星期日）\n";

static const char *const g_month_names[12] = {
    "一月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "十一月", "十二月",
};

static int monday_first, highlight, today_y, today_m, today_d;

static int is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && is_leap(y) ? 29 : d[m - 1];
}

/* 0 = 星期日 */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* 一个月排成 8 行（标题、星期、最多 6 周），每行宽 20 列，写进 lines[8][128] */
static void render_month(int y, int m, int with_year, char lines[8][128])
{
    char title[64];
    if (with_year)
        snprintf(title, sizeof title, "%d年%d月", y, m);
    else
        sk_strlcpy(title, g_month_names[m - 1], sizeof title);
    int tw = sk_utf8_width(title);
    int left = (20 - tw + 1) / 2;
    snprintf(lines[0], 128, "%*s%s", left > 0 ? left : 0, "", title);
    sk_strlcpy(lines[1], monday_first ? "一 二 三 四 五 六 日" : "日 一 二 三 四 五 六", 128);

    int first = weekday(y, m, 1);
    if (monday_first)
        first = (first + 6) % 7;
    int nd = days_in(y, m);
    int day = 1;
    for (int row = 0; row < 6; row++) {
        char *p = lines[2 + row];
        size_t len = 0;
        p[0] = 0;
        for (int col = 0; col < 7; col++) {
            int cell = row * 7 + col;
            char item[32];
            if (cell < first || day > nd) {
                snprintf(item, sizeof item, "  ");
            } else {
                int is_today = highlight && y == today_y && m == today_m && day == today_d;
                if (is_today)
                    snprintf(item, sizeof item, "\033[7m%2d\033[0m", day);
                else
                    snprintf(item, sizeof item, "%2d", day);
                day++;
            }
            len += (size_t)snprintf(p + len, 128 - len, "%s%s", col ? " " : "", item);
        }
    }
}

/* 去掉行尾空格后输出 */
static void put_trimmed(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ')
        n--;
    fwrite(s, 1, n, stdout);
    putchar('\n');
}

static int line_empty(const char *s)
{
    for (; *s; s++)
        if (*s != ' ')
            return 0;
    return 1;
}

/* 横着排几个月，各月之间空两格 */
static void print_row(const int *ys, const int *ms, int n, int with_year)
{
    char lines[3][8][128];
    for (int i = 0; i < n; i++)
        render_month(ys[i], ms[i], with_year, lines[i]);
    for (int r = 0; r < 8; r++) {
        int all_empty = r >= 2;
        for (int i = 0; i < n && all_empty; i++)
            if (!line_empty(lines[i][r]))
                all_empty = 0;
        if (all_empty && n == 1)
            continue;   /* 单个月：不输出空的周行 */
        char out[512];
        size_t len = 0;
        out[0] = 0;
        for (int i = 0; i < n; i++) {
            /* 按显示宽度补齐到 20 列（转义序列和汉字都会让字节数和列数不一致） */
            const char *s = lines[i][r];
            int w = 0;
            for (const char *q = s; *q;) {
                if (*q == '\033') {
                    while (*q && *q != 'm')
                        q++;
                    if (*q)
                        q++;
                    continue;
                }
                uint32_t cp;
                int k = sk_utf8_decode(q, strlen(q), &cp);
                w += sk_wcwidth(cp);
                q += k;
            }
            len += (size_t)snprintf(out + len, sizeof out - len, "%s%s%*s", i ? "  " : "", s,
                                    i + 1 < n ? (20 - w > 0 ? 20 - w : 0) : 0, "");
        }
        put_trimmed(out);
    }
}

static int parse_num(const char *s, int lo, int hi, int *out)
{
    long long v;
    if (sb_parse_ll(s, &v) != 0 || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

int applet_cal(int argc, char **argv)
{
    int whole_year = 0, three = 0, c;
    while ((c = sb_getopt(argc, argv, "y3m", usage)) != -1) {
        switch (c) {
        case 'y': whole_year = 1; break;
        case '3': three = 1; break;
        case 'm': monday_first = 1; break;
        default:  return 1;
        }
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    today_y = tm.tm_year + 1900;
    today_m = tm.tm_mon + 1;
    today_d = tm.tm_mday;
    highlight = isatty(STDOUT_FILENO);

    int y = today_y, m = today_m, n = argc - optind;
    if (n == 1) {
        if (parse_num(argv[optind], 1, 9999, &y) != 0) {
            sb_err("无效的年份 '%s'（应为 1~9999）", argv[optind]);
            return 1;
        }
        whole_year = 1;
    } else if (n == 2) {
        if (parse_num(argv[optind], 1, 12, &m) != 0) {
            sb_err("无效的月份 '%s'（应为 1~12）", argv[optind]);
            return 1;
        }
        if (parse_num(argv[optind + 1], 1, 9999, &y) != 0) {
            sb_err("无效的年份 '%s'（应为 1~9999）", argv[optind + 1]);
            return 1;
        }
    } else if (n > 2) {
        sb_err("多余的操作数 '%s'", argv[optind + 2]);
        sb_hint();
        return 1;
    }

    if (whole_year) {
        char title[32];
        snprintf(title, sizeof title, "%d年", y);
        int tw = sk_utf8_width(title);
        printf("%*s%s\n\n", (64 - tw) / 2, "", title);
        for (int q = 0; q < 4; q++) {
            int ys[3] = { y, y, y }, ms[3] = { q * 3 + 1, q * 3 + 2, q * 3 + 3 };
            print_row(ys, ms, 3, 0);
            if (q < 3)
                putchar('\n');
        }
        return 0;
    }
    if (three) {
        int ys[3], ms[3];
        for (int i = -1; i <= 1; i++) {
            int mm = m + i, yy = y;
            if (mm < 1) {
                mm = 12;
                yy--;
            } else if (mm > 12) {
                mm = 1;
                yy++;
            }
            ys[i + 1] = yy;
            ms[i + 1] = mm;
        }
        if (ys[0] < 1)
            ys[0] = 1;
        print_row(ys, ms, 3, 1);
        return 0;
    }
    int ys[1] = { y }, ms[1] = { m };
    print_row(ys, ms, 1, 1);
    return 0;
}
