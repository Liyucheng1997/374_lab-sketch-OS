/*
 * test / [ — 计算条件表达式，用退出状态表示真假
 *
 * shell 的 if 并不懂“条件”，它只看命令的退出状态：0 为真，非 0 为假。
 *   if [ -f /etc/profile ]; then ...      其实就是运行了命令 "[" 参数 "-f" "/etc/profile" "]"
 * 这就是为什么 [ 后面和 ] 前面必须有空格：它们是独立的参数。
 */
#include "skbox.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(test, "检查文件类型、比较字符串和数字（也可以写成 [ ... ]）")

static const char usage[] =
    "用法: test 表达式\n"
    "  或: [ 表达式 ]\n"
    "计算表达式：为真时退出状态 0，为假时 1，表达式有错时 2。\n\n"
    "文件测试：\n"
    "  -e 文件 存在       -f 普通文件       -d 目录          -L / -h 符号链接\n"
    "  -r / -w / -x 可读 / 可写 / 可执行     -s 存在且非空     -p 命名管道\n"
    "  -b 块设备   -c 字符设备   -S 套接字   -u setuid   -g setgid   -k 粘滞位\n"
    "  -O 属于当前用户   -G 属于当前组   -t 文件描述符  是终端\n"
    "  文件1 -nt 文件2  更新      文件1 -ot 文件2  更旧      文件1 -ef 文件2  是同一个文件\n"
    "字符串：\n"
    "  -z 串 长度为 0     -n 串 长度不为 0     串 单独一个串：不为空即真\n"
    "  串1 = 串2   串1 == 串2   串1 != 串2   串1 < 串2   串1 > 串2（按字节比较）\n"
    "整数：\n"
    "  数1 -eq 数2  等于   -ne 不等于   -lt 小于   -le 小于等于   -gt 大于   -ge 大于等于\n"
    "组合：\n"
    "  ! 表达式  非      表达式1 -a 表达式2  与      表达式1 -o 表达式2  或      ( 表达式 )\n";

static char **g_args;
static int g_n, g_pos;

static void fail(const char *fmt, const char *arg) __attribute__((noreturn));
static void fail(const char *fmt, const char *arg)
{
    char msg[512];
    snprintf(msg, sizeof msg, fmt, arg);
    sb_err("%s", msg);
    exit(2);
}

static long long to_int(const char *s)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    char *end;
    errno = 0;
    long long v = strtoll(p, &end, 10);
    if (end == p || errno)
        fail("需要整数表达式: '%s'", s);
    while (isspace((unsigned char)*end))
        end++;
    if (*end)
        fail("需要整数表达式: '%s'", s);
    return v;
}

static int is_unary(const char *op)
{
    return op[0] == '-' && op[1] && !op[2] && strchr("bcdefgGhLknOprsStuwxz", op[1]);
}

static int is_binary(const char *op)
{
    static const char *const ops[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt",
                                       "-ge", "-nt", "-ot", "-ef", NULL };
    for (int i = 0; ops[i]; i++)
        if (strcmp(op, ops[i]) == 0)
            return 1;
    return 0;
}

static int unary(const char *op, const char *arg)
{
    struct stat st;
    switch (op[1]) {
    case 'z': return arg[0] == 0;
    case 'n': return arg[0] != 0;
    case 't': return isatty((int)to_int(arg));
    case 'h':
    case 'L': return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
    }
    if (stat(arg, &st) != 0)
        return 0;
    switch (op[1]) {
    case 'e': return 1;
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'b': return S_ISBLK(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case 'S': return S_ISSOCK(st.st_mode);
    case 's': return st.st_size > 0;
    case 'u': return (st.st_mode & S_ISUID) != 0;
    case 'g': return (st.st_mode & S_ISGID) != 0;
    case 'k': return (st.st_mode & S_ISVTX) != 0;
    case 'O': return st.st_uid == geteuid();
    case 'G': return st.st_gid == getegid();
    /* access() 用进程的实际身份问内核“我能不能读/写/执行它”，这比自己看权限位准确（root 例外等） */
    case 'r': return access(arg, R_OK) == 0;
    case 'w': return access(arg, W_OK) == 0;
    case 'x': return access(arg, X_OK) == 0;
    }
    return 0;
}

static int binary(const char *a, const char *op, const char *b)
{
    if (!strcmp(op, "=") || !strcmp(op, "=="))
        return strcmp(a, b) == 0;
    if (!strcmp(op, "!="))
        return strcmp(a, b) != 0;
    if (!strcmp(op, "<"))
        return strcmp(a, b) < 0;
    if (!strcmp(op, ">"))
        return strcmp(a, b) > 0;
    if (op[0] == '-' && (op[1] == 'n' || op[1] == 'o') && op[2] == 't') {
        struct stat sa, sb;
        int ea = stat(a, &sa) == 0, eb = stat(b, &sb) == 0;
        /* 不存在的文件算作“最旧” */
        if (op[1] == 'n') {
            if (!ea)
                return 0;
            if (!eb)
                return 1;
            return sa.st_mtim.tv_sec > sb.st_mtim.tv_sec ||
                   (sa.st_mtim.tv_sec == sb.st_mtim.tv_sec && sa.st_mtim.tv_nsec > sb.st_mtim.tv_nsec);
        }
        if (!eb)
            return 0;
        if (!ea)
            return 1;
        return sa.st_mtim.tv_sec < sb.st_mtim.tv_sec ||
               (sa.st_mtim.tv_sec == sb.st_mtim.tv_sec && sa.st_mtim.tv_nsec < sb.st_mtim.tv_nsec);
    }
    if (!strcmp(op, "-ef")) {
        struct stat sa, sb;
        return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    }
    long long x = to_int(a), y = to_int(b);
    if (!strcmp(op, "-eq")) return x == y;
    if (!strcmp(op, "-ne")) return x != y;
    if (!strcmp(op, "-lt")) return x < y;
    if (!strcmp(op, "-le")) return x <= y;
    if (!strcmp(op, "-gt")) return x > y;
    return x >= y;   /* -ge */
}

/* ---------- 超过 4 个参数时用的通用递归下降解析 ---------- */

static int expr_or(void);

static const char *peek(int k)
{
    return g_pos + k < g_n ? g_args[g_pos + k] : NULL;
}

static int primary(void)
{
    const char *a = peek(0);
    if (!a)
        fail("%s", "缺少参数");
    if (!strcmp(a, "!")) {
        g_pos++;
        return !primary();
    }
    if (!strcmp(a, "(")) {
        g_pos++;
        int v = expr_or();
        const char *close = peek(0);
        if (!close || strcmp(close, ")"))
            fail("%s", "缺少 ')'");
        g_pos++;
        return v;
    }
    const char *b = peek(1);
    if (b && is_binary(b) && peek(2)) {
        g_pos += 3;
        return binary(a, b, g_args[g_pos - 1]);
    }
    if (is_unary(a) && b) {
        g_pos += 2;
        return unary(a, b);
    }
    g_pos++;
    return a[0] != 0;
}

static int expr_and(void)
{
    int v = primary();
    while (peek(0) && !strcmp(peek(0), "-a")) {
        g_pos++;
        int r = primary();
        v = v && r;
    }
    return v;
}

static int expr_or(void)
{
    int v = expr_and();
    while (peek(0) && !strcmp(peek(0), "-o")) {
        g_pos++;
        int r = expr_and();
        v = v || r;
    }
    return v;
}

/* POSIX 规定了 0~4 个参数时的精确含义（这样 [ "$x" = "!" ] 之类的写法不会出歧义） */
static int eval_n(char **a, int n)
{
    switch (n) {
    case 0:
        return 0;
    case 1:
        return a[0][0] != 0;
    case 2:
        if (!strcmp(a[0], "!"))
            return !eval_n(a + 1, 1);
        if (is_unary(a[0]))
            return unary(a[0], a[1]);
        fail("'%s': 需要一元运算符", a[0]);
    case 3:
        if (is_binary(a[1]))
            return binary(a[0], a[1], a[2]);
        if (!strcmp(a[1], "-a"))
            return a[0][0] && a[2][0];
        if (!strcmp(a[1], "-o"))
            return a[0][0] || a[2][0];
        if (!strcmp(a[0], "!"))
            return !eval_n(a + 1, 2);
        if (!strcmp(a[0], "(") && !strcmp(a[2], ")"))
            return eval_n(a + 1, 1);
        fail("'%s': 需要二元运算符", a[1]);
    case 4:
        if (!strcmp(a[0], "!"))
            return !eval_n(a + 1, 3);
        if (!strcmp(a[0], "(") && !strcmp(a[3], ")"))
            return eval_n(a + 1, 2);
        break;
    }
    g_args = a;
    g_n = n;
    g_pos = 0;
    int v = expr_or();
    if (g_pos < g_n)
        fail("多余的参数 '%s'", g_args[g_pos]);
    return v;
}

int applet_test(int argc, char **argv)
{
    if (strcmp(sb_applet, "[") == 0) {
        if (argc == 2 && strcmp(argv[1], "--help") == 0) {
            fputs(usage, stdout);
            return 0;
        }
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            sb_err("缺少 ']'");
            return 2;
        }
        argc--;
    } else if (sb_help(argc, argv, usage)) {
        return 0;
    }
    return eval_n(argv + 1, argc - 1) ? 0 : 1;
}
