/*
 * find — 在目录树中查找文件
 *
 * find 从起点目录开始“深度优先”地走遍整棵目录树，对每个文件求一个表达式的值：
 *   find /etc -name '*.conf' -type f     等于   (-name *.conf) 并且 (-type f)
 * 表达式由“测试”（-name、-type……）、“动作”（-print、-exec……）和运算符
 * （! 非、-a 与、-o 或、括号）组成。我们先把它解析成一棵语法树，再对每个文件求值。
 */
#include "skbox.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

SKBOX_APPLET(find, "在目录树中查找文件")

static const char usage[] =
    "用法: find [起点...] [表达式]\n"
    "从每个起点（默认 .）开始递归查找，对每个文件计算表达式，为真时执行动作（默认 -print）。\n\n"
    "全局选项:\n"
    "  -maxdepth 层数   最多进入这么多层（0 表示只看起点本身）\n"
    "  -mindepth 层数   比这浅的文件不做测试和动作\n"
    "  -depth           先处理目录里的内容，再处理目录本身\n"
    "测试:\n"
    "  -name 模式       文件名匹配通配符模式（* ? [...]），记得给模式加引号\n"
    "  -iname 模式      同上，忽略大小写\n"
    "  -path 模式       完整路径匹配通配符模式\n"
    "  -type 类型       f 普通文件  d 目录  l 符号链接  c 字符设备  b 块设备  p 管道  s 套接字\n"
    "  -size [+-]N[ckMG]  大小（默认单位是 512 字节的块；c 字节 k KB M MB G GB）\n"
    "  -empty           空文件或空目录\n"
    "  -newer 文件      修改时间比这个文件新\n"
    "  -mtime [+-]N     修改时间在 N 天前（+N 超过 N 天，-N 不到 N 天）\n"
    "  -mmin [+-]N      修改时间在 N 分钟前\n"
    "  -user 名字/uid   属主  -perm 八进制  权限正好等于  -true  -false\n"
    "动作:\n"
    "  -print           输出路径（换行结尾）       -print0  输出路径（NUL 结尾）\n"
    "  -exec 命令 {} ;  对每个文件执行命令，{} 会换成路径（; 要写成 \\; 或 ';'）\n"
    "  -exec 命令 {} +  把很多文件一起传给命令\n"
    "  -delete          删除文件（隐含 -depth）    -prune   不进入这个目录\n"
    "运算符（优先级从高到低）:\n"
    "  ( 表达式 )   ! 表达式 / -not 表达式   表达式 [-a] 表达式   表达式 -o 表达式\n"
    "例如: find / -name '*.conf'      find . -type f -size +100k      find /tmp -mtime +7 -delete\n";

/* ---------- 表达式语法树 ---------- */

enum node_kind {
    N_AND, N_OR, N_NOT, N_TRUE, N_FALSE,
    N_NAME, N_INAME, N_PATH, N_TYPE, N_SIZE, N_EMPTY, N_NEWER, N_MTIME, N_MMIN, N_USER, N_PERM,
    N_PRINT, N_PRINT0, N_EXEC, N_DELETE, N_PRUNE,
};

typedef struct node {
    enum node_kind kind;
    struct node *a, *b;
    const char *str;
    char type;
    int cmp;                /* -1 小于、0 等于、+1 大于 */
    long long num;
    long long unit;
    struct timespec ts;
    char **argv;            /* -exec 的命令 */
    int argc;
    int plus;               /* -exec ... {} + */
    char **batch;           /* + 模式攒下的路径 */
    int nbatch, capbatch;
} node;

static char **g_tok;
static int g_ntok, g_pos;
static int g_maxdepth = INT_MAX, g_mindepth, g_depth_first, g_has_action, g_status;
static time_t g_now;
static node **g_execs;   /* 所有 -exec + 节点，结束前要把剩下的批处理执行完 */
static int g_nexecs;

static node *new_node(enum node_kind k)
{
    node *n = calloc(1, sizeof *n);
    if (!n) {
        sb_err("内存不足");
        exit(1);
    }
    n->kind = k;
    return n;
}

static const char *next_arg(const char *opt)
{
    if (g_pos >= g_ntok) {
        sb_err("'%s' 缺少参数", opt);
        exit(1);
    }
    return g_tok[g_pos++];
}

/* "+5"、"-5"、"5" → 比较方式和数值 */
static void parse_cmp_num(const char *opt, const char *s, node *n)
{
    n->cmp = 0;
    if (*s == '+') {
        n->cmp = 1;
        s++;
    } else if (*s == '-') {
        n->cmp = -1;
        s++;
    }
    char *end;
    errno = 0;
    n->num = strtoll(s, &end, 10);
    n->unit = 0;
    if (n->kind == N_SIZE) {
        n->unit = 512;
        if (*end == 'c') n->unit = 1, end++;
        else if (*end == 'w') n->unit = 2, end++;
        else if (*end == 'b') n->unit = 512, end++;
        else if (*end == 'k') n->unit = 1024, end++;
        else if (*end == 'M') n->unit = 1024 * 1024, end++;
        else if (*end == 'G') n->unit = 1024LL * 1024 * 1024, end++;
    }
    if (end == s || *end || errno) {
        sb_err("%s 的参数无效: '%s'", opt, s);
        exit(1);
    }
}

static node *parse_or(void);

static node *parse_primary(void)
{
    if (g_pos >= g_ntok) {
        sb_err("表达式不完整");
        exit(1);
    }
    const char *t = g_tok[g_pos++];
    node *n;
    if (strcmp(t, "(") == 0) {
        n = parse_or();
        if (g_pos >= g_ntok || strcmp(g_tok[g_pos], ")") != 0) {
            sb_err("缺少右括号 ')'");
            exit(1);
        }
        g_pos++;
        return n;
    }
    if (strcmp(t, "-name") == 0 || strcmp(t, "-iname") == 0 || strcmp(t, "-path") == 0 ||
        strcmp(t, "-wholename") == 0) {
        n = new_node(t[1] == 'n' ? N_NAME : t[1] == 'i' ? N_INAME : N_PATH);
        n->str = next_arg(t);
        return n;
    }
    if (strcmp(t, "-type") == 0) {
        n = new_node(N_TYPE);
        const char *v = next_arg(t);
        if (strlen(v) != 1 || !strchr("fdlcbps", v[0])) {
            sb_err("-type 的参数无效: '%s'（可用 f d l c b p s）", v);
            exit(1);
        }
        n->type = v[0];
        return n;
    }
    if (strcmp(t, "-size") == 0 || strcmp(t, "-mtime") == 0 || strcmp(t, "-mmin") == 0) {
        n = new_node(t[1] == 's' ? N_SIZE : t[2] == 't' ? N_MTIME : N_MMIN);
        parse_cmp_num(t, next_arg(t), n);
        return n;
    }
    if (strcmp(t, "-empty") == 0)
        return new_node(N_EMPTY);
    if (strcmp(t, "-true") == 0)
        return new_node(N_TRUE);
    if (strcmp(t, "-false") == 0)
        return new_node(N_FALSE);
    if (strcmp(t, "-newer") == 0) {
        n = new_node(N_NEWER);
        const char *f = next_arg(t);
        struct stat st;
        if (stat(f, &st) != 0) {
            sb_perr("-newer '%s'", f);
            exit(1);
        }
        n->ts = st.st_mtim;
        return n;
    }
    if (strcmp(t, "-user") == 0) {
        n = new_node(N_USER);
        n->str = next_arg(t);
        return n;
    }
    if (strcmp(t, "-perm") == 0) {
        n = new_node(N_PERM);
        const char *v = next_arg(t);
        char *end;
        n->num = strtol(v, &end, 8);
        if (*end || end == v) {
            sb_err("-perm 只支持八进制权限，如 755: '%s'", v);
            exit(1);
        }
        return n;
    }
    if (strcmp(t, "-print") == 0 || strcmp(t, "-print0") == 0) {
        g_has_action = 1;
        return new_node(t[6] == '0' ? N_PRINT0 : N_PRINT);
    }
    if (strcmp(t, "-delete") == 0) {
        g_has_action = 1;
        g_depth_first = 1;   /* 必须先删里面的，才能删目录 */
        return new_node(N_DELETE);
    }
    if (strcmp(t, "-prune") == 0)
        return new_node(N_PRUNE);
    if (strcmp(t, "-exec") == 0) {
        g_has_action = 1;
        n = new_node(N_EXEC);
        int start = g_pos;
        while (g_pos < g_ntok && strcmp(g_tok[g_pos], ";") != 0 &&
               !(strcmp(g_tok[g_pos], "+") == 0 && g_pos > start && strcmp(g_tok[g_pos - 1], "{}") == 0))
            g_pos++;
        if (g_pos >= g_ntok || g_pos == start) {
            sb_err("-exec 缺少命令或结尾的 ';'");
            exit(1);
        }
        n->plus = g_tok[g_pos][0] == '+';
        n->argc = g_pos - start;
        n->argv = g_tok + start;
        g_pos++;
        if (n->plus) {
            node **ne = realloc(g_execs, sizeof *ne * (size_t)(g_nexecs + 1));
            if (!ne)
                exit(1);
            g_execs = ne;
            g_execs[g_nexecs++] = n;
        }
        return n;
    }
    if (strcmp(t, "-maxdepth") == 0 || strcmp(t, "-mindepth") == 0) {
        const char *v = next_arg(t);
        long long d;
        if (sb_parse_ll(v, &d) != 0 || d < 0) {
            sb_err("%s 的参数无效: '%s'", t, v);
            exit(1);
        }
        if (t[2] == 'a')
            g_maxdepth = (int)SK_MIN(d, (long long)INT_MAX);
        else
            g_mindepth = (int)SK_MIN(d, (long long)INT_MAX);
        return new_node(N_TRUE);
    }
    if (strcmp(t, "-depth") == 0) {
        g_depth_first = 1;
        return new_node(N_TRUE);
    }
    sb_err("未知的表达式 '%s'", t);
    sb_hint();
    exit(1);
}

static int is_op(const char *t, const char *a, const char *b)
{
    return strcmp(t, a) == 0 || (b && strcmp(t, b) == 0);
}

static node *parse_not(void)
{
    if (g_pos < g_ntok && is_op(g_tok[g_pos], "!", "-not")) {
        g_pos++;
        node *n = new_node(N_NOT);
        n->a = parse_not();
        return n;
    }
    return parse_primary();
}

static node *parse_and(void)
{
    node *left = parse_not();
    for (;;) {
        if (g_pos >= g_ntok || is_op(g_tok[g_pos], "-o", "-or") || strcmp(g_tok[g_pos], ")") == 0)
            return left;
        if (is_op(g_tok[g_pos], "-a", "-and"))
            g_pos++;   /* 两个表达式挨着写就是隐含的 -a */
        node *n = new_node(N_AND);
        n->a = left;
        n->b = parse_not();
        left = n;
    }
}

static node *parse_or(void)
{
    node *left = parse_and();
    while (g_pos < g_ntok && is_op(g_tok[g_pos], "-o", "-or")) {
        g_pos++;
        node *n = new_node(N_OR);
        n->a = left;
        n->b = parse_and();
        left = n;
    }
    return left;
}

/* ---------- 执行命令 ---------- */

static int run_cmd(char **argv)
{
    fflush(stdout);
    /* fork() 复制出一个子进程，子进程用 execvp() 变成要运行的命令；父进程 waitpid() 等它结束 */
    pid_t pid = fork();
    if (pid < 0) {
        sb_perr("fork 失败");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        sb_err("'%s': %s", argv[0], sk_strerror(errno));
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int exec_single(node *n, const char *path)
{
    char **argv = calloc((size_t)n->argc + 1, sizeof *argv);
    char **owned = calloc((size_t)n->argc + 1, sizeof *owned);
    if (!argv || !owned)
        exit(1);
    for (int i = 0; i < n->argc; i++) {
        const char *a = n->argv[i];
        const char *hit = strstr(a, "{}");
        if (!hit) {
            argv[i] = (char *)a;
            continue;
        }
        /* 把参数里所有的 {} 换成路径 */
        size_t len = 0;
        for (const char *p = a; *p;) {
            if (p[0] == '{' && p[1] == '}') {
                len += strlen(path);
                p += 2;
            } else {
                len++;
                p++;
            }
        }
        char *s = malloc(len + 1), *w = s;
        if (!s)
            exit(1);
        for (const char *p = a; *p;) {
            if (p[0] == '{' && p[1] == '}') {
                strcpy(w, path);
                w += strlen(path);
                p += 2;
            } else {
                *w++ = *p++;
            }
        }
        *w = 0;
        argv[i] = owned[i] = s;
    }
    int r = run_cmd(argv);
    for (int i = 0; i < n->argc; i++)
        free(owned[i]);
    free(owned);
    free(argv);
    return r;
}

static void flush_batch(node *n)
{
    if (n->nbatch == 0)
        return;
    /* 命令 参数... {} + ：{} 的位置换成一批路径 */
    int fixed = n->argc - 1;
    char **argv = calloc((size_t)(fixed + n->nbatch + 1), sizeof *argv);
    if (!argv)
        exit(1);
    for (int i = 0; i < fixed; i++)
        argv[i] = n->argv[i];
    for (int i = 0; i < n->nbatch; i++)
        argv[fixed + i] = n->batch[i];
    if (run_cmd(argv) != 0)
        g_status = 1;
    for (int i = 0; i < n->nbatch; i++)
        free(n->batch[i]);
    n->nbatch = 0;
    free(argv);
}

/* ---------- 求值 ---------- */

typedef struct {
    const char *path;      /* 显示/操作用的路径 */
    const char *name;      /* 最后一段 */
    struct stat *st;
    int prune;             /* 被 -prune 标记：不进入 */
} visit;

static int type_matches(char t, mode_t m)
{
    switch (t) {
    case 'f': return S_ISREG(m);
    case 'd': return S_ISDIR(m);
    case 'l': return S_ISLNK(m);
    case 'c': return S_ISCHR(m);
    case 'b': return S_ISBLK(m);
    case 'p': return S_ISFIFO(m);
    case 's': return S_ISSOCK(m);
    }
    return 0;
}

static int cmp_val(int cmp, long long v, long long want)
{
    return cmp > 0 ? v > want : cmp < 0 ? v < want : v == want;
}

static int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *e;
    int empty = 1;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            empty = 0;
            break;
        }
    closedir(d);
    return empty;
}

static int eval(node *n, visit *v)
{
    struct stat *st = v->st;
    switch (n->kind) {
    case N_AND:   return eval(n->a, v) && eval(n->b, v);
    case N_OR:    return eval(n->a, v) || eval(n->b, v);
    case N_NOT:   return !eval(n->a, v);
    case N_TRUE:  return 1;
    case N_FALSE: return 0;
    case N_NAME:  return fnmatch(n->str, v->name, 0) == 0;
    case N_INAME: return fnmatch(n->str, v->name, FNM_CASEFOLD) == 0;
    case N_PATH:  return fnmatch(n->str, v->path, 0) == 0;
    case N_TYPE:  return type_matches(n->type, st->st_mode);
    case N_SIZE: {
        /* 和 GNU 一样，按单位向上取整后再比较：-size -1k 只匹配空文件 */
        long long units = (st->st_size + n->unit - 1) / n->unit;
        return cmp_val(n->cmp, units, n->num);
    }
    case N_EMPTY:
        if (S_ISREG(st->st_mode))
            return st->st_size == 0;
        return S_ISDIR(st->st_mode) && dir_is_empty(v->path);
    case N_NEWER:
        return st->st_mtim.tv_sec > n->ts.tv_sec ||
               (st->st_mtim.tv_sec == n->ts.tv_sec && st->st_mtim.tv_nsec > n->ts.tv_nsec);
    case N_MTIME: {
        long long days = (g_now - st->st_mtim.tv_sec) / 86400;
        return cmp_val(n->cmp, days, n->num);
    }
    case N_MMIN: {
        long long age = g_now - st->st_mtim.tv_sec;
        long long mins = (age + 59) / 60;
        return cmp_val(n->cmp, mins, n->num);
    }
    case N_USER: {
        char buf[64];
        char *end;
        unsigned long id = strtoul(n->str, &end, 10);
        if (*end == 0 && end != n->str)
            return st->st_uid == id;
        return strcmp(sk_user_name(st->st_uid, buf, sizeof buf), n->str) == 0;
    }
    case N_PERM:
        return (long long)(st->st_mode & 07777) == n->num;
    case N_PRINT:
        puts(v->path);
        return 1;
    case N_PRINT0:
        fputs(v->path, stdout);
        putchar(0);
        return 1;
    case N_DELETE: {
        int r = S_ISDIR(st->st_mode) ? rmdir(v->path) : unlink(v->path);
        if (r != 0) {
            sb_perr("无法删除 '%s'", v->path);
            g_status = 1;
            return 0;
        }
        return 1;
    }
    case N_PRUNE:
        v->prune = 1;
        return 1;
    case N_EXEC:
        if (n->plus) {
            if (n->nbatch == n->capbatch) {
                n->capbatch = n->capbatch ? n->capbatch * 2 : 64;
                char **nb = realloc(n->batch, sizeof *nb * (size_t)n->capbatch);
                if (!nb)
                    exit(1);
                n->batch = nb;
            }
            n->batch[n->nbatch++] = strdup(v->path);
            if (n->nbatch >= 512)
                flush_batch(n);
            return 1;
        }
        return exec_single(n, v->path) == 0;
    }
    return 0;
}

/* ---------- 遍历目录树 ---------- */

static node *g_expr;

static void walk(const char *path, const char *name, int depth)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        sb_perr("'%s'", path);
        g_status = 1;
        return;
    }
    visit v = { path, name, &st, 0 };
    int test_here = depth >= g_mindepth;
    if (test_here && !g_depth_first)
        eval(g_expr, &v);
    if (S_ISDIR(st.st_mode) && depth < g_maxdepth && !v.prune) {
        DIR *d = opendir(path);
        if (!d) {
            sb_perr("'%s'", path);
            g_status = 1;
        } else {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                size_t pl = strlen(path);
                char *child = sk_strdup_printf("%s%s%s", path, (pl && path[pl - 1] == '/') ? "" : "/", e->d_name);
                if (!child)
                    continue;
                walk(child, child + strlen(child) - strlen(e->d_name), depth + 1);
                free(child);
            }
            closedir(d);
        }
    }
    if (test_here && g_depth_first)
        eval(g_expr, &v);
}

int applet_find(int argc, char **argv)
{
    if (sb_help(argc, argv, usage))
        return 0;
    int i = 1;
    /* 起点：直到第一个以 - 开头、或者是 ( ! 的参数为止 */
    while (i < argc && argv[i][0] != '-' && strcmp(argv[i], "(") != 0 && strcmp(argv[i], "!") != 0)
        i++;
    int nstart = i - 1;
    g_tok = argv + i;
    g_ntok = argc - i;
    g_now = time(NULL);
    if (g_ntok > 0) {
        g_expr = parse_or();
        if (g_pos < g_ntok) {
            sb_err("多余的 '%s'", g_tok[g_pos]);
            return 1;
        }
    } else {
        g_expr = new_node(N_TRUE);
    }
    if (!g_has_action) {
        /* 没写动作时，等于 ( 表达式 ) -print */
        node *n = new_node(N_AND);
        n->a = g_expr;
        n->b = new_node(N_PRINT);
        g_expr = n;
    }
    if (nstart == 0) {
        walk(".", ".", 0);
    } else {
        for (int k = 1; k <= nstart; k++) {
            char base[4096];
            sb_basename(argv[k], base, sizeof base);   /* 起点的“名字”也参加 -name 测试 */
            walk(argv[k], base, 0);
        }
    }
    for (int k = 0; k < g_nexecs; k++)
        flush_batch(g_execs[k]);
    return g_status;
}
