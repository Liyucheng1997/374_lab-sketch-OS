/*
 * pty.c — 伪终端（pseudo terminal）和 shell 子进程
 *
 * ┌──────────────────────────── 伪终端是什么 ────────────────────────────┐
 *
 *   很久以前，“终端”是一台真正的机器（键盘 + 屏幕），用串口线连到主机。内核里管理串口终端的
 *   代码叫 tty 驱动，它在“线”和程序之间夹了一层**线路规程**（line discipline）：
 *     - 回显：你敲的字由它送回屏幕（程序根本没参与）；
 *     - 行编辑：规范模式下攒够一整行（按回车）才交给程序，退格键由它处理；
 *     - 信号：看到 Ctrl+C（0x03）就给“前台进程组”发 SIGINT，Ctrl+Z 发 SIGTSTP，Ctrl+\ 发 SIGQUIT；
 *     - 换行转换：程序写的 \n 变成 \r\n（ONLCR）。
 *   图形窗口里的终端没有串口线，于是内核提供“伪终端”：一对互相连通的设备文件，
 *
 *       主机端 (master)  /dev/ptmx 打开得到        从机端 (slave)  /dev/pts/N
 *       ── skterm 持有 ──   ⇄  线路规程  ⇄   ── shell 把它当作 0/1/2 和控制终端 ──
 *
 *   我们往主机端写的字节，就像是从键盘敲进来的；shell 往从机端写的输出，我们从主机端读到，
 *   解析后画在窗口里。shell 和它运行的程序完全不知道对面是个图形程序。
 *
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * 会话（session）、进程组和控制终端：
 *   - 一个终端窗口里的所有进程属于同一个“会话”，会话的首进程是 shell（setsid() 创建新会话）。
 *   - 会话可以有一个“控制终端”：就是我们的 pty 从机端（TIOCSCTTY）。
 *   - 会话里的进程再分成若干“进程组”（一个管道 ls | more 是一个进程组，也就是一个“作业”）。
 *     同一时刻只有一个是“前台进程组”（shell 用 tcsetpgrp 设置），Ctrl+C 产生的 SIGINT、
 *     改变窗口大小产生的 SIGWINCH 都只发给它；后台进程组想读终端会被 SIGTTIN 停住。
 */
#define _GNU_SOURCE
#include "skterm.h"

#include <skutil.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

void skt_set_winsize(skterm *s)
{
    if (s->master < 0 || !s->t)
        return;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)s->t->cols;
    ws.ws_row = (unsigned short)s->t->rows;
    ws.ws_xpixel = (unsigned short)(s->t->cols * s->cw);
    ws.ws_ypixel = (unsigned short)(s->t->rows * s->lh);
    /* 窗口大小保存在内核的 tty 结构里。尺寸真的变了时，内核会给这个终端的前台进程组
     * 发送 SIGWINCH（“窗口变了”），top 这样的全屏程序收到后用 TIOCGWINSZ 查询新尺寸、重画。 */
    if (ioctl(s->master, TIOCSWINSZ, &ws) < 0)
        sk_log("设置终端窗口大小失败：%s", sk_strerror(errno));
}

/* 子进程里出错：写到 stderr（已经是 pty 了，用户能在窗口里看到）然后退出 */
static void child_fail(const char *what, const char *arg)
{
    int e = errno;
    char msg[512];
    int n = snprintf(msg, sizeof msg, "skterm: %s%s%s：%s\r\n", what, arg ? " " : "", arg ? arg : "",
                     sk_strerror(e));
    if (n > 0) {
        ssize_t w = write(STDERR_FILENO, msg, (size_t)(n < (int)sizeof msg ? n : (int)sizeof msg - 1));
        (void)w;
    }
    _exit(e == ENOENT ? 127 : 126);
}

int skt_spawn(skterm *s, char *const argv[], const char *dir)
{
    /* 1. 打开主机端。posix_openpt = open("/dev/ptmx")，内核顺便新建一个 /dev/pts/N。
     *    O_NOCTTY：别让它成为 skterm 自己的控制终端（skterm 是 sketchd 用 setsid 启动的会话首进程，
     *    会话首进程打开终端设备时，默认会把它当成控制终端）。 */
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;
    /* grantpt 设置从机端的属主/权限，unlockpt 解开“锁”（新建的 pty 默认锁着，防止别人抢先打开） */
    if (grantpt(m) < 0 || unlockpt(m) < 0) {
        int e = errno;
        close(m);
        errno = e;
        return -1;
    }
    char name[64];
    if (ptsname_r(m, name, sizeof name) != 0) {
        int e = errno;
        close(m);
        errno = e;
        return -1;
    }
    /* 主机端：exec 时自动关闭（shell 不需要它），非阻塞（事件循环里读写，绝不能卡住界面） */
    if (fcntl(m, F_SETFD, FD_CLOEXEC) < 0 || fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK) < 0) {
        int e = errno;
        close(m);
        errno = e;
        return -1;
    }

    /* 2. 在 fork 之前就打开从机端，子进程直接继承这个 fd。
     *    这样从 fork 到 shell 运行起来的整个过程中，从机端始终有人打开着——否则主机端可能在
     *    子进程打开它之前就报告“挂断”（POLLHUP），我们会误以为 shell 已经退出。 */
    int sl = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (sl < 0) {
        int e = errno;
        close(m);
        errno = e;
        return -1;
    }
    struct termios tio;
    if (tcgetattr(sl, &tio) == 0) {
        /* IUTF8：告诉线路规程输入是 UTF-8，规范模式下退格键一次删掉一个完整的汉字（3 个字节），
         * 而不是只删最后一个字节留下乱码。VERASE=0x7F 与我们的退格键一致。 */
        tio.c_iflag |= IUTF8;
        tio.c_cc[VERASE] = 0x7F;
        if (tcsetattr(sl, TCSANOW, &tio) < 0)
            sk_log("设置终端属性失败：%s", sk_strerror(errno));
    }
    s->master = m;
    skt_set_winsize(s);

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(sl);
        close(m);
        s->master = -1;
        errno = e;
        return -1;
    }
    if (pid == 0) {
        /* ---- 子进程 ---- */
        /* setsid：新建一个会话，自己当首进程。这一步让 shell 脱离 skterm 的会话，
         * 才能拥有属于自己的控制终端。 */
        if (setsid() < 0)
            child_fail("setsid 失败", NULL);
        /* TIOCSCTTY：把 pty 从机端设为本会话的控制终端。从此：
         *   - 这个终端上的 Ctrl+C 会变成发给前台进程组的 SIGINT；
         *   - 终端挂断（skterm 关窗口）时，内核给会话首进程发 SIGHUP；
         *   - shell 可以用 tcsetpgrp 指定前台进程组（作业控制）。 */
        if (ioctl(sl, TIOCSCTTY, 0) < 0)
            child_fail("无法设置控制终端", name);
        /* 标准输入、输出、错误都接到从机端 */
        if (dup2(sl, 0) < 0 || dup2(sl, 1) < 0 || dup2(sl, 2) < 0)
            _exit(126);
        if (sl > 2)
            close(sl);
        /* 信号设置会被 exec 继承：恢复默认，确保 shell 里的程序能正常收到 SIGINT 等 */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        for (int sig = 1; sig < NSIG; sig++)
            signal(sig, SIG_DFL);
        /* 环境变量：告诉程序“对面是 xterm 兼容、支持 256 色的终端”（docs 4.5）。
         * COLUMNS/LINES 可能是从别处继承来的旧值，删掉，让程序用 TIOCGWINSZ 查询真实尺寸。 */
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        if (dir && chdir(dir) < 0)
            child_fail("无法进入目录", dir);
        execv(argv[0], argv);
        child_fail("无法运行", argv[0]);
    }

    /* ---- 父进程 ---- */
    close(sl);              /* 从机端交给子进程了；我们只用主机端 */
    s->child = pid;
    s->child_running = 1;
    return 0;
}

void skt_hangup(skterm *s)
{
    if (!s->child_running || s->child <= 0)
        return;
    /* 真实终端（以及 xterm）关闭时会“挂断”：内核给会话里的进程发 SIGHUP（hang up，
     * 这个名字来自调制解调器挂电话），默认动作是退出——所以关掉终端窗口，里面运行的程序也会结束。
     * shell 是会话首进程，它的进程号也是它的进程组号；前台作业（例如正在运行的 top）在
     * 另一个进程组里，用 tcgetpgrp 查出来，一起通知。kill 的参数取负数 = 发给整个进程组。
     * 再补一个 SIGCONT：被 Ctrl+Z 停住的进程要先醒过来，才能处理 SIGHUP。 */
    pid_t fg = s->master >= 0 ? tcgetpgrp(s->master) : -1;
    if (kill(-s->child, SIGHUP) < 0 && errno == ESRCH)
        kill(s->child, SIGHUP);
    kill(-s->child, SIGCONT);
    if (fg > 0 && fg != s->child) {
        kill(-fg, SIGHUP);
        kill(-fg, SIGCONT);
    }
}
