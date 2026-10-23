/*
 * builtins.c — 内建命令表，以及 cd、export、set、alias、type 等内建命令
 *
 * 为什么有些命令必须“内建”？因为它们要改变 shell 进程自己的状态：
 *   cd 改当前目录、export/unset 改变量、exit 退出 shell、source 在当前 shell 里执行脚本……
 * 如果它们是独立的程序，shell 会 fork 一个子进程去运行，改变的只是子进程，毫无作用。
 * 另外一些（echo、test、printf、true）内建只是为了快：省掉一次 fork+exec。
 */
#include "sksh.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <unistd.h>


static int bi_true(int argc, char **argv) { return 0; }
static int bi_false(int argc, char **argv) { return 1; }

/* ======================= cd / pwd ======================= */

/* 规范化绝对路径：去掉 . 和 ..（逻辑路径，不解析符号链接，和 bash 的 cd 默认行为一致） */
static char *normalize_path(const char *path)
{
    strbuf out;
    sb_init(&out);
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len == 1 && s[0] == '.')
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            size_t l = out.len;
            while (l > 0 && out.s[l - 1] != '/')
                l--;
            if (l > 0)
                l--;
            sb_setlen(&out, l);
            continue;
        }
        sb_putc(&out, '/');
        sb_putn(&out, s, len);
    }
    if (out.len == 0)
        sb_putc(&out, '/');
    return sb_take(&out);
}

static char *current_dir(void)
{
    const char *pwd = var_get("PWD");
    struct stat a, b;
    if (pwd && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino)
        return xstrdup(pwd);
    char *cwd = getcwd(NULL, 0);
    return cwd ? cwd : NULL;
}

static int bi_cd(int argc, char **argv)
{
    int i = 1, physical = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-P") == 0)
            physical = 1;
        else if (strcmp(argv[i], "-L") == 0)
            physical = 0;
        else {
            sh_error("cd: %s: 无效的选项\n用法: cd [-L|-P] [目录]", argv[i]);
            return 2;
        }
    }
    const char *target;
    int print = 0;
    if (i >= argc) {
        target = var_get("HOME");
        if (!target || !*target) {
            sh_error("cd: 没有设置 HOME");
            return 1;
        }
    } else if (i + 1 < argc) {
        sh_error("cd: 参数太多");
        return 1;
    } else if (strcmp(argv[i], "-") == 0) {
        target = var_get("OLDPWD");
        if (!target || !*target) {
            sh_error("cd: 没有设置 OLDPWD");
            return 1;
        }
        print = 1;
    } else {
        target = argv[i];
    }

    /* CDPATH：cd 一个相对目录名时，先在 CDPATH 列出的目录里找 */
    char *via_cdpath = NULL;
    const char *cdpath = var_get("CDPATH");
    if (cdpath && *cdpath && target[0] != '/' && strncmp(target, "./", 2) != 0 &&
        strncmp(target, "../", 3) != 0 && strcmp(target, ".") != 0 && strcmp(target, "..") != 0) {
        const char *p = cdpath;
        for (;;) {
            const char *colon = strchr(p, ':');
            size_t dl = colon ? (size_t)(colon - p) : strlen(p);
            if (dl > 0) {
                char *cand = sk_strdup_printf("%.*s/%s", (int)dl, p, target);
                struct stat st;
                if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
                    via_cdpath = cand;
                    print = 1;
                    break;
                }
                free(cand);
            }
            if (!colon)
                break;
            p = colon + 1;
        }
    }
    const char *dest = via_cdpath ? via_cdpath : target;

    char *old = current_dir();
    char *newpwd = NULL;
    if (!physical) {
        char *joined;
        if (dest[0] == '/')
            joined = xstrdup(dest);
        else
            joined = sk_strdup_printf("%s/%s", old ? old : ".", dest);
        newpwd = normalize_path(joined);
        free(joined);
        if (chdir(newpwd) < 0) {
            free(newpwd);
            newpwd = NULL;
        }
    }
    if (!newpwd) {
        if (chdir(dest) < 0) {
            sh_error("cd: %s: %s", target, sh_strerror(errno));
            free(old);
            free(via_cdpath);
            return 1;
        }
        newpwd = getcwd(NULL, 0);
        if (!newpwd)
            newpwd = xstrdup(dest);
    }
    var_set("OLDPWD", old ? old : newpwd, V_EXPORT);
    var_set("PWD", newpwd, V_EXPORT);
    if (print) {
        printf("%s\n", newpwd);
    }
    free(newpwd);
    free(old);
    free(via_cdpath);
    return 0;
}

static int bi_pwd(int argc, char **argv)
{
    int physical = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-P") == 0)
            physical = 1;
        else if (strcmp(argv[i], "-L") == 0)
            physical = 0;
        else {
            sh_error("pwd: %s: 无效的选项", argv[i]);
            return 2;
        }
    }
    char *d = physical ? getcwd(NULL, 0) : current_dir();
    if (!d) {
        sh_error("pwd: %s", sh_strerror(errno));
        return 1;
    }
    printf("%s\n", d);
    free(d);
    return 0;
}

/* ======================= exit / logout ======================= */

static int parse_int(const char *s, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (!*s || *end || errno)
        return -1;
    *out = v;
    return 0;
}

static int bi_exit(int argc, char **argv)
{
    int st = sh.last_status;
    if (argc > 2) {
        sh_error("exit: 参数太多");
        return 1;
    }
    if (argc == 2) {
        long v;
        if (parse_int(argv[1], &v) < 0) {
            sh_error("exit: %s: 需要数字参数", argv[1]);
            v = 2;
        }
        st = (int)(v & 0xff);
    }
    if (sh.interactive && !sh.subshell) {
        if (jobs_stopped_count() > 0 && sh.exit_warned == 0) {
            fputs("还有已停止的作业。（再输入一次 exit 就退出）\n", stderr);
            sh.exit_warned = 2;
            return 1;
        }
        fputs("exit\n", stderr);
    }
    exit_shell(st);
}

static int bi_logout(int argc, char **argv)
{
    if (!sh.login) {
        sh_error("logout: 不是登录 shell，请使用 exit");
        return 1;
    }
    return bi_exit(argc, argv);
}

/* ======================= 变量相关 ======================= */

static void print_decl(const char *name, const char *value, int flags, void *ud)
{
    int want = *(int *)ud;
    if (!(flags & want))
        return;
    strbuf b;
    sb_init(&b);
    sb_puts(&b, want == V_EXPORT ? "export " : "readonly ");
    sb_puts(&b, name);
    if (value) {
        sb_putc(&b, '=');
        shell_quote(&b, value);
    }
    puts(sb_str(&b));
    sb_free(&b);
}

static int declare_common(int argc, char **argv, int flag, const char *cmd)
{
    int i = 1, remove = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-p") == 0)
            continue;
        if (strcmp(argv[i], "-n") == 0 && flag == V_EXPORT) {
            remove = 1;
            continue;
        }
        sh_error("%s: %s: 无效的选项", cmd, argv[i]);
        return 2;
    }
    if (i >= argc) {
        var_foreach_sorted(print_decl, &flag);
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        size_t nl = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!is_name_n(argv[i], nl)) {
            sh_error("%s: '%s': 不是合法的变量名", cmd, argv[i]);
            st = 1;
            continue;
        }
        char *name = xstrndup(argv[i], nl);
        if (remove) {
            var_clear_flags(name, flag);
        } else if (eq) {
            if (var_set(name, eq + 1, flag) < 0)
                st = 1;
        } else {
            var_add_flags(name, flag);
        }
        free(name);
    }
    return st;
}

static int bi_export(int argc, char **argv)
{
    return declare_common(argc, argv, V_EXPORT, "export");
}

static int bi_readonly(int argc, char **argv)
{
    return declare_common(argc, argv, V_READONLY, "readonly");
}

static int bi_unset(int argc, char **argv)
{
    int i = 1, mode = 0;    /* 0 自动，1 只变量，2 只函数 */
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-v") == 0)
            mode = 1;
        else if (strcmp(argv[i], "-f") == 0)
            mode = 2;
        else {
            sh_error("unset: %s: 无效的选项", argv[i]);
            return 2;
        }
    }
    int st = 0;
    for (; i < argc; i++) {
        if (mode != 2 && !is_name(argv[i])) {
            sh_error("unset: '%s': 不是合法的变量名", argv[i]);
            st = 1;
            continue;
        }
        if (mode == 2) {
            func_unset(argv[i]);
        } else if (var_flags(argv[i]) >= 0 || mode == 1) {
            if (var_unset(argv[i]) < 0)
                st = 1;
        } else {
            func_unset(argv[i]);
        }
    }
    return st;
}

static int bi_local(int argc, char **argv)
{
    if (!var_in_function()) {
        sh_error("local: 只能在函数里使用");
        return 1;
    }
    int st = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        size_t nl = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!is_name_n(argv[i], nl)) {
            sh_error("local: '%s': 不是合法的变量名", argv[i]);
            st = 1;
            continue;
        }
        char *name = xstrndup(argv[i], nl);
        var_make_local(name);
        if (eq) {
            if (var_set(name, eq + 1, 0) < 0)
                st = 1;
        } else if (!(var_flags(name) & V_READONLY)) {
            var_unset(name);
        }
        free(name);
    }
    return st;
}

static void print_var(const char *name, const char *value, int flags, void *ud)
{
    if (!value)
        return;
    strbuf b;
    sb_init(&b);
    sb_puts(&b, name);
    sb_putc(&b, '=');
    shell_quote(&b, value);
    puts(sb_str(&b));
    sb_free(&b);
}

static const struct {
    const char *name;
    char letter;
    int *flag;
} options[] = {
    { "errexit",   'e', &sh.opt_errexit },
    { "noglob",    'f', &sh.opt_noglob },
    { "monitor",   'm', &sh.jobctl },
    { "noexec",    'n', &sh.opt_noexec },
    { "nounset",   'u', &sh.opt_nounset },
    { "verbose",   'v', &sh.opt_verbose },
    { "xtrace",    'x', &sh.opt_xtrace },
    { "noclobber", 'C', &sh.opt_noclobber },
    { "pipefail",  0,   &sh.opt_pipefail },
    { "ignoreeof", 0,   &sh.opt_ignoreeof },
};

static int set_option(const char *name, char letter, int on)
{
    for (size_t k = 0; k < SK_ARRAY_LEN(options); k++) {
        if ((name && strcmp(name, options[k].name) == 0) || (!name && letter && letter == options[k].letter)) {
            if (options[k].flag == &sh.jobctl) {
                if (on && !sh.jobctl) {
                    jobs_init();
                    signals_init();
                } else if (!on) {
                    sh.jobctl = 0;
                }
                return 0;
            }
            *options[k].flag = on;
            return 0;
        }
    }
    /* 兼容 bash 的一些无害选项 */
    if (name && (strcmp(name, "emacs") == 0 || strcmp(name, "vi") == 0 || strcmp(name, "histexpand") == 0))
        return 0;
    if (!name && (letter == 'h' || letter == 'B' || letter == 'H'))
        return 0;
    return -1;
}

static int bi_set(int argc, char **argv)
{
    if (argc == 1) {
        var_foreach_sorted(print_var, NULL);
        return 0;
    }
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            pos_set(argc - i, argv + i);
            return 0;
        }
        if (strcmp(a, "-") == 0) {
            sh.opt_xtrace = sh.opt_verbose = 0;
            i++;
            pos_set(argc - i, argv + i);
            return 0;
        }
        if ((a[0] != '-' && a[0] != '+') || !a[1])
            break;
        int on = a[0] == '-';
        if (strcmp(a + 1, "o") == 0) {
            if (i + 1 >= argc) {
                for (size_t k = 0; k < SK_ARRAY_LEN(options); k++) {
                    if (on)
                        printf("%-15s %s\n", options[k].name, *options[k].flag ? "on" : "off");
                    else
                        printf("set %co %s\n", *options[k].flag ? '-' : '+', options[k].name);
                }
                return 0;
            }
            if (set_option(argv[++i], 0, on) < 0) {
                sh_error("set: %s: 无效的选项名", argv[i]);
                return 2;
            }
            continue;
        }
        for (const char *o = a + 1; *o; o++) {
            if (set_option(NULL, *o, on) < 0) {
                sh_error("set: %c%c: 无效的选项", a[0], *o);
                return 2;
            }
        }
    }
    if (i < argc)
        pos_set(argc - i, argv + i);
    return 0;
}

static int bi_shift(int argc, char **argv)
{
    long n = 1;
    if (argc > 1 && parse_int(argv[1], &n) < 0) {
        sh_error("shift: %s: 需要数字参数", argv[1]);
        return 2;
    }
    if (n < 0 || pos_shift((int)n) < 0) {
        sh_error("shift: 移位次数超出范围（当前只有 %d 个参数）", pos_count());
        return 1;
    }
    return 0;
}

/* ======================= 别名 ======================= */

static void print_alias(const char *name, const char *value, int flags, void *ud)
{
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "alias %s=", name);
    shell_quote(&b, value);
    puts(sb_str(&b));
    sb_free(&b);
}

static int bi_alias(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "-p") == 0)
        i++;
    if (i >= argc) {
        alias_foreach_sorted(print_alias, NULL);
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            char *name = xstrndup(argv[i], (size_t)(eq - argv[i]));
            if (!*name || strpbrk(name, " \t\n/$`'\"\\;|&<>()")) {
                sh_error("alias: '%s': 不是合法的别名", name);
                st = 1;
            } else {
                alias_set(name, eq + 1);
            }
            free(name);
        } else {
            const char *v = alias_get(argv[i]);
            if (v)
                print_alias(argv[i], v, 0, NULL);
            else {
                sh_error("alias: %s: 未找到", argv[i]);
                st = 1;
            }
        }
    }
    return st;
}

static int bi_unalias(int argc, char **argv)
{
    if (argc < 2) {
        sh_error("unalias: 用法: unalias [-a] 名字...");
        return 2;
    }
    if (strcmp(argv[1], "-a") == 0) {
        alias_clear();
        return 0;
    }
    int st = 0;
    for (int i = 1; i < argc; i++)
        if (alias_unset(argv[i]) < 0) {
            sh_error("unalias: %s: 未找到", argv[i]);
            st = 1;
        }
    return st;
}

/* ======================= source / eval ======================= */

static int bi_source(int argc, char **argv)
{
    if (argc < 2) {
        sh_error("%s: 需要文件名参数\n用法: %s 文件 [参数...]", argv[0], argv[0]);
        return 2;
    }
    const char *name = argv[1];
    char *path = NULL;
    if (!strchr(name, '/')) {
        /* 和 bash 一样：先在 PATH 里找，找不到再用当前目录 */
        const char *pp = var_get("PATH");
        if (pp) {
            const char *p = pp;
            for (;;) {
                const char *colon = strchr(p, ':');
                size_t dl = colon ? (size_t)(colon - p) : strlen(p);
                char *cand = sk_strdup_printf("%.*s/%s", (int)(dl ? dl : 1), dl ? p : ".", name);
                struct stat st;
                if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) && access(cand, R_OK) == 0) {
                    path = cand;
                    break;
                }
                free(cand);
                if (!colon)
                    break;
                p = colon + 1;
            }
        }
    }
    int st = exec_file(path ? path : name, argc - 2, argv + 2, 1);
    free(path);
    return st;
}

static int bi_eval(int argc, char **argv)
{
    strbuf b;
    sb_init(&b);
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            sb_putc(&b, ' ');
        sb_puts(&b, argv[i]);
    }
    int st = b.len ? exec_string(sb_str(&b), "eval") : 0;
    sb_free(&b);
    return st;
}

/* ======================= 控制流 ======================= */

static int bi_return(int argc, char **argv)
{
    if (sh.func_depth == 0 && sh.source_depth == 0) {
        sh_error("return: 只能在函数或者被 source 的脚本里使用");
        return 1;
    }
    long n = sh.last_status;
    if (argc > 1 && parse_int(argv[1], &n) < 0) {
        sh_error("return: %s: 需要数字参数", argv[1]);
        n = 2;
    }
    sh.returning = 1;
    sh.return_status = (int)(n & 0xff);
    return sh.return_status;
}

static int loop_count(int argc, char **argv, const char *cmd, long *n)
{
    *n = 1;
    if (argc > 1 && (parse_int(argv[1], n) < 0 || *n < 1)) {
        sh_error("%s: %s: 循环层数必须是 >= 1 的整数", cmd, argv[1]);
        return -1;
    }
    if (sh.loop_depth == 0) {
        sh_error("%s: 只能在 for、while、until 循环里使用", cmd);
        return -1;
    }
    if (*n > sh.loop_depth)
        *n = sh.loop_depth;
    return 0;
}

static int bi_break(int argc, char **argv)
{
    long n;
    if (loop_count(argc, argv, "break", &n) < 0)
        return 1;
    sh.break_n = (int)n;
    return 0;
}

static int bi_continue(int argc, char **argv)
{
    long n;
    if (loop_count(argc, argv, "continue", &n) < 0)
        return 1;
    sh.continue_n = (int)n;
    return 0;
}

/* ======================= type ======================= */

static int describe(const char *name, int mode)     /* mode: 0 详细，'t' 类型词，'p' 路径，'v' command -v */
{
    const char *a = alias_get(name);
    if (a) {
        if (mode == 't') puts("alias");
        else if (mode == 'v') {
            strbuf b;
            sb_init(&b);
            sb_printf(&b, "alias %s=", name);
            shell_quote(&b, a);
            puts(sb_str(&b));
            sb_free(&b);
        } else if (mode == 0) printf("%s 是“%s”的别名\n", name, a);
        if (mode != 'p')
            return 0;
    }
    if (is_keyword(name) && mode != 'p') {
        if (mode == 't') puts("keyword");
        else if (mode == 'v') puts(name);
        else printf("%s 是 shell 关键字\n", name);
        return 0;
    }
    shfunc *f = func_get(name);
    if (f && mode != 'p') {
        if (mode == 't') puts("function");
        else if (mode == 'v') puts(name);
        else {
            char *body = node_text(f->body);
            printf("%s 是函数\n%s () %s\n", name, name, body);
            free(body);
        }
        return 0;
    }
    if (builtin_find(name) && mode != 'p') {
        if (mode == 't') puts("builtin");
        else if (mode == 'v') puts(name);
        else printf("%s 是 shell 内建命令\n", name);
        return 0;
    }
    strbuf path;
    sb_init(&path);
    int found = 0;
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            sb_puts(&path, name);
            found = 1;
        }
    } else {
        found = find_in_path(name, &path);
    }
    if (found) {
        if (mode == 't') puts("file");
        else if (mode == 'p' || mode == 'v') puts(sb_str(&path));
        else printf("%s 是 %s\n", name, sb_str(&path));
    }
    sb_free(&path);
    return found ? 0 : 1;
}

static int bi_type(int argc, char **argv)
{
    int mode = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-t") == 0) mode = 't';
        else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "-P") == 0) mode = 'p';
        else if (strcmp(argv[i], "-a") == 0) continue;
        else {
            sh_error("type: %s: 无效的选项\n用法: type [-tp] 名字...", argv[i]);
            return 2;
        }
    }
    int st = 0;
    for (; i < argc; i++) {
        if (describe(argv[i], mode) != 0) {
            if (mode == 0)
                sh_error("type: %s: 未找到", argv[i]);
            st = 1;
        }
    }
    return st;
}

int bi_command_describe(int argc, char **argv, int verbose)
{
    int st = 0;
    for (int i = 0; i < argc; i++) {
        if (describe(argv[i], verbose ? 0 : 'v') != 0) {
            if (verbose)
                sh_error("command: %s: 未找到", argv[i]);
            st = 1;
        }
    }
    return st;
}

/* ======================= umask / times ======================= */

static int bi_umask(int argc, char **argv)
{
    int i = 1, symbolic = 0;
    if (i < argc && strcmp(argv[i], "-S") == 0) {
        symbolic = 1;
        i++;
    }
    mode_t cur = umask(0);
    umask(cur);
    if (i >= argc) {
        if (symbolic) {
            mode_t perm = ~cur & 0777;
            char out[32];
            int k = 0;
            const char *who = "ugo";
            for (int w = 0; w < 3; w++) {
                int shift = 6 - 3 * w;
                if (w)
                    out[k++] = ',';
                out[k++] = who[w];
                out[k++] = '=';
                if (perm & (4 << shift)) out[k++] = 'r';
                if (perm & (2 << shift)) out[k++] = 'w';
                if (perm & (1 << shift)) out[k++] = 'x';
            }
            out[k] = 0;
            puts(out);
        } else {
            printf("%04o\n", (unsigned)cur);
        }
        return 0;
    }
    const char *m = argv[i];
    if (m[0] >= '0' && m[0] <= '7') {
        char *end;
        long v = strtol(m, &end, 8);
        if (*end || v < 0 || v > 0777) {
            sh_error("umask: %s: 八进制数超出范围", m);
            return 1;
        }
        umask((mode_t)v);
        return 0;
    }
    /* 符号模式：u=rwx,g=rx,o= 或 go-w */
    mode_t perm = ~cur & 0777;
    const char *p = m;
    while (*p) {
        mode_t who = 0;
        for (; *p && strchr("ugoa", *p); p++)
            who |= *p == 'u' ? 0700 : *p == 'g' ? 0070 : *p == 'o' ? 0007 : 0777;
        if (!who)
            who = 0777;
        if (!*p || !strchr("=+-", *p)) {
            sh_error("umask: %s: 无效的模式", m);
            return 1;
        }
        char op = *p++;
        mode_t bits = 0;
        for (; *p && strchr("rwx", *p); p++)
            bits |= *p == 'r' ? 0444 : *p == 'w' ? 0222 : 0111;
        bits &= who;
        if (op == '=')
            perm = (perm & ~who) | bits;
        else if (op == '+')
            perm |= bits;
        else
            perm &= ~bits;
        if (*p == ',')
            p++;
        else if (*p) {
            sh_error("umask: %s: 无效的模式", m);
            return 1;
        }
    }
    umask(~perm & 0777);
    return 0;
}

static void print_time(clock_t t, long tck)
{
    double s = (double)t / (double)tck;
    int mins = (int)(s / 60);
    printf("%dm%.3fs", mins, s - mins * 60);
}

static int bi_times(int argc, char **argv)
{
    struct tms t;
    times(&t);
    long tck = sk_clk_tck();
    print_time(t.tms_utime, tck);
    putchar(' ');
    print_time(t.tms_stime, tck);
    putchar('\n');
    print_time(t.tms_cutime, tck);
    putchar(' ');
    print_time(t.tms_cstime, tck);
    putchar('\n');
    return 0;
}

/* ======================= 内建命令表 ======================= */

static const builtin_def table[] = {
    { ".",        bi_source,    1 },
    { ":",        bi_true,      1 },
    { "[",        bi_test,      0 },
    { "alias",    bi_alias,     0 },
    { "bg",       bi_bg,        0 },
    { "break",    bi_break,     1 },
    { "cd",       bi_cd,        0 },
    { "command",  bi_command,   0 },
    { "continue", bi_continue,  1 },
    { "echo",     bi_echo,      0 },
    { "eval",     bi_eval,      1 },
    { "exec",     bi_exec_args, 1 },
    { "exit",     bi_exit,      1 },
    { "export",   bi_export,    1 },
    { "false",    bi_false,     0 },
    { "fg",       bi_fg,        0 },
    { "help",     bi_help,      0 },
    { "history",  bi_history,   0 },
    { "jobs",     bi_jobs,      0 },
    { "kill",     bi_kill,      0 },
    { "local",    bi_local,     0 },
    { "logout",   bi_logout,    0 },
    { "printf",   bi_printf,    0 },
    { "pwd",      bi_pwd,       0 },
    { "read",     bi_read,      0 },
    { "readonly", bi_readonly,  1 },
    { "return",   bi_return,    1 },
    { "set",      bi_set,       1 },
    { "shift",    bi_shift,     1 },
    { "source",   bi_source,    0 },
    { "test",     bi_test,      0 },
    { "times",    bi_times,     1 },
    { "trap",     bi_trap,      1 },
    { "true",     bi_true,      0 },
    { "type",     bi_type,      0 },
    { "umask",    bi_umask,     0 },
    { "unalias",  bi_unalias,   0 },
    { "unset",    bi_unset,     1 },
    { "wait",     bi_wait,      0 },
};

const builtin_def *builtin_find(const char *name)
{
    size_t lo = 0, hi = SK_ARRAY_LEN(table);
    while (lo < hi) {           /* 表按名字排好序了：二分查找 */
        size_t mid = (lo + hi) / 2;
        int c = strcmp(name, table[mid].name);
        if (c == 0)
            return &table[mid];
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

int builtin_count(void)
{
    return (int)SK_ARRAY_LEN(table);
}

const char *builtin_name(int i)
{
    return i >= 0 && i < (int)SK_ARRAY_LEN(table) ? table[i].name : NULL;
}
