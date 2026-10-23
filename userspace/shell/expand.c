/*
 * expand.c — 单词展开
 *
 * 命令真正执行之前，每个单词要依次经过（顺序是 POSIX 规定的）：
 *   1. 波浪号展开      ~  ~/x  ~root
 *   2. 参数展开        $x ${x:-默认} ${#x} ${x%.txt} $1 $@ $? ...
 *   3. 命令替换        $(cmd) `cmd`   —— fork 一个子 shell，把它的输出读回来
 *   4. 算术展开        $((1 + 2))
 *   5. 分词            未加引号的展开结果按 IFS（默认空格/Tab/换行）切成多个参数
 *   6. 文件名展开      * ? [abc] 匹配文件（glob），没有匹配就保留原样
 *   7. 去引号          （引号在词法分析时已经去掉了）
 *
 * 关键：只有“未加引号的展开结果”才分词，只有“未加引号的 * ? [”才是通配符。
 * 所以展开时每个字符都带着标志位，记住它从哪里来：
 *   F_QUOTED  在引号里       → 不分词、不当通配符
 *   F_SPLIT   来自未加引号的 $x / $(..) → 要按 IFS 分词
 *   F_BREAK   "$@" 产生的强制参数边界（伪字符）
 *   F_QMARK   这里出现过引号（伪字符）→ 保证 "" 能产生一个空参数
 */
#include "sksh.h"

#include <ctype.h>
#include <fnmatch.h>
#include <glob.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define F_QUOTED 1
#define F_SPLIT  2
#define F_BREAK  4
#define F_QMARK  8

#define XM_ASSIGN  1    /* 赋值的值：冒号后面的 ~ 也展开 */
#define XM_OPERAND 2    /* ${x:-w} 的 w：无引号上下文里字面文本也参与分词 */

int g_cmdsub_count;     /* 执行过的命令替换次数（exec.c 用来决定纯赋值命令的状态码） */
int g_expand_fatal;     /* 刚才的展开错误在非交互 shell 里应该导致退出（${x:?}、set -u） */

typedef struct {
    char          *s;
    unsigned char *f;
    size_t         len, cap;
} xbuf;

static void xb_grow(xbuf *x, size_t extra)
{
    if (x->len + extra + 1 <= x->cap)
        return;
    size_t cap = x->cap ? x->cap : 64;
    while (cap < x->len + extra + 1)
        cap *= 2;
    x->s = xrealloc(x->s, cap);
    x->f = xrealloc(x->f, cap);
    x->cap = cap;
}

static void xb_put(xbuf *x, char c, int fl)
{
    xb_grow(x, 1);
    x->s[x->len] = c;
    x->f[x->len] = (unsigned char)fl;
    x->len++;
}

static void xb_putn(xbuf *x, const char *s, size_t n, int fl)
{
    xb_grow(x, n);
    memcpy(x->s + x->len, s, n);
    memset(x->f + x->len, fl, n);
    x->len += n;
}

static void xb_free(xbuf *x)
{
    free(x->s);
    free(x->f);
    x->s = NULL;
    x->f = NULL;
    x->len = x->cap = 0;
}

static int expand_word_x(word *w, xbuf *x, int mode);

/* ======================= 参数 ======================= */

static void option_flags(strbuf *out)
{
    if (sh.opt_errexit) sb_putc(out, 'e');
    if (sh.opt_noglob) sb_putc(out, 'f');
    if (sh.interactive) sb_putc(out, 'i');
    if (sh.jobctl) sb_putc(out, 'm');
    if (sh.opt_noexec) sb_putc(out, 'n');
    if (sh.opt_nounset) sb_putc(out, 'u');
    if (sh.opt_verbose) sb_putc(out, 'v');
    if (sh.opt_xtrace) sb_putc(out, 'x');
    if (sh.opt_noclobber) sb_putc(out, 'C');
}

int param_special_get(const char *name, strbuf *out)
{
    if (name[0] && !name[1]) {
        switch (name[0]) {
        case '?': sb_printf(out, "%d", sh.last_status); return 1;
        case '$': sb_printf(out, "%d", (int)sh.rootpid); return 1;
        case '!':
            if (sh.last_bg <= 0)
                return 0;
            sb_printf(out, "%d", (int)sh.last_bg);
            return 1;
        case '#': sb_printf(out, "%d", pos_count()); return 1;
        case '-': option_flags(out); return 1;
        case '0': sb_puts(out, sh.arg0 ? sh.arg0 : "sksh"); return 1;
        case '@':
        case '*':
            for (int i = 1; i <= pos_count(); i++) {
                if (i > 1)
                    sb_putc(out, ' ');
                sb_puts(out, pos_get(i));
            }
            return pos_count() > 0;
        default:
            break;
        }
    }
    if (isdigit((unsigned char)name[0])) {
        const char *v = pos_get(atoi(name));
        if (!v)
            return 0;
        sb_puts(out, v);
        return 1;
    }
    /* 动态变量：每次读取都重新计算 */
    if (strcmp(name, "RANDOM") == 0) {
        sb_printf(out, "%d", (int)(random() % 32768));
        return 1;
    }
    if (strcmp(name, "LINENO") == 0) {
        sb_printf(out, "%d", sh.lineno);
        return 1;
    }
    if (strcmp(name, "SECONDS") == 0) {
        sb_printf(out, "%ld", (long)time(NULL) - sh.start_time);
        return 1;
    }
    const char *v = var_get(name);
    if (!v)
        return 0;
    sb_puts(out, v);
    return 1;
}

static int is_special_param(const char *name)
{
    return (name[0] && !name[1] && strchr("?$!#-@*", name[0]));
}

static void expand_positional(xbuf *x, int is_at, int q)
{
    int n = pos_count();
    if (q && !is_at) {          /* "$*"：用 IFS 的第一个字符连接成一个参数 */
        const char *ifs = var_get("IFS");
        char sep = ifs ? ifs[0] : ' ';
        for (int i = 1; i <= n; i++) {
            if (i > 1 && sep)
                xb_put(x, sep, F_QUOTED);
            const char *v = pos_get(i);
            xb_putn(x, v, strlen(v), F_QUOTED);
        }
        xb_put(x, 0, F_QMARK);
        return;
    }
    /* "$@"：每个位置参数各自成为一个参数（中间插入强制边界） */
    for (int i = 1; i <= n; i++) {
        if (i > 1)
            xb_put(x, 0, F_BREAK);
        const char *v = pos_get(i);
        xb_putn(x, v, strlen(v), q ? F_QUOTED : F_SPLIT);
        if (q)
            xb_put(x, 0, F_QMARK);
    }
}

/* ${x#pat} 之类：按字符（不是字节）边界尝试匹配，避免把汉字切成两半 */
static char *remove_pattern(const char *val, const char *pat, enum param_op op)
{
    size_t len = strlen(val);
    char *tmp = xmalloc(len + 1);
    char *res = NULL;
    size_t *bounds = xmalloc(sizeof(size_t) * (len + 2));
    size_t nb = 0;
    for (size_t i = 0;;) {
        bounds[nb++] = i;
        if (i >= len)
            break;
        i += (size_t)utf8_seqlen((unsigned char)val[i]);
        if (i > len)
            i = len;
    }
    switch (op) {
    case PO_RM_SPREFIX:
    case PO_RM_LPREFIX:
        for (size_t k = 0; k < nb && !res; k++) {
            size_t i = op == PO_RM_SPREFIX ? bounds[k] : bounds[nb - 1 - k];
            memcpy(tmp, val, i);
            tmp[i] = 0;
            if (fnmatch(pat, tmp, 0) == 0)
                res = xstrdup(val + i);
        }
        break;
    case PO_RM_SSUFFIX:
    case PO_RM_LSUFFIX:
        for (size_t k = 0; k < nb && !res; k++) {
            size_t i = op == PO_RM_LSUFFIX ? bounds[k] : bounds[nb - 1 - k];
            if (fnmatch(pat, val + i, 0) == 0)
                res = xstrndup(val, i);
        }
        break;
    default:
        break;
    }
    free(bounds);
    free(tmp);
    return res ? res : xstrdup(val);
}

static void put_value(xbuf *x, const char *v, size_t n, int q)
{
    xb_putn(x, v, n, q ? F_QUOTED : F_SPLIT);
    if (q)
        xb_put(x, 0, F_QMARK);
}

static int expand_param(wpart *pt, xbuf *x)
{
    const char *name = pt->text;
    int q = pt->quoted;
    int is_at = strcmp(name, "@") == 0, is_star = strcmp(name, "*") == 0;
    if ((is_at || is_star) && pt->op == PO_NONE) {
        expand_positional(x, is_at, q);
        return 0;
    }
    strbuf val;
    sb_init(&val);
    int set = param_special_get(name, &val);
    int null = !set || val.len == 0;
    int rc = 0;
    switch (pt->op) {
    case PO_NONE:
        if (!set && sh.opt_nounset && !is_special_param(name)) {
            sh_error("%s: 未绑定的变量", name);
            g_expand_fatal = 1;
            rc = -1;
            break;
        }
        put_value(x, sb_str(&val), val.len, q);
        break;
    case PO_LEN: {
        if (!set && sh.opt_nounset && !is_special_param(name)) {
            sh_error("%s: 未绑定的变量", name);
            g_expand_fatal = 1;
            rc = -1;
            break;
        }
        char num[32];
        int n;
        if (is_at || is_star)
            n = snprintf(num, sizeof num, "%d", pos_count());
        else
            n = snprintf(num, sizeof num, "%zu", utf8_count(sb_str(&val)));
        put_value(x, num, (size_t)n, q);
        break;
    }
    case PO_DEFAULT:
        if (pt->colon ? null : !set)
            rc = expand_word_x(pt->arg, x, XM_OPERAND);
        else
            put_value(x, sb_str(&val), val.len, q);
        break;
    case PO_ALT:
        if (pt->colon ? !null : set)
            rc = expand_word_x(pt->arg, x, XM_OPERAND);
        else if (q)
            xb_put(x, 0, F_QMARK);
        break;
    case PO_ASSIGN:
        if (pt->colon ? null : !set) {
            if (!is_name(name)) {
                sh_error("$%s: 不能这样赋值", name);
                rc = -1;
                break;
            }
            char *v = expand_string(pt->arg, 0);
            if (!v || var_set(name, v, 0) < 0) {
                free(v);
                rc = -1;
                break;
            }
            put_value(x, v, strlen(v), q);
            free(v);
        } else {
            put_value(x, sb_str(&val), val.len, q);
        }
        break;
    case PO_ERROR:
        if (pt->colon ? null : !set) {
            char *msg = pt->arg && pt->arg->parts ? expand_string(pt->arg, 0) : NULL;
            sh_error("%s: %s", name, msg && *msg ? msg : (set ? "参数为空" : "参数为空或未设置"));
            free(msg);
            g_expand_fatal = 1;
            rc = -1;
        } else {
            put_value(x, sb_str(&val), val.len, q);
        }
        break;
    default: {      /* 去前缀/后缀 */
        if (!set && sh.opt_nounset && !is_special_param(name)) {
            sh_error("%s: 未绑定的变量", name);
            g_expand_fatal = 1;
            rc = -1;
            break;
        }
        char *pat = expand_pattern(pt->arg);
        if (!pat) {
            rc = -1;
            break;
        }
        char *r = remove_pattern(sb_str(&val), pat, pt->op);
        put_value(x, r, strlen(r), q);
        free(r);
        free(pat);
        break;
    }
    }
    sb_free(&val);
    return rc;
}

/* ======================= 波浪号 ======================= */

char *tilde_lookup(const char *user)
{
    if (!*user) {
        const char *h = var_get("HOME");
        if (h)
            return xstrdup(h);
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir)
            return xstrdup(pw->pw_dir);
        return geteuid() == 0 ? xstrdup("/root") : NULL;
    }
    if (strcmp(user, "+") == 0) {
        const char *v = var_get("PWD");
        return v ? xstrdup(v) : NULL;
    }
    if (strcmp(user, "-") == 0) {
        const char *v = var_get("OLDPWD");
        return v ? xstrdup(v) : NULL;
    }
    struct passwd *pw = getpwnam(user);
    if (pw && pw->pw_dir)
        return xstrdup(pw->pw_dir);
    if (strcmp(user, "root") == 0)      /* SketchOS 可能没有 /etc/passwd */
        return xstrdup("/root");
    return NULL;
}

static void expand_lit(wpart *pt, xbuf *x, int mode, int first)
{
    const char *s = pt->text;
    size_t n = strlen(s);
    if (pt->quoted) {
        xb_putn(x, s, n, F_QUOTED);
        xb_put(x, 0, F_QMARK);
        return;
    }
    int fl = (mode & XM_OPERAND) ? F_SPLIT : 0;
    int at_start = first;
    size_t i = 0;
    while (i < n) {
        if (s[i] == '~' && at_start) {
            size_t j = i + 1;
            while (j < n && s[j] != '/' && !((mode & XM_ASSIGN) && s[j] == ':'))
                j++;
            /* 波浪号前缀里不能有带引号的字符：前缀必须在这一段里结束 */
            if (j < n || !pt->next) {
                char *user = xstrndup(s + i + 1, j - i - 1);
                char *home = tilde_lookup(user);
                free(user);
                if (home) {
                    xb_putn(x, home, strlen(home), F_QUOTED);
                    free(home);
                    i = j;
                    at_start = 0;
                    continue;
                }
            }
        }
        at_start = (mode & XM_ASSIGN) && s[i] == ':';
        xb_put(x, s[i], fl);
        i++;
    }
}

/* ======================= 展开一个单词 ======================= */

static int expand_word_x(word *w, xbuf *x, int mode)
{
    int first = 1;
    for (wpart *pt = w ? w->parts : NULL; pt; pt = pt->next, first = 0) {
        switch (pt->kind) {
        case WP_LIT:
            expand_lit(pt, x, mode, first);
            break;
        case WP_PARAM:
            if (expand_param(pt, x) < 0)
                return -1;
            break;
        case WP_CMDSUB: {
            strbuf out;
            sb_init(&out);
            cmdsub_run(pt->cmd, &out);
            /* 命令替换会去掉输出末尾所有的换行符 */
            while (out.len > 0 && out.s[out.len - 1] == '\n')
                out.len--;
            put_value(x, sb_str(&out), out.len, pt->quoted);
            sb_free(&out);
            if (sh.abort)
                return -1;
            break;
        }
        case WP_ARITH: {
            char *e = expand_string(pt->arg, 0);
            if (!e)
                return -1;
            long long v;
            int r = arith_eval(e, &v);
            free(e);
            if (r < 0)
                return -1;
            char num[32];
            int n = snprintf(num, sizeof num, "%lld", v);
            put_value(x, num, (size_t)n, pt->quoted);
            break;
        }
        }
    }
    return 0;
}

/* ======================= 分词 ======================= */

typedef struct {
    xbuf *v;
    int   n, cap;
} xfields;

static void fields_emit(xfields *fs, xbuf *cur)
{
    if (fs->n == fs->cap) {
        fs->cap = fs->cap ? fs->cap * 2 : 8;
        fs->v = xrealloc(fs->v, sizeof(xbuf) * (size_t)fs->cap);
    }
    fs->v[fs->n++] = *cur;
    memset(cur, 0, sizeof *cur);
}

static int ifs_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

static void split_fields(xbuf *x, xfields *out)
{
    const char *ifs = var_get("IFS");
    if (!ifs)
        ifs = " \t\n";
    xbuf cur = { 0 };
    int has = 0;
    size_t i = 0;
#define IS_IFS(k) ((x->f[k] & F_SPLIT) && x->s[k] && strchr(ifs, x->s[k]))
    while (i < x->len) {
        char c = x->s[i];
        int f = x->f[i];
        if (f & F_BREAK) {
            if (has)
                fields_emit(out, &cur);
            has = 0;
            i++;
            continue;
        }
        if (f & F_QMARK) {
            has = 1;
            i++;
            continue;
        }
        if (IS_IFS(i)) {
            if (ifs_ws(c)) {
                if (has)
                    fields_emit(out, &cur);
                has = 0;
                while (i < x->len && IS_IFS(i) && ifs_ws(x->s[i]))
                    i++;
                if (i < x->len && IS_IFS(i) && !ifs_ws(x->s[i])) {
                    i++;
                    while (i < x->len && IS_IFS(i) && ifs_ws(x->s[i]))
                        i++;
                }
            } else {
                /* 非空白的 IFS 字符（例如 IFS=:）每个都分隔出一个参数，"a::b" → a、空、b */
                fields_emit(out, &cur);
                has = 0;
                i++;
                while (i < x->len && IS_IFS(i) && ifs_ws(x->s[i]))
                    i++;
            }
            continue;
        }
        xb_put(&cur, c, f);
        has = 1;
        i++;
    }
#undef IS_IFS
    if (has)
        fields_emit(out, &cur);
    else
        xb_free(&cur);
}

/* ======================= 文件名展开 ======================= */

static int has_glob(const xbuf *fl)
{
    for (size_t i = 0; i < fl->len; i++) {
        if (fl->f[i] & F_QUOTED)
            continue;
        char c = fl->s[i];
        if (c == '*' || c == '?')
            return 1;
        if (c == '[') {
            for (size_t j = i + 1; j < fl->len; j++)
                if (fl->s[j] == ']' && !(fl->f[j] & F_QUOTED))
                    return 1;
        }
    }
    return 0;
}

static void glob_field(xbuf *fl, strvec *out)
{
    if (!sh.opt_noglob && has_glob(fl)) {
        strbuf pat;
        sb_init(&pat);
        for (size_t i = 0; i < fl->len; i++) {
            char c = fl->s[i];
            if (c == '\\' || ((fl->f[i] & F_QUOTED) && strchr("*?[]", c)))
                sb_putc(&pat, '\\');
            sb_putc(&pat, c);
        }
        glob_t g;
        memset(&g, 0, sizeof g);
        int r = glob(sb_str(&pat), 0, NULL, &g);
        sb_free(&pat);
        if (r == 0 && g.gl_pathc > 0) {
            for (size_t i = 0; i < g.gl_pathc; i++)
                sv_push(out, xstrdup(g.gl_pathv[i]));
            globfree(&g);
            return;
        }
        if (r == 0)
            globfree(&g);
        /* 没有匹配：保留原样（这也是 POSIX 的规定） */
    }
    sv_push(out, xstrndup(fl->s ? fl->s : "", fl->len));
}

/* ======================= 对外接口 ======================= */

int expand_args(word *words, strvec *out)
{
    g_expand_fatal = 0;
    sv_argv(out);
    for (word *w = words; w; w = w->next) {
        xbuf x = { 0 };
        if (expand_word_x(w, &x, 0) < 0) {
            xb_free(&x);
            return -1;
        }
        xfields fs = { 0 };
        split_fields(&x, &fs);
        xb_free(&x);
        for (int i = 0; i < fs.n; i++) {
            glob_field(&fs.v[i], out);
            xb_free(&fs.v[i]);
        }
        free(fs.v);
    }
    return 0;
}

char *expand_string(word *w, int assign)
{
    xbuf x = { 0 };
    if (expand_word_x(w, &x, assign ? XM_ASSIGN : 0) < 0) {
        xb_free(&x);
        return NULL;
    }
    strbuf out;
    sb_init(&out);
    sb_putn(&out, "", 0);
    for (size_t i = 0; i < x.len; i++) {
        if (x.f[i] & F_QMARK)
            continue;
        sb_putc(&out, (x.f[i] & F_BREAK) ? ' ' : x.s[i]);
    }
    xb_free(&x);
    return sb_take(&out);
}

char *expand_pattern(word *w)
{
    xbuf x = { 0 };
    if (expand_word_x(w, &x, 0) < 0) {
        xb_free(&x);
        return NULL;
    }
    strbuf out;
    sb_init(&out);
    sb_putn(&out, "", 0);
    for (size_t i = 0; i < x.len; i++) {
        if (x.f[i] & F_QMARK)
            continue;
        if (x.f[i] & F_BREAK) {
            sb_putc(&out, ' ');
            continue;
        }
        char c = x.s[i];
        if ((x.f[i] & F_QUOTED) && strchr("*?[]\\", c))
            sb_putc(&out, '\\');
        sb_putc(&out, c);
    }
    xb_free(&x);
    return sb_take(&out);
}

/* ======================= 提示符 ======================= */

static void put_escaped(strbuf *b, const char *s)
{
    /* 提示符替换完反斜杠转义后还要做一次 $ 展开，目录名里的 $ ` \ 要先保护起来 */
    for (; *s; s++) {
        if (*s == '$' || *s == '`' || *s == '\\')
            sb_putc(b, '\\');
        sb_putc(b, *s);
    }
}

static void prompt_cwd(strbuf *b, int base_only)
{
    char buf[4096];
    const char *pwd = var_get("PWD");
    if (!pwd || pwd[0] != '/')
        pwd = getcwd(buf, sizeof buf) ? buf : "?";
    const char *home = var_get("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (home && hl > 1 && strncmp(pwd, home, hl) == 0 && (pwd[hl] == 0 || pwd[hl] == '/')) {
        if (base_only && pwd[hl] == '/') {
            const char *slash = strrchr(pwd, '/');
            put_escaped(b, slash + 1);
            return;
        }
        sb_putc(b, '~');
        put_escaped(b, pwd + hl);
        return;
    }
    if (base_only && strcmp(pwd, "/") != 0) {
        const char *slash = strrchr(pwd, '/');
        put_escaped(b, slash ? slash + 1 : pwd);
        return;
    }
    put_escaped(b, pwd);
}

static void strip_escapes(strbuf *b)
{
    size_t o = 0;
    for (size_t i = 0; i < b->len;) {
        if (b->s[i] == 0x1b && i + 1 < b->len && b->s[i + 1] == '[') {
            i += 2;
            while (i < b->len && !((unsigned char)b->s[i] >= 0x40 && (unsigned char)b->s[i] <= 0x7E))
                i++;
            i++;
            continue;
        }
        b->s[o++] = b->s[i++];
    }
    sb_setlen(b, o);
}

char *expand_prompt(const char *ps)
{
    strbuf b;
    sb_init(&b);
    sb_putn(&b, "", 0);
    char host[256];
    for (const char *s = ps; *s; s++) {
        if (*s != '\\' || !s[1]) {
            sb_putc(&b, *s);
            continue;
        }
        s++;
        switch (*s) {
        case 'u': {
            const char *u = var_get("USER");
            struct passwd *pw = NULL;
            if (!u || !*u) {
                pw = getpwuid(geteuid());
                u = pw && pw->pw_name ? pw->pw_name : (geteuid() == 0 ? "root" : "user");
            }
            put_escaped(&b, u);
            break;
        }
        case 'h':
        case 'H':
            if (gethostname(host, sizeof host) != 0)
                sk_strlcpy(host, "sketchos", sizeof host);
            host[sizeof host - 1] = 0;
            if (*s == 'h') {
                char *dot = strchr(host, '.');
                if (dot)
                    *dot = 0;
            }
            put_escaped(&b, host);
            break;
        case 'w': prompt_cwd(&b, 0); break;
        case 'W': prompt_cwd(&b, 1); break;
        case '$': sb_putc(&b, geteuid() == 0 ? '#' : '$'); break;
        case 'n': sb_putc(&b, '\n'); break;
        case 'r': sb_putc(&b, '\r'); break;
        case 'e': sb_putc(&b, 0x1b); break;
        case 'a': sb_putc(&b, 0x07); break;
        case '[': case ']': break;
        case 's': sb_puts(&b, "sksh"); break;
        case 'v': case 'V': sb_puts(&b, SKSH_VERSION); break;
        case 'j': sb_printf(&b, "%d", jobs_stopped_count()); break;
        case 't': case 'T': case 'A': case 'd': {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            char tb[64];
            static const char *const wd[] = { "日", "一", "二", "三", "四", "五", "六" };
            if (*s == 'd')
                snprintf(tb, sizeof tb, "%d月%d日 周%s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
            else if (*s == 'A')
                strftime(tb, sizeof tb, "%H:%M", &tm);
            else
                strftime(tb, sizeof tb, "%H:%M:%S", &tm);
            sb_puts(&b, tb);
            break;
        }
        case '\\': sb_puts(&b, "\\\\"); break;
        default:
            if (*s >= '0' && *s <= '7') {
                int v = 0, n = 0;
                while (n < 3 && *s >= '0' && *s <= '7') {
                    v = v * 8 + (*s - '0');
                    s++;
                    n++;
                }
                s--;
                sb_putc(&b, v);
            } else {
                sb_putc(&b, '\\');
                sb_putc(&b, *s);
            }
        }
    }
    if (strpbrk(sb_str(&b), "$`\\")) {
        arena *ar = arena_new();
        word *w = parse_heredoc_text(sb_str(&b), ar);
        if (w) {
            int save = sh.last_status;
            int save_abort = sh.abort;
            char *r = expand_string(w, 0);
            sh.last_status = save;
            sh.abort = save_abort;
            if (r) {
                sb_free(&b);
                sb_puts(&b, r);
                free(r);
            }
        }
        arena_unref(ar);
    }
    const char *term = var_get("TERM");
    if (!isatty(STDERR_FILENO) || (term && strcmp(term, "dumb") == 0))
        strip_escapes(&b);
    return sb_take(&b);
}
