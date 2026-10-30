/*
 * json.c — 小型 JSON 库的实现（接口说明见 json.h）
 */
#include "json.h"

#include <skutil.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ 节点 */

static jnode *node_new(jtype t)
{
    jnode *n = calloc(1, sizeof *n);
    if (!n) {
        fprintf(stderr, "skaid: 内存不足\n");
        abort();            /* 守护进程由 sinit 看护，崩溃后会被重新拉起来 */
    }
    n->t = t;
    return n;
}

void json_free(jnode *n)
{
    while (n) {
        jnode *next = n->next;
        json_free(n->child);
        free(n->key);
        free(n->s);
        free(n);
        n = next;
    }
}

jnode *json_null(void) { return node_new(J_NULL); }
jnode *json_arr(void) { return node_new(J_ARR); }
jnode *json_obj(void) { return node_new(J_OBJ); }

jnode *json_bool(int b)
{
    jnode *n = node_new(J_BOOL);
    n->b = !!b;
    return n;
}

jnode *json_num(double v)
{
    jnode *n = node_new(J_NUM);
    n->n = v;
    return n;
}

jnode *json_strn(const char *s, size_t len)
{
    jnode *n = node_new(J_STR);
    n->s = malloc(len + 1);
    if (!n->s)
        abort();
    if (len)
        memcpy(n->s, s, len);
    n->s[len] = 0;
    n->slen = len;
    return n;
}

jnode *json_str(const char *s)
{
    return json_strn(s ? s : "", s ? strlen(s) : 0);
}

void json_str_append(jnode *n, const char *s, size_t len)
{
    if (!n || n->t != J_STR || !len)
        return;
    char *p = realloc(n->s, n->slen + len + 1);
    if (!p)
        abort();
    memcpy(p + n->slen, s, len);
    n->slen += len;
    p[n->slen] = 0;
    n->s = p;
}

jnode *json_clone(const jnode *n)
{
    if (!n)
        return NULL;
    jnode *c = node_new(n->t);
    c->n = n->n;
    c->b = n->b;
    if (n->s) {
        c->s = malloc(n->slen + 1);
        if (!c->s)
            abort();
        memcpy(c->s, n->s, n->slen + 1);
        c->slen = n->slen;
    }
    for (const jnode *k = n->child; k; k = k->next)
        json_add(c, k->key, json_clone(k));
    return c;
}

jnode *json_add(jnode *parent, const char *key, jnode *child)
{
    if (!parent || !child)
        return child;
    free(child->key);
    child->key = (parent->t == J_OBJ && key) ? strdup(key) : NULL;
    child->next = NULL;
    if (parent->last)
        parent->last->next = child;
    else
        parent->child = child;
    parent->last = child;
    return child;
}

void json_del(jnode *obj, const char *key)
{
    if (!obj || obj->t != J_OBJ)
        return;
    jnode *prev = NULL;
    for (jnode *k = obj->child; k; prev = k, k = k->next) {
        if (k->key && strcmp(k->key, key) == 0) {
            if (prev)
                prev->next = k->next;
            else
                obj->child = k->next;
            if (obj->last == k)
                obj->last = prev;
            k->next = NULL;
            json_free(k);
            return;
        }
    }
}

void json_set(jnode *obj, const char *key, jnode *child)
{
    json_del(obj, key);
    json_add(obj, key, child);
}

jnode *json_get(const jnode *obj, const char *key)
{
    if (!obj || obj->t != J_OBJ || !key)
        return NULL;
    for (jnode *k = obj->child; k; k = k->next)
        if (k->key && strcmp(k->key, key) == 0)
            return k;
    return NULL;
}

const char *json_get_str(const jnode *obj, const char *key)
{
    jnode *n = json_get(obj, key);
    return (n && n->t == J_STR) ? n->s : NULL;
}

double json_get_num(const jnode *obj, const char *key, double def)
{
    jnode *n = json_get(obj, key);
    return (n && n->t == J_NUM) ? n->n : def;
}

int json_get_bool(const jnode *obj, const char *key, int def)
{
    jnode *n = json_get(obj, key);
    return (n && n->t == J_BOOL) ? n->b : def;
}

int json_count(const jnode *arr)
{
    int c = 0;
    if (arr && (arr->t == J_ARR || arr->t == J_OBJ))
        for (jnode *k = arr->child; k; k = k->next)
            c++;
    return c;
}

jnode *json_at(const jnode *arr, int i)
{
    if (!arr || arr->t != J_ARR)
        return NULL;
    for (jnode *k = arr->child; k; k = k->next)
        if (i-- == 0)
            return k;
    return NULL;
}

/* ================================================================ 解析（递归下降） */

typedef struct {
    const char *p, *end;
    const char *err;
    int depth;
} parser;

#define MAX_DEPTH 64        /* 防止恶意的深层嵌套把栈耗尽 */

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static jnode *parse_value(parser *ps);

/* 一个动态增长的字节缓冲区 */
typedef struct {
    char *d;
    size_t len, cap;
} buf_t;

static void buf_put(buf_t *b, const void *data, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *d = realloc(b->d, cap);
        if (!d)
            abort();
        b->d = d;
        b->cap = cap;
    }
    memcpy(b->d + b->len, data, n);
    b->len += n;
    b->d[b->len] = 0;
}

static void buf_putc(buf_t *b, char c) { buf_put(b, &c, 1); }

static void buf_put_utf8(buf_t *b, uint32_t cp)
{
    char u[4];
    int n;
    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    buf_put(b, u, n);
}

static int hex4(parser *ps, uint32_t *out)
{
    if (ps->end - ps->p < 4)
        return -1;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return -1;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

/* 解析一个字符串（ps->p 指向开头的引号），结果放进 b */
static int parse_string_raw(parser *ps, buf_t *b)
{
    ps->p++;                                    /* 跳过 " */
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"')
            return 0;
        if ((unsigned char)c < 0x20) {
            ps->err = "字符串里有未转义的控制字符";
            return -1;
        }
        if (c != '\\') {
            buf_putc(b, c);
            continue;
        }
        if (ps->p >= ps->end)
            break;
        char e = *ps->p++;
        switch (e) {
        case '"': buf_putc(b, '"'); break;
        case '\\': buf_putc(b, '\\'); break;
        case '/': buf_putc(b, '/'); break;
        case 'b': buf_putc(b, '\b'); break;
        case 'f': buf_putc(b, '\f'); break;
        case 'n': buf_putc(b, '\n'); break;
        case 'r': buf_putc(b, '\r'); break;
        case 't': buf_putc(b, '\t'); break;
        case 'u': {
            uint32_t cp;
            if (hex4(ps, &cp) < 0) {
                ps->err = "\\u 后面不是 4 位十六进制数";
                return -1;
            }
            /* UTF-16 代理对：U+10000 以上的字符（比如表情符号）在 JSON 里写成两个 \uXXXX */
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                const char *save = ps->p;
                ps->p += 2;
                uint32_t lo;
                if (hex4(ps, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF)
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else
                    ps->p = save;
            }
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = 0xFFFD;                    /* 落单的代理项不是合法字符 */
            buf_put_utf8(b, cp);
            break;
        }
        default:
            ps->err = "未知的转义字符";
            return -1;
        }
    }
    ps->err = "字符串没有结束";
    return -1;
}

static jnode *parse_string(parser *ps)
{
    buf_t b = { 0 };
    if (parse_string_raw(ps, &b) < 0) {
        free(b.d);
        return NULL;
    }
    jnode *n = node_new(J_STR);
    if (!b.d) {
        b.d = calloc(1, 1);
        if (!b.d)
            abort();
    }
    n->s = b.d;
    n->slen = b.len;
    return n;
}

static jnode *parse_number(parser *ps)
{
    /* strtod 需要以 \0 结尾的字符串：数字最多几十个字符，先拷到栈上 */
    char tmp[64];
    size_t n = 0;
    while (ps->p + n < ps->end && n < sizeof tmp - 1 && strchr("+-0123456789.eE", ps->p[n]))
        n++;
    memcpy(tmp, ps->p, n);
    tmp[n] = 0;
    char *e;
    double v = strtod(tmp, &e);
    if (e == tmp) {
        ps->err = "不认识的数字";
        return NULL;
    }
    ps->p += e - tmp;
    return json_num(v);
}

static int match_word(parser *ps, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, w, n) == 0) {
        ps->p += n;
        return 1;
    }
    return 0;
}

static jnode *parse_container(parser *ps, int is_obj)
{
    if (++ps->depth > MAX_DEPTH) {
        ps->err = "嵌套太深";
        return NULL;
    }
    jnode *c = node_new(is_obj ? J_OBJ : J_ARR);
    ps->p++;                                    /* 跳过 { 或 [ */
    skip_ws(ps);
    char close = is_obj ? '}' : ']';
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        ps->depth--;
        return c;
    }
    for (;;) {
        buf_t key = { 0 };
        if (is_obj) {
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') {
                ps->err = "对象的键必须是字符串";
                goto fail;
            }
            if (parse_string_raw(ps, &key) < 0)
                goto fail;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key.d);
                ps->err = "键后面缺少冒号";
                goto fail;
            }
            ps->p++;
        }
        jnode *v = parse_value(ps);
        if (!v) {
            free(key.d);
            goto fail;
        }
        json_add(c, key.d ? key.d : "", v);
        free(key.d);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
            ps->depth--;
            return c;
        }
        ps->err = is_obj ? "对象里缺少逗号或 }" : "数组里缺少逗号或 ]";
        goto fail;
    }
fail:
    json_free(c);
    return NULL;
}

static jnode *parse_value(parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) {
        ps->err = "意外的结尾";
        return NULL;
    }
    char c = *ps->p;
    if (c == '{') return parse_container(ps, 1);
    if (c == '[') return parse_container(ps, 0);
    if (c == '"') return parse_string(ps);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps);
    if (match_word(ps, "true")) return json_bool(1);
    if (match_word(ps, "false")) return json_bool(0);
    if (match_word(ps, "null")) return json_null();
    ps->err = "不认识的字符";
    return NULL;
}

jnode *json_parse(const char *text, size_t len, const char **err)
{
    parser ps = { text, text + len, NULL, 0 };
    jnode *n = parse_value(&ps);
    if (n) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            ps.err = "JSON 后面还有多余的内容";
            json_free(n);
            n = NULL;
        }
    }
    if (err)
        *err = n ? NULL : (ps.err ? ps.err : "解析失败");
    return n;
}

/* ================================================================ 序列化 */

static void dump_string(buf_t *b, const char *s, size_t len)
{
    buf_putc(b, '"');
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            switch (c) {
            case '"': buf_put(b, "\\\"", 2); break;
            case '\\': buf_put(b, "\\\\", 2); break;
            case '\n': buf_put(b, "\\n", 2); break;
            case '\r': buf_put(b, "\\r", 2); break;
            case '\t': buf_put(b, "\\t", 2); break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char u[8];
                    snprintf(u, sizeof u, "\\u%04x", c);
                    buf_put(b, u, 6);
                } else {
                    buf_putc(b, (char)c);
                }
            }
            i++;
            continue;
        }
        /* 多字节字符：合法就原样输出，不合法（比如二进制文件、截断的 UTF-8）就换成 U+FFFD */
        uint32_t cp;
        int n = sk_utf8_decode(s + i, len - i, &cp);
        if (cp == 0xFFFD && !(n == 3 && (unsigned char)s[i] == 0xEF))
            buf_put(b, "\xEF\xBF\xBD", 3);
        else
            buf_put(b, s + i, (size_t)n);
        i += (size_t)n;
    }
    buf_putc(b, '"');
}

static void dump_node(buf_t *b, const jnode *n)
{
    switch (n->t) {
    case J_NULL: buf_put(b, "null", 4); break;
    case J_BOOL: n->b ? buf_put(b, "true", 4) : buf_put(b, "false", 5); break;
    case J_NUM: {
        char t[40];
        if (!isfinite(n->n))
            snprintf(t, sizeof t, "0");
        else if (n->n == floor(n->n) && fabs(n->n) < 1e15)
            snprintf(t, sizeof t, "%.0f", n->n);
        else
            snprintf(t, sizeof t, "%.17g", n->n);
        buf_put(b, t, strlen(t));
        break;
    }
    case J_STR: dump_string(b, n->s, n->slen); break;
    case J_ARR:
    case J_OBJ:
        buf_putc(b, n->t == J_OBJ ? '{' : '[');
        for (const jnode *k = n->child; k; k = k->next) {
            if (k != n->child)
                buf_putc(b, ',');
            if (n->t == J_OBJ) {
                dump_string(b, k->key ? k->key : "", k->key ? strlen(k->key) : 0);
                buf_putc(b, ':');
            }
            dump_node(b, k);
        }
        buf_putc(b, n->t == J_OBJ ? '}' : ']');
        break;
    }
}

char *json_dump(const jnode *n, size_t *len)
{
    buf_t b = { 0 };
    if (n)
        dump_node(&b, n);
    else
        buf_put(&b, "null", 4);
    if (len)
        *len = b.len;
    return b.d;
}
