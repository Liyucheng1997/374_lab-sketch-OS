/*
 * echo — 把参数打印到标准输出
 */
#include "skbox.h"

#include <string.h>

SKBOX_APPLET(echo, "显示一行文字")

static const char usage[] =
    "用法: echo [-neE] [字符串...]\n"
    "把参数用空格连接后输出，最后换行。\n\n"
    "  -n    最后不输出换行\n"
    "  -e    解释反斜杠转义：\\n 换行 \\t 制表 \\\\ 反斜杠 \\a 响铃 \\b 退格 \\e ESC\n"
    "        \\c 停止输出  \\0NNN 八进制字节  \\xHH 十六进制字节\n"
    "  -E    不解释转义（默认）\n";

static int hexval(char h)
{
    if (h >= '0' && h <= '9')
        return h - '0';
    if (h >= 'a' && h <= 'f')
        return h - 'a' + 10;
    if (h >= 'A' && h <= 'F')
        return h - 'A' + 10;
    return -1;
}

/* 输出带转义的字符串；遇到 \c 返回 1（表示后面全部不再输出） */
static int put_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            putchar(*s);
            continue;
        }
        s++;
        int v;
        switch (*s) {
        case 'a': putchar('\a'); break;
        case 'b': putchar('\b'); break;
        case 'c': return 1;
        case 'e': putchar('\033'); break;
        case 'f': putchar('\f'); break;
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case 'v': putchar('\v'); break;
        case '\\': putchar('\\'); break;
        case '0':
            v = 0;
            for (int i = 0; i < 3 && s[1] >= '0' && s[1] <= '7'; i++)
                v = v * 8 + (*++s - '0');
            putchar(v);
            break;
        case 'x':
            if (hexval(s[1]) < 0) {
                putchar('\\');
                putchar('x');
                break;
            }
            v = 0;
            for (int i = 0; i < 2 && hexval(s[1]) >= 0; i++)
                v = v * 16 + hexval(*++s);
            putchar(v);
            break;
        default:
            putchar('\\');
            putchar(*s);
            break;
        }
    }
    return 0;
}

int applet_echo(int argc, char **argv)
{
    if (sb_help(argc, argv, usage))
        return 0;
    int newline = 1, escapes = 0, i = 1;
    /* 只有完全由 n/e/E 组成的参数才算选项，"echo -x" 会原样输出 "-x" */
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p = argv[i] + 1;
        if (strspn(p, "neE") != strlen(p))
            break;
        for (; *p; p++) {
            if (*p == 'n')
                newline = 0;
            else if (*p == 'e')
                escapes = 1;
            else
                escapes = 0;
        }
    }
    for (int first = 1; i < argc; i++, first = 0) {
        if (!first)
            putchar(' ');
        if (escapes) {
            if (put_escaped(argv[i]))
                return 0;
        } else {
            fputs(argv[i], stdout);
        }
    }
    if (newline)
        putchar('\n');
    return 0;
}
