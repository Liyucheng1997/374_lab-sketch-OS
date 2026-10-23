/*
 * printf — 按格式输出（和 C 语言的 printf 用法一样）
 *
 * shell 里没有类型，所有参数都是字符串；printf 根据格式里的 %d、%f 等
 * 把对应参数当成整数、浮点数来解析，再交给 C 库的 printf 去排版。
 * 格式用完了而参数还有剩余时，会从头再用一遍格式（方便批量输出表格）。
 */
#include "skbox.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(printf, "按格式输出文字和数字")

static const char usage[] =
    "用法: printf 格式 [参数...]\n"
    "按“格式”输出参数。格式里的普通字符原样输出，转义序列和 % 指令会被替换：\n\n"
    "  \\n 换行  \\t 制表  \\\\ 反斜杠  \\\" 双引号  \\a 响铃  \\e ESC  \\c 停止输出\n"
    "  \\NNN 八进制字节  \\xHH 十六进制字节  \\uHHHH Unicode 字符\n\n"
    "  %s 字符串   %d %i 整数   %u 无符号整数   %o 八进制   %x %X 十六进制\n"
    "  %f %e %g 浮点数   %c 字符   %b 解释参数里的转义   %q 加上 shell 引号   %% 百分号\n"
    "  可以带标志 -+ #0、宽度和精度，例如 %-10s %05d %.2f %*d\n\n"
    "整数参数可以写成 0x1F（十六进制）、017（八进制）或 'A（字符的编码）。\n"
    "格式用完而参数还有剩余时，会重复使用格式。\n"
    "例如: printf '%-6s|%4d\\n' 苹果 3 香蕉 12\n";

static int g_status;    /* 有参数转换失败时为 1 */

static int isodigit_c(int c)
{
    return c >= '0' && c <= '7';
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 0x20;
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

static void put_utf8(uint32_t cp)
{
    if (cp < 0x80) {
        putchar((int)cp);
    } else if (cp < 0x800) {
        putchar((int)(0xC0 | (cp >> 6)));
        putchar((int)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        putchar((int)(0xE0 | (cp >> 12)));
        putchar((int)(0x80 | ((cp >> 6) & 0x3F)));
        putchar((int)(0x80 | (cp & 0x3F)));
    } else {
        putchar((int)(0xF0 | (cp >> 18)));
        putchar((int)(0x80 | ((cp >> 12) & 0x3F)));
        putchar((int)(0x80 | ((cp >> 6) & 0x3F)));
        putchar((int)(0x80 | (cp & 0x3F)));
    }
}

/* 处理 p 处的一个反斜杠转义并输出，返回反斜杠之后消耗的字符数。
 * octal_0：%b 参数里八进制写作 \0NNN。遇到 \c 时直接结束程序（不再输出任何东西）。 */
static size_t put_escape(const char *p, int octal_0)
{
    const char *s = p + 1;
    if (*s == 'x') {
        int v = 0, n = 0;
        for (s++; n < 2 && hexval((unsigned char)*s) >= 0; n++, s++)
            v = v * 16 + hexval((unsigned char)*s);
        if (n == 0) {
            sb_err("转义序列 \\x 后面缺少十六进制数字");
            exit(1);
        }
        putchar(v);
    } else if (isodigit_c(*s)) {
        int v = 0, n = 0;
        if (octal_0 && *s == '0')
            s++;
        for (; n < 3 && isodigit_c(*s); n++, s++)
            v = v * 8 + (*s - '0');
        putchar(v);
    } else if (*s == 'u' || *s == 'U') {
        int want = *s == 'u' ? 4 : 8;
        uint32_t cp = 0;
        int n = 0;
        for (s++; n < want && hexval((unsigned char)*s) >= 0; n++, s++)
            cp = cp * 16 + (uint32_t)hexval((unsigned char)*s);
        if (n != want || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            sb_err("无效的 Unicode 转义");
            exit(1);
        }
        put_utf8(cp);
    } else if (*s && strchr("\"\\abcefnrtv", *s)) {
        switch (*s) {
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'c': fflush(stdout); exit(g_status);
        case 'e': putchar('\033'); break;
        case 'f': putchar('\f'); break;
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case 'v': putchar('\v'); break;
        default:  putchar(*s); break;
        }
        s++;
    } else {
        putchar('\\');
        if (*s)
            putchar(*s++);
    }
    return (size_t)(s - p - 1);
}

/* 检查数字转换的结果，出错时打印信息（但仍然继续，和 GNU 一致） */
static void check_conv(const char *arg, const char *end)
{
    if (errno == ERANGE) {
        sb_err("'%s': 数值超出范围", arg);
        g_status = 1;
    } else if (end == arg) {
        sb_err("'%s': 应为数值", arg);
        g_status = 1;
    } else if (*end) {
        sb_err("'%s': 数值没有完全转换", arg);
        g_status = 1;
    }
}

/* 'A 或 "A 形式：取字符的编码（UTF-8 下是 Unicode 码位） */
static int char_value(const char *arg, long long *v)
{
    if ((arg[0] == '\'' || arg[0] == '"') && arg[1]) {
        uint32_t cp;
        int k = sk_utf8_decode(arg + 1, strlen(arg + 1), &cp);
        *v = cp == 0xFFFD ? (unsigned char)arg[1] : (long long)cp;
        if (arg[1 + k])
            sb_err("警告: %s: 后面的字符被忽略", arg);
        return 1;
    }
    return 0;
}

static intmax_t arg_int(const char *arg)
{
    long long v;
    if (char_value(arg, &v))
        return v;
    char *end;
    errno = 0;
    intmax_t r = strtoimax(arg, &end, 0);
    check_conv(arg, end);
    return r;
}

static uintmax_t arg_uint(const char *arg)
{
    long long v;
    if (char_value(arg, &v))
        return (uintmax_t)v;
    char *end;
    errno = 0;
    uintmax_t r = strtoumax(arg, &end, 0);
    check_conv(arg, end);
    return r;
}

static long double arg_float(const char *arg)
{
    long long v;
    if (char_value(arg, &v))
        return (long double)v;
    char *end;
    errno = 0;
    long double r = strtold(arg, &end);
    check_conv(arg, end);
    return r;
}

/* %q：输出能被 shell 原样读回的写法 */
static void put_quoted(const char *s)
{
    if (!*s) {
        fputs("''", stdout);
        return;
    }
    int plain = 1, ctrl = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7F)
            ctrl = 1;
        if (!(isalnum(*p) || strchr("_@%+=:,./-", *p) || *p >= 0x80))
            plain = 0;
    }
    if (plain) {
        fputs(s, stdout);
    } else if (!ctrl) {
        putchar('\'');
        for (; *s; s++) {
            if (*s == '\'')
                fputs("'\\''", stdout);
            else
                putchar(*s);
        }
        putchar('\'');
    } else {
        fputs("$'", stdout);
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
            case '\n': fputs("\\n", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\033': fputs("\\E", stdout); break;
            case '\'': fputs("\\'", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            default:
                if (*p < 0x20 || *p == 0x7F)
                    printf("\\%03o", *p);
                else
                    putchar(*p);
            }
        }
        putchar('\'');
    }
}

/* 执行一遍格式串，返回用掉的参数个数 */
static int run_format(const char *fmt, int argc, char **argv)
{
    int used = 0;
#define NEXT_ARG() (used < argc ? argv[used++] : NULL)
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\') {
            p += put_escape(p, 0);
            continue;
        }
        if (*p != '%') {
            putchar(*p);
            continue;
        }
        if (p[1] == '%') {
            putchar('%');
            p++;
            continue;
        }

        /* 收集 %[标志][宽度][.精度]，把 * 替换成参数给出的数字 */
        char spec[64];
        size_t sl = 0;
        spec[sl++] = '%';
        const char *q = p + 1;
        while (*q && strchr("-+ #0'", *q)) {
            if (sl < 20)
                spec[sl++] = *q;
            q++;
        }
        char num[32];
        if (*q == '*') {
            const char *a = NEXT_ARG();
            snprintf(num, sizeof num, "%d", a ? (int)arg_int(a) : 0);
            sk_strlcpy(spec + sl, num, sizeof spec - sl);
            sl = strlen(spec);
            q++;
        } else {
            while (isdigit((unsigned char)*q)) {
                if (sl < 40)
                    spec[sl++] = *q;
                q++;
            }
        }
        if (*q == '.') {
            spec[sl++] = '.';
            q++;
            if (*q == '*') {
                const char *a = NEXT_ARG();
                snprintf(num, sizeof num, "%d", a ? (int)arg_int(a) : 0);
                sk_strlcpy(spec + sl, num, sizeof spec - sl);
                sl = strlen(spec);
                q++;
            } else {
                while (isdigit((unsigned char)*q)) {
                    if (sl < 50)
                        spec[sl++] = *q;
                    q++;
                }
            }
        }
        while (*q && strchr("hlLqjzt", *q))   /* 长度修饰符没有意义（参数本来就是字符串），忽略 */
            q++;
        char conv = *q;
        spec[sl] = 0;
        const char *arg;

        switch (conv) {
        case 'd':
        case 'i':
            arg = NEXT_ARG();
            sk_strlcat(spec, "jd", sizeof spec);
            printf(spec, arg ? arg_int(arg) : (intmax_t)0);
            break;
        case 'o':
        case 'u':
        case 'x':
        case 'X': {
            arg = NEXT_ARG();
            char tail[3] = { 'j', conv, 0 };
            sk_strlcat(spec, tail, sizeof spec);
            printf(spec, arg ? arg_uint(arg) : (uintmax_t)0);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            arg = NEXT_ARG();
            char tail[3] = { 'L', conv, 0 };
            sk_strlcat(spec, tail, sizeof spec);
            printf(spec, arg ? arg_float(arg) : 0.0L);
            break;
        }
        case 'c':
            arg = NEXT_ARG();
            sk_strlcat(spec, "c", sizeof spec);
            printf(spec, arg && *arg ? (unsigned char)arg[0] : 0);
            break;
        case 's':
            arg = NEXT_ARG();
            sk_strlcat(spec, "s", sizeof spec);
            printf(spec, arg ? arg : "");
            break;
        case 'b':
            arg = NEXT_ARG();
            if (arg) {
                for (const char *b = arg; *b; b++) {
                    if (*b == '\\')
                        b += put_escape(b, 1);
                    else
                        putchar(*b);
                }
            }
            break;
        case 'q':
            arg = NEXT_ARG();
            put_quoted(arg ? arg : "");
            break;
        default:
            if (conv)
                sb_err("'%.*s': 无效的格式指令", (int)(q - p + 1), p);
            else
                sb_err("格式 '%s' 以 %% 结尾", fmt);
            exit(1);
        }
        p = q;
    }
#undef NEXT_ARG
    return used;
}

int applet_printf(int argc, char **argv)
{
    if (sb_help(argc, argv, usage))
        return 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        sb_err("缺少操作数");
        sb_hint();
        return 1;
    }
    const char *fmt = argv[i++];
    int nargs = argc - i;
    char **args = argv + i;
    do {
        int used = run_format(fmt, nargs, args);
        if (used == 0) {
            if (nargs > 0)
                sb_err("警告: 忽略多余的参数，从 '%s' 开始", args[0]);
            break;
        }
        args += used;
        nargs -= used;
    } while (nargs > 0);
    return g_status;
}
