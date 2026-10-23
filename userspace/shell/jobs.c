/*
 * jobs.c — 作业控制
 *
 * 几个操作系统概念：
 *
 * 进程组(process group)：一组进程共用一个“组号”(pgid)。shell 把一条管道线
 *   a | b | c 的三个进程放进同一个组，这就是一个“作业”(job)。kill(-pgid, 信号)
 *   可以一次把信号发给整个组。
 *
 * 会话(session)与控制终端(controlling terminal)：登录时 sinit 调用 setsid() 创建会话，
 *   并把串口 /dev/ttyS0 设为这个会话的控制终端。一个终端在任何时刻只有一个
 *   “前台进程组”(tcsetpgrp 设置)：
 *     - 在终端上按 Ctrl-C，终端驱动（内核里的 tty 层）给前台进程组发 SIGINT；
 *     - 按 Ctrl-Z 发 SIGTSTP（让进程停止）；
 *     - 后台进程组的进程想读终端，内核会给它发 SIGTTIN 让它停下来。
 *   所以 shell 运行前台命令时，要先用 tcsetpgrp 把终端“交给”那个作业的进程组，
 *   命令结束（或被 Ctrl-Z 停止）后再把终端拿回来。shell 自己忽略 SIGTTOU，
 *   否则它在后台时调用 tcsetpgrp 会被内核停住。
 *
 * 等待：waitpid(-1, &st, WUNTRACED) 不仅在子进程退出时返回，子进程“停止”时也返回
 *   (WIFSTOPPED)，shell 就是靠这个知道你按了 Ctrl-Z。fg/bg 用 kill(-pgid, SIGCONT) 让作业继续。
 *
 * 孤儿进程组：shell 退出后，如果它留下的作业还停在那里，内核会给这个“孤儿”进程组发
 *   SIGHUP + SIGCONT，免得它们永远停着。
 */
#include "sksh.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { P_RUN, P_STOP, P_DONE };

typedef struct proc {
    pid_t pid;
    int   status;           /* waitpid 返回的状态 */
    int   state;
} proc;

struct job {
    int            id;      /* 作业号 %1 %2 …；0 = 还没登记 */
    pid_t          pgid;
    char          *cmd;
    proc          *procs;
    int            nprocs, cap;
    int            bg;
    int            notify;  /* 状态有变化，还没告诉用户 */
    unsigned long  seq;     /* 越大越“新”：决定 + 和 - */
    struct termios tmodes;  /* 被停止时的终端设置（例如 vi 的原始模式），fg 时恢复 */
    int            have_tmodes;
    struct job    *next;
};

static job          *jobs;      /* 后台作业和已停止的作业 */
static job          *fg_job;    /* 正在前台等待的作业 */
static job          *inherited; /* 子 shell 里：父 shell 的作业表（只用来让 jobs | grep 能看到） */
static unsigned long seq_counter;

/* ======================= 基本操作 ======================= */

job *job_new(const char *cmd)
{
    job *j = xcalloc(1, sizeof *j);
    j->cmd = xstrdup(cmd ? cmd : "");
    return j;
}

void job_free(job *j)
{
    if (!j)
        return;
    free(j->procs);
    free(j->cmd);
    free(j);
}

static int job_running(const job *j)
{
    for (int i = 0; i < j->nprocs; i++)
        if (j->procs[i].state == P_RUN)
            return 1;
    return 0;
}

static int job_done(const job *j)
{
    for (int i = 0; i < j->nprocs; i++)
        if (j->procs[i].state != P_DONE)
            return 0;
    return 1;
}

static int job_stopped(const job *j)
{
    return !job_running(j) && !job_done(j);
}

int status_from_wait(int ws)
{
    if (WIFEXITED(ws))
        return WEXITSTATUS(ws);
    if (WIFSIGNALED(ws))
        return 128 + WTERMSIG(ws);
    if (WIFSTOPPED(ws))
        return 128 + WSTOPSIG(ws);
    return 0;
}

/* 作业的状态码：最后一个进程的状态；set -o pipefail 时取最右边那个非 0 的 */
static int job_status(const job *j)
{
    if (j->nprocs == 0)
        return 0;
    if (sh.opt_pipefail) {
        for (int i = j->nprocs - 1; i >= 0; i--) {
            int s = status_from_wait(j->procs[i].status);
            if (s)
                return s;
        }
        return 0;
    }
    return status_from_wait(j->procs[j->nprocs - 1].status);
}

static void job_unlink(job *j)
{
    for (job **pp = &jobs; *pp; pp = &(*pp)->next)
        if (*pp == j) {
            *pp = j->next;
            j->next = NULL;
            return;
        }
}

static int job_listed(const job *j)
{
    for (job *k = jobs; k; k = k->next)
        if (k == j)
            return 1;
    return 0;
}

/* 登记到作业表（分配作业号），按作业号排序 */
static void job_register(job *j)
{
    j->seq = ++seq_counter;
    if (job_listed(j))
        return;
    if (j->id == 0) {
        int max = 0;
        for (job *k = jobs; k; k = k->next)
            if (k->id > max)
                max = k->id;
        j->id = max + 1;
    }
    job **pp = &jobs;
    while (*pp && (*pp)->id < j->id)
        pp = &(*pp)->next;
    j->next = *pp;
    *pp = j;
}

static proc *find_proc(pid_t pid, job **owner)
{
    if (fg_job)
        for (int i = 0; i < fg_job->nprocs; i++)
            if (fg_job->procs[i].pid == pid) {
                *owner = fg_job;
                return &fg_job->procs[i];
            }
    for (job *j = jobs; j; j = j->next)
        for (int i = 0; i < j->nprocs; i++)
            if (j->procs[i].pid == pid) {
                *owner = j;
                return &j->procs[i];
            }
    return NULL;
}

static void update_proc(pid_t pid, int ws)
{
    job *j;
    proc *p = find_proc(pid, &j);
    if (!p)
        return;         /* 不属于任何作业的子进程（例如 here-doc 的写入进程） */
    if (WIFSTOPPED(ws)) {
        p->state = P_STOP;
        p->status = ws;
        j->notify = 1;
    } else if (WIFCONTINUED(ws)) {
        p->state = P_RUN;
    } else {
        p->state = P_DONE;
        p->status = ws;
        if (job_done(j))
            j->notify = 1;
    }
}

static void reap_nohang(void)
{
    int flags = WNOHANG | (sh.jobctl ? WUNTRACED | WCONTINUED : 0);
    for (;;) {
        int ws;
        pid_t pid = waitpid(-1, &ws, flags);
        if (pid <= 0) {
            if (pid < 0 && errno == EINTR)
                continue;
            return;
        }
        update_proc(pid, ws);
    }
}

/* 阻塞等待，直到作业 j 不再有“运行中”的进程（全部结束或停止） */
static int wait_job(job *j, int interruptible)
{
    while (job_running(j)) {
        int ws;
        pid_t pid = waitpid(-1, &ws, sh.jobctl ? WUNTRACED : 0);
        if (pid < 0) {
            if (errno == EINTR) {
                if (g_trap_pending)
                    traps_run_pending();
                if (interruptible && g_sigint && sh.interactive)
                    return -1;
                continue;
            }
            /* ECHILD：已经没有子进程了（不应该发生），把剩下的当作结束 */
            for (int i = 0; i < j->nprocs; i++)
                if (j->procs[i].state == P_RUN)
                    j->procs[i].state = P_DONE;
            break;
        }
        update_proc(pid, ws);
    }
    return 0;
}

/* ======================= 显示 ======================= */

static job *current_job(int which)      /* 0 = 当前(+)，1 = 上一个(-) */
{
    job *best = NULL, *second = NULL;
    for (job *j = jobs ? jobs : inherited; j; j = j->next) {
        if (!best || j->seq > best->seq) {
            second = best;
            best = j;
        } else if (!second || j->seq > second->seq) {
            second = j;
        }
    }
    return which == 0 ? best : second;
}

static char job_mark(const job *j)
{
    if (j == current_job(0))
        return '+';
    if (j == current_job(1))
        return '-';
    return ' ';
}

static void state_text(const job *j, strbuf *out)
{
    if (job_running(j)) {
        sb_puts(out, "运行中");
        return;
    }
    if (job_stopped(j)) {
        int sig = SIGTSTP;
        for (int i = 0; i < j->nprocs; i++)
            if (j->procs[i].state == P_STOP)
                sig = WSTOPSIG(j->procs[i].status);
        sb_puts(out, sig == SIGTTIN ? "已停止（等待终端输入）" : sig == SIGTTOU ? "已停止（等待终端输出）" : "已停止");
        return;
    }
    int ws = j->procs[j->nprocs - 1].status;
    if (WIFSIGNALED(ws)) {
        sb_puts(out, sig_desc(WTERMSIG(ws)));
        if (WCOREDUMP(ws))
            sb_puts(out, "（核心已转储）");
    } else if (WEXITSTATUS(ws) == 0) {
        sb_puts(out, "已完成");
    } else {
        sb_printf(out, "退出 %d", WEXITSTATUS(ws));
    }
}

/* 形如 "[1]+  已停止                  sleep 10"。
 * jobs 命令的输出写到 stdout；异步的状态通知（已停止、已完成）和 bash 一样写到 stderr。 */
static void print_job_to(const job *j, int with_pid, int amp, FILE *out)
{
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "[%d]%c  ", j->id, job_mark(j));
    if (with_pid)
        sb_printf(&b, "%d ", (int)j->pgid);
    size_t start = b.len;
    state_text(j, &b);
    int w = str_width(b.s + start, b.len - start);
    for (; w < 24; w++)
        sb_putc(&b, ' ');
    sb_putc(&b, ' ');
    sb_puts(&b, j->cmd);
    if (amp && job_running(j))
        sb_puts(&b, " &");
    sb_putc(&b, '\n');
    fflush(stdout);
    fputs(sb_str(&b), out);
    fflush(out);
    sb_free(&b);
}

static void print_job(const job *j, int with_pid, int amp)
{
    print_job_to(j, with_pid, amp, stdout);
}

static void notify_job(const job *j)
{
    print_job_to(j, 0, 0, stderr);
}

/* ======================= fork 前后 ======================= */

void jobs_child_reset(void)
{
    /* 子进程拿到的是父进程作业表的副本。那些进程不是它的孩子（不能 wait 它们），
     * 所以移到 inherited 里，只留给 jobs 命令显示（这样 jobs | grep sleep 才有结果） */
    if (jobs) {
        while (inherited) {
            job *j = inherited;
            inherited = j->next;
            job_free(j);
        }
        inherited = jobs;
        jobs = NULL;
    }
    fg_job = NULL;
    sh.jobctl = 0;
    sh.interactive = 0;
    if (sh.tty_fd >= 0) {
        close(sh.tty_fd);
        sh.tty_fd = -1;
    }
}

void job_child_setup(job *j, int foreground)
{
    if (sh.jobctl) {
        pid_t me = getpid();
        pid_t pg = j->pgid ? j->pgid : me;
        /* 父子进程都调用 setpgid/tcsetpgrp：谁先运行都不会出现竞争 */
        setpgid(0, pg);
        if (foreground && sh.tty_fd >= 0)
            tcsetpgrp(sh.tty_fd, pg);
    }
    jobs_child_reset();
}

void job_add_pid(job *j, pid_t pid)
{
    if (!j->pgid)
        j->pgid = pid;
    if (sh.jobctl)
        setpgid(pid, j->pgid);      /* 子进程已经 exec 时会失败(EACCES)，没关系 */
    if (j->nprocs == j->cap) {
        j->cap = j->cap ? j->cap * 2 : 4;
        j->procs = xrealloc(j->procs, sizeof(proc) * (size_t)j->cap);
    }
    j->procs[j->nprocs].pid = pid;
    j->procs[j->nprocs].status = 0;
    j->procs[j->nprocs].state = P_RUN;
    j->nprocs++;
}

/* ======================= 前台 / 后台 ======================= */

int job_wait_fg(job *j)
{
    fg_job = j;
    if (sh.jobctl && sh.tty_fd >= 0)
        tcsetpgrp(sh.tty_fd, j->pgid);
    wait_job(j, 0);
    fg_job = NULL;
    int st = job_status(j);
    int last_ws = j->procs[j->nprocs - 1].status;
    if (sh.jobctl && sh.tty_fd >= 0) {
        tcsetpgrp(sh.tty_fd, sh.pgid);      /* 把终端拿回来 */
        if (job_stopped(j)) {
            tcgetattr(sh.tty_fd, &j->tmodes);
            j->have_tmodes = 1;
            if (sh.have_tmodes)
                tcsetattr(sh.tty_fd, TCSADRAIN, &sh.tmodes);
        } else if (WIFSIGNALED(last_ws)) {
            if (sh.have_tmodes)
                tcsetattr(sh.tty_fd, TCSADRAIN, &sh.tmodes);
        } else if (tcgetattr(sh.tty_fd, &sh.tmodes) == 0) {
            sh.have_tmodes = 1;     /* 正常结束：保留它做的终端设置（例如 stty） */
        }
    }
    if (job_stopped(j)) {
        j->bg = 0;
        job_register(j);
        j->notify = 0;
        fputs("\n", stderr);
        notify_job(j);
        if (sh.interactive)
            sh.abort = 1;       /* 被 Ctrl-Z 停止：这一行剩下的命令不再执行 */
        return st;
    }
    if (sh.interactive && WIFSIGNALED(last_ws)) {
        int sig = WTERMSIG(last_ws);
        fflush(stdout);
        if (sig == SIGINT) {
            fputs("\n", stderr);
            sh.abort = 1;       /* 前台命令被 Ctrl-C 打断：整行都放弃 */
        } else if (sig != SIGPIPE) {
            fprintf(stderr, "%s%s\n", sig_desc(sig), WCOREDUMP(last_ws) ? "（核心已转储）" : "");
        }
    }
    if (job_listed(j))
        job_unlink(j);
    job_free(j);
    return st;
}

void job_put_bg(job *j)
{
    j->bg = 1;
    job_register(j);
    if (sh.interactive) {
        fflush(stdout);
        fprintf(stderr, "[%d] %d\n", j->id, (int)j->procs[j->nprocs - 1].pid);
    }
    /* 非交互 shell 里大量后台作业结束后要清理，避免作业表无限增长 */
    if (!sh.interactive) {
        reap_nohang();
        int count = 0;
        for (job *k = jobs; k; k = k->next)
            count++;
        if (count > 1000) {
            for (job **pp = &jobs; *pp;) {
                job *k = *pp;
                if (job_done(k) && k != j) {
                    *pp = k->next;
                    job_free(k);
                } else {
                    pp = &k->next;
                }
            }
        }
    }
}

void jobs_notify(void)
{
    reap_nohang();
    for (job **pp = &jobs; *pp;) {
        job *j = *pp;
        if (job_done(j)) {
            if (sh.interactive)
                notify_job(j);
            *pp = j->next;
            job_free(j);
            continue;
        }
        if (j->notify && job_stopped(j) && sh.interactive)
            notify_job(j);
        j->notify = 0;
        pp = &j->next;
    }
}

int jobs_stopped_count(void)
{
    int n = 0;
    for (job *j = jobs; j; j = j->next)
        if (job_stopped(j))
            n++;
    return n;
}

int jobs_count(void)
{
    int n = 0;
    for (job *j = jobs; j; j = j->next)
        if (!job_done(j))
            n++;
    return n;
}

void jobs_restore_terminal(void)
{
    if (sh.jobctl && sh.tty_fd >= 0 && sh.orig_pgid > 0 && sh.orig_pgid != sh.pgid)
        tcsetpgrp(sh.tty_fd, sh.orig_pgid);
}

/* 交互式 shell 启动时：确认自己在前台，然后自立一个进程组并接管终端 */
void jobs_init(void)
{
    sh.jobctl = 0;
    sh.tty_fd = -1;
    int fd = -1;
    if (isatty(STDIN_FILENO))
        fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, SH_FD_BASE);
    else if (isatty(STDERR_FILENO))
        fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, SH_FD_BASE);
    if (fd < 0)
        return;
    /* 如果我们被放在后台启动（例如 sksh &），先给自己发 SIGTTIN 停下来，等用户 fg */
    for (;;) {
        pid_t fgp = tcgetpgrp(fd);
        if (fgp < 0) {
            sh_error_noloc("无法使用作业控制：没有控制终端（%s）", sh_strerror(errno));
            close(fd);
            return;
        }
        if (fgp == getpgrp())
            break;
        kill(-getpgrp(), SIGTTIN);
    }
    sh.orig_pgid = tcgetpgrp(fd);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    pid_t me = getpid();
    if (getpgrp() != me && setpgid(0, 0) < 0) {
        sh_error_noloc("无法使用作业控制：setpgid 失败（%s）", sh_strerror(errno));
        close(fd);
        return;
    }
    if (tcsetpgrp(fd, me) < 0) {
        sh_error_noloc("无法使用作业控制：tcsetpgrp 失败（%s）", sh_strerror(errno));
        close(fd);
        return;
    }
    sh.pgid = me;
    sh.tty_fd = fd;
    sh.jobctl = 1;
    if (tcgetattr(fd, &sh.tmodes) == 0)
        sh.have_tmodes = 1;
}

/* ======================= 作业说明符 %1 %+ %- %str %?str ======================= */

static job *job_lookup(const char *spec, const char *cmd)
{
    reap_nohang();
    if (!spec || !*spec || strcmp(spec, "%") == 0 || strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0) {
        job *j = current_job(0);
        if (!j)
            sh_error("%s: 当前没有作业", cmd);
        return j;
    }
    if (strcmp(spec, "%-") == 0) {
        job *j = current_job(1);
        if (!j)
            sh_error("%s: 没有上一个作业", cmd);
        return j;
    }
    const char *s = spec[0] == '%' ? spec + 1 : spec;
    char *end;
    long n = strtol(s, &end, 10);
    if (*s && !*end) {
        for (job *j = jobs; j; j = j->next)
            if (j->id == n)
                return j;
        sh_error("%s: %s: 没有这个作业", cmd, spec);
        return NULL;
    }
    if (spec[0] != '%') {
        sh_error("%s: %s: 没有这个作业", cmd, spec);
        return NULL;
    }
    int contains = s[0] == '?';
    if (contains)
        s++;
    job *found = NULL;
    for (job *j = jobs; j; j = j->next) {
        int m = contains ? strstr(j->cmd, s) != NULL : strncmp(j->cmd, s, strlen(s)) == 0;
        if (!m)
            continue;
        if (found) {
            sh_error("%s: %s: 有多个作业符合，说得更具体一些", cmd, spec);
            return NULL;
        }
        found = j;
    }
    if (!found)
        sh_error("%s: %s: 没有这个作业", cmd, spec);
    return found;
}

static void job_continue(job *j)
{
    for (int i = 0; i < j->nprocs; i++)
        if (j->procs[i].state == P_STOP)
            j->procs[i].state = P_RUN;
    j->notify = 0;
    if (sh.jobctl)
        kill(-j->pgid, SIGCONT);
    else
        for (int i = 0; i < j->nprocs; i++)
            kill(j->procs[i].pid, SIGCONT);
}

/* ======================= 内建命令 ======================= */

int bi_jobs(int argc, char **argv)
{
    int with_pid = 0, only_pid = 0, only_run = 0, only_stop = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'l': with_pid = 1; break;
            case 'p': only_pid = 1; break;
            case 'r': only_run = 1; break;
            case 's': only_stop = 1; break;
            default:
                sh_error("jobs: -%c: 无效的选项\n用法: jobs [-lprs] [作业...]", *o);
                return 2;
            }
        }
    }
    reap_nohang();
    int st = 0;
    if (i < argc) {
        for (; i < argc; i++) {
            job *j = job_lookup(argv[i], "jobs");
            if (!j) {
                st = 1;
                continue;
            }
            if (only_pid)
                printf("%d\n", (int)j->pgid);
            else
                print_job(j, with_pid, 1);
        }
        return st;
    }
    for (job *j = jobs ? jobs : inherited; j; j = j->next) {
        if ((only_run && !job_running(j)) || (only_stop && !job_stopped(j)))
            continue;
        if (only_pid)
            printf("%d\n", (int)j->pgid);
        else
            print_job(j, with_pid, 1);
        j->notify = 0;
    }
    fflush(stdout);
    if (!jobs)
        return 0;
    /* 已经报告过的“已完成”作业从表里删掉 */
    for (job **pp = &jobs; *pp;) {
        job *j = *pp;
        if (job_done(j) && !only_pid) {
            *pp = j->next;
            job_free(j);
        } else {
            pp = &j->next;
        }
    }
    return 0;
}

int bi_fg(int argc, char **argv)
{
    if (!sh.jobctl) {
        sh_error("fg: 没有作业控制（只有交互式终端里才能用）");
        return 1;
    }
    job *j = job_lookup(argc > 1 ? argv[1] : NULL, "fg");
    if (!j)
        return 1;
    if (job_done(j)) {
        sh_error("fg: 作业 %d 已经结束", j->id);
        return 1;
    }
    printf("%s\n", j->cmd);
    fflush(stdout);
    if (j->have_tmodes && sh.tty_fd >= 0)
        tcsetattr(sh.tty_fd, TCSADRAIN, &j->tmodes);
    j->bg = 0;
    /* 先把终端交给作业，再让它继续，否则它一醒来读终端就会收到 SIGTTIN */
    if (sh.tty_fd >= 0)
        tcsetpgrp(sh.tty_fd, j->pgid);
    job_continue(j);
    return job_wait_fg(j);
}

int bi_bg(int argc, char **argv)
{
    if (!sh.jobctl) {
        sh_error("bg: 没有作业控制（只有交互式终端里才能用）");
        return 1;
    }
    int st = 0;
    int n = argc > 1 ? argc - 1 : 1;
    for (int i = 0; i < n; i++) {
        job *j = job_lookup(argc > 1 ? argv[i + 1] : NULL, "bg");
        if (!j) {
            st = 1;
            continue;
        }
        if (job_running(j)) {
            sh_error("bg: 作业 %d 已经在后台运行", j->id);
            continue;
        }
        if (job_done(j)) {
            sh_error("bg: 作业 %d 已经结束", j->id);
            st = 1;
            continue;
        }
        j->bg = 1;
        job_continue(j);
        printf("[%d]%c %s &\n", j->id, job_mark(j), j->cmd);
        fflush(stdout);
    }
    return st;
}

int bi_wait(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        /* 等所有运行中的后台作业 */
        for (;;) {
            job *running = NULL;
            for (job *j = jobs; j; j = j->next)
                if (job_running(j)) {
                    running = j;
                    break;
                }
            if (!running)
                break;
            if (wait_job(running, 1) < 0) {
                g_sigint = 0;
                return 130;
            }
            if (g_trap_pending)
                traps_run_pending();
        }
        for (job **pp = &jobs; *pp;) {
            job *j = *pp;
            if (job_done(j)) {
                *pp = j->next;
                job_free(j);
            } else {
                pp = &j->next;
            }
        }
        return 0;
    }
    int st = 0;
    for (; i < argc; i++) {
        job *j = NULL;
        proc *pr = NULL;
        if (argv[i][0] == '%') {
            j = job_lookup(argv[i], "wait");
            if (!j) {
                st = 127;
                continue;
            }
        } else {
            char *end;
            long pid = strtol(argv[i], &end, 10);
            if (!*argv[i] || *end || pid <= 0) {
                sh_error("wait: '%s': 不是 pid 或合法的作业说明符", argv[i]);
                st = 2;
                continue;
            }
            reap_nohang();
            pr = find_proc((pid_t)pid, &j);
            if (!pr) {
                sh_error("wait: pid %ld 不是本 shell 的子进程", pid);
                st = 127;
                continue;
            }
        }
        if (wait_job(j, 1) < 0) {
            g_sigint = 0;
            return 130;
        }
        if (pr)
            st = status_from_wait(pr->status);
        else
            st = job_status(j);
        if (job_done(j)) {
            job_unlink(j);
            job_free(j);
        }
    }
    return st;
}

static void list_signals(void)
{
    int col = 0;
    for (int s = 1; s < 65; s++) {
        const char *n = sig_name(s);
        if (!n)
            continue;
        printf("%2d) SIG%-8s", s, n);
        if (++col % 5 == 0)
            putchar('\n');
    }
    if (col % 5)
        putchar('\n');
}

int bi_kill(int argc, char **argv)
{
    if (argc < 2) {
        sh_error("kill: 用法: kill [-s 信号 | -信号] pid | %%作业 ...  或  kill -l [信号]");
        return 2;
    }
    int sig = SIGTERM;
    int i = 1;
    if (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "-L") == 0) {
        if (argc == 2) {
            list_signals();
            return 0;
        }
        int st = 0;
        for (i = 2; i < argc; i++) {
            char *end;
            long n = strtol(argv[i], &end, 10);
            if (*argv[i] && !*end) {
                if (n > 128)
                    n -= 128;       /* kill -l $? 可以把 130 翻译成 INT */
                const char *name = sig_name((int)n);
                if (name)
                    printf("%s\n", name);
                else {
                    sh_error("kill: %s: 无效的信号", argv[i]);
                    st = 1;
                }
            } else {
                int s = sig_from_name(argv[i]);
                if (s > 0)
                    printf("%d\n", s);
                else {
                    sh_error("kill: %s: 无效的信号", argv[i]);
                    st = 1;
                }
            }
        }
        return st;
    }
    if (strcmp(argv[1], "-s") == 0 || strcmp(argv[1], "-n") == 0) {
        if (argc < 3) {
            sh_error("kill: %s 需要一个信号名", argv[1]);
            return 2;
        }
        sig = sig_from_name(argv[2]);
        i = 3;
    } else if (argv[1][0] == '-' && argv[1][1] && strcmp(argv[1], "--") != 0) {
        sig = sig_from_name(argv[1] + 1);
        i = 2;
    }
    if (sig < 0) {
        sh_error("kill: %s: 无效的信号", argv[i - 1][0] == '-' ? argv[i - 1] + 1 : argv[i - 1]);
        return 1;
    }
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        sh_error("kill: 需要指定 pid 或作业");
        return 2;
    }
    int st = 0;
    for (; i < argc; i++) {
        if (argv[i][0] == '%') {
            job *j = job_lookup(argv[i], "kill");
            if (!j) {
                st = 1;
                continue;
            }
            int r = sh.jobctl ? kill(-j->pgid, sig) : 0;
            if (!sh.jobctl)
                for (int k = 0; k < j->nprocs; k++)
                    if (j->procs[k].state != P_DONE)
                        r |= kill(j->procs[k].pid, sig);
            if (r < 0) {
                sh_error("kill: %s: %s", argv[i], sh_strerror(errno));
                st = 1;
            } else if (job_stopped(j) && (sig == SIGTERM || sig == SIGHUP)) {
                job_continue(j);    /* 停止的进程收不到 TERM，先唤醒它 */
            }
            continue;
        }
        char *end;
        long pid = strtol(argv[i], &end, 10);
        if (!*argv[i] || *end) {
            sh_error("kill: %s: 参数必须是进程号或作业号", argv[i]);
            st = 1;
            continue;
        }
        if (kill((pid_t)pid, sig) < 0) {
            sh_error("kill: (%ld) - %s", pid, sh_strerror(errno));
            st = 1;
        }
    }
    return st;
}
