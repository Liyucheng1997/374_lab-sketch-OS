/*
 * signals.c — 信号处置、trap 内建命令、信号名字表
 *
 * 信号是内核发给进程的“软件中断”：Ctrl-C 产生 SIGINT，kill 默认发 SIGTERM，
 * 子进程结束时父进程收到 SIGCHLD……每个信号有三种处置方式：
 *   SIG_DFL  默认动作（多数是终止进程，SIGTSTP 是停止进程）
 *   SIG_IGN  忽略
 *   处理函数  内核打断进程当前的执行，先去运行这个函数
 *
 * 交互式 shell 不能被 Ctrl-C 杀掉，但它启动的命令必须能被 Ctrl-C 杀掉。规则是：
 *   - execve 时，“处理函数”会被重置为 SIG_DFL（新程序里没有那个函数了），
 *     但 SIG_IGN 会被保留！所以 fork 之后、exec 之前，子进程要手动把信号恢复成默认。
 *   - shell 给 SIGINT 装一个只设置标志的处理函数（而不是直接忽略），这样
 *     正在阻塞的 read()/waitpid() 会返回 EINTR，内建命令 read、wait 可以被 Ctrl-C 打断。
 *   - 处理函数里只做最简单的事（设置 volatile sig_atomic_t 标志），真正的工作
 *     （运行 trap 命令）在主流程里检查标志后再做，因为信号可能在任何时刻打断程序。
 */
#include "sksh.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef NSIG
#define NSIG 65
#endif

volatile sig_atomic_t g_sigint;
volatile sig_atomic_t g_trap_pending;
static volatile sig_atomic_t pending[NSIG];
static char *trap_cmd[NSIG];    /* NULL：没有 trap；""：忽略 */
static char *trap_exit;

static const struct {
    int         sig;
    const char *name;
    const char *desc;
} sigtab[] = {
    { SIGHUP,    "HUP",    "挂断" },
    { SIGINT,    "INT",    "中断" },
    { SIGQUIT,   "QUIT",   "退出" },
    { SIGILL,    "ILL",    "非法指令" },
    { SIGTRAP,   "TRAP",   "跟踪/断点陷阱" },
    { SIGABRT,   "ABRT",   "已中止" },
    { SIGBUS,    "BUS",    "总线错误" },
    { SIGFPE,    "FPE",    "浮点数异常" },
    { SIGKILL,   "KILL",   "已杀死" },
    { SIGUSR1,   "USR1",   "用户定义信号 1" },
    { SIGSEGV,   "SEGV",   "段错误" },
    { SIGUSR2,   "USR2",   "用户定义信号 2" },
    { SIGPIPE,   "PIPE",   "管道已断开" },
    { SIGALRM,   "ALRM",   "闹钟" },
    { SIGTERM,   "TERM",   "已终止" },
    { SIGSTKFLT, "STKFLT", "协处理器栈错误" },
    { SIGCHLD,   "CHLD",   "子进程状态改变" },
    { SIGCONT,   "CONT",   "已继续" },
    { SIGSTOP,   "STOP",   "已停止（信号）" },
    { SIGTSTP,   "TSTP",   "已停止" },
    { SIGTTIN,   "TTIN",   "已停止（终端输入）" },
    { SIGTTOU,   "TTOU",   "已停止（终端输出）" },
    { SIGURG,    "URG",    "紧急 I/O 状况" },
    { SIGXCPU,   "XCPU",   "超出 CPU 时间限制" },
    { SIGXFSZ,   "XFSZ",   "超出文件大小限制" },
    { SIGVTALRM, "VTALRM", "虚拟定时器到期" },
    { SIGPROF,   "PROF",   "性能分析定时器到期" },
    { SIGWINCH,  "WINCH",  "窗口大小改变" },
    { SIGIO,     "IO",     "I/O 就绪" },
    { SIGPWR,    "PWR",    "电源故障" },
    { SIGSYS,    "SYS",    "错误的系统调用" },
};

const char *sig_name(int sig)
{
    for (size_t i = 0; i < SK_ARRAY_LEN(sigtab); i++)
        if (sigtab[i].sig == sig)
            return sigtab[i].name;
    return NULL;
}

const char *sig_desc(int sig)
{
    static char buf[32];
    for (size_t i = 0; i < SK_ARRAY_LEN(sigtab); i++)
        if (sigtab[i].sig == sig)
            return sigtab[i].desc;
    snprintf(buf, sizeof buf, "信号 %d", sig);
    return buf;
}

int sig_count(void)
{
    return (int)SK_ARRAY_LEN(sigtab);
}

/* "INT"、"SIGINT"、"int"、"2" 都可以；"EXIT" 是 0（只用于 trap） */
int sig_from_name(const char *s)
{
    if (!s || !*s)
        return -1;
    if (isdigit((unsigned char)*s)) {
        char *end;
        long n = strtol(s, &end, 10);
        if (*end || n < 0 || n >= NSIG)
            return -1;
        return (int)n;
    }
    if (strncasecmp(s, "SIG", 3) == 0)
        s += 3;
    if (strcasecmp(s, "EXIT") == 0)
        return 0;
    for (size_t i = 0; i < SK_ARRAY_LEN(sigtab); i++)
        if (strcasecmp(s, sigtab[i].name) == 0)
            return sigtab[i].sig;
    return -1;
}

static void on_signal(int sig)
{
    if (sig <= 0 || sig >= NSIG)
        return;
    if (trap_cmd[sig] && trap_cmd[sig][0]) {
        pending[sig] = 1;
        g_trap_pending = 1;
    } else if (sig == SIGINT) {
        g_sigint = 1;
    }
}

/* 不设置 SA_RESTART：让阻塞中的 read/waitpid 返回 EINTR，主流程才有机会检查标志 */
static void set_handler(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void set_disp(int sig, void (*disp)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = disp;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

/* shell 自身对某个信号的“基本处置”（没有 trap 时） */
static void base_disposition(int sig)
{
    if (sh.interactive) {
        if (sig == SIGINT) {
            set_handler(SIGINT);
            return;
        }
        if (sig == SIGQUIT || sig == SIGTERM) {
            set_disp(sig, SIG_IGN);
            return;
        }
        if (sh.jobctl && (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU)) {
            set_disp(sig, SIG_IGN);
            return;
        }
    }
    set_disp(sig, SIG_DFL);
}

void signals_init(void)
{
    /* 如果父进程把 SIGCHLD 设成了忽略，内核会自动回收子进程，waitpid 就拿不到退出状态了 */
    set_disp(SIGCHLD, SIG_DFL);
    if (sh.interactive) {
        base_disposition(SIGINT);
        base_disposition(SIGQUIT);
        base_disposition(SIGTERM);
        if (sh.jobctl) {
            base_disposition(SIGTSTP);
            base_disposition(SIGTTIN);
            base_disposition(SIGTTOU);
        }
    }
    for (int s = 1; s < NSIG; s++)
        if (trap_cmd[s])
            set_disp(s, trap_cmd[s][0] ? on_signal : SIG_IGN);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

void signals_child_reset(int async_noninteractive)
{
    static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGTERM, SIGCHLD, SIGPIPE };
    for (size_t i = 0; i < SK_ARRAY_LEN(sigs); i++)
        if (!(trap_cmd[sigs[i]] && !trap_cmd[sigs[i]][0]))   /* 用户 trap '' 忽略的保持忽略 */
            set_disp(sigs[i], SIG_DFL);
    /* 子 shell 不继承 trap 命令（被忽略的信号除外） */
    for (int s = 1; s < NSIG; s++) {
        if (trap_cmd[s] && trap_cmd[s][0]) {
            free(trap_cmd[s]);
            trap_cmd[s] = NULL;
            set_disp(s, SIG_DFL);
        }
        pending[s] = 0;
    }
    free(trap_exit);
    trap_exit = NULL;
    if (async_noninteractive) {
        /* 没有作业控制时，后台命令要忽略 Ctrl-C，否则在前台按 Ctrl-C 会把它也杀掉 */
        set_disp(SIGINT, SIG_IGN);
        set_disp(SIGQUIT, SIG_IGN);
    }
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    g_sigint = 0;
    g_trap_pending = 0;
}

void traps_run_pending(void)
{
    static int running;
    if (running)
        return;
    running = 1;
    while (g_trap_pending) {
        g_trap_pending = 0;
        for (int s = 1; s < NSIG; s++) {
            if (!pending[s])
                continue;
            pending[s] = 0;
            if (!trap_cmd[s] || !trap_cmd[s][0])
                continue;
            int save = sh.last_status;
            char *cmd = xstrdup(trap_cmd[s]);
            exec_string(cmd, "trap");
            free(cmd);
            sh.last_status = save;
        }
    }
    running = 0;
}

void trap_exit_run(void)
{
    if (!trap_exit)
        return;
    char *cmd = trap_exit;
    trap_exit = NULL;       /* 防止 EXIT trap 里再调用 exit 时无限递归 */
    if (*cmd)
        exec_string(cmd, "trap");
    free(cmd);
}

int trap_exit_set(void)
{
    return trap_exit != NULL;
}

static void print_trap(const char *cmd, const char *name)
{
    strbuf b;
    sb_init(&b);
    sb_puts(&b, "trap -- ");
    shell_quote(&b, cmd);
    printf("%s %s\n", sb_str(&b), name);
    sb_free(&b);
}

int bi_trap(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i < argc && strcmp(argv[i], "-l") == 0) {
        for (size_t k = 0; k < SK_ARRAY_LEN(sigtab); k++)
            printf("%2d) SIG%s\n", sigtab[k].sig, sigtab[k].name);
        return 0;
    }
    if (i >= argc || strcmp(argv[i], "-p") == 0) {
        if (trap_exit)
            print_trap(trap_exit, "EXIT");
        for (int s = 1; s < NSIG; s++)
            if (trap_cmd[s] && sig_name(s))
                print_trap(trap_cmd[s], sig_name(s));
        return 0;
    }
    const char *action = argv[i];
    int first_sig = i + 1;
    /* trap INT（只有信号、没有动作）等同于 trap - INT */
    if (argc - i == 1 || (sig_from_name(action) >= 0 && isdigit((unsigned char)action[0]))) {
        action = "-";
        first_sig = i;
    }
    if (first_sig >= argc) {
        sh_error("trap: 用法: trap [-lp] ['命令' | '' | -] 信号...");
        return 2;
    }
    int st = 0;
    for (int k = first_sig; k < argc; k++) {
        int s = sig_from_name(argv[k]);
        if (s < 0) {
            sh_error("trap: %s: 无效的信号说明", argv[k]);
            st = 1;
            continue;
        }
        if (s == SIGKILL || s == SIGSTOP) {
            sh_error("trap: %s: 这个信号不能被捕获或忽略", argv[k]);
            st = 1;
            continue;
        }
        if (s == 0) {
            free(trap_exit);
            trap_exit = strcmp(action, "-") == 0 ? NULL : xstrdup(action);
            continue;
        }
        free(trap_cmd[s]);
        trap_cmd[s] = NULL;
        if (strcmp(action, "-") == 0) {
            base_disposition(s);
        } else {
            trap_cmd[s] = xstrdup(action);
            set_disp(s, *action ? on_signal : SIG_IGN);
        }
    }
    return st;
}
