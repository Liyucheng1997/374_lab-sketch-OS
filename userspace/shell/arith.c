/*
 * arith.c — $(( ... )) 算术求值
 *
 * 递归下降求值器，运算符和优先级与 C 语言相同（从低到高）：
 *     ,   = += -= *= /= %= <<= >>= &= ^= |=   ?:   ||   &&   |   ^   &
 *     == !=   < <= > >=   << >>   + -   * / %   **   一元 + - ! ~ ++ --
 * 全部是 64 位整数运算。变量名直接写（不用 $），未设置的变量当作 0。
 * && || ?: 支持“短路”：不会被求值的那一边里的赋值不会执行。
 */
#include "sksh.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *expr;
    const char *p;
    int         noeval;
    int         depth;
    int         failed;
    char        msg[160];
} actx;

static long long a_comma(actx *a);
static long long a_assign(actx *a);

static void a_fail(actx *a, const char *msg)
{
    if (!a->failed) {
        a->failed = 1;
        sk_strlcpy(a->msg, msg, sizeof a->msg);
    }
}

static void skip_ws(actx *a)
{
    while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n' || *a->p == '\r')
        a->p++;
}

static int peek_op(actx *a, const char *op)
{
    skip_ws(a);
    return strncmp(a->p, op, strlen(op)) == 0;
}

static int accept(actx *a, const char *op)
{
    if (peek_op(a, op)) {
        a->p += strlen(op);
        return 1;
    }
    return 0;
}

static int is_ident_start(char c)
{
    return c == '_' || isalpha((unsigned char)c);
}

static int is_ident_char(char c)
{
    return c == '_' || isalnum((unsigned char)c);
}

/* 解析数字：123、0x1f、017（八进制）、2#1010（任意进制 2..64） */
static int parse_number(const char *s, const char **end, long long *out)
{
    const char *p = s;
    unsigned long long v = 0;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && isdigit((unsigned char)p[1])) {
        base = 8;
        p++;
    } else {
        const char *q = p;
        int b = 0;
        while (isdigit((unsigned char)*q))
            b = b * 10 + (*q++ - '0');
        if (*q == '#' && q > p) {
            if (b < 2 || b > 64)
                return -1;
            base = b;
            p = q + 1;
        }
    }
    const char *start = p;
    for (;; p++) {
        int d;
        char c = *p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = base <= 36 ? c - 'A' + 10 : c - 'A' + 36;
        else if (c == '@') d = 62;
        else if (c == '_') d = 63;
        else break;
        if (d >= base)
            return -1;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (p == start && base != 8)
        return -1;
    *end = p;
    *out = (long long)v;
    return 0;
}

/* 变量的值本身也可以是表达式（x='y+1'），递归求值 */
static long long var_value(actx *a, const char *name)
{
    const char *v = var_get(name);
    if (!v || !*v)
        return 0;
    const char *end;
    long long n;
    const char *s = v;
    while (*s == ' ' || *s == '\t')
        s++;
    int neg = 0;
    if (*s == '-' || *s == '+') {
        neg = *s == '-';
        s++;
    }
    if (isdigit((unsigned char)*s) && parse_number(s, &end, &n) == 0) {
        while (*end == ' ' || *end == '\t')
            end++;
        if (!*end)
            return neg ? -n : n;
    }
    if (a->depth > 32) {
        a_fail(a, "表达式递归层数过多");
        return 0;
    }
    actx sub = { .expr = v, .p = v, .noeval = a->noeval, .depth = a->depth + 1 };
    long long r = a_comma(&sub);
    skip_ws(&sub);
    if (sub.failed) {
        a_fail(a, sub.msg);
        return 0;
    }
    if (*sub.p) {
        a_fail(a, "变量的值不是合法的算术表达式");
        return 0;
    }
    return r;
}

static void var_store(actx *a, const char *name, long long v)
{
    if (a->noeval || a->failed)
        return;
    char num[32];
    snprintf(num, sizeof num, "%lld", v);
    if (var_set(name, num, 0) < 0)
        a_fail(a, "不能给只读变量赋值");
}

static int read_ident(actx *a, char *name, size_t n)
{
    skip_ws(a);
    if (!is_ident_start(*a->p))
        return 0;
    size_t k = 0;
    while (is_ident_char(*a->p)) {
        if (k + 1 < n)
            name[k++] = *a->p;
        a->p++;
    }
    name[k] = 0;
    return 1;
}

static long long a_primary(actx *a)
{
    skip_ws(a);
    if (a->failed)
        return 0;
    if (accept(a, "(")) {
        long long v = a_comma(a);
        if (!accept(a, ")"))
            a_fail(a, "缺少 ')'");
        return v;
    }
    if (isdigit((unsigned char)*a->p)) {
        const char *end;
        long long v;
        if (parse_number(a->p, &end, &v) < 0 || is_ident_char(*end)) {
            a_fail(a, "数字格式不对");
            return 0;
        }
        a->p = end;
        return v;
    }
    char name[256];
    if (read_ident(a, name, sizeof name)) {
        long long v = var_value(a, name);
        if (accept(a, "++")) {
            var_store(a, name, v + 1);
        } else if (accept(a, "--")) {
            var_store(a, name, v - 1);
        }
        return v;
    }
    if (!*a->p)
        a_fail(a, "表达式不完整");
    else
        a_fail(a, "无法识别的符号");
    return 0;
}

static long long a_unary(actx *a)
{
    skip_ws(a);
    if (a->failed)
        return 0;
    if (peek_op(a, "++") || peek_op(a, "--")) {
        int inc = a->p[0] == '+';
        const char *save = a->p;
        a->p += 2;
        char name[256];
        if (read_ident(a, name, sizeof name)) {
            long long v = var_value(a, name) + (inc ? 1 : -1);
            var_store(a, name, v);
            return v;
        }
        a->p = save + 1;    /* 不是 ++x，就当成两个一元运算符：- -3 */
        long long v = a_unary(a);
        return inc ? v : -v;
    }
    if (accept(a, "+"))
        return a_unary(a);
    if (accept(a, "-"))
        return -a_unary(a);
    if (accept(a, "!"))
        return !a_unary(a);
    if (accept(a, "~"))
        return ~a_unary(a);
    return a_primary(a);
}

static long long a_power(actx *a)
{
    long long base = a_unary(a);
    if (accept(a, "**")) {
        long long e = a_power(a);
        if (e < 0) {
            a_fail(a, "指数不能是负数");
            return 0;
        }
        long long r = 1;
        while (e-- > 0)
            r *= base;
        return r;
    }
    return base;
}

static long long a_mul(actx *a)
{
    long long v = a_power(a);
    for (;;) {
        if (peek_op(a, "*=") || peek_op(a, "/=") || peek_op(a, "%=") || peek_op(a, "**"))
            return v;
        int op;
        if (accept(a, "*")) op = '*';
        else if (accept(a, "/")) op = '/';
        else if (accept(a, "%")) op = '%';
        else return v;
        long long r = a_power(a);
        if (op == '*') {
            v *= r;
        } else if (r == 0) {
            if (!a->noeval)
                a_fail(a, "除以零");
            v = 0;
        } else if (v == LLONG_MIN && r == -1) {
            v = op == '/' ? v : 0;
        } else {
            v = op == '/' ? v / r : v % r;
        }
    }
}

static long long a_add(actx *a)
{
    long long v = a_mul(a);
    for (;;) {
        if (peek_op(a, "+=") || peek_op(a, "-="))
            return v;
        if (accept(a, "+"))
            v += a_mul(a);
        else if (accept(a, "-"))
            v -= a_mul(a);
        else
            return v;
    }
}

static long long a_shift(actx *a)
{
    long long v = a_add(a);
    for (;;) {
        if (peek_op(a, "<<=") || peek_op(a, ">>="))
            return v;
        if (accept(a, "<<"))
            v = (long long)((unsigned long long)v << (a_add(a) & 63));
        else if (accept(a, ">>"))
            v >>= (a_add(a) & 63);
        else
            return v;
    }
}

static long long a_rel(actx *a)
{
    long long v = a_shift(a);
    for (;;) {
        if (accept(a, "<="))
            v = v <= a_shift(a);
        else if (accept(a, ">="))
            v = v >= a_shift(a);
        else if (peek_op(a, "<<") || peek_op(a, ">>"))
            return v;
        else if (accept(a, "<"))
            v = v < a_shift(a);
        else if (accept(a, ">"))
            v = v > a_shift(a);
        else
            return v;
    }
}

static long long a_eq(actx *a)
{
    long long v = a_rel(a);
    for (;;) {
        if (accept(a, "=="))
            v = v == a_rel(a);
        else if (accept(a, "!="))
            v = v != a_rel(a);
        else
            return v;
    }
}

static long long a_band(actx *a)
{
    long long v = a_eq(a);
    while (!peek_op(a, "&&") && !peek_op(a, "&=") && accept(a, "&"))
        v &= a_eq(a);
    return v;
}

static long long a_bxor(actx *a)
{
    long long v = a_band(a);
    while (!peek_op(a, "^=") && accept(a, "^"))
        v ^= a_band(a);
    return v;
}

static long long a_bor(actx *a)
{
    long long v = a_bxor(a);
    while (!peek_op(a, "||") && !peek_op(a, "|=") && accept(a, "|"))
        v |= a_bxor(a);
    return v;
}

static long long a_land(actx *a)
{
    long long v = a_bor(a);
    while (accept(a, "&&")) {
        if (!v)
            a->noeval++;
        long long r = a_bor(a);
        if (!v)
            a->noeval--;
        v = v && r;
    }
    return v;
}

static long long a_lor(actx *a)
{
    long long v = a_land(a);
    while (accept(a, "||")) {
        if (v)
            a->noeval++;
        long long r = a_land(a);
        if (v)
            a->noeval--;
        v = v || r;
    }
    return v;
}

static long long a_ternary(actx *a)
{
    long long c = a_lor(a);
    if (!accept(a, "?"))
        return c;
    if (!c)
        a->noeval++;
    long long x = a_comma(a);
    if (!c)
        a->noeval--;
    if (!accept(a, ":")) {
        a_fail(a, "?: 缺少 ':'");
        return 0;
    }
    if (c)
        a->noeval++;
    long long y = a_ternary(a);
    if (c)
        a->noeval--;
    return c ? x : y;
}

static long long a_assign(actx *a)
{
    skip_ws(a);
    const char *save = a->p;
    char name[256];
    if (read_ident(a, name, sizeof name)) {
        static const char *const ops[] = { "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=", "=" };
        skip_ws(a);
        for (size_t i = 0; i < SK_ARRAY_LEN(ops); i++) {
            size_t l = strlen(ops[i]);
            if (strncmp(a->p, ops[i], l) != 0)
                continue;
            if (ops[i][0] == '=' && a->p[1] == '=')
                break;              /* 是 ==，不是赋值 */
            a->p += l;
            long long r = a_assign(a);
            long long v = l == 1 ? r : var_value(a, name);
            switch (ops[i][0]) {
            case '+': v += r; break;
            case '-': v -= r; break;
            case '*': v *= r; break;
            case '/':
            case '%':
                if (r == 0) {
                    if (!a->noeval)
                        a_fail(a, "除以零");
                    return 0;
                }
                v = ops[i][0] == '/' ? v / r : v % r;
                break;
            case '&': v &= r; break;
            case '^': v ^= r; break;
            case '|': v |= r; break;
            case '<': v = (long long)((unsigned long long)v << (r & 63)); break;
            case '>': v >>= (r & 63); break;
            default: break;
            }
            var_store(a, name, v);
            return v;
        }
        a->p = save;
    }
    return a_ternary(a);
}

static long long a_comma(actx *a)
{
    long long v = a_assign(a);
    while (accept(a, ","))
        v = a_assign(a);
    return v;
}

int arith_eval(const char *expr, long long *result)
{
    actx a = { .expr = expr, .p = expr };
    skip_ws(&a);
    if (!*a.p) {            /* $(( )) 是 0 */
        *result = 0;
        return 0;
    }
    long long v = a_comma(&a);
    skip_ws(&a);
    if (!a.failed && *a.p)
        a_fail(&a, "表达式里有多余的内容");
    if (a.failed) {
        sh_error("算术表达式错误: %s（表达式：\"%s\"，出错位置：\"%s\"）", a.msg, expr, a.p);
        return -1;
    }
    *result = v;
    return 0;
}
