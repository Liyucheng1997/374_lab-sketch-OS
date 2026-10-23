/*
 * builtin_io.c — 内建命令 echo、printf、read、test / [
 *
 * read 是一个有意思的例子：它必须“一个字节一个字节”地读 stdin。
 * 如果一次 read() 读 4KB，就会把下一行（属于别的命令的输入）也吃掉，
 * 而这些多读的数据在 shell 进程里，退不回去给管道。
 */
#include "sksh.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/* ======================= 转义序列 ======================= */

static void put_cp(strbuf *out, uint32_t cp)
{
    if (cp < 0x80) {
        sb_putc(out, (int)cp);
    } else if (cp < 0x800) {
        sb_putc(out, 0xC0 | (int)(cp >> 6));
        sb_putc(out, 0x80 | (int)(cp & 0x3F));
    } else if (cp < 0x10000) {
        sb_putc(out, 0xE0 | (int)(cp >> 12));
        sb_putc(out, 0x80 | (int)((cp >> 6) & 0x3F));
        sb_putc(out, 0x80 | (int)(cp & 0x3F));
    } else {
        sb_putc(out, 0xF0 | (int)(cp >> 18));
        sb_putc(out, 0x80 | (int)((cp >> 12) & 0x3F));
        sb_putc(out, 0x80 | (int)((cp >> 6) & 0x3F));
        sb_putc(out, 0x80 | (int)(cp & 0x3F));
    }
}

static int hexdig(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 处理一个反斜杠转义（*pp 指向反斜杠后面的字符）。
 * echo_style：echo -e 和 printf %b 的八进制写法是 \0nnn；printf 格式串里是 \nnn。
 * 返回 1 表示遇到 \c（停止输出）。 */
static int one_escape(const char **pp, strbuf *out, int echo_style)
{
    const char *p = *pp;
    int c = (unsigned char)*p++;
    switch (c) {
    case 'a': sb_putc(out, '\a'); break;
    case 'b': sb_putc(out, '\b'); break;
    case 'c': *pp = p; return 1;
    case 'e': case 'E': sb_putc(out, 0x1b); break;
    case 'f': sb_putc(out, '\f'); break;
    case 'n': sb_putc(out, '\n'); break;
    case 'r': sb_putc(out, '\r'); break;
    case 't': sb_putc(out, '\t'); break;
    case 'v': sb_putc(out, '\v'); break;
    case '\\': sb_putc(out, '\\'); break;
    case '"': sb_putc(out, '"'); break;
    case '\'': sb_putc(out, '\''); break;
    case 'x': {
        int v = 0, n = 0, d;
        while (n < 2 && (d = hexdig((unsigned char)*p)) >= 0) {
            v = v * 16 + d;
            p++;
            n++;
        }
        if (n == 0) {
            sb_puts(out, "\\x");
        } else {
            sb_putc(out, v);
        }
        break;
    }
    case 'u': case 'U': {
        int max = c == 'u' ? 4 : 8, n = 0, d;
        uint32_t v = 0;
        while (n < max && (d = hexdig((unsigned char)*p)) >= 0) {
            v = v * 16 + (uint32_t)d;
            p++;
            n++;
        }
        if (n == 0) {
            sb_putc(out, '\\');
            sb_putc(out, c);
        } else {
            put_cp(out, v);
        }
        break;
    }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        if (echo_style && c != '0') {
            sb_putc(out, '\\');
            sb_putc(out, c);
            break;
        }
        int v = echo_style ? 0 : c - '0', n = echo_style ? 0 : 1;
        while (n < 3 && *p >= '0' && *p <= '7') {
            v = v * 8 + (*p - '0');
            p++;
            n++;
        }
        sb_putc(out, v & 0xFF);
        break;
    }
    case 0:
        sb_putc(out, '\\');
        p--;
        break;
    default:
        sb_putc(out, '\\');
        sb_putc(out, c);
        break;
    }
    *pp = p;
    return 0;
}

static int expand_escapes(const char *s, strbuf *out, int echo_style)
{
    while (*s) {
        if (*s == '\\') {
            s++;
            if (one_escape(&s, out, echo_style))
                return 1;
        } else {
            sb_putc(out, *s++);
        }
    }
    return 0;
}

/* ======================= echo ======================= */

int bi_echo(int argc, char **argv)
{
    int i = 1, newline = 1, escapes = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1] || strspn(a + 1, "neE") != strlen(a + 1))
            break;
        for (const char *o = a + 1; *o; o++) {
            if (*o == 'n') newline = 0;
            else if (*o == 'e') escapes = 1;
            else escapes = 0;
        }
    }
    strbuf out;
    sb_init(&out);
    for (; i < argc; i++) {
        if (escapes) {
            if (expand_escapes(argv[i], &out, 1)) {
                newline = 0;
                break;
            }
        } else {
            sb_puts(&out, argv[i]);
        }
        if (i + 1 < argc)
            sb_putc(&out, ' ');
    }
    if (newline)
        sb_putc(&out, '\n');
    if (out.len)
        fwrite(out.s, 1, out.len, stdout);
    sb_free(&out);
    return 0;
}

/* ======================= printf ======================= */

typedef struct {
    char **args;
    int    n, i;
    int    err;
} pargs;

static const char *next_arg(pargs *a)
{
    return a->i < a->n ? a->args[a->i++] : NULL;
}

static long long arg_int(pargs *a, int *had)
{
    const char *s = next_arg(a);
    *had = s != NULL;
    if (!s || !*s)
        return 0;
    if (s[0] == '\'' || s[0] == '"') {     /* 'A → 字符的编码 */
        uint32_t cp = 0;
        if (s[1])
            utf8_decode(s + 1, strlen(s + 1), &cp);
        return cp;
    }
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 0);
    if (*end == 0 && errno == 0)
        return v;
    if (*s == '-' || *s == '+') {
        unsigned long long u = strtoull(s, &end, 0);
        if (*end == 0)
            return (long long)u;
    }
    sh_error("printf: %s: 需要数字", s);
    a->err = 1;
    return v;
}

static void append_fmt(strbuf *out, const char *spec, ...) __attribute__((format(printf, 2, 3)));
static void append_fmt(strbuf *out, const char *spec, ...)
{
    va_list ap;
    va_start(ap, spec);
    int n = vsnprintf(NULL, 0, spec, ap);
    va_end(ap);
    if (n <= 0)
        return;
    char *tmp = xmalloc((size_t)n + 1);
    va_start(ap, spec);
    vsnprintf(tmp, (size_t)n + 1, spec, ap);
    va_end(ap);
    sb_putn(out, tmp, (size_t)n);
    free(tmp);
}

/* 返回：0 正常；1 遇到 %b 里的 \c（立即停止所有输出）；-1 格式错误 */
static int printf_once(const char *fmt, pargs *a, strbuf *out)
{
    const char *p = fmt;
    while (*p) {
        if (*p == '\\') {
            p++;
            if (one_escape(&p, out, 0))
                return 1;
            continue;
        }
        if (*p != '%') {
            sb_putc(out, *p++);
            continue;
        }
        p++;
        if (*p == '%') {
            sb_putc(out, '%');
            p++;
            continue;
        }
        char spec[64];
        size_t sl = 0;
        spec[sl++] = '%';
        while (*p && strchr("-+ #0", *p) && sl < 10)
            spec[sl++] = *p++;
        int had;
        if (*p == '*') {
            p++;
            sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", (int)arg_int(a, &had));
        } else {
            while (isdigit((unsigned char)*p) && sl < 30)
                spec[sl++] = *p++;
        }
        if (*p == '.') {
            spec[sl++] = *p++;
            if (*p == '*') {
                p++;
                sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", (int)arg_int(a, &had));
            } else {
                while (isdigit((unsigned char)*p) && sl < 50)
                    spec[sl++] = *p++;
            }
        }
        char conv = *p;
        if (!conv) {
            sh_error("printf: '%s': 格式不完整", fmt);
            return -1;
        }
        p++;
        spec[sl] = 0;
        switch (conv) {
        case 's': case 'b': {
            const char *s = next_arg(a);
            if (!s)
                s = "";
            strbuf tmp;
            sb_init(&tmp);
            int stop = 0;
            if (conv == 'b')
                stop = expand_escapes(s, &tmp, 1);
            else
                sb_puts(&tmp, s);
            spec[sl] = 's';
            spec[sl + 1] = 0;
            append_fmt(out, spec, sb_str(&tmp));
            sb_free(&tmp);
            if (stop)
                return 1;
            break;
        }
        case 'c': {
            const char *s = next_arg(a);
            char one[5] = { 0 };
            if (s && *s) {
                size_t l = (size_t)utf8_seqlen((unsigned char)*s);
                if (l > strlen(s))
                    l = 1;
                memcpy(one, s, l);      /* %c 输出第一个字符（汉字是多个字节） */
            }
            spec[sl] = 's';
            spec[sl + 1] = 0;
            append_fmt(out, spec, one);
            break;
        }
        case 'd': case 'i': {
            long long v = arg_int(a, &had);
            memcpy(spec + sl, "lld", 4);
            append_fmt(out, spec, v);
            break;
        }
        case 'o': case 'u': case 'x': case 'X': {
            long long v = arg_int(a, &had);
            spec[sl] = 'l';
            spec[sl + 1] = 'l';
            spec[sl + 2] = conv;
            spec[sl + 3] = 0;
            append_fmt(out, spec, (unsigned long long)v);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            const char *s = next_arg(a);
            double v = 0;
            if (s && *s) {
                char *end;
                v = strtod(s, &end);
                if (*end) {
                    sh_error("printf: %s: 需要数字", s);
                    a->err = 1;
                }
            }
            spec[sl] = conv;
            spec[sl + 1] = 0;
            append_fmt(out, spec, v);
            break;
        }
        default:
            sh_error("printf: %%%c: 无效的格式字符", conv);
            return -1;
        }
    }
    return 0;
}

int bi_printf(int argc, char **argv)
{
    int i = 1;
    const char *var = NULL;
    if (i + 1 < argc && strcmp(argv[i], "-v") == 0) {
        var = argv[i + 1];
        if (!is_name(var)) {
            sh_error("printf: '%s': 不是合法的变量名", var);
            return 2;
        }
        i += 2;
    }
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        sh_error("printf: 用法: printf [-v 变量] 格式 [参数...]");
        return 2;
    }
    const char *fmt = argv[i++];
    pargs a = { argv + i, argc - i, 0, 0 };
    strbuf out;
    sb_init(&out);
    int r;
    /* 参数比格式里的 % 多：重复使用格式，直到参数用完 */
    do {
        int before = a.i;
        r = printf_once(fmt, &a, &out);
        if (r != 0 || a.i == before)
            break;
    } while (a.i < a.n);
    if (r >= 0) {
        if (var)
            var_set(var, sb_str(&out), 0);
        else if (out.len)
            fwrite(out.s, 1, out.len, stdout);
    }
    sb_free(&out);
    return r < 0 ? 1 : a.err;
}

/* ======================= read ======================= */

int bi_read(int argc, char **argv)
{
    int raw = 0, silent = 0;
    long nchars = -1;
    int delim = '\n';
    const char *prompt = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r') {
                raw = 1;
            } else if (*o == 's') {
                silent = 1;
            } else if (*o == 'p' || *o == 'n' || *o == 'd') {
                const char *val = o[1] ? o + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!val) {
                    sh_error("read: -%c: 选项需要一个参数", *o);
                    return 2;
                }
                if (*o == 'p') {
                    prompt = val;
                } else if (*o == 'd') {
                    delim = (unsigned char)val[0];
                } else {
                    char *end;
                    nchars = strtol(val, &end, 10);
                    if (*end || nchars < 0) {
                        sh_error("read: %s: 无效的字符数", val);
                        return 2;
                    }
                }
                break;
            } else {
                sh_error("read: -%c: 无效的选项\n用法: read [-rs] [-p 提示] [-n 个数] [-d 分隔符] [变量...]", *o);
                return 2;
            }
        }
    }
    char **names = argv + i;
    int nnames = argc - i;
    for (int k = 0; k < nnames; k++)
        if (!is_name(names[k])) {
            sh_error("read: '%s': 不是合法的变量名", names[k]);
            return 2;
        }
    int tty = isatty(STDIN_FILENO);
    if (prompt && tty) {
        fputs(prompt, stderr);
        fflush(stderr);
    }
    struct termios saved;
    int restore = 0;
    if (tty && (silent || nchars >= 0) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        struct termios t = saved;
        if (silent)
            t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
        if (nchars >= 0) {
            t.c_lflag &= ~(tcflag_t)ICANON;
            t.c_cc[VMIN] = 1;
            t.c_cc[VTIME] = 0;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        restore = 1;
    }

    /* 逐字节读一行；esc 记录哪些字符被反斜杠转义了（它们不参与 IFS 分割） */
    strbuf line, esc;
    sb_init(&line);
    sb_init(&esc);
    int eof = 0, status = 0;
    long count = 0;
    g_sigint = 0;
    while (nchars < 0 || count < nchars) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0) {
            if (errno == EINTR) {
                if (g_sigint) {
                    status = 130;
                    break;
                }
                if (g_trap_pending)
                    traps_run_pending();
                continue;
            }
            sh_error("read: 读取错误: %s", sh_strerror(errno));
            status = 1;
            break;
        }
        if (r == 0) {
            eof = 1;
            break;
        }
        if (!raw && c == '\\') {
            char n;
            ssize_t r2 = full_read(STDIN_FILENO, &n, 1);
            if (r2 <= 0) {
                eof = 1;
                break;
            }
            if (n == '\n')
                continue;           /* 反斜杠-换行：续行 */
            sb_putc(&line, n);
            sb_putc(&esc, 1);
            count++;
            continue;
        }
        if ((unsigned char)c == (unsigned)delim && delim != 0)
            break;
        if (c == 0 && delim == 0)
            break;
        sb_putc(&line, c);
        sb_putc(&esc, 0);
        count++;
    }
    if (restore) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        if (silent)
            fputs("\n", stderr);
    }
    if (status) {
        sb_free(&line);
        sb_free(&esc);
        return status;
    }

    const char *s = sb_str(&line);
    const char *e = sb_str(&esc);
    size_t len = line.len;
    if (nnames == 0) {
        var_set("REPLY", s, 0);
    } else {
        const char *ifs = var_get("IFS");
        if (!ifs)
            ifs = " \t\n";
#define IS_SEP(k) (!e[k] && s[k] && strchr(ifs, s[k]))
#define IS_WS(k) (IS_SEP(k) && (s[k] == ' ' || s[k] == '\t' || s[k] == '\n'))
        size_t pos = 0;
        while (pos < len && IS_WS(pos))
            pos++;
        for (int k = 0; k < nnames; k++) {
            strbuf v;
            sb_init(&v);
            sb_putn(&v, "", 0);
            if (k == nnames - 1) {
                /* 最后一个变量拿走剩下的全部（去掉结尾的 IFS 空白） */
                size_t end = len;
                while (end > pos && IS_WS(end - 1))
                    end--;
                sb_putn(&v, s + pos, end - pos);
                pos = len;
            } else {
                while (pos < len && !IS_SEP(pos))
                    sb_putc(&v, s[pos++]);
                while (pos < len && IS_WS(pos))
                    pos++;
                if (pos < len && IS_SEP(pos) && !IS_WS(pos)) {
                    pos++;
                    while (pos < len && IS_WS(pos))
                        pos++;
                }
            }
            if (var_set(names[k], sb_str(&v), 0) < 0)
                status = 1;
            sb_free(&v);
        }
#undef IS_SEP
#undef IS_WS
    }
    sb_free(&line);
    sb_free(&esc);
    if (eof)
        return 1;
    return status;
}

/* ======================= test / [ ======================= */

typedef struct {
    char **v;
    int    n, i;
    int    err;
} tctx;

static int t_expr(tctx *t);

static void t_fail(tctx *t, const char *msg, const char *arg)
{
    if (!t->err) {
        if (arg)
            sh_error("test: %s: %s", arg, msg);
        else
            sh_error("test: %s", msg);
    }
    t->err = 1;
}

static int is_unary(const char *s)
{
    static const char *const ops[] = { "-z", "-n", "-e", "-f", "-d", "-r", "-w", "-x", "-s", "-L",
                                        "-h", "-b", "-c", "-p", "-S", "-t", "-g", "-u", "-k", "-O", "-G" };
    for (size_t i = 0; i < SK_ARRAY_LEN(ops); i++)
        if (strcmp(s, ops[i]) == 0)
            return 1;
    return 0;
}

static int is_binary(const char *s)
{
    static const char *const ops[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
                                        "-nt", "-ot", "-ef" };
    for (size_t i = 0; i < SK_ARRAY_LEN(ops); i++)
        if (strcmp(s, ops[i]) == 0)
            return 1;
    return 0;
}

static long long t_int(tctx *t, const char *s)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    char *end;
    errno = 0;
    long long v = strtoll(p, &end, 10);
    while (*end == ' ' || *end == '\t')
        end++;
    if (p == end || *end || errno) {
        t_fail(t, "需要整数表达式", s);
        return 0;
    }
    return v;
}

static int t_unary(tctx *t, const char *op, const char *arg)
{
    struct stat st;
    switch (op[1]) {
    case 'z': return arg[0] == 0;
    case 'n': return arg[0] != 0;
    case 't': {
        long long fd = t_int(t, arg);
        return !t->err && isatty((int)fd);
    }
    case 'L':
    case 'h':
        return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
    case 'r': return access(arg, R_OK) == 0;
    case 'w': return access(arg, W_OK) == 0;
    case 'x': return access(arg, X_OK) == 0;
    default:
        break;
    }
    if (stat(arg, &st) != 0)
        return 0;
    switch (op[1]) {
    case 'e': return 1;
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 's': return st.st_size > 0;
    case 'b': return S_ISBLK(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case 'S': return S_ISSOCK(st.st_mode);
    case 'g': return (st.st_mode & S_ISGID) != 0;
    case 'u': return (st.st_mode & S_ISUID) != 0;
    case 'k': return (st.st_mode & S_ISVTX) != 0;
    case 'O': return st.st_uid == geteuid();
    case 'G': return st.st_gid == getegid();
    default: return 0;
    }
}

static int t_binary(tctx *t, const char *a, const char *op, const char *b)
{
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return strcmp(a, b) == 0;
    if (strcmp(op, "!=") == 0) return strcmp(a, b) != 0;
    if (strcmp(op, "<") == 0) return strcmp(a, b) < 0;
    if (strcmp(op, ">") == 0) return strcmp(a, b) > 0;
    if (op[0] == '-' && (op[1] == 'n' || op[1] == 'o') && op[2] == 't') {
        struct stat sa, sb2;
        int ha = stat(a, &sa) == 0, hb = stat(b, &sb2) == 0;
        if (op[1] == 'n')
            return ha && (!hb || sa.st_mtime > sb2.st_mtime);
        return hb && (!ha || sa.st_mtime < sb2.st_mtime);
    }
    if (strcmp(op, "-ef") == 0) {
        struct stat sa, sb2;
        return stat(a, &sa) == 0 && stat(b, &sb2) == 0 && sa.st_dev == sb2.st_dev && sa.st_ino == sb2.st_ino;
    }
    long long x = t_int(t, a), y = t_int(t, b);
    if (t->err)
        return 0;
    if (strcmp(op, "-eq") == 0) return x == y;
    if (strcmp(op, "-ne") == 0) return x != y;
    if (strcmp(op, "-lt") == 0) return x < y;
    if (strcmp(op, "-le") == 0) return x <= y;
    if (strcmp(op, "-gt") == 0) return x > y;
    return x >= y;
}

static const char *t_peek(tctx *t, int k)
{
    return t->i + k < t->n ? t->v[t->i + k] : NULL;
}

static int t_primary(tctx *t)
{
    const char *a = t_peek(t, 0);
    if (!a) {
        t_fail(t, "缺少参数", NULL);
        return 0;
    }
    if (strcmp(a, "(") == 0) {
        t->i++;
        int r = t_expr(t);
        const char *c = t_peek(t, 0);
        if (!c || strcmp(c, ")") != 0) {
            t_fail(t, "缺少 ')'", NULL);
            return 0;
        }
        t->i++;
        return r;
    }
    const char *b = t_peek(t, 1);
    if (b && is_binary(b) && t_peek(t, 2)) {
        t->i += 3;
        return t_binary(t, a, b, t->v[t->i - 1]);
    }
    if (is_unary(a) && b) {
        t->i += 2;
        return t_unary(t, a, b);
    }
    t->i++;
    return a[0] != 0;
}

static int t_not(tctx *t)
{
    const char *a = t_peek(t, 0);
    if (a && strcmp(a, "!") == 0 && t_peek(t, 1)) {
        t->i++;
        return !t_not(t);
    }
    return t_primary(t);
}

static int t_and(tctx *t)
{
    int r = t_not(t);
    while (t_peek(t, 0) && strcmp(t_peek(t, 0), "-a") == 0) {
        t->i++;
        int r2 = t_not(t);
        r = r && r2;
    }
    return r;
}

static int t_expr(tctx *t)
{
    int r = t_and(t);
    while (t_peek(t, 0) && strcmp(t_peek(t, 0), "-o") == 0) {
        t->i++;
        int r2 = t_and(t);
        r = r || r2;
    }
    return r;
}

/* POSIX 规定按参数个数决定含义（这样 [ -n ]、[ = = = ] 之类才能得到正确结果） */
static int t_eval(tctx *t, char **v, int n)
{
    switch (n) {
    case 0:
        return 0;
    case 1:
        return v[0][0] != 0;
    case 2:
        if (strcmp(v[0], "!") == 0)
            return v[1][0] == 0;
        if (is_unary(v[0]))
            return t_unary(t, v[0], v[1]);
        t_fail(t, "需要一元运算符", v[0]);
        return 0;
    case 3:
        if (is_binary(v[1]))
            return t_binary(t, v[0], v[1], v[2]);
        if (strcmp(v[1], "-a") == 0)
            return v[0][0] && v[2][0];
        if (strcmp(v[1], "-o") == 0)
            return v[0][0] || v[2][0];
        if (strcmp(v[0], "!") == 0)
            return !t_eval(t, v + 1, 2);
        if (strcmp(v[0], "(") == 0 && strcmp(v[2], ")") == 0)
            return v[1][0] != 0;
        t_fail(t, "需要二元运算符", v[1]);
        return 0;
    case 4:
        if (strcmp(v[0], "!") == 0)
            return !t_eval(t, v + 1, 3);
        if (strcmp(v[0], "(") == 0 && strcmp(v[3], ")") == 0)
            return t_eval(t, v + 1, 2);
        break;
    default:
        break;
    }
    t->v = v;
    t->n = n;
    t->i = 0;
    int r = t_expr(t);
    if (!t->err && t->i < t->n)
        t_fail(t, "多余的参数", t->v[t->i]);
    return r;
}

int bi_test(int argc, char **argv)
{
    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            sh_error("[: 缺少 ']'");
            return 2;
        }
        argc--;
    }
    tctx t = { 0 };
    int r = t_eval(&t, argv + 1, argc - 1);
    if (t.err)
        return 2;
    return r ? 0 : 1;
}
