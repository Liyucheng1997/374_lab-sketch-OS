/*
 * exec.c — 执行器：遍历语法树，真正去运行命令
 *
 * 运行一个外部程序的经典三步：
 *     pid = fork();          复制出一个一模一样的子进程（写时复制，很便宜）
 *     子进程: execve(...)    把自己“换成”目标程序：代码、数据、栈全部替换，
 *                           但打开的文件描述符、当前目录、进程组等会保留下来
 *     父进程: waitpid(pid)   等子进程结束，取回它的退出状态 ($?)
 * 为什么要分 fork 和 exec 两步？因为在 fork 之后、exec 之前，子进程可以先“布置环境”：
 * 用 dup2 把 stdout 换成文件（重定向）、换成管道（管道线）、加入新的进程组（作业控制）、
 * 恢复默认的信号处理……然后再 exec。新程序完全不需要知道这些。
 *
 * 内建命令（cd、export……）必须在 shell 进程自己里执行：如果 fork 出子进程去 cd，
 * 改变的只是子进程的当前目录，子进程一退出就没了。
 */
#include "sksh.h"

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

shell sh;
arena *g_cur_arena;     /* 正在执行的命令所在的内存池（函数定义要引用它） */

/* ======================= 退出 ======================= */

void exit_shell(int status)
{
    trap_exit_run();
    fflush(stdout);
    fflush(stderr);
    if (sh.subshell)
        _exit(status & 0xff);
    jobs_restore_terminal();
    exit(status & 0xff);
}

void exit_child(int status)
{
    trap_exit_run();
    fflush(stdout);
    fflush(stderr);
    _exit(status & 0xff);
}

/* fork 之前一定要清空 stdio 缓冲区，否则缓冲区里还没写出去的内容会被复制一份，
 * 父子进程各输出一次。 */
pid_t sh_fork(void)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        sh_error("fork 失败: %s", sh_strerror(errno));
    else if (pid == 0)
        sh.subshell = 1;
    return pid;
}

int sh_should_stop(void)
{
    if (g_sigint && sh.interactive) {
        g_sigint = 0;
        sh.abort = 1;
        sh.last_status = 130;
    }
    if (g_trap_pending)
        traps_run_pending();
    return sh.abort || sh.break_n || sh.continue_n || sh.returning;
}

/* ======================= 重定向 =======================
 * 文件描述符就是进程“打开文件表”的下标：0 标准输入、1 标准输出、2 标准错误。
 * 重定向 > file 的本质是：open(file) 得到新描述符 n，再 dup2(n, 1) 让 1 号也指向这个文件。
 * 内建命令在 shell 进程里执行，所以要先把原来的 1 号备份(dup)起来，执行完再还原。 */

typedef struct saved_fd {
    int              fd;
    int              saved;     /* -1：原来是关闭的 */
    struct saved_fd *next;
} saved_fd;

static int save_fd(saved_fd **list, int fd)
{
    for (saved_fd *s = *list; s; s = s->next)
        if (s->fd == fd)
            return 0;
    saved_fd *s = xcalloc(1, sizeof *s);
    s->fd = fd;
    s->saved = fcntl(fd, F_DUPFD_CLOEXEC, SH_FD_BASE);
    if (s->saved < 0 && errno != EBADF) {
        sh_error("无法保存文件描述符 %d: %s", fd, sh_strerror(errno));
        free(s);
        return -1;
    }
    s->next = *list;
    *list = s;
    return 0;
}

static void restore_fds(saved_fd *list)
{
    fflush(stdout);
    fflush(stderr);
    while (list) {
        saved_fd *n = list->next;
        if (list->saved >= 0) {
            dup2(list->saved, list->fd);
            close(list->saved);
        } else {
            close(list->fd);
        }
        free(list);
        list = n;
    }
}

static char *redir_path(redir *r)
{
    strvec v;
    sv_init(&v);
    word one = *r->target;
    one.next = NULL;
    if (expand_args(&one, &v) < 0) {
        sv_free(&v);
        return NULL;
    }
    if (v.n != 1) {
        sh_error("%s: 模糊的重定向", r->target->raw ? r->target->raw : "?");
        sv_free(&v);
        return NULL;
    }
    char *s = v.v[0];
    v.v[0] = NULL;
    v.n = 0;
    sv_free(&v);
    return s;
}

/* here-doc 的正文放进一个内存文件(memfd)里，再把它接到标准输入上 */
static int heredoc_fd(redir *r)
{
    char *body;
    if (r->herestring) {
        word one = *r->target;
        one.next = NULL;
        char *s = expand_string(&one, 0);
        if (!s)
            return -1;
        body = sk_strdup_printf("%s\n", s);
        free(s);
    } else if (!r->body) {
        body = xstrdup("");
    } else if (r->body_quoted) {
        body = xstrdup(r->body->parts ? r->body->parts->text : "");
    } else {
        body = expand_string(r->body, 0);
        if (!body)
            return -1;
    }
    size_t len = strlen(body);
    int fd = memfd_create("sksh-heredoc", MFD_CLOEXEC);
    if (fd >= 0) {
        if (full_write(fd, body, len) < 0 || lseek(fd, 0, SEEK_SET) < 0) {
            sh_error("here-document: %s", sh_strerror(errno));
            close(fd);
            fd = -1;
        }
        free(body);
        return fd;
    }
    /* 退路：管道。正文比管道缓冲区大时交给一个子进程去写，否则会因为管道写满而卡死 */
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) {
        sh_error("here-document: %s", sh_strerror(errno));
        free(body);
        return -1;
    }
    if (len <= 4096) {
        full_write(p[1], body, len);
        close(p[1]);
    } else {
        pid_t pid = sh_fork();
        if (pid == 0) {
            close(p[0]);
            full_write(p[1], body, len);
            _exit(0);
        }
        close(p[1]);
    }
    free(body);
    return p[0];
}

static int apply_redirs(redir *list, saved_fd **save)
{
    fflush(stdout);
    fflush(stderr);
    for (redir *r = list; r; r = r->next) {
        int fd = r->fd;
        if (fd >= SH_FD_BASE) {
            sh_error("%d: 文件描述符超出范围（最大 %d）", fd, SH_FD_BASE - 1);
            return -1;
        }
        int newfd = -1, both = 0;
        if (r->type == R_DUPIN || r->type == R_DUPOUT) {
            char *t = redir_path(r);
            if (!t)
                return -1;
            if (strcmp(t, "-") == 0) {          /* n>&- 关闭 */
                free(t);
                if (save && save_fd(save, fd) < 0)
                    return -1;
                close(fd);
                continue;
            }
            char *end;
            long n = strtol(t, &end, 10);
            if (*t && !*end && n >= 0) {        /* n>&m 复制 */
                if (n >= SH_FD_BASE || fcntl((int)n, F_GETFD) < 0) {
                    sh_error("%s: 错误的文件描述符", t);
                    free(t);
                    return -1;
                }
                free(t);
                if (n == fd)
                    continue;
                if (save && save_fd(save, fd) < 0)
                    return -1;
                if (dup2((int)n, fd) < 0) {
                    sh_error("dup2: %s", sh_strerror(errno));
                    return -1;
                }
                continue;
            }
            if (r->type == R_DUPOUT && !r->fd_given) {      /* >&file 等同于 &>file */
                if (save && (save_fd(save, 1) < 0 || save_fd(save, 2) < 0)) {
                    free(t);
                    return -1;
                }
                newfd = open(t, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
                if (newfd < 0) {
                    sh_error("%s: %s", t, sh_strerror(errno));
                    free(t);
                    return -1;
                }
                free(t);
                both = 1;
            } else {
                sh_error("%s: 模糊的重定向", t);
                free(t);
                return -1;
            }
        } else {
            both = r->type == R_OUTERR || r->type == R_APPENDERR;
            if (save && (save_fd(save, fd) < 0 || (both && save_fd(save, 2) < 0)))
                return -1;
            if (r->type == R_HEREDOC) {
                newfd = heredoc_fd(r);
                if (newfd < 0)
                    return -1;
            } else {
                char *path = redir_path(r);
                if (!path)
                    return -1;
                int flags;
                switch (r->type) {
                case R_IN:        flags = O_RDONLY; break;
                case R_INOUT:     flags = O_RDWR | O_CREAT; break;
                case R_APPEND:
                case R_APPENDERR: flags = O_WRONLY | O_CREAT | O_APPEND; break;
                default:          flags = O_WRONLY | O_CREAT | O_TRUNC; break;
                }
                if (sh.opt_noclobber && (r->type == R_OUT || r->type == R_OUTERR)) {
                    struct stat st;
                    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                        sh_error("%s: 不能覆盖已存在的文件（set -C 已开启，可以用 >| 强制覆盖）", path);
                        free(path);
                        return -1;
                    }
                }
                newfd = open(path, flags | O_CLOEXEC, 0666);
                if (newfd < 0) {
                    sh_error("%s: %s", path, sh_strerror(errno));
                    free(path);
                    return -1;
                }
                free(path);
            }
        }
        if (newfd != fd) {
            if (dup2(newfd, fd) < 0) {
                sh_error("dup2: %s", sh_strerror(errno));
                close(newfd);
                return -1;
            }
            close(newfd);
        } else {
            fcntl(fd, F_SETFD, 0);      /* open 正好返回了目标编号：去掉 CLOEXEC */
        }
        if (both && fd != 2)
            dup2(fd, 2);
    }
    return 0;
}

/* ======================= 赋值 ======================= */

typedef struct {
    char *name;
    char *value;
} assign_t;

static void free_assigns(assign_t *as, int n)
{
    for (int i = 0; i < n; i++) {
        free(as[i].name);
        free(as[i].value);
    }
    free(as);
}

/* 把 NAME=value 单词拆开并展开右边（不分词、不匹配文件名，~ 可以出现在 : 之后） */
static char *expand_assign_word(word *w, char **name_out)
{
    const char *text = w->parts->text;
    const char *eq = strchr(text, '=');
    *name_out = xstrndup(text, (size_t)(eq - text));
    wpart first = *w->parts;
    first.text = (char *)eq + 1;
    word tmp = *w;
    tmp.parts = &first;
    tmp.next = NULL;
    char *v = expand_string(&tmp, 1);
    if (!v) {
        free(*name_out);
        *name_out = NULL;
    }
    return v;
}

static int expand_assigns(word *list, assign_t **out, int *count)
{
    int n = 0, cap = 0;
    assign_t *as = NULL;
    for (word *w = list; w; w = w->next) {
        char *name;
        char *v = expand_assign_word(w, &name);
        if (!v) {
            free_assigns(as, n);
            return -1;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            as = xrealloc(as, sizeof *as * (size_t)cap);
        }
        as[n].name = name;
        as[n].value = v;
        n++;
    }
    *out = as;
    *count = n;
    return 0;
}

/* export/local/readonly 的参数里的 a=$x 和赋值一样不做分词 */
static int is_decl_builtin(const word *w)
{
    if (!w || !w->parts || w->parts->next || w->parts->kind != WP_LIT || w->parts->quoted)
        return 0;
    const char *t = w->parts->text;
    return strcmp(t, "export") == 0 || strcmp(t, "local") == 0 || strcmp(t, "readonly") == 0;
}

static int looks_like_assign(const word *w)
{
    if (!w->parts || w->parts->kind != WP_LIT || w->parts->quoted)
        return 0;
    const char *eq = strchr(w->parts->text, '=');
    return eq && is_name_n(w->parts->text, (size_t)(eq - w->parts->text));
}

static int expand_cmd_args(node *n, strvec *argv)
{
    word *w = n->u.simple.args;
    if (!is_decl_builtin(w))
        return expand_args(w, argv);
    sv_argv(argv);
    sv_push(argv, xstrdup(w->parts->text));
    for (w = w->next; w; w = w->next) {
        if (looks_like_assign(w)) {
            char *name;
            char *v = expand_assign_word(w, &name);
            if (!v)
                return -1;
            sv_push(argv, sk_strdup_printf("%s=%s", name, v));
            free(name);
            free(v);
        } else {
            word one = *w;
            one.next = NULL;
            if (expand_args(&one, argv) < 0)
                return -1;
        }
    }
    return 0;
}

static void xtrace(assign_t *as, int na, strvec *argv)
{
    const char *ps4 = var_get("PS4");
    char *p = expand_prompt(ps4 ? ps4 : "+ ");
    strbuf b;
    sb_init(&b);
    sb_puts(&b, p);
    free(p);
    for (int i = 0; i < na; i++) {
        sb_printf(&b, "%s=", as[i].name);
        shell_quote(&b, as[i].value);
        sb_putc(&b, ' ');
    }
    for (int i = 0; i < argv->n; i++) {
        if (i)
            sb_putc(&b, ' ');
        shell_quote(&b, argv->v[i]);
    }
    sb_putc(&b, '\n');
    full_write(STDERR_FILENO, b.s, b.len);
    sb_free(&b);
}

/* 展开出错：放弃整行命令；${x:?}、set -u 出错时非交互 shell 直接退出 */
static int expansion_failed(void)
{
    sh.abort = 1;
    sh.last_status = 1;
    if (g_expand_fatal && !sh.interactive)
        exit_shell(1);
    return 1;
}

/* ======================= 外部命令 ======================= */

int find_in_path(const char *name, strbuf *out)
{
    const char *path = var_get("PATH");
    if (!path)
        path = "/bin:/sbin:/usr/bin:/usr/sbin";
    const char *p = path;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t dl = colon ? (size_t)(colon - p) : strlen(p);
        sb_clear(out);
        if (dl == 0)
            sb_putc(out, '.');
        else
            sb_putn(out, p, dl);
        sb_putc(out, '/');
        sb_puts(out, name);
        struct stat st;
        if (stat(sb_str(out), &st) == 0 && S_ISREG(st.st_mode) && access(sb_str(out), X_OK) == 0)
            return 1;
        if (!colon)
            break;
        p = colon + 1;
    }
    sb_clear(out);
    return 0;
}

/* execve 返回 ENOEXEC：文件不是 ELF 程序，又没有 #! 行。POSIX 规定这时把它当 shell 脚本运行。 */
static void run_as_script(const char *path, char **argv)
{
    char head[128];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = full_read(fd, head, sizeof head);
        close(fd);
        if (n > 0 && memchr(head, 0, (size_t)n)) {
            sh_error("%s: 无法执行二进制文件: %s", path, sh_strerror(ENOEXEC));
            exit_child(126);
        }
    }
    int argc = 0;
    while (argv[argc])
        argc++;
    char **nargv = xcalloc((size_t)argc + 2, sizeof(char *));
    nargv[0] = (char *)"sksh";
    nargv[1] = (char *)path;
    for (int i = 1; i < argc; i++)
        nargv[i + 1] = argv[i];
    char **envp = var_environ();
    execve("/proc/self/exe", nargv, envp);
    execve("/bin/sksh", nargv, envp);
    execve("/bin/sh", nargv, envp);
    free(nargv);
}

static int exec_error_status(const char *name, int err, int has_slash)
{
    struct stat st;
    if (err == ENOENT && !has_slash) {
        sh_error("%s: 未找到命令", name);
        return 127;
    }
    if (err == ENOENT) {
        sh_error("%s: %s", name, sh_strerror(err));
        return 127;
    }
    if (err == EACCES && stat(name, &st) == 0 && S_ISDIR(st.st_mode)) {
        sh_error("%s: %s", name, sh_strerror(EISDIR));
        return 126;
    }
    sh_error("%s: %s", name, sh_strerror(err));
    return 126;
}

/* 尝试 execve；失败时打印错误并返回退出码（126/127） */
static int try_exec(char **argv)
{
    char **envp = var_environ();
    const char *name = argv[0];
    if (strchr(name, '/')) {
        execve(name, argv, envp);
        int e = errno;
        if (e == ENOEXEC) {
            run_as_script(name, argv);
            e = ENOEXEC;
        }
        return exec_error_status(name, e, 1);
    }
    if (!*name) {
        sh_error(": 未找到命令");
        return 127;
    }
    const char *path = var_get("PATH");
    if (!path)
        path = "/bin:/sbin:/usr/bin:/usr/sbin";
    strbuf full;
    sb_init(&full);
    int eacces = 0, other = 0;
    const char *p = path;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t dl = colon ? (size_t)(colon - p) : strlen(p);
        sb_clear(&full);
        if (dl == 0)
            sb_putc(&full, '.');
        else
            sb_putn(&full, p, dl);
        sb_putc(&full, '/');
        sb_puts(&full, name);
        execve(sb_str(&full), argv, envp);
        int e = errno;
        if (e == ENOEXEC) {
            run_as_script(sb_str(&full), argv);
        } else if (e == EACCES) {
            struct stat st;
            if (stat(sb_str(&full), &st) == 0 && !S_ISDIR(st.st_mode))
                eacces = 1;
        } else if (e != ENOENT && e != ENOTDIR && e != ELOOP && e != ENAMETOOLONG) {
            other = e;
            break;
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    int st;
    if (other) {
        sh_error("%s: %s", sb_str(&full), sh_strerror(other));
        st = 126;
    } else if (eacces) {
        sh_error("%s: %s", name, sh_strerror(EACCES));
        st = 126;
    } else {
        st = exec_error_status(name, ENOENT, 0);
    }
    sb_free(&full);
    return st;
}

void exec_external(char **argv)
{
    exit_child(try_exec(argv));
}

int bi_exec_args(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc)
        return 0;
    /* exec：shell 进程直接变成新程序（不 fork）。被忽略的信号会被新程序继承，先恢复默认。 */
    signals_child_reset(0);
    jobs_restore_terminal();
    int st = try_exec(argv + i);
    if (!sh.interactive)
        exit_shell(st);
    signals_init();
    return st;
}

/* command 名字 参数...：跳过同名的 shell 函数，直接执行内建命令或外部程序 */
int bi_command(int argc, char **argv)
{
    int i = 1, mode = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-v") == 0)
            mode = 'v';
        else if (strcmp(argv[i], "-V") == 0)
            mode = 'V';
        else if (strcmp(argv[i], "-p") != 0) {
            sh_error("command: %s: 无效的选项\n用法: command [-vV] 命令 [参数...]", argv[i]);
            return 2;
        }
    }
    if (i >= argc)
        return 0;
    if (mode)
        return bi_command_describe(argc - i, argv + i, mode == 'V');
    const builtin_def *b = strchr(argv[i], '/') ? NULL : builtin_find(argv[i]);
    if (b)
        return b->fn(argc - i, argv + i);
    strbuf text;
    sb_init(&text);
    for (int k = i; k < argc; k++) {
        if (k > i)
            sb_putc(&text, ' ');
        sb_puts(&text, argv[k]);
    }
    job *j = job_new(sb_str(&text));
    sb_free(&text);
    pid_t pid = sh_fork();
    if (pid < 0) {
        job_free(j);
        return 1;
    }
    if (pid == 0) {
        job_child_setup(j, 1);
        signals_child_reset(0);
        exec_external(argv + i);
    }
    job_add_pid(j, pid);
    return job_wait_fg(j);
}

/* ======================= 简单命令 ======================= */

static int call_function(shfunc *f, node *n, strvec *argv);

static int run_builtin(const builtin_def *b, node *n, strvec *argv, assign_t *as, int na)
{
    varsave *saves = NULL;
    int is_exec = strcmp(b->name, "exec") == 0;
    for (int i = 0; i < na; i++) {
        saves = var_save(saves, as[i].name);
        if (var_set(as[i].name, as[i].value, is_exec ? V_EXPORT : 0) < 0) {
            var_restore(saves);
            return 1;
        }
    }
    int st;
    if (is_exec) {
        /* exec 不带命令时，重定向永久生效：exec >log 2>&1 */
        st = apply_redirs(n->redirs, NULL) < 0 ? 1 : b->fn(argv->n, argv->v);
        fflush(stdout);
    } else {
        saved_fd *sv = NULL;
        if (apply_redirs(n->redirs, &sv) < 0) {
            st = 1;
        } else {
            clearerr(stdout);
            errno = 0;
            st = b->fn(argv->n, argv->v);
            if (fflush(stdout) != 0 || ferror(stdout)) {
                int e = errno;
                if (e != EPIPE)
                    sh_error("%s: 写入错误: %s", b->name, sh_strerror(e ? e : EIO));
                clearerr(stdout);
                if (st == 0)
                    st = 1;
            }
        }
        restore_fds(sv);
    }
    var_restore(saves);
    return st;
}

static int exec_simple(node *n, int flags)
{
    sh.lineno = n->lineno;
    int sub_before = g_cmdsub_count;
    strvec argv;
    sv_init(&argv);
    if (expand_cmd_args(n, &argv) < 0) {
        sv_free(&argv);
        return expansion_failed();
    }
    assign_t *as = NULL;
    int na = 0;
    if (expand_assigns(n->u.simple.assigns, &as, &na) < 0) {
        sv_free(&argv);
        return expansion_failed();
    }
    int st;

    if (argv.n == 0) {
        /* 只有赋值（和重定向）：赋值永久生效；状态码是最后一个命令替换的状态 */
        if (sh.opt_xtrace && na)
            xtrace(as, na, &argv);
        st = g_cmdsub_count != sub_before ? sh.last_status : 0;
        for (int i = 0; i < na; i++)
            if (var_set(as[i].name, as[i].value, 0) < 0)
                st = 1;
        if (n->redirs) {
            saved_fd *sv = NULL;
            if (apply_redirs(n->redirs, &sv) < 0)
                st = 1;
            restore_fds(sv);
        }
        free_assigns(as, na);
        sv_free(&argv);
        return st;
    }
    if (sh.opt_xtrace)
        xtrace(as, na, &argv);

    const char *name = argv.v[0];
    int has_slash = strchr(name, '/') != NULL;
    shfunc *f = has_slash ? NULL : func_get(name);
    if (f) {
        varsave *saves = NULL;
        for (int i = 0; i < na; i++) {
            saves = var_save(saves, as[i].name);
            var_set(as[i].name, as[i].value, V_EXPORT);
        }
        st = call_function(f, n, &argv);
        var_restore(saves);
        free_assigns(as, na);
        sv_free(&argv);
        return st;
    }
    const builtin_def *b = has_slash ? NULL : builtin_find(name);
    if (b) {
        st = run_builtin(b, n, &argv, as, na);
        free_assigns(as, na);
        sv_free(&argv);
        return st;
    }

    /* 外部命令。EF_TAIL：我们本来就是要退出的子进程，直接 exec，不用再 fork 一次 */
    if ((flags & EF_TAIL) && sh.subshell && !trap_exit_set()) {
        for (int i = 0; i < na; i++)
            var_set(as[i].name, as[i].value, V_EXPORT);
        if (apply_redirs(n->redirs, NULL) < 0)
            exit_child(1);
        exec_external(argv.v);
    }
    char *text = node_text(n);
    job *j = job_new(text);
    free(text);
    pid_t pid = sh_fork();
    if (pid < 0) {
        job_free(j);
        free_assigns(as, na);
        sv_free(&argv);
        return 1;
    }
    if (pid == 0) {
        job_child_setup(j, 1);
        signals_child_reset(0);
        for (int i = 0; i < na; i++)
            var_set(as[i].name, as[i].value, V_EXPORT);
        if (apply_redirs(n->redirs, NULL) < 0)
            exit_child(1);
        exec_external(argv.v);
    }
    job_add_pid(j, pid);
    free_assigns(as, na);
    sv_free(&argv);
    return job_wait_fg(j);
}

static int call_function(shfunc *f, node *n, strvec *argv)
{
    if (sh.func_depth >= 400) {
        sh_error("%s: 函数调用层数过多（超过 400 层，可能是无限递归）", f->name);
        sh.abort = 1;
        return 1;
    }
    saved_fd *sv = NULL;
    if (apply_redirs(n->redirs, &sv) < 0) {
        restore_fds(sv);
        return 1;
    }
    posparams np;
    np.n = argv->n - 1;
    np.v = xcalloc((size_t)argv->n + 1, sizeof(char *));
    for (int i = 1; i < argv->n; i++)
        np.v[i - 1] = xstrdup(argv->v[i]);
    pos_swap(&np);
    arena *ar = f->ar;
    node *body = f->body;
    arena_ref(ar);              /* 函数执行期间就算被重新定义/unset，语法树也不能释放 */
    int save_loop = sh.loop_depth;
    sh.loop_depth = 0;
    var_scope_push();
    sh.func_depth++;
    int st = exec_node(body, 0);
    if (sh.returning) {
        st = sh.return_status;
        sh.returning = 0;
    }
    sh.func_depth--;
    var_scope_pop();
    sh.loop_depth = save_loop;
    sh.break_n = sh.continue_n = 0;
    arena_unref(ar);
    pos_swap(&np);
    for (int i = 0; i < np.n; i++)
        free(np.v[i]);
    free(np.v);
    restore_fds(sv);
    return st;
}

/* ======================= 管道线 =======================
 * a | b | c：创建 2 个管道，fork 3 个子进程。
 *   a 的 stdout → 管道1写端；b 的 stdin ← 管道1读端，b 的 stdout → 管道2写端；……
 * 父进程必须关掉自己手里所有的管道端：只要还有一个写端开着，读的一方就永远等不到 EOF。
 * 三个进程放进同一个进程组（= 一个“作业”），这样 Ctrl-C / Ctrl-Z 能一起作用于它们。 */
static int exec_pipeline_cmds(node **cmds, int nc, int bg, const char *text)
{
    job *j = job_new(text);
    int async_nojc = bg && !sh.jobctl;
    int prev = -1;
    pid_t last = -1;
    for (int i = 0; i < nc; i++) {
        int pfd[2] = { -1, -1 };
        if (i < nc - 1 && pipe2(pfd, O_CLOEXEC) < 0) {
            sh_error("无法创建管道: %s", sh_strerror(errno));
            break;
        }
        pid_t pid = sh_fork();
        if (pid < 0) {
            if (pfd[0] >= 0) {
                close(pfd[0]);
                close(pfd[1]);
            }
            break;
        }
        if (pid == 0) {
            job_child_setup(j, !bg);
            signals_child_reset(async_nojc);
            if (prev >= 0) {
                dup2(prev, STDIN_FILENO);
                close(prev);
            } else if (async_nojc) {
                /* 没有作业控制时，后台命令的标准输入接到 /dev/null，免得和前台抢终端输入 */
                int nul = open("/dev/null", O_RDONLY);
                if (nul >= 0 && nul != STDIN_FILENO) {
                    dup2(nul, STDIN_FILENO);
                    close(nul);
                }
            }
            if (pfd[1] >= 0) {
                dup2(pfd[1], STDOUT_FILENO);
                close(pfd[1]);
                close(pfd[0]);
            }
            int st = exec_node(cmds[i], EF_TAIL);
            exit_child(st);
        }
        job_add_pid(j, pid);
        last = pid;
        if (prev >= 0)
            close(prev);
        if (pfd[1] >= 0)
            close(pfd[1]);
        prev = pfd[0];
    }
    if (prev >= 0)
        close(prev);
    if (last < 0) {
        job_free(j);
        return 1;
    }
    if (bg) {
        sh.last_bg = last;
        job_put_bg(j);
        return 0;
    }
    return job_wait_fg(j);
}

static void exec_async(node *n)
{
    char *text = node_text(n);
    if (n->type == N_PIPELINE && n->u.pipe.ncmds > 1 && !n->u.pipe.negate)
        exec_pipeline_cmds(n->u.pipe.cmds, n->u.pipe.ncmds, 1, text);
    else
        exec_pipeline_cmds(&n, 1, 1, text);
    free(text);
}

static int exec_subshell(node *n, int flags)
{
    if ((flags & EF_TAIL) && sh.subshell) {
        if (apply_redirs(n->redirs, NULL) < 0)
            return 1;
        return exec_node(n->u.group.body, EF_TAIL);
    }
    char *text = node_text(n);
    job *j = job_new(text);
    free(text);
    pid_t pid = sh_fork();
    if (pid < 0) {
        job_free(j);
        return 1;
    }
    if (pid == 0) {
        job_child_setup(j, 1);
        signals_child_reset(0);
        if (apply_redirs(n->redirs, NULL) < 0)
            exit_child(1);
        exit_child(exec_node(n->u.group.body, EF_TAIL));
    }
    job_add_pid(j, pid);
    return job_wait_fg(j);
}

/* ======================= 复合命令 ======================= */

/* 循环体执行完之后处理 break/continue，返回 1 表示退出本层循环 */
static int loop_ctl(void)
{
    if (sh.abort || sh.returning)
        return 1;
    if (sh.break_n > 0) {
        sh.break_n--;
        return 1;
    }
    if (sh.continue_n > 0) {
        sh.continue_n--;
        return sh.continue_n > 0;
    }
    return 0;
}

static int exec_loop(node *n)
{
    int st = 0;
    sh.loop_depth++;
    for (;;) {
        sh.cond_depth++;
        int c = exec_node(n->u.loop.cond, 0);
        sh.cond_depth--;
        if (sh_should_stop() && loop_ctl())
            break;
        if (n->type == N_WHILE ? c != 0 : c == 0)
            break;
        st = exec_node(n->u.loop.body, 0);
        sh_should_stop();
        if (loop_ctl())
            break;
    }
    sh.loop_depth--;
    return st;
}

static int exec_for(node *n)
{
    strvec words;
    sv_init(&words);
    if (n->u.forn.has_in) {
        if (expand_args(n->u.forn.words, &words) < 0) {
            sv_free(&words);
            return expansion_failed();
        }
    } else {
        for (int i = 1; i <= pos_count(); i++)
            sv_push(&words, xstrdup(pos_get(i)));
    }
    int st = 0;
    sh.loop_depth++;
    for (int i = 0; i < words.n; i++) {
        if (var_set(n->u.forn.var, words.v[i], 0) < 0) {
            st = 1;
            break;
        }
        st = exec_node(n->u.forn.body, 0);
        sh_should_stop();
        if (loop_ctl())
            break;
    }
    sh.loop_depth--;
    sv_free(&words);
    return st;
}

static int exec_case(node *n, int flags)
{
    char *subj = expand_string(n->u.casen.subject, 0);
    if (!subj)
        return expansion_failed();
    int st = 0;
    for (case_item *it = n->u.casen.items; it; it = it->next) {
        for (word *p = it->patterns; p; p = p->next) {
            word one = *p;
            one.next = NULL;
            char *pat = expand_pattern(&one);
            if (!pat) {
                free(subj);
                return expansion_failed();
            }
            int m = fnmatch(pat, subj, 0) == 0;
            free(pat);
            if (m) {
                free(subj);
                return it->body ? exec_node(it->body, flags) : 0;
            }
        }
    }
    free(subj);
    return st;
}

static int with_redirs(node *n, int (*fn)(node *, int), int flags)
{
    if (!n->redirs)
        return fn(n, flags);
    saved_fd *sv = NULL;
    int st;
    if (apply_redirs(n->redirs, &sv) < 0)
        st = 1;
    else
        st = fn(n, flags);
    restore_fds(sv);
    return st;
}

static int exec_compound(node *n, int flags)
{
    int st = 0;
    switch (n->type) {
    case N_BRACE:
        st = exec_node(n->u.group.body, flags);
        break;
    case N_IF: {
        sh.cond_depth++;
        int c = exec_node(n->u.ifn.cond, 0);
        sh.cond_depth--;
        if (sh_should_stop())
            return c;
        if (c == 0)
            st = exec_node(n->u.ifn.then_part, flags);
        else if (n->u.ifn.else_part)
            st = exec_node(n->u.ifn.else_part, flags);
        else
            st = 0;
        break;
    }
    case N_WHILE:
    case N_UNTIL:
        st = exec_loop(n);
        break;
    case N_FOR:
        st = exec_for(n);
        break;
    case N_CASE:
        st = exec_case(n, flags);
        break;
    default:
        break;
    }
    return st;
}

int exec_node(node *n, int flags)
{
    if (!n)
        return 0;
    if (sh_should_stop())
        return sh.last_status;
    int st = 0;
    switch (n->type) {
    case N_SIMPLE:
        st = exec_simple(n, flags);
        break;
    case N_PIPELINE:
        if (n->u.pipe.ncmds == 1) {
            if (n->u.pipe.negate)
                sh.cond_depth++;
            st = exec_node(n->u.pipe.cmds[0], flags & ~EF_TAIL);
            if (n->u.pipe.negate)
                sh.cond_depth--;
        } else {
            char *text = node_text(n);
            st = exec_pipeline_cmds(n->u.pipe.cmds, n->u.pipe.ncmds, 0, text);
            free(text);
        }
        if (n->u.pipe.negate)
            st = !st;
        break;
    case N_AND:
    case N_OR:
        sh.cond_depth++;
        st = exec_node(n->u.andor.left, 0);
        sh.cond_depth--;
        if (sh_should_stop())
            break;
        if ((n->type == N_AND) == (st == 0))
            st = exec_node(n->u.andor.right, flags);
        break;
    case N_LIST:
        for (list_item *it = n->u.list.items; it; it = it->next) {
            if (it->async) {
                exec_async(it->node);
                st = 0;
            } else {
                st = exec_node(it->node, it->next ? 0 : flags);
            }
            sh.last_status = st;
            if (sh_should_stop())
                break;
        }
        break;
    case N_SUBSHELL:
        st = exec_subshell(n, flags);
        break;
    case N_FUNCDEF:
        func_set(n->u.func.name, n->u.func.body, g_cur_arena);
        st = 0;
        break;
    default:
        st = with_redirs(n, exec_compound, flags);
        break;
    }
    sh.last_status = st;
    /* set -e：命令失败就退出（条件里、&& || 左边、! 后面的除外） */
    if (st != 0 && sh.opt_errexit && sh.cond_depth == 0 && !sh.abort &&
        (n->type == N_SIMPLE || n->type == N_SUBSHELL ||
         (n->type == N_PIPELINE && !n->u.pipe.negate)))
        exit_shell(st);
    return st;
}

/* ======================= 命令替换 =======================
 * $(cmd)：创建管道，fork 子 shell，子 shell 的 stdout 接到管道写端；
 * 父进程从读端读出全部输出，直到 EOF（子进程退出、写端全部关闭），再 waitpid 取状态。 */
int cmdsub_run(node *n, strbuf *out)
{
    g_cmdsub_count++;
    if (!n) {
        sh.last_status = 0;
        return 0;
    }
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) {
        sh_error("无法创建管道: %s", sh_strerror(errno));
        sh.last_status = 1;
        return 1;
    }
    pid_t pid = sh_fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        sh.last_status = 1;
        return 1;
    }
    if (pid == 0) {
        close(pfd[0]);
        if (pfd[1] != STDOUT_FILENO) {
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);
        }
        jobs_child_reset();
        signals_child_reset(0);
        exit_child(exec_node(n, EF_TAIL));
    }
    close(pfd[1]);
    char buf[4096];
    for (;;) {
        ssize_t r = read(pfd[0], buf, sizeof buf);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        for (ssize_t i = 0; i < r; i++)
            if (buf[i])
                sb_putc(out, buf[i]);
    }
    close(pfd[0]);
    int ws = 0;
    while (waitpid(pid, &ws, 0) < 0) {
        if (errno != EINTR) {
            ws = 0;
            break;
        }
    }
    int st = status_from_wait(ws);
    sh.last_status = st;
    if (WIFSIGNALED(ws) && WTERMSIG(ws) == SIGINT && sh.interactive)
        sh.abort = 1;
    return st;
}

/* ======================= 读-解析-执行 循环 ======================= */

int exec_source(source *src, int toplevel)
{
    parser *p = parser_new(src);
    int st = sh.last_status;
    for (;;) {
        if (toplevel && sh.interactive) {
            jobs_notify();
            /* “还有已停止的作业”的提醒只管紧接着的下一条命令：连续两次 exit 才真正退出 */
            if (sh.exit_warned > 0)
                sh.exit_warned--;
        }
        g_sigint = 0;
        node *n;
        arena *ar;
        int r = parser_next(p, &n, &ar);
        if (r == PARSE_EOF) {
            if (toplevel && sh.interactive) {
                if (jobs_stopped_count() > 0 && sh.exit_warned == 0) {
                    fputs("还有已停止的作业。（再按一次 Ctrl-D 就退出）\n", stderr);
                    sh.exit_warned = 2;
                    continue;
                }
                if (sh.opt_ignoreeof) {
                    fputs("请用 \"exit\" 退出 shell。\n", stderr);
                    continue;
                }
                fputs("exit\n", stderr);
            }
            break;
        }
        if (r == PARSE_CANCEL) {
            sh.last_status = 130;
            continue;
        }
        if (r == PARSE_ERROR) {
            sh.last_status = st = 2;
            if (!toplevel || !sh.interactive)
                break;
            continue;
        }
        if (r == PARSE_EMPTY)
            continue;
        source_sync(src);
        if (toplevel && sh.interactive) {
            char *t = parser_command_text(p);
            history_add(t);
            free(t);
        }
        if (sh.opt_noexec && !sh.interactive) {
            arena_unref(ar);
            continue;
        }
        arena *save_ar = g_cur_arena;
        g_cur_arena = ar;
        st = exec_node(n, 0);
        g_cur_arena = save_ar;
        arena_unref(ar);
        if (g_trap_pending)
            traps_run_pending();
        if (sh.abort) {
            if (!toplevel)
                break;
            sh.abort = 0;
            st = sh.last_status;
            if (g_expand_fatal && !sh.interactive)
                exit_shell(st ? st : 1);
        }
        if (sh.returning && !toplevel)
            break;
        if (toplevel) {
            sh.break_n = sh.continue_n = 0;
            sh.returning = 0;
        }
    }
    parser_free(p);
    return st;
}

int exec_string(const char *s, const char *name)
{
    source *src = source_string(s, name);
    int st = exec_source(src, 0);
    source_free(src);
    return st;
}

int exec_file(const char *path, int argc, char **argv, int must_exist)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (must_exist || errno != ENOENT) {
            sh_error("%s: %s", path, sh_strerror(errno));
            return 1;
        }
        return 0;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        sh_error("%s: %s", path, sh_strerror(EISDIR));
        close(fd);
        return 1;
    }
    fd = fd_move_high(fd);
    source *src = source_fd(fd, path, 1);
    posparams saved = { 0, NULL };
    int swapped = 0;
    if (argc > 0) {
        saved.n = argc;
        saved.v = xcalloc((size_t)argc + 1, sizeof(char *));
        for (int i = 0; i < argc; i++)
            saved.v[i] = xstrdup(argv[i]);
        pos_swap(&saved);
        swapped = 1;
    }
    const char *save_name = sh.script_name;
    int save_line = sh.lineno;
    sh.script_name = path;
    sh.source_depth++;
    int r = exec_source(src, 0);
    sh.source_depth--;
    if (sh.returning) {
        r = sh.return_status;
        sh.returning = 0;
    }
    sh.script_name = save_name;
    sh.lineno = save_line;
    if (swapped) {
        pos_swap(&saved);
        for (int i = 0; i < saved.n; i++)
            free(saved.v[i]);
        free(saved.v);
    }
    source_free(src);
    return r;
}
