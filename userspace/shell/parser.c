/*
 * parser.c — 词法分析 + 语法分析：把输入的文本变成语法树(AST)
 *
 * 为什么不能“按空格切分”就完事？因为 shell 的语法其实相当复杂：
 *     echo "a  b" 'c$d' $(ls | wc -l) > out.txt && echo ok
 * 引号里的空格不切分，单引号里的 $ 不展开，$( ) 里面又是一整段完整的 shell 程序……
 *
 * 所以分两步：
 *   1. 词法分析(lexer)：把字符流切成“记号”(token)：单词、| ; & && || ( ) < > >> 等。
 *      单词内部按引号情况切成“片段”，引号字符本身在这一步就被去掉了
 *      （“引号去除”），但每个片段都记得自己是否被引号保护。
 *   2. 语法分析(parser)：用“递归下降”法，按下面的文法把记号组装成树：
 *
 *      list      : and_or ((';' | '&' | 换行) and_or)*
 *      and_or    : pipeline (('&&' | '||') pipeline)*
 *      pipeline  : ['!'] command ('|' command)*
 *      command   : simple | compound redirect* | funcdef
 *      compound  : '{' list '}' | '(' list ')' | if | while | until | for | case
 *      simple    : (赋值 | 重定向)* 单词 (单词 | 重定向)*
 *
 *   每条文法规则对应一个 parse_xxx() 函数，函数之间互相调用——这就是“递归下降”。
 *
 * 交互式输入是一行一行来的：语法分析器读到 if 还没读到 fi 时，会向输入源要下一行
 * （这时显示 PS2 提示符 "> "）。$( ... ) 里面的命令直接递归调用语法分析器解析。
 *
 * 出错处理：语法错误可能发生在很深的递归里，这里用 setjmp/longjmp 直接跳回
 * parser_next()，语法树所在的内存池整体丢弃，不会泄漏。
 */
#include "sksh.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ======================= 记号 ======================= */

enum tok_type {
    T_EOF, T_NEWLINE, T_WORD, T_IO_NUMBER,
    T_SEMI, T_DSEMI, T_AMP, T_AND_IF, T_OR_IF, T_PIPE, T_LPAREN, T_RPAREN,
    T_LESS, T_GREAT, T_DGREAT, T_DLESS, T_DLESSDASH, T_TLESS, T_LESSAND, T_GREATAND,
    T_LESSGREAT, T_CLOBBER, T_ANDGREAT, T_ANDDGREAT,
};

static const char *const tok_names[] = {
    "文件结束", "换行", "单词", "数字",
    ";", ";;", "&", "&&", "||", "|", "(", ")",
    "<", ">", ">>", "<<", "<<-", "<<<", "<&", ">&",
    "<>", ">|", "&>", "&>>",
};

typedef struct token {
    int   type;
    word *w;
    int   num;          /* T_IO_NUMBER 的值 */
    int   cmd_pos;      /* 扫描时处于“命令位置”（可能是关键字） */
    int   lineno;
} token;

/* 别名展开：把别名的值当作一段新的输入压栈 */
typedef struct frame {
    char         *s;
    size_t        pos, len;
    int           trailing_blank;
    struct frame *prev;
} frame;

typedef struct pending_hd {
    redir             *r;
    struct pending_hd *next;
} pending_hd;

#define MAX_PUSHBACK  16
#define MAX_RECORDERS 64
#define POOL_SIZE     256
#define MAX_DEPTH     300
#define MAX_ALIAS_SEEN 32

struct parser {
    source         *src;
    strbuf          buf;            /* 当前命令的原文（逐行追加） */
    size_t          pos;
    int             src_eof;
    int             lineno;
    frame          *frames;
    int             pushback[MAX_PUSHBACK];
    int             npush;
    strbuf         *rec[MAX_RECORDERS];     /* 正在记录原文的单词 */
    int             nrec, rec_skipped;
    arena          *ar;
    jmp_buf        *jb;
    int             err;
    char            errmsg[512];
    token           tok;
    int             have_tok;
    int             cmd_pos;
    int             saved_cmd_pos;
    int             redir_target;
    int             no_alias;
    int             alias_next;
    char           *alias_seen[MAX_ALIAS_SEEN];
    int             nalias_seen;
    pending_hd     *heredocs;
    int             had_heredoc;
    int             depth;
    struct parser  *root;
    struct parser  *subs;           /* 根解析器：正在使用的子解析器（出错时统一释放） */
    struct parser  *next_sub;
    strbuf          pool[POOL_SIZE];    /* 临时缓冲区栈（只在根解析器里用） */
    int             pool_used;
};

/* ======================= 出错 ======================= */

static void syntax_error(parser *p, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void syntax_error(parser *p, const char *fmt, ...)
{
    parser *r = p->root;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->errmsg, sizeof r->errmsg, fmt, ap);
    va_end(ap);
    r->err = PARSE_ERROR;
    if (p != r)
        r->lineno = p->lineno;
    longjmp(*r->jb, 1);
}

static const char *tok_desc(const token *t)
{
    if (t->type == T_WORD)
        return t->w && t->w->raw ? t->w->raw : "单词";
    if (t->type >= 0 && t->type < (int)SK_ARRAY_LEN(tok_names))
        return tok_names[t->type];
    return "?";
}

static void unexpected(parser *p, const token *t) __attribute__((noreturn));
static void unexpected(parser *p, const token *t)
{
    if (t->type == T_EOF)
        syntax_error(p, "意外的文件结束");
    if (t->type == T_NEWLINE)
        syntax_error(p, "意外的换行");
    syntax_error(p, "意外的符号 '%s'", tok_desc(t));
}

/* ======================= 临时缓冲区池 ======================= */

static strbuf *pool_get(parser *p)
{
    parser *r = p->root;
    if (r->pool_used >= POOL_SIZE)
        syntax_error(p, "嵌套太深");
    strbuf *b = &r->pool[r->pool_used++];
    sb_clear(b);
    return b;
}

static void pool_put(parser *p)
{
    if (p->root->pool_used > 0)
        p->root->pool_used--;
}

/* ======================= 读字符 ======================= */

static int raw_getc(parser *p)
{
    if (p->npush > 0)
        return p->pushback[--p->npush];
    while (p->frames) {
        frame *f = p->frames;
        if (f->pos < f->len)
            return (unsigned char)f->s[f->pos++];
        p->frames = f->prev;
        if (f->trailing_blank)
            p->alias_next = 1;  /* 别名值以空格结尾：下一个单词也检查别名 */
        free(f->s);
        free(f);
    }
    while (p->pos >= p->buf.len) {
        if (p->src_eof || !p->src)
            return EOF;
        /* 当前命令还没读完（buf 非空）时，交互式输入显示 PS2 续行提示符 */
        int r = source_getline(p->src, &p->buf, p->buf.len > 0);
        if (r < 0) {
            p->root->err = PARSE_CANCEL;
            longjmp(*p->root->jb, 1);
        }
        if (r == 0) {
            p->src_eof = 1;
            return EOF;
        }
    }
    return (unsigned char)p->buf.s[p->pos++];
}

/* 行号在这里统计：读到换行 +1，把换行退回去 -1，这样同一个换行不会被数两次 */
static int pgetc(parser *p)
{
    int c = raw_getc(p);
    if (c != EOF) {
        if (c == '\n')
            p->lineno++;
        for (int i = 0; i < p->nrec; i++)
            sb_putc(p->rec[i], c);
    }
    return c;
}

static void pungetc(parser *p, int c)
{
    if (c == EOF)
        return;
    if (p->npush >= MAX_PUSHBACK)
        syntax_error(p, "内部错误：回退缓冲区溢出");
    if (c == '\n')
        p->lineno--;
    p->pushback[p->npush++] = c;
    for (int i = 0; i < p->nrec; i++)
        if (p->rec[i]->len > 0)
            sb_setlen(p->rec[i], p->rec[i]->len - 1);
}

static void rec_drop(parser *p, size_t n)
{
    for (int i = 0; i < p->nrec; i++)
        sb_setlen(p->rec[i], p->rec[i]->len > n ? p->rec[i]->len - n : 0);
}

static void rec_push(parser *p, strbuf *b)
{
    if (p->nrec < MAX_RECORDERS)
        p->rec[p->nrec++] = b;
    else
        p->rec_skipped++;
}

static void rec_pop(parser *p)
{
    if (p->rec_skipped > 0)
        p->rec_skipped--;
    else if (p->nrec > 0)
        p->nrec--;
}

/* ======================= 单词的构造 ======================= */

typedef struct wb {
    parser *p;
    word   *w;
    wpart **tail;
    strbuf *lit;
    int     lit_q;
    int     has_lit;
    long    added;          /* 已加入的字符/片段数，用来判断 "" 是不是空的 */
} wb;

static wpart *new_part(parser *p, enum wpart_kind kind)
{
    wpart *pt = arena_alloc(p->ar, sizeof *pt);
    pt->kind = kind;
    return pt;
}

static void wb_init(wb *b, parser *p)
{
    b->p = p;
    b->w = arena_alloc(p->ar, sizeof(word));
    b->tail = &b->w->parts;
    b->lit = pool_get(p);
    b->lit_q = 0;
    b->has_lit = 0;
    b->added = 0;
}

static void wb_flush(wb *b)
{
    if (!b->has_lit)
        return;
    wpart *pt = new_part(b->p, WP_LIT);
    pt->quoted = (unsigned char)b->lit_q;
    pt->text = arena_strndup(b->p->ar, sb_str(b->lit), b->lit->len);
    *b->tail = pt;
    b->tail = &pt->next;
    sb_clear(b->lit);
    b->has_lit = 0;
}

static void wb_char(wb *b, int c, int q)
{
    if (b->has_lit && b->lit_q != q)
        wb_flush(b);
    sb_putc(b->lit, c);
    b->has_lit = 1;
    b->lit_q = q;
    b->added++;
}

static void wb_part(wb *b, wpart *pt)
{
    wb_flush(b);
    *b->tail = pt;
    b->tail = &pt->next;
    b->added++;
}

/* "" 或 '' ：加一个空的“带引号”片段，展开后它保证产生一个空参数 */
static void wb_empty_quoted(wb *b)
{
    wb_flush(b);
    b->has_lit = 1;
    b->lit_q = 1;
    wb_flush(b);
}

static word *wb_finish(wb *b)
{
    wb_flush(b);
    pool_put(b->p);
    return b->w;
}

/* 单词是否是“纯文本”（没有任何引号和展开），关键字和别名只认这种 */
static int word_plain(const word *w)
{
    return w && w->parts && !w->parts->next && w->parts->kind == WP_LIT && !w->parts->quoted;
}

static const char *word_text(const word *w)
{
    return w->parts->text;
}

static int word_is(const token *t, const char *s)
{
    return t->type == T_WORD && word_plain(t->w) && strcmp(word_text(t->w), s) == 0;
}

static int tok_kw(const token *t, const char *kw)
{
    return t->cmd_pos && word_is(t, kw);
}

static int is_assignment(const word *w)
{
    if (!w->parts || w->parts->kind != WP_LIT || w->parts->quoted)
        return 0;
    const char *eq = strchr(w->parts->text, '=');
    return eq && is_name_n(w->parts->text, (size_t)(eq - w->parts->text));
}

static const char *const keywords[] = {
    "if", "then", "else", "elif", "fi", "do", "done", "case", "esac",
    "while", "until", "for", "in", "{", "}", "!", "function",
};

int is_keyword(const char *s)
{
    for (size_t i = 0; i < SK_ARRAY_LEN(keywords); i++)
        if (strcmp(s, keywords[i]) == 0)
            return 1;
    return 0;
}

/* 这些关键字之后仍然是“命令位置”（例如 then 后面跟的是命令） */
static int kw_keeps_cmd_pos(const char *s)
{
    static const char *const k[] = { "if", "then", "else", "elif", "do", "while", "until", "{", "!" };
    for (size_t i = 0; i < SK_ARRAY_LEN(k); i++)
        if (strcmp(s, k[i]) == 0)
            return 1;
    return 0;
}

static int is_meta(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' ||
           c == '<' || c == '>' || c == '(' || c == ')';
}

static int is_name_start(int c)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_name_char(int c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

/* ======================= 单词扫描 ======================= */

enum { WM_TOP, WM_DQ, WM_BRACE, WM_ARITH, WM_HEREDOC };

static void scan_into(wb *b, int mode, int q, int sq_literal);
static token *peek(parser *p);
static node *parse_list(parser *p, int top);
static node *parse_compound_list_opt(parser *p);
static node *parse_substring(parser *p, const char *text, size_t len);
static void enter(parser *p);
static void leave(parser *p);

static void scan_squote(wb *b)
{
    parser *p = b->p;
    long before = b->added;
    for (;;) {
        int c = pgetc(p);
        if (c == EOF)
            syntax_error(p, "缺少与之匹配的 '''");
        if (c == '\'')
            break;
        wb_char(b, c, 1);
    }
    if (b->added == before)
        wb_empty_quoted(b);
}

static void scan_dq(wb *b)
{
    long before = b->added;
    scan_into(b, WM_DQ, 1, 1);
    if (b->added == before)
        wb_empty_quoted(b);
}

/* bash 的 $'...'：支持 \n \t \x41 你 这样的转义 */
static void put_utf8(wb *b, uint32_t cp)
{
    char tmp[4];
    int n;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    for (int i = 0; i < n; i++)
        wb_char(b, (unsigned char)tmp[i], 1);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void scan_ansi_c(wb *b)
{
    parser *p = b->p;
    long before = b->added;
    for (;;) {
        int c = pgetc(p);
        if (c == EOF)
            syntax_error(p, "缺少与之匹配的 '''");
        if (c == '\'')
            break;
        if (c != '\\') {
            wb_char(b, c, 1);
            continue;
        }
        c = pgetc(p);
        switch (c) {
        case 'n': wb_char(b, '\n', 1); break;
        case 't': wb_char(b, '\t', 1); break;
        case 'r': wb_char(b, '\r', 1); break;
        case 'a': wb_char(b, '\a', 1); break;
        case 'b': wb_char(b, '\b', 1); break;
        case 'f': wb_char(b, '\f', 1); break;
        case 'v': wb_char(b, '\v', 1); break;
        case 'e': case 'E': wb_char(b, 0x1b, 1); break;
        case '\\': case '\'': case '"': case '?': wb_char(b, c, 1); break;
        case 'x': case 'u': case 'U': {
            int max = c == 'x' ? 2 : c == 'u' ? 4 : 8, n = 0;
            uint32_t v = 0;
            int d;
            while (n < max && (d = hexval(c = pgetc(p))) >= 0) {
                v = v * 16 + (uint32_t)d;
                n++;
            }
            if (n < max)
                pungetc(p, c);
            if (n == 0) {
                wb_char(b, '\\', 1);
                wb_char(b, max == 2 ? 'x' : 'u', 1);
            } else if (max == 2) {
                wb_char(b, (int)v, 1);
            } else {
                put_utf8(b, v);
            }
            break;
        }
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            int v = c - '0', n = 1;
            while (n < 3 && (c = pgetc(p)) >= '0' && c <= '7') {
                v = v * 8 + (c - '0');
                n++;
            }
            if (n < 3)
                pungetc(p, c);
            wb_char(b, v & 0xFF, 1);
            break;
        }
        case EOF:
            syntax_error(p, "缺少与之匹配的 '''");
        default:
            wb_char(b, '\\', 1);
            wb_char(b, c, 1);
        }
    }
    if (b->added == before)
        wb_empty_quoted(b);
}

/* $(( 之后：判断这是算术展开还是 $( (子shell) ) 。
 * 在当前缓冲区里向前看：找到深度为 0 的 ')' 时，后面紧跟 ')' 才是算术。 */
static int arith_lookahead(parser *p)
{
    if (p->npush > 0 || p->frames)
        return 1;
    int depth = 0;
    for (size_t i = p->pos; i < p->buf.len; i++) {
        char c = p->buf.s[i];
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth == 0)
                return i + 1 >= p->buf.len || p->buf.s[i + 1] == ')';
            depth--;
        }
    }
    return 1;
}

static wpart *scan_arith(parser *p, int q)
{
    wpart *pt = new_part(p, WP_ARITH);
    pt->quoted = (unsigned char)q;
    wb ab;
    wb_init(&ab, p);
    scan_into(&ab, WM_ARITH, 1, 0);
    pt->arg = wb_finish(&ab);
    return pt;
}

/* $( ... )：直接递归调用语法分析器，解析到配对的 ')' 为止 */
static wpart *scan_cmdsub(parser *p, int q)
{
    wpart *pt = new_part(p, WP_CMDSUB);
    pt->quoted = (unsigned char)q;
    int save_cmd = p->cmd_pos, save_na = p->no_alias, save_rt = p->redir_target;
    int save_scp = p->saved_cmd_pos, save_an = p->alias_next, save_nas = p->nalias_seen;
    p->have_tok = 0;
    p->cmd_pos = 1;
    p->no_alias = 0;
    p->redir_target = 0;
    p->alias_next = 0;
    p->nalias_seen = 0;
    enter(p);
    pt->cmd = parse_compound_list_opt(p);
    token *t = peek(p);
    if (t->type != T_RPAREN) {
        if (t->type == T_EOF)
            syntax_error(p, "缺少与 '$(' 匹配的 ')'");
        unexpected(p, t);
    }
    p->have_tok = 0;
    leave(p);
    p->cmd_pos = save_cmd;
    p->no_alias = save_na;
    p->redir_target = save_rt;
    p->saved_cmd_pos = save_scp;
    p->alias_next = save_an;
    p->nalias_seen = save_nas;
    return pt;
}

/* `...`：先按反引号的转义规则收集文本，再交给子解析器 */
static wpart *scan_backquote(parser *p, int q)
{
    strbuf *tb = pool_get(p);
    for (;;) {
        int c = pgetc(p);
        if (c == EOF)
            syntax_error(p, "缺少与之匹配的 '`'");
        if (c == '`')
            break;
        if (c == '\\') {
            int n = pgetc(p);
            if (n == '$' || n == '`' || n == '\\' || (q && n == '"')) {
                sb_putc(tb, n);
                continue;
            }
            if (n == '\n')
                continue;
            sb_putc(tb, '\\');
            if (n == EOF)
                syntax_error(p, "缺少与之匹配的 '`'");
            sb_putc(tb, n);
            continue;
        }
        sb_putc(tb, c);
    }
    wpart *pt = new_part(p, WP_CMDSUB);
    pt->quoted = (unsigned char)q;
    char *text = xstrndup(sb_str(tb), tb->len);
    pool_put(p);
    pt->cmd = parse_substring(p, text, strlen(text));
    free(text);
    return pt;
}

static void scan_name(parser *p, strbuf *nb, int c)
{
    sb_putc(nb, c);
    for (;;) {
        c = pgetc(p);
        if (!is_name_char(c)) {
            pungetc(p, c);
            return;
        }
        sb_putc(nb, c);
    }
}

static wpart *scan_braced(parser *p, int q)
{
    wpart *pt = new_part(p, WP_PARAM);
    pt->quoted = (unsigned char)q;
    strbuf *nb = pool_get(p);
    int c = pgetc(p);
    if (c == '#') {
        int c2 = pgetc(p);
        if (c2 == '}') {
            pt->text = arena_strdup(p->ar, "#");
            pool_put(p);
            return pt;
        }
        if (is_name_start(c2) || isdigit(c2) || (c2 != EOF && strchr("?$!@*-#", c2))) {
            pt->op = PO_LEN;
            c = c2;
        } else {
            pungetc(p, c2);
        }
    }
    if (is_name_start(c)) {
        scan_name(p, nb, c);
    } else if (isdigit(c)) {
        sb_putc(nb, c);
        while (isdigit(c = pgetc(p)))
            sb_putc(nb, c);
        pungetc(p, c);
    } else if (c != EOF && strchr("?$!@*-#", c)) {
        sb_putc(nb, c);
    } else {
        syntax_error(p, "错误的替换：${ 后面需要变量名");
    }
    pt->text = arena_strndup(p->ar, sb_str(nb), nb->len);
    pool_put(p);

    c = pgetc(p);
    if (c == '}')
        return pt;
    if (pt->op == PO_LEN)
        syntax_error(p, "错误的替换：${#%s 后面应该是 '}'", pt->text);
    if (c == ':') {
        pt->colon = 1;
        c = pgetc(p);
        if (c == EOF || !strchr("-=+?", c))
            syntax_error(p, "错误的替换：${%s: 后面应该是 - = + ?", pt->text);
    }
    switch (c) {
    case '-': pt->op = PO_DEFAULT; break;
    case '=': pt->op = PO_ASSIGN; break;
    case '+': pt->op = PO_ALT; break;
    case '?': pt->op = PO_ERROR; break;
    case '%': {
        int c2 = pgetc(p);
        if (c2 == '%') pt->op = PO_RM_LSUFFIX;
        else { pungetc(p, c2); pt->op = PO_RM_SSUFFIX; }
        break;
    }
    case '#': {
        int c2 = pgetc(p);
        if (c2 == '#') pt->op = PO_RM_LPREFIX;
        else { pungetc(p, c2); pt->op = PO_RM_SPREFIX; }
        break;
    }
    case EOF:
        syntax_error(p, "缺少与之匹配的 '}'");
    default:
        syntax_error(p, "错误的替换：${%s%c...}", pt->text, c);
    }
    /* 模式类操作符（% #）里，外层双引号不让 * 失效；
     * 默认值类操作符（:- 等）在双引号里时，单引号只是普通字符。 */
    int pattern = pt->op >= PO_RM_SSUFFIX;
    wb ob;
    wb_init(&ob, p);
    scan_into(&ob, WM_BRACE, pattern ? 0 : q, !pattern && q);
    pt->arg = wb_finish(&ob);
    return pt;
}

static void scan_dollar(wb *b, int mode, int q)
{
    parser *p = b->p;
    int c = pgetc(p);
    if (c == '{') {
        wb_part(b, scan_braced(p, q));
        return;
    }
    if (c == '(') {
        int c2 = pgetc(p);
        if (c2 == '(' && arith_lookahead(p)) {
            wb_part(b, scan_arith(p, q));
            return;
        }
        pungetc(p, c2);
        wb_part(b, scan_cmdsub(p, q));
        return;
    }
    if (c == '\'' && !q && mode != WM_HEREDOC) {
        scan_ansi_c(b);
        return;
    }
    if (c == '"' && !q && (mode == WM_TOP || mode == WM_BRACE)) {
        scan_dq(b);
        return;
    }
    if (is_name_start(c) || isdigit(c) || (c != EOF && strchr("?$!#@*-", c))) {
        wpart *pt = new_part(p, WP_PARAM);
        pt->quoted = (unsigned char)q;
        if (is_name_start(c)) {
            strbuf *nb = pool_get(p);
            scan_name(p, nb, c);
            pt->text = arena_strndup(p->ar, sb_str(nb), nb->len);
            pool_put(p);
        } else {
            char one[2] = { (char)c, 0 };
            pt->text = arena_strdup(p->ar, one);
        }
        wb_part(b, pt);
        return;
    }
    wb_char(b, '$', q);
    pungetc(p, c);
}

static void scan_backslash(wb *b, int mode, int q)
{
    parser *p = b->p;
    int n = pgetc(p);
    if (n == EOF) {
        wb_char(b, '\\', q);
        return;
    }
    if (n == '\n') {            /* 反斜杠-换行：续行，两个字符都去掉 */
        rec_drop(p, 2);
        return;
    }
    const char *esc;
    switch (mode) {
    case WM_TOP:
        wb_char(b, n, 1);
        return;
    case WM_BRACE:
        if (!q) {
            wb_char(b, n, 1);
            return;
        }
        esc = "$`\"\\}";
        break;
    case WM_HEREDOC:
        esc = "$`\\";
        break;
    default:
        esc = "$`\"\\";
        break;
    }
    if (strchr(esc, n)) {
        wb_char(b, n, 1);
        return;
    }
    wb_char(b, '\\', q);
    pungetc(p, n);
}

/* 读取单词内容追加到 b，直到遇到本模式的结束符 */
static void scan_into(wb *b, int mode, int q, int sq_literal)
{
    parser *p = b->p;
    int depth = 0;
    for (;;) {
        int c = pgetc(p);
        switch (mode) {
        case WM_TOP:
            if (c == EOF || is_meta(c)) {
                pungetc(p, c);
                return;
            }
            break;
        case WM_DQ:
            if (c == EOF)
                syntax_error(p, "缺少与之匹配的 '\"'");
            if (c == '"')
                return;
            break;
        case WM_BRACE:
            if (c == EOF)
                syntax_error(p, "缺少与之匹配的 '}'");
            if (c == '}') {
                if (depth == 0)
                    return;
                depth--;
            } else if (c == '{') {
                depth++;
            }
            break;
        case WM_ARITH:
            if (c == EOF)
                syntax_error(p, "缺少与之匹配的 '))'");
            if (c == '(') {
                depth++;
            } else if (c == ')') {
                if (depth > 0) {
                    depth--;
                } else {
                    int c2 = pgetc(p);
                    if (c2 == ')')
                        return;
                    syntax_error(p, "算术表达式缺少 '))'");
                }
            }
            break;
        default:        /* WM_HEREDOC */
            if (c == EOF)
                return;
            break;
        }
        switch (c) {
        case '\\':
            scan_backslash(b, mode, q);
            break;
        case '\'':
            if (mode == WM_TOP || ((mode == WM_BRACE || mode == WM_ARITH) && !sq_literal))
                scan_squote(b);
            else
                wb_char(b, c, q);
            break;
        case '"':
            if (mode == WM_TOP || mode == WM_BRACE || mode == WM_ARITH)
                scan_dq(b);
            else
                wb_char(b, c, q);
            break;
        case '$':
            scan_dollar(b, mode, q);
            break;
        case '`':
            wb_part(b, scan_backquote(p, q));
            break;
        default:
            wb_char(b, c, q);
            break;
        }
    }
}

static word *scan_word_top(parser *p)
{
    strbuf *rb = pool_get(p);
    rec_push(p, rb);
    wb b;
    wb_init(&b, p);
    scan_into(&b, WM_TOP, 0, 0);
    word *w = wb_finish(&b);
    rec_pop(p);
    w->raw = arena_strndup(p->ar, sb_str(rb), rb->len);
    pool_put(p);
    return w;
}

/* ======================= here-doc ======================= */

static word *literal_word(parser *p, const char *s, size_t n)
{
    word *w = arena_alloc(p->ar, sizeof *w);
    wpart *pt = new_part(p, WP_LIT);
    pt->quoted = 1;
    pt->text = arena_strndup(p->ar, s, n);
    w->parts = pt;
    w->raw = pt->text;
    return w;
}

static word *parse_heredoc_body(parser *p, const char *text, size_t len);

/* 遇到换行时：把这一行里登记的 here-doc 正文依次读进来 */
static void read_heredocs(parser *p)
{
    pending_hd *list = p->heredocs;
    p->heredocs = NULL;
    while (list) {
        pending_hd *h = list;
        list = h->next;
        redir *r = h->r;
        free(h);
        strbuf *body = pool_get(p);
        strbuf *line = pool_get(p);
        for (;;) {
            sb_clear(line);
            int c;
            while ((c = pgetc(p)) != EOF && c != '\n')
                sb_putc(line, c);
            const char *l = sb_str(line);
            if (r->strip_tabs)
                while (*l == '\t')
                    l++;
            if (strcmp(l, r->delim) == 0)
                break;
            if (c == EOF) {
                if (*l) {
                    sb_puts(body, l);
                    sb_putc(body, '\n');
                }
                sh_error_noloc("警告: here-document 在文件结束处终止（应以 '%s' 结束）", r->delim);
                break;
            }
            sb_puts(body, l);
            sb_putc(body, '\n');
        }
        if (r->body_quoted)
            r->body = literal_word(p, sb_str(body), body->len);
        else
            r->body = parse_heredoc_body(p, sb_str(body), body->len);
        pool_put(p);
        pool_put(p);
    }
}

static void add_heredoc(parser *p, redir *r)
{
    pending_hd *h = xcalloc(1, sizeof *h);
    h->r = r;
    pending_hd **pp = &p->heredocs;
    while (*pp)
        pp = &(*pp)->next;
    *pp = h;
    p->root->had_heredoc = 1;
}

/* ======================= 词法分析 ======================= */

static int alias_seen(parser *p, const char *name)
{
    for (int i = 0; i < p->nalias_seen; i++)
        if (strcmp(p->alias_seen[i], name) == 0)
            return 1;
    return 0;
}

static void push_alias(parser *p, const char *name, const char *value)
{
    /* 已经被回退的字符必须排在别名文本之后，先把它们变成一个帧 */
    if (p->npush > 0) {
        frame *f = xcalloc(1, sizeof *f);
        f->s = xmalloc((size_t)p->npush + 1);
        for (int i = 0; i < p->npush; i++)
            f->s[i] = (char)p->pushback[p->npush - 1 - i];
        f->len = (size_t)p->npush;
        f->s[f->len] = 0;
        f->prev = p->frames;
        p->frames = f;
        p->npush = 0;
    }
    frame *f = xcalloc(1, sizeof *f);
    f->len = strlen(value);
    f->s = xstrndup(value, f->len);
    f->trailing_blank = f->len > 0 && (value[f->len - 1] == ' ' || value[f->len - 1] == '\t');
    f->prev = p->frames;
    p->frames = f;
    if (p->nalias_seen < MAX_ALIAS_SEEN)
        p->alias_seen[p->nalias_seen++] = arena_strdup(p->ar, name);
}

static void lex(parser *p, token *t)
{
restart:
    memset(t, 0, sizeof *t);
    int c;
    for (;;) {
        c = pgetc(p);
        if (c == ' ' || c == '\t')
            continue;
        if (c == '\\') {
            int n = pgetc(p);
            if (n == '\n') {
                rec_drop(p, 2);
                continue;
            }
            pungetc(p, n);
        }
        break;
    }
    if (c == '#') {
        while ((c = pgetc(p)) != EOF && c != '\n')
            ;
    }
    t->lineno = p->lineno;
    t->cmd_pos = p->cmd_pos;
    int c2, c3;
    switch (c) {
    case EOF:
        t->type = T_EOF;
        if (p->heredocs)
            read_heredocs(p);
        p->cmd_pos = 1;
        return;
    case '\n':
        t->type = T_NEWLINE;
        if (p->heredocs)
            read_heredocs(p);
        p->cmd_pos = 1;
        p->nalias_seen = 0;
        return;
    case ';':
        c2 = pgetc(p);
        if (c2 == ';') t->type = T_DSEMI;
        else { pungetc(p, c2); t->type = T_SEMI; }
        p->cmd_pos = 1;
        p->nalias_seen = 0;
        return;
    case '&':
        c2 = pgetc(p);
        if (c2 == '&') { t->type = T_AND_IF; p->cmd_pos = 1; }
        else if (c2 == '>') {
            c3 = pgetc(p);
            if (c3 == '>') t->type = T_ANDDGREAT;
            else { pungetc(p, c3); t->type = T_ANDGREAT; }
        } else { pungetc(p, c2); t->type = T_AMP; p->cmd_pos = 1; }
        p->nalias_seen = 0;
        return;
    case '|':
        c2 = pgetc(p);
        if (c2 == '|') t->type = T_OR_IF;
        else { pungetc(p, c2); t->type = T_PIPE; }
        p->cmd_pos = 1;
        p->nalias_seen = 0;
        return;
    case '(':
        t->type = T_LPAREN;
        p->cmd_pos = 1;
        return;
    case ')':
        t->type = T_RPAREN;
        p->cmd_pos = 1;
        return;
    case '<':
        c2 = pgetc(p);
        if (c2 == '<') {
            c3 = pgetc(p);
            if (c3 == '-') t->type = T_DLESSDASH;
            else if (c3 == '<') t->type = T_TLESS;
            else { pungetc(p, c3); t->type = T_DLESS; }
        } else if (c2 == '&') t->type = T_LESSAND;
        else if (c2 == '>') t->type = T_LESSGREAT;
        else { pungetc(p, c2); t->type = T_LESS; }
        return;
    case '>':
        c2 = pgetc(p);
        if (c2 == '>') t->type = T_DGREAT;
        else if (c2 == '&') t->type = T_GREATAND;
        else if (c2 == '|') t->type = T_CLOBBER;
        else { pungetc(p, c2); t->type = T_GREAT; }
        return;
    default:
        break;
    }

    /* 数字紧跟 < 或 > ：文件描述符编号，例如 2>err.txt */
    if (c >= '0' && c <= '9' && !p->redir_target) {
        int digits[10], nd = 0, d = c;
        while (d >= '0' && d <= '9' && nd < 9) {
            digits[nd++] = d;
            d = pgetc(p);
        }
        if ((d == '<' || d == '>') && nd < 9) {
            pungetc(p, d);
            int v = 0;
            for (int i = 0; i < nd; i++)
                v = v * 10 + (digits[i] - '0');
            t->type = T_IO_NUMBER;
            t->num = v;
            return;
        }
        pungetc(p, d);
        for (int i = nd - 1; i > 0; i--)
            pungetc(p, digits[i]);
    }

    pungetc(p, c);
    t->w = scan_word_top(p);
    t->type = T_WORD;

    if (p->redir_target) {      /* 重定向的文件名：不是命令名，不做关键字/别名处理 */
        p->redir_target = 0;
        p->cmd_pos = p->saved_cmd_pos;
        t->cmd_pos = 0;
        return;
    }
    int was_cmd = t->cmd_pos;
    int try_alias = (was_cmd || p->alias_next) && !p->no_alias;
    p->alias_next = 0;
    if (word_plain(t->w)) {
        const char *text = word_text(t->w);
        if (was_cmd && is_keyword(text)) {
            p->cmd_pos = kw_keeps_cmd_pos(text);
            return;
        }
        if (try_alias) {
            const char *val = alias_get(text);
            if (val && !alias_seen(p, text)) {
                push_alias(p, text, val);
                p->cmd_pos = was_cmd;
                goto restart;
            }
        }
    }
    p->nalias_seen = 0;
    if (was_cmd && is_assignment(t->w))
        p->cmd_pos = 1;
    else
        p->cmd_pos = 0;
}

/* 先扫描到局部变量里再存进 p->tok：扫描单词时可能递归解析 $(...)，
 * 递归过程中 p->tok 会被反复改写。 */
static token *peek(parser *p)
{
    if (!p->have_tok) {
        token t;
        lex(p, &t);
        p->tok = t;
        p->have_tok = 1;
    }
    return &p->tok;
}

static token next(parser *p)
{
    peek(p);
    p->have_tok = 0;
    return p->tok;
}

static void consume(parser *p)
{
    p->have_tok = 0;
}

static void linebreak(parser *p)
{
    while (peek(p)->type == T_NEWLINE)
        consume(p);
}

static void enter(parser *p)
{
    if (++p->root->depth > MAX_DEPTH)
        syntax_error(p, "嵌套太深");
}

static void leave(parser *p)
{
    p->root->depth--;
}

/* ======================= 语法分析 ======================= */

static node *new_node(parser *p, enum node_type type)
{
    node *n = arena_alloc(p->ar, sizeof *n);
    n->type = type;
    n->lineno = p->tok.lineno ? p->tok.lineno : p->lineno;
    return n;
}

static int is_list_end(const token *t)
{
    switch (t->type) {
    case T_EOF: case T_RPAREN: case T_DSEMI:
        return 1;
    case T_WORD: {
        static const char *const ends[] = { "then", "else", "elif", "fi", "do", "done", "esac", "}" };
        for (size_t i = 0; i < SK_ARRAY_LEN(ends); i++)
            if (tok_kw(t, ends[i]))
                return 1;
        return 0;
    }
    default:
        return 0;
    }
}

static int is_redir_op(int type)
{
    return type >= T_LESS && type <= T_ANDDGREAT;
}

static void expect_kw(parser *p, const char *kw)
{
    token *t = peek(p);
    if (!tok_kw(t, kw)) {
        if (t->type == T_EOF)
            syntax_error(p, "意外的文件结束（缺少 '%s'）", kw);
        syntax_error(p, "意外的符号 '%s'（这里应该是 '%s'）", tok_desc(t), kw);
    }
    consume(p);
}

/* 去掉定界符里的引号：<<'EOF'、<<"EOF"、<<\EOF 都表示正文不展开 */
static void heredoc_delim(parser *p, redir *r)
{
    const char *raw = r->target->raw ? r->target->raw : "";
    strbuf *d = pool_get(p);
    int quoted = 0;
    for (const char *s = raw; *s; s++) {
        if (*s == '\'' || *s == '"') {
            quoted = 1;
            continue;
        }
        if (*s == '\\' && s[1]) {
            quoted = 1;
            s++;
        }
        sb_putc(d, *s);
    }
    r->delim = arena_strndup(p->ar, sb_str(d), d->len);
    r->body_quoted = (unsigned char)quoted;
    pool_put(p);
}

static redir *parse_redir(parser *p)
{
    redir *r = arena_alloc(p->ar, sizeof *r);
    token t = next(p);
    int fd = -1;
    if (t.type == T_IO_NUMBER) {
        fd = t.num;
        r->fd_given = 1;
        t = next(p);
        if (!is_redir_op(t.type))
            unexpected(p, &t);
    }
    int deffd = 1;
    switch (t.type) {
    case T_LESS:      r->type = R_IN; deffd = 0; break;
    case T_GREAT:     r->type = R_OUT; break;
    case T_DGREAT:    r->type = R_APPEND; break;
    case T_CLOBBER:   r->type = R_CLOBBER; break;
    case T_LESSGREAT: r->type = R_INOUT; deffd = 0; break;
    case T_LESSAND:   r->type = R_DUPIN; deffd = 0; break;
    case T_GREATAND:  r->type = R_DUPOUT; break;
    case T_DLESS:     r->type = R_HEREDOC; deffd = 0; break;
    case T_DLESSDASH: r->type = R_HEREDOC; r->strip_tabs = 1; deffd = 0; break;
    case T_TLESS:     r->type = R_HEREDOC; r->herestring = 1; deffd = 0; break;
    case T_ANDGREAT:  r->type = R_OUTERR; break;
    case T_ANDDGREAT: r->type = R_APPENDERR; break;
    default:          unexpected(p, &t);
    }
    r->fd = fd >= 0 ? fd : deffd;
    p->saved_cmd_pos = p->cmd_pos;
    p->redir_target = 1;
    token w = next(p);
    p->redir_target = 0;
    if (w.type != T_WORD) {
        if (w.type == T_EOF || w.type == T_NEWLINE)
            syntax_error(p, "重定向 '%s' 后面缺少文件名", tok_desc(&t));
        unexpected(p, &w);
    }
    r->target = w.w;
    if (r->type == R_HEREDOC && !r->herestring) {
        heredoc_delim(p, r);
        add_heredoc(p, r);
    }
    return r;
}

static node *parse_command(parser *p);

static node *parse_compound_list(parser *p)
{
    linebreak(p);
    token *t = peek(p);
    if (is_list_end(t))
        unexpected(p, t);
    return parse_list(p, 0);
}

static node *parse_compound_list_opt(parser *p)
{
    linebreak(p);
    if (is_list_end(peek(p)))
        return NULL;
    return parse_list(p, 0);
}

static node *parse_pipeline(parser *p)
{
    int negate = 0;
    token *t = peek(p);
    while (tok_kw(t, "!")) {
        negate = !negate;
        consume(p);
        t = peek(p);
    }
    node *first = parse_command(p);
    if (peek(p)->type != T_PIPE && !negate)
        return first;
    /* 数组也从内存池分配：parse_command 出错 longjmp 时不会泄漏 */
    int cap = 4, n = 0;
    node **cmds = arena_alloc(p->ar, sizeof(node *) * (size_t)cap);
    cmds[n++] = first;
    while (peek(p)->type == T_PIPE) {
        consume(p);
        linebreak(p);
        if (n == cap) {
            node **nc = arena_alloc(p->ar, sizeof(node *) * (size_t)cap * 2);
            memcpy(nc, cmds, sizeof(node *) * (size_t)n);
            cmds = nc;
            cap *= 2;
        }
        cmds[n++] = parse_command(p);
    }
    node *pn = new_node(p, N_PIPELINE);
    pn->lineno = first->lineno;
    pn->u.pipe.cmds = cmds;
    pn->u.pipe.ncmds = n;
    pn->u.pipe.negate = negate;
    return pn;
}

static node *parse_and_or(parser *p)
{
    node *n = parse_pipeline(p);
    for (;;) {
        token *t = peek(p);
        if (t->type != T_AND_IF && t->type != T_OR_IF)
            return n;
        enum node_type type = t->type == T_AND_IF ? N_AND : N_OR;
        consume(p);
        linebreak(p);
        node *r = parse_pipeline(p);
        node *a = new_node(p, type);
        a->lineno = n->lineno;
        a->u.andor.left = n;
        a->u.andor.right = r;
        n = a;
    }
}

static node *parse_list(parser *p, int top)
{
    list_item *head = NULL, **tail = &head;
    int count = 0;
    for (;;) {
        node *n = parse_and_or(p);
        list_item *it = arena_alloc(p->ar, sizeof *it);
        it->node = n;
        *tail = it;
        tail = &it->next;
        count++;
        token *t = peek(p);
        if (t->type == T_SEMI || t->type == T_AMP) {
            it->async = t->type == T_AMP;
            consume(p);
            t = peek(p);
            if (top) {
                if (t->type == T_NEWLINE || t->type == T_EOF)
                    break;
            } else {
                while (t->type == T_NEWLINE) {
                    consume(p);
                    t = peek(p);
                }
            }
            if (is_list_end(t))
                break;
            continue;
        }
        if (t->type == T_NEWLINE && !top) {
            while (t->type == T_NEWLINE) {
                consume(p);
                t = peek(p);
            }
            if (is_list_end(t))
                break;
            continue;
        }
        break;
    }
    if (count == 1 && !head->async)
        return head->node;
    node *ln = new_node(p, N_LIST);
    ln->lineno = head->node->lineno;
    ln->u.list.items = head;
    return ln;
}

static node *parse_if_body(parser *p, int lineno)
{
    node *n = new_node(p, N_IF);
    n->lineno = lineno;
    n->u.ifn.cond = parse_compound_list(p);
    expect_kw(p, "then");
    n->u.ifn.then_part = parse_compound_list(p);
    token *t = peek(p);
    if (tok_kw(t, "elif")) {
        int ln = t->lineno;
        consume(p);
        n->u.ifn.else_part = parse_if_body(p, ln);
        return n;
    }
    if (tok_kw(t, "else")) {
        consume(p);
        n->u.ifn.else_part = parse_compound_list(p);
    }
    expect_kw(p, "fi");
    return n;
}

static node *parse_loop(parser *p, enum node_type type)
{
    node *n = new_node(p, type);
    consume(p);
    n->u.loop.cond = parse_compound_list(p);
    expect_kw(p, "do");
    n->u.loop.body = parse_compound_list(p);
    expect_kw(p, "done");
    return n;
}

static node *parse_for(parser *p)
{
    node *n = new_node(p, N_FOR);
    consume(p);
    token t = next(p);
    if (t.type != T_WORD || !word_plain(t.w) || !is_name(word_text(t.w)))
        syntax_error(p, "for 后面需要一个变量名");
    n->u.forn.var = arena_strdup(p->ar, word_text(t.w));
    linebreak(p);
    token *tp = peek(p);
    if (word_is(tp, "in")) {
        consume(p);
        n->u.forn.has_in = 1;
        word **wt = &n->u.forn.words;
        while ((tp = peek(p))->type == T_WORD) {
            *wt = tp->w;
            wt = &tp->w->next;
            consume(p);
        }
        if (tp->type != T_SEMI && tp->type != T_NEWLINE)
            unexpected(p, tp);
        consume(p);
    } else if (tp->type == T_SEMI) {
        consume(p);
    }
    linebreak(p);
    tp = peek(p);
    if (!word_is(tp, "do")) {
        if (tp->type == T_EOF)
            syntax_error(p, "意外的文件结束（缺少 'do'）");
        syntax_error(p, "意外的符号 '%s'（这里应该是 'do'）", tok_desc(tp));
    }
    consume(p);
    p->cmd_pos = 1;
    n->u.forn.body = parse_compound_list(p);
    expect_kw(p, "done");
    return n;
}

static node *parse_case(parser *p)
{
    node *n = new_node(p, N_CASE);
    consume(p);
    token t = next(p);
    if (t.type != T_WORD)
        unexpected(p, &t);
    n->u.casen.subject = t.w;
    linebreak(p);
    token *tp = peek(p);
    if (!word_is(tp, "in"))
        syntax_error(p, "case 后面缺少 'in'");
    consume(p);
    case_item **it_tail = &n->u.casen.items;
    for (;;) {
        p->no_alias = 1;
        linebreak(p);
        tp = peek(p);
        if (word_is(tp, "esac")) {
            consume(p);
            break;
        }
        if (tp->type == T_LPAREN)
            consume(p);
        case_item *it = arena_alloc(p->ar, sizeof *it);
        word **pt = &it->patterns;
        for (;;) {
            token w = next(p);
            if (w.type != T_WORD)
                unexpected(p, &w);
            *pt = w.w;
            pt = &w.w->next;
            if (peek(p)->type == T_PIPE) {
                consume(p);
                continue;
            }
            break;
        }
        token rp = next(p);
        if (rp.type != T_RPAREN)
            syntax_error(p, "case 模式后面缺少 ')'");
        p->no_alias = 0;
        p->cmd_pos = 1;
        linebreak(p);
        tp = peek(p);
        if (tp->type != T_DSEMI && !word_is(tp, "esac"))
            it->body = parse_list(p, 0);
        *it_tail = it;
        it_tail = &it->next;
        tp = peek(p);
        if (tp->type == T_DSEMI) {
            consume(p);
            continue;
        }
        if (word_is(tp, "esac")) {
            consume(p);
            break;
        }
        unexpected(p, tp);
    }
    p->no_alias = 0;
    return n;
}

static int starts_compound(const token *t)
{
    return t->type == T_LPAREN || tok_kw(t, "{") || tok_kw(t, "if") || tok_kw(t, "while") ||
           tok_kw(t, "until") || tok_kw(t, "for") || tok_kw(t, "case");
}

static node *parse_funcdef_body(parser *p, const char *name, int lineno)
{
    linebreak(p);
    token *t = peek(p);
    if (!starts_compound(t))
        syntax_error(p, "函数 %s 的函数体必须是复合命令（例如 { ...; }）", name);
    node *f = new_node(p, N_FUNCDEF);
    f->lineno = lineno;
    f->u.func.name = arena_strdup(p->ar, name);
    f->u.func.body = parse_command(p);
    return f;
}

static node *parse_simple(parser *p)
{
    node *n = new_node(p, N_SIMPLE);
    n->lineno = peek(p)->lineno;
    word **atail = &n->u.simple.assigns, **wtail = &n->u.simple.args;
    redir **rtail = &n->redirs;
    int nwords = 0;
    for (;;) {
        token *t = peek(p);
        if (t->type == T_IO_NUMBER || is_redir_op(t->type)) {
            *rtail = parse_redir(p);
            rtail = &(*rtail)->next;
            continue;
        }
        if (t->type != T_WORD)
            break;
        word *w = t->w;
        if (nwords == 0 && is_assignment(w)) {
            consume(p);
            *atail = w;
            atail = &w->next;
            continue;
        }
        consume(p);
        *wtail = w;
        wtail = &w->next;
        nwords++;
        if (nwords == 1 && !n->u.simple.assigns && !n->redirs && peek(p)->type == T_LPAREN) {
            if (!word_plain(w) || strchr(word_text(w), '=') || strchr(word_text(w), '$'))
                syntax_error(p, "'%s' 不能用作函数名", w->raw);
            consume(p);
            token rp = next(p);
            if (rp.type != T_RPAREN)
                syntax_error(p, "函数定义 %s() 缺少 ')'", word_text(w));
            return parse_funcdef_body(p, word_text(w), n->lineno);
        }
    }
    if (nwords == 0 && !n->u.simple.assigns && !n->redirs)
        unexpected(p, peek(p));
    return n;
}

static node *parse_command(parser *p)
{
    enter(p);
    token *t = peek(p);
    node *n = NULL;
    int compound = 1;
    if (t->type == T_LPAREN) {
        n = new_node(p, N_SUBSHELL);
        consume(p);
        n->u.group.body = parse_compound_list(p);
        token rp = next(p);
        if (rp.type != T_RPAREN) {
            if (rp.type == T_EOF)
                syntax_error(p, "意外的文件结束（缺少 ')'）");
            unexpected(p, &rp);
        }
    } else if (t->type == T_WORD && t->cmd_pos && word_plain(t->w) && is_keyword(word_text(t->w))) {
        const char *kw = word_text(t->w);
        int lineno = t->lineno;
        if (strcmp(kw, "if") == 0) {
            consume(p);
            n = parse_if_body(p, lineno);
        } else if (strcmp(kw, "while") == 0) {
            n = parse_loop(p, N_WHILE);
        } else if (strcmp(kw, "until") == 0) {
            n = parse_loop(p, N_UNTIL);
        } else if (strcmp(kw, "for") == 0) {
            n = parse_for(p);
        } else if (strcmp(kw, "case") == 0) {
            n = parse_case(p);
        } else if (strcmp(kw, "{") == 0) {
            n = new_node(p, N_BRACE);
            consume(p);
            n->u.group.body = parse_compound_list(p);
            expect_kw(p, "}");
        } else if (strcmp(kw, "function") == 0) {
            consume(p);
            token nt = next(p);
            if (nt.type != T_WORD || !word_plain(nt.w))
                syntax_error(p, "function 后面需要函数名");
            const char *name = word_text(nt.w);
            p->cmd_pos = 1;         /* 函数名后面的 { 要当关键字识别 */
            if (peek(p)->type == T_LPAREN) {
                consume(p);
                token rp = next(p);
                if (rp.type != T_RPAREN)
                    syntax_error(p, "函数定义 %s() 缺少 ')'", name);
            }
            n = parse_funcdef_body(p, name, lineno);
            compound = 0;
        } else if (strcmp(kw, "in") == 0) {
            n = parse_simple(p);
            compound = 0;
        } else {
            unexpected(p, t);
        }
    } else {
        n = parse_simple(p);
        compound = 0;
    }
    if (compound) {
        redir **rtail = &n->redirs;
        for (;;) {
            token *r = peek(p);
            if (r->type != T_IO_NUMBER && !is_redir_op(r->type))
                break;
            *rtail = parse_redir(p);
            rtail = &(*rtail)->next;
        }
    }
    leave(p);
    return n;
}

/* ======================= 解析器对象 ======================= */

static parser *parser_alloc(source *src)
{
    parser *p = xcalloc(1, sizeof *p);
    p->src = src;
    p->lineno = 1;
    p->cmd_pos = 1;
    p->root = p;
    return p;
}

parser *parser_new(source *src)
{
    return parser_alloc(src);
}

static void free_frames(parser *p)
{
    while (p->frames) {
        frame *f = p->frames;
        p->frames = f->prev;
        free(f->s);
        free(f);
    }
}

static void free_heredocs(parser *p)
{
    while (p->heredocs) {
        pending_hd *h = p->heredocs;
        p->heredocs = h->next;
        free(h);
    }
}

static void parser_free_sub(parser *s)
{
    free_frames(s);
    free_heredocs(s);
    sb_free(&s->buf);
    free(s);
}

void parser_free(parser *p)
{
    if (!p)
        return;
    while (p->subs) {
        parser *s = p->subs;
        p->subs = s->next_sub;
        parser_free_sub(s);
    }
    free_frames(p);
    free_heredocs(p);
    sb_free(&p->buf);
    for (int i = 0; i < POOL_SIZE; i++)
        sb_free(&p->pool[i]);
    if (p->ar)
        arena_unref(p->ar);
    free(p);
}

/* 子解析器：解析一段已经在内存里的文本（反引号内容、here-doc 正文），与根解析器共用内存池 */
static parser *sub_new(parser *p, const char *text, size_t len)
{
    parser *root = p->root;
    parser *s = xcalloc(1, sizeof *s);
    s->root = root;
    s->ar = root->ar;
    s->lineno = p->lineno;
    s->cmd_pos = 1;
    s->jb = root->jb;
    sb_putn(&s->buf, text, len);
    s->next_sub = root->subs;
    root->subs = s;
    return s;
}

static void sub_done(parser *s)
{
    parser *root = s->root;
    for (parser **pp = &root->subs; *pp; pp = &(*pp)->next_sub)
        if (*pp == s) {
            *pp = s->next_sub;
            break;
        }
    parser_free_sub(s);
}

static node *parse_substring(parser *p, const char *text, size_t len)
{
    parser *s = sub_new(p, text, len);
    enter(p);
    node *n = parse_compound_list_opt(s);
    token *t = peek(s);
    if (t->type != T_EOF)
        unexpected(s, t);
    leave(p);
    sub_done(s);
    return n;
}

static word *parse_heredoc_body(parser *p, const char *text, size_t len)
{
    parser *s = sub_new(p, text, len);
    wb b;
    wb_init(&b, s);
    scan_into(&b, WM_HEREDOC, 1, 1);
    word *w = wb_finish(&b);
    sub_done(s);
    return w;
}

word *parse_heredoc_text(const char *text, arena *ar)
{
    parser *p = parser_alloc(NULL);
    p->ar = ar;
    sb_puts(&p->buf, text);
    jmp_buf jb;
    p->jb = &jb;
    word *w = NULL;
    if (setjmp(jb) == 0) {
        wb b;
        wb_init(&b, p);
        scan_into(&b, WM_HEREDOC, 1, 1);
        w = wb_finish(&b);
    }
    p->ar = NULL;
    parser_free(p);
    return w;
}

int parser_next(parser *p, node **out, arena **arp)
{
    *out = NULL;
    *arp = NULL;
    /* 上一条命令剩下的字符（正常情况下没有）挪到缓冲区开头 */
    if (p->pos < p->buf.len) {
        size_t rest = p->buf.len - p->pos;
        memmove(p->buf.s, p->buf.s + p->pos, rest);
        sb_setlen(&p->buf, rest);
    } else {
        sb_clear(&p->buf);
    }
    p->pos = 0;
    if (p->ar)
        arena_unref(p->ar);
    p->ar = arena_new();
    p->have_tok = 0;
    p->cmd_pos = 1;
    p->no_alias = 0;
    p->alias_next = 0;
    p->nalias_seen = 0;
    p->redir_target = 0;
    p->depth = 0;
    p->pool_used = 0;
    p->nrec = 0;
    p->rec_skipped = 0;
    p->had_heredoc = 0;
    p->err = 0;
    p->src_eof = 0;         /* 终端上按 Ctrl-D 之后还可以继续读（例如“有已停止的作业”的提醒） */
    free_heredocs(p);

    jmp_buf jb;
    p->jb = &jb;
    if (setjmp(jb)) {
        while (p->subs) {
            parser *s = p->subs;
            p->subs = s->next_sub;
            parser_free_sub(s);
        }
        free_frames(p);
        free_heredocs(p);
        p->npush = 0;
        p->have_tok = 0;
        p->pos = p->buf.len;        /* 丢掉这一行剩下的内容 */
        arena_unref(p->ar);
        p->ar = NULL;
        if (p->err == PARSE_CANCEL)
            return PARSE_CANCEL;
        /* 脚本、-c、被 source 的文件里出错时带上位置；在终端上直接输入的就不用了 */
        if (p->src && (!sh.interactive || sh.source_depth > 0))
            sh_error_noloc("语法错误: %s（%s 第 %d 行）", p->errmsg, source_name(p->src), p->lineno);
        else
            sh_error_noloc("语法错误: %s", p->errmsg);
        return PARSE_ERROR;
    }

    token *t = peek(p);
    if (t->type == T_EOF) {
        arena_unref(p->ar);
        p->ar = NULL;
        return PARSE_EOF;
    }
    if (t->type == T_NEWLINE) {
        consume(p);
        arena_unref(p->ar);
        p->ar = NULL;
        return PARSE_EMPTY;
    }
    node *n = parse_list(p, 1);
    t = peek(p);
    if (t->type == T_NEWLINE)
        consume(p);
    else if (t->type != T_EOF)
        unexpected(p, t);
    *out = n;
    *arp = p->ar;
    p->ar = NULL;
    return PARSE_OK;
}

/* 历史记录里一条命令占一行：多行命令用 "; " 连起来（在 then/do/| 之后用空格） */
char *parser_command_text(parser *p)
{
    size_t len = p->pos <= p->buf.len ? p->pos : p->buf.len;
    const char *s = p->buf.s ? p->buf.s : "";
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;
    if (p->had_heredoc) {
        const char *nl = memchr(s, '\n', len);
        if (nl)
            len = (size_t)(nl - s);
    }
    /* 合成一行之后意思不能变：
     *   顶层的换行 → "; "（在 then、do、|、&& 之类后面换成空格，否则是语法错误）
     *   引号里的换行 → 先结束引号，插入 $'\n'，再接着引号：'a⏎b' 变成 'a'$'\n''b'
     *   # 注释去掉（否则合成一行后，后面的命令都会变成注释）
     *   反斜杠续行直接去掉 */
    strbuf out;
    sb_init(&out);
    sb_putn(&out, "", 0);
    int sq = 0, dq = 0;
    size_t i = 0;
    while (i < len) {
        char c = s[i];
        if (sq) {
            if (c == '\'')
                sq = 0;
            if (c == '\n')
                sb_puts(&out, "'$'\\n''");
            else
                sb_putc(&out, c);
            i++;
            continue;
        }
        if (c == '\\' && i + 1 < len) {
            if (s[i + 1] != '\n') {
                sb_putc(&out, c);
                sb_putc(&out, s[i + 1]);
            }
            i += 2;
            continue;
        }
        if (dq) {
            if (c == '"')
                dq = 0;
            if (c == '\n')
                sb_puts(&out, "\"$'\\n'\"");
            else
                sb_putc(&out, c);
            i++;
            continue;
        }
        if (c == '\'' || c == '"') {
            if (c == '\'')
                sq = 1;
            else
                dq = 1;
            sb_putc(&out, c);
            i++;
            continue;
        }
        if (c == '#' && (out.len == 0 || strchr(" \t;&|()", out.s[out.len - 1]))) {
            while (i < len && s[i] != '\n')
                i++;
            continue;
        }
        if (c != '\n') {
            sb_putc(&out, c);
            i++;
            continue;
        }
        /* 顶层换行：跳过后面的空行和缩进，决定用什么连接 */
        while (i < len && (s[i] == '\n' || s[i] == ' ' || s[i] == '\t'))
            i++;
        size_t ol = out.len;
        while (ol > 0 && (out.s[ol - 1] == ' ' || out.s[ol - 1] == '\t'))
            ol--;
        sb_setlen(&out, ol);
        if (i >= len || out.len == 0)
            continue;
        static const char *const no_semi[] = { "then", "do", "else", "{", "(", "|", "&&", "||", ";", "&", "in" };
        int semi = 1;
        for (size_t k = 0; k < SK_ARRAY_LEN(no_semi); k++) {
            size_t kl = strlen(no_semi[k]);
            if (out.len >= kl && memcmp(out.s + out.len - kl, no_semi[k], kl) == 0 &&
                (out.len == kl || !is_name_char((unsigned char)out.s[out.len - kl - 1]) ||
                 !is_name_char((unsigned char)no_semi[k][0])))
                semi = 0;
        }
        if (len - i >= 2 && s[i] == ';' && s[i + 1] == ';')
            semi = 0;
        sb_puts(&out, semi ? "; " : " ");
    }
    size_t ol = out.len;
    while (ol > 0 && (out.s[ol - 1] == ' ' || out.s[ol - 1] == '\t'))
        ol--;
    sb_setlen(&out, ol);
    return sb_take(&out);
}

/* ======================= 语法树 → 文本 =======================
 * 作业表(jobs)要显示命令的样子，type 要显示函数体。
 * 单词保存了原始写法，这里按结构把它们拼回去。 */

static void dp_words(const word *w, strbuf *out, int *first)
{
    for (; w; w = w->next) {
        if (!*first)
            sb_putc(out, ' ');
        sb_puts(out, w->raw ? w->raw : "");
        *first = 0;
    }
}

static void dp_redirs(const redir *r, strbuf *out, int first)
{
    static const char *const ops[] = { "<", ">", ">>", ">|", "<>", "<&", ">&", "<<", "&>", "&>>" };
    for (; r; r = r->next) {
        if (!first)
            sb_putc(out, ' ');
        first = 0;
        if (r->fd_given)
            sb_printf(out, "%d", r->fd);
        if (r->type == R_HEREDOC)
            sb_puts(out, r->herestring ? "<<<" : r->strip_tabs ? "<<-" : "<<");
        else
            sb_puts(out, ops[r->type]);
        sb_puts(out, r->target && r->target->raw ? r->target->raw : "");
    }
}

static void dp_list_body(const node *n, strbuf *out)
{
    deparse(n, out);
    if (n && n->type == N_LIST) {
        const list_item *it = n->u.list.items;
        while (it && it->next)
            it = it->next;
        if (it && it->async)
            return;
    }
    sb_putc(out, ';');
}

void deparse(const node *n, strbuf *out)
{
    if (!n)
        return;
    int first = 1;
    switch (n->type) {
    case N_SIMPLE:
        dp_words(n->u.simple.assigns, out, &first);
        dp_words(n->u.simple.args, out, &first);
        dp_redirs(n->redirs, out, first);
        return;
    case N_PIPELINE:
        if (n->u.pipe.negate)
            sb_puts(out, "! ");
        for (int i = 0; i < n->u.pipe.ncmds; i++) {
            if (i)
                sb_puts(out, " | ");
            deparse(n->u.pipe.cmds[i], out);
        }
        return;
    case N_AND:
    case N_OR:
        deparse(n->u.andor.left, out);
        sb_puts(out, n->type == N_AND ? " && " : " || ");
        deparse(n->u.andor.right, out);
        return;
    case N_LIST:
        for (const list_item *it = n->u.list.items; it; it = it->next) {
            deparse(it->node, out);
            if (it->async)
                sb_puts(out, " &");
            else if (it->next)
                sb_putc(out, ';');
            if (it->next)
                sb_putc(out, ' ');
        }
        return;
    case N_SUBSHELL:
        sb_puts(out, "( ");
        deparse(n->u.group.body, out);
        sb_puts(out, " )");
        break;
    case N_BRACE:
        sb_puts(out, "{ ");
        dp_list_body(n->u.group.body, out);
        sb_puts(out, " }");
        break;
    case N_IF: {
        const node *c = n;
        sb_puts(out, "if ");
        for (;;) {
            dp_list_body(c->u.ifn.cond, out);
            sb_puts(out, " then ");
            dp_list_body(c->u.ifn.then_part, out);
            const node *e = c->u.ifn.else_part;
            if (!e)
                break;
            if (e->type == N_IF && !e->redirs) {
                sb_puts(out, " elif ");
                c = e;
                continue;
            }
            sb_puts(out, " else ");
            dp_list_body(e, out);
            break;
        }
        sb_puts(out, " fi");
        break;
    }
    case N_WHILE:
    case N_UNTIL:
        sb_puts(out, n->type == N_WHILE ? "while " : "until ");
        dp_list_body(n->u.loop.cond, out);
        sb_puts(out, " do ");
        dp_list_body(n->u.loop.body, out);
        sb_puts(out, " done");
        break;
    case N_FOR: {
        sb_printf(out, "for %s", n->u.forn.var);
        if (n->u.forn.has_in) {
            sb_puts(out, " in");
            for (const word *w = n->u.forn.words; w; w = w->next) {
                sb_putc(out, ' ');
                sb_puts(out, w->raw ? w->raw : "");
            }
        }
        sb_puts(out, "; do ");
        dp_list_body(n->u.forn.body, out);
        sb_puts(out, " done");
        break;
    }
    case N_CASE:
        sb_printf(out, "case %s in", n->u.casen.subject->raw ? n->u.casen.subject->raw : "");
        for (const case_item *it = n->u.casen.items; it; it = it->next) {
            sb_putc(out, ' ');
            for (const word *w = it->patterns; w; w = w->next) {
                sb_puts(out, w->raw ? w->raw : "");
                if (w->next)
                    sb_putc(out, '|');
            }
            sb_puts(out, ") ");
            if (it->body) {
                deparse(it->body, out);
                sb_putc(out, ' ');
            }
            sb_puts(out, ";;");
        }
        sb_puts(out, " esac");
        break;
    case N_FUNCDEF:
        sb_printf(out, "%s() ", n->u.func.name);
        deparse(n->u.func.body, out);
        return;
    }
    if (n->redirs)
        dp_redirs(n->redirs, out, 0);
}

char *node_text(const node *n)
{
    strbuf b;
    sb_init(&b);
    deparse(n, &b);
    return sb_take(&b);
}
