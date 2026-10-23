/*
 * seq — 输出一串数字
 */
#include "skbox.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(seq, "输出一个数列")

static const char usage[] =
    "用法: seq [选项] 末项\n"
    "  或: seq [选项] 首项 末项\n"
    "  或: seq [选项] 首项 增量 末项\n"
    "从首项（默认 1）开始，按增量（默认 1）输出数字，直到超过末项。可以是小数和负数。\n\n"
    "  -s 分隔符   数字之间的分隔符（默认换行）\n"
    "  -w          用前导 0 补齐，使所有数字一样宽\n"
    "  -f 格式     用 printf 风格的浮点格式输出，例如 -f %.3f\n";

/* 参数是不是一个数（用来区分负数 "-1" 和选项 "-s"） */
static int looks_numeric(const char *s)
{
    char *end;
    errno = 0;
    strtold(s, &end);
    return end != s && *end == 0;
}

typedef struct {
    long double v;
    int is_int;      /* 没有小数点和指数 */
    int frac;        /* 小数位数 */
} number;

static int parse_number(const char *s, number *n)
{
    char *end;
    errno = 0;
    n->v = strtold(s, &end);
    if (end == s || *end || errno == ERANGE) {
        sb_err("无效的浮点数参数 '%s'", s);
        sb_hint();
        return -1;
    }
    const char *dot = strchr(s, '.');
    n->is_int = !dot && !strpbrk(s, "eEpPxXnNiI");
    n->frac = 0;
    if (dot && !strpbrk(s, "eE")) {
        for (const char *p = dot + 1; isdigit((unsigned char)*p); p++)
            n->frac++;
    }
    return 0;
}

/* 校验 -f 的格式：恰好一个浮点转换（%e %f %g %a 等），把它改写成 long double 版本 */
static int make_float_format(const char *user, char *out, size_t n)
{
    size_t k = 0;
    int convs = 0;
    for (const char *p = user; *p && k + 4 < n; p++) {
        if (*p != '%') {
            out[k++] = *p;
            continue;
        }
        if (p[1] == '%') {
            out[k++] = '%';
            out[k++] = '%';
            p++;
            continue;
        }
        out[k++] = *p++;
        while (*p && strchr("-+ #0'", *p) && k + 4 < n)
            out[k++] = *p++;
        while ((isdigit((unsigned char)*p) || *p == '.') && k + 4 < n)
            out[k++] = *p++;
        if (!*p || !strchr("aAeEfFgG", *p))
            return -1;
        out[k++] = 'L';
        out[k++] = *p;
        convs++;
    }
    out[k] = 0;
    return convs == 1 ? 0 : -1;
}

int applet_seq(int argc, char **argv)
{
    const char *sep = "\n", *ufmt = NULL;
    int equal_width = 0, i = 1;

    /* 手工解析选项：负数（如 seq -3 3）不能被当成选项 */
    while (i < argc && argv[i][0] == '-' && argv[i][1] && !looks_numeric(argv[i])) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "--help") == 0) {
            fputs(usage, stdout);
            return 0;
        }
        if (a[1] == 's' || a[1] == 'f') {
            const char *val = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
            if (!val) {
                sb_err("选项需要一个参数 -- '%c'", a[1]);
                sb_hint();
                return 1;
            }
            if (a[1] == 's')
                sep = val;
            else
                ufmt = val;
        } else if (a[1] == 'w' && !a[2]) {
            equal_width = 1;
        } else {
            sb_err("无效的选项 -- '%c'", a[1]);
            sb_hint();
            return 1;
        }
        i++;
    }
    int n = argc - i;
    if (n < 1 || n > 3) {
        if (n < 1)
            sb_err("缺少操作数");
        else
            sb_err("多余的操作数 '%s'", argv[i + 3]);
        sb_hint();
        return 1;
    }
    number first = { 1, 1, 0 }, step = { 1, 1, 0 }, last;
    if (n >= 2 && parse_number(argv[i], &first) != 0)
        return 1;
    if (n == 3 && parse_number(argv[i + 1], &step) != 0)
        return 1;
    if (parse_number(argv[argc - 1], &last) != 0)
        return 1;
    if (step.v == 0) {
        sb_err("无效的零增量值 '%s'", argv[i + 1]);
        sb_hint();
        return 1;
    }

    char fmt[128];
    if (ufmt) {
        if (equal_width) {
            sb_err("不能同时使用 -f 和 -w");
            return 1;
        }
        if (make_float_format(ufmt, fmt, sizeof fmt) != 0) {
            sb_err("格式 '%s' 必须恰好包含一个浮点转换（如 %%f、%%g）", ufmt);
            return 1;
        }
    } else {
        /* 小数位数取首项和增量里较多的那个（和 GNU seq 一致） */
        snprintf(fmt, sizeof fmt, "%%.%dLf", SK_MAX(first.frac, step.frac));
    }

    /* 整数且不需要特殊格式时用 long long 精确计算，避免浮点误差 */
    int use_int = !ufmt && first.is_int && step.is_int && last.is_int &&
                  first.v > -9e18L && first.v < 9e18L && last.v > -9e18L && last.v < 9e18L &&
                  step.v > -9e18L && step.v < 9e18L;

    int width = 0;
    if (equal_width) {
        char a[64], b[64];
        if (use_int) {
            snprintf(a, sizeof a, "%lld", (long long)first.v);
            snprintf(b, sizeof b, "%lld", (long long)last.v);
        } else {
            snprintf(a, sizeof a, fmt, first.v);
            snprintf(b, sizeof b, fmt, last.v);
        }
        width = SK_MAX((int)strlen(a), (int)strlen(b));
    }

    for (long long k = 0;; k++) {
        char out[128];
        if (use_int) {
            long long a = (long long)first.v, s = (long long)step.v, l = (long long)last.v;
            long long x = a + k * s;   /* 用乘法而不是累加：每一项都直接算，没有误差积累 */
            if ((s > 0 && x > l) || (s < 0 && x < l))
                break;
            snprintf(out, sizeof out, "%lld", x);
        } else {
            long double x = first.v + (long double)k * step.v;
            if ((step.v > 0 && x > last.v) || (step.v < 0 && x < last.v))
                break;
            snprintf(out, sizeof out, fmt, x);
        }
        if (k > 0)
            fputs(sep, stdout);
        if (equal_width) {
            int len = (int)strlen(out);
            const char *digits = out;
            if (*digits == '-') {
                putchar('-');
                digits++;
            }
            for (int z = len; z < width; z++)
                putchar('0');
            fputs(digits, stdout);
        } else {
            fputs(out, stdout);
        }
        if (ferror(stdout))
            return 1;
    }
    /* 至少输出了一个数时才补结尾换行 */
    if ((step.v > 0 && first.v <= last.v) || (step.v < 0 && first.v >= last.v))
        putchar('\n');
    return 0;
}
