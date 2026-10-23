/*
 * signals.c — 1 号进程怎样接收信号
 *
 * 信号是内核发给进程的“异步通知”：子进程退出了 (SIGCHLD)、有人要求关机 (SIGTERM)……
 * 传统做法是注册一个信号处理函数，但处理函数可能在程序执行到任何一行时插进来，
 * 里面几乎什么都不能安全地做（不能 malloc、不能 printf）。
 * 更好的办法是把信号变成“可以 read 的数据”，放进主循环里和其它事件一起按顺序处理：
 *
 *   方案一 signalfd（Linux 特有）：先用 sigprocmask 屏蔽这些信号，让它们只“挂起”不打断程序，
 *          再创建一个 signalfd；挂起的信号就可以从这个文件描述符里一条条 read 出来，
 *          还能知道是哪个进程发的。
 *   方案二 自管道 (self-pipe trick)：信号处理函数只做一件事——往一个管道里写一个字节；
 *          主循环 poll 这个管道。处理函数里只调用 write()，这是“异步信号安全”的。
 *
 * 优先用 signalfd，内核不支持时退回自管道。
 *
 * 1 号进程的特殊规则：内核不会把“默认动作是终止”的信号投递给 1 号进程（防止误杀 init），
 * 没有处理函数的信号会被直接丢弃。但“被屏蔽”的信号例外——它们总是先挂起，
 * 所以 signalfd 方案对 1 号进程同样有效。
 */
#define _GNU_SOURCE
#include "sinit.h"
#include "skutil.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

/* sinit 关心的信号 */
static const int k_signals[] = { SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2, SIGPWR };

static int g_sigfd = -1;          /* 方案一 */
static int g_pipe[2] = { -1, -1 };/* 方案二 */

static void pipe_handler(int signo)
{
    int saved = errno;            /* 处理函数不能弄乱被打断代码的 errno */
    unsigned char b = (unsigned char)signo;
    ssize_t r = write(g_pipe[1], &b, 1);
    (void)r;                      /* 管道满了也没关系：主循环反正会醒来去回收所有子进程 */
    errno = saved;
}

int sig_setup(void)
{
    sigset_t set;
    sigemptyset(&set);
    for (size_t i = 0; i < SK_ARRAY_LEN(k_signals); i++)
        sigaddset(&set, k_signals[i]);

    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
        sk_log("sigprocmask 失败: %s", sk_strerror(errno));
    g_sigfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    if (g_sigfd >= 0)
        return g_sigfd;

    sk_log_console("signalfd 不可用 (%s)，改用自管道接收信号", sk_strerror(errno));
    if (pipe2(g_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        sk_log_console("连管道都创建不了: %s", sk_strerror(errno));
        return -1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = pipe_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < SK_ARRAY_LEN(k_signals); i++)
        sigaction(k_signals[i], &sa, NULL);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    return g_pipe[0];
}

int sig_fd(void)
{
    return g_sigfd >= 0 ? g_sigfd : g_pipe[0];
}

int sig_read(sig_event *ev, int max)
{
    int n = 0;
    if (g_sigfd >= 0) {
        struct signalfd_siginfo si;
        while (n < max) {
            ssize_t r = read(g_sigfd, &si, sizeof si);
            if (r != (ssize_t)sizeof si) {
                if (r < 0 && errno == EINTR)
                    continue;
                break;            /* EAGAIN：读完了 */
            }
            ev[n].signo = (int)si.ssi_signo;
            ev[n].sender = (pid_t)si.ssi_pid;
            n++;
        }
    } else if (g_pipe[0] >= 0) {
        unsigned char buf[64];
        while (n < max) {
            ssize_t r = read(g_pipe[0], buf, (size_t)SK_MIN(max - n, (int)sizeof buf));
            if (r <= 0) {
                if (r < 0 && errno == EINTR)
                    continue;
                break;
            }
            for (ssize_t i = 0; i < r; i++) {
                ev[n].signo = buf[i];
                ev[n].sender = 0;
                n++;
            }
        }
    }
    return n;
}

/* fork 出来的子进程会继承父进程的“信号屏蔽字”和“忽略”设置，而且 execve 之后依然保留！
 * 如果不恢复，服务程序里 SIGTERM、SIGINT 都是被屏蔽的——按 Ctrl+C 没反应，关机时也杀不掉。
 * 这是写 init 最容易犯的错误之一。 */
void sig_reset_in_child(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (int s = 1; s < _NSIG; s++)
        sigaction(s, &sa, NULL);    /* SIGKILL/SIGSTOP 等会失败，无所谓 */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

const char *sig_name(int signo)
{
    switch (signo) {
    case SIGHUP:  return "SIGHUP";
    case SIGINT:  return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGSEGV: return "SIGSEGV";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    case SIGCHLD: return "SIGCHLD";
    case SIGPWR:  return "SIGPWR";
    default:      return "信号";
    }
}
