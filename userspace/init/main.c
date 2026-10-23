/*
 * main.c — sinit：SketchOS 的 1 号进程
 *
 * 内核把 initramfs 解压成内存里的根文件系统后，运行 /init（它是指向 /sbin/sinit 的符号链接），
 * 这个进程的 pid 就是 1。它是所有用户进程的祖先，整个“用户态世界”从这里开始。
 *
 * 开机流程（pid1_main）：
 *   保证 0/1/2 有效 → 挂载 /proc /sys /dev … → 打印里程碑和横幅
 *   → 主机名、时区、环境变量、回环网卡、接管 Ctrl+Alt+Del
 *   → 准备接收信号 → 按服务表启动服务 → 进入事件循环，永不退出
 *
 * 事件循环只等两类事情：信号（子进程退出、关机/重载请求）和定时器（延时启动的服务）。
 * 没有事件时 sinit 在 poll() 里睡觉，不占 CPU。
 *
 * 不是 1 号进程时（在 shell 里运行 sinit），它是一个小工具：查看版本、检查配置、
 * 或者给 1 号进程发信号请求关机/重启/重读配置。
 */
#define _GNU_SOURCE
#include "sinit.h"
#include "skutil.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ======================= 回收子进程 =======================
 * 进程退出后并不会立刻消失：内核会保留一小块记录（退出码、CPU 用时），
 * 等父进程调用 wait() 来“领取”。领取之前，它就是一个“僵尸进程”（ps 里状态为 Z）。
 * 如果父进程先退出了，这些孩子就成了“孤儿”，内核会把它们过继给 1 号进程。
 * 所以 1 号进程必须不停地 wait，否则僵尸会越积越多，最终耗尽进程号。
 *
 * 多个子进程几乎同时退出时，SIGCHLD 只会到达一次（同类的普通信号不排队），
 * 因此每次都要用 WNOHANG 循环 waitpid(-1)，直到没有可回收的为止。 */
void reap_children(void)
{
    for (;;) {
        int st;
        pid_t pid = waitpid(-1, &st, WNOHANG);
        if (pid > 0) {
            if (!svc_child_exited(pid, st)) {
                int code = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : st;
                sk_log("回收了孤儿进程 %d（退出状态 %d）", (int)pid, code);
            }
            continue;
        }
        if (pid < 0 && errno == EINTR)
            continue;
        break;    /* 0：还有孩子但都活着；-1 ECHILD：一个孩子都没有 */
    }
}

/* 关机流程走完、却没法断电（或者在停机）时，1 号进程就停在这里“待命”：
 * 它依然不能退出，也依然要回收进程；其它信号读出来后直接丢掉。 */
void sinit_idle_forever(void)
{
    for (;;) {
        int fd = sig_fd();
        struct pollfd p = { .fd = fd, .events = POLLIN };
        poll(fd >= 0 ? &p : NULL, fd >= 0 ? 1 : 0, fd >= 0 ? -1 : 1000);
        sig_event ev[16];
        sig_read(ev, 16);
        reap_children();
    }
}

/* ======================= 事件循环 ======================= */

/* 说明信号是谁发的：en=1 时给屏幕上的虚拟终端用英文（它的字体没有汉字） */
static const char *sender_desc(const sig_event *e, int en, char *buf, size_t n)
{
    if (e->sender > 0) {
        /* 发信号的进程（比如 kill 命令）可能已经退出了，那就读不到它的名字 */
        char comm[32], path[48];
        snprintf(path, sizeof path, "/proc/%d/comm", (int)e->sender);
        if (sk_read_line(path, comm, sizeof comm) == 0)
            snprintf(buf, n, en ? "from pid %d %s" : "来自 pid %d %s", (int)e->sender, comm);
        else
            snprintf(buf, n, en ? "from pid %d, which has already exited" : "来自 pid %d，它已经退出了",
                     (int)e->sender);
    } else if (e->signo == SIGINT) {
        snprintf(buf, n, "%s", en ? "from the kernel: Ctrl+Alt+Del was pressed" : "来自内核：有人按了 Ctrl+Alt+Del");
    } else {
        snprintf(buf, n, "%s", en ? "from the kernel" : "来自内核");
    }
    return buf;
}

static void __attribute__((noreturn)) event_loop(void)
{
    for (;;) {
        int fd = sig_fd();
        int timeout = svc_next_timeout(now_ms());
        if (fd < 0 && (timeout < 0 || timeout > 500))
            timeout = 500;           /* 没有信号通道时退而求其次：定期检查一下 */
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(fd >= 0 ? &p : NULL, fd >= 0 ? 1 : 0, timeout) < 0 && errno != EINTR) {
            sk_log("poll 失败: %s", sk_strerror(errno));
            struct timespec ts = { 0, 100 * 1000000L };
            nanosleep(&ts, NULL);    /* 防止出错时空转占满 CPU */
        }

        sig_event ev[32];
        int n = sig_read(ev, (int)SK_ARRAY_LEN(ev));
        int reload = 0;
        sd_kind request = SD_NONE;
        for (int i = 0; i < n; i++) {
            sd_kind k = SD_NONE;
            switch (ev[i].signo) {
            case SIGCHLD:                          break;   /* 下面统一回收 */
            case SIGHUP:  reload = 1;              break;
            case SIGTERM:
            case SIGINT:  k = SD_REBOOT;           break;
            case SIGUSR1: k = SD_HALT;             break;
            case SIGUSR2:
            case SIGPWR:  k = SD_POWEROFF;         break;
            default:                               break;
            }
            if (ev[i].signo != SIGCHLD) {
                char who[96];
                sk_log("收到 %s（%s）", sig_name(ev[i].signo), sender_desc(&ev[i], 0, who, sizeof who));
            }
            if (k != SD_NONE && request == SD_NONE) {
                char who[96], who_en[96], zh[256], en[256];
                request = k;
                sender_desc(&ev[i], 0, who, sizeof who);
                sender_desc(&ev[i], 1, who_en, sizeof who_en);
                snprintf(zh, sizeof zh, "\nsinit: 收到 %s（%s）\n", sig_name(ev[i].signo), who);
                snprintf(en, sizeof en, "\nsinit: got %s (%s)\n", sig_name(ev[i].signo), who_en);
                con_write_split(zh, en);
            }
        }

        reap_children();
        if (request != SD_NONE)
            sinit_shutdown(request);
        svc_run_timers(now_ms());
        if (reload)
            svc_reload();
    }
}

/* ======================= 作为 1 号进程运行 ======================= */

static void __attribute__((noreturn)) pid1_main(void)
{
    /* 内核给 1 号进程的 umask 是 0（新文件默认人人可写），改成常规的 022 */
    umask(022);
    sys_stdio_ensure();
    if (chdir("/") != 0) {
        /* 根目录总是存在的，这里不会失败 */
    }
    /* /init 是符号链接，内核按文件名把进程命名为 "init"；改成 sinit，ps 里看得更清楚 */
    prctl(PR_SET_NAME, "sinit", 0, 0, 0);

    /* 先挂载，再写日志：sk_log 要写 /dev/kmsg，而它在 devtmpfs 挂上之后才存在。
     * 写第一条日志之前，把 sinit 自己的 0/1/2 换成 /dev/null（原因见 sys_stdio_detach） */
    mount_early();
    sys_stdio_detach();
    sk_log_init("sinit");
    sk_log_console("SketchOS %s starting", SKETCHOS_VERSION);
    mount_early_report();

    long boot_cs = sys_uptime_centis();

    sys_disable_cad();
    char host[65];
    sys_set_hostname(host, sizeof host);
    char tz[64];
    sys_setup_env(tz, sizeof tz);
    sys_loopback_up();
    int gui = sys_gui_enabled();

    con_init();
    con_banner(host, tz, boot_cs);

    if (sig_setup() < 0)
        sinit_report("无法接收信号：只能定期检查子进程，关机信号将不起作用");

    sk_log("运行模式：%s", gui ? "图形桌面" : "文字界面 (sketch.nogui)");
    svc_init(gui);
    svc_load_and_boot();
    event_loop();
}

/* ======================= 作为普通命令运行 ======================= */

static void usage(FILE *out)
{
    fprintf(out,
        "sinit %s — SketchOS 的 1 号进程（init）\n"
        "\n"
        "开机时由内核启动（/init → /sbin/sinit），负责挂载文件系统、按\n"
        SINIT_CONF " 启动并看护服务、回收孤儿进程、关机重启。\n"
        "\n"
        "在 shell 里可以这样用：\n"
        "  sinit --version          显示版本\n"
        "  sinit --help             显示这个帮助\n"
        "  sinit --check [文件]     检查服务表写得对不对（默认 " SINIT_CONF "）\n"
        "  sinit reload             让 1 号进程重新读取服务表，启动新增的服务（= kill -HUP 1）\n"
        "  sinit reboot             重启（= kill -TERM 1，也可以按 Ctrl+Alt+Del）\n"
        "  sinit halt               停机（= kill -USR1 1）\n"
        "  sinit poweroff           关机（= kill -USR2 1）\n"
        "\n"
        "服务状态在 " SINIT_STATUS "（svc 命令会读它），日志用 dmesg 查看。\n",
        SKETCHOS_VERSION);
}

static int send_to_init(int sig, const char *what)
{
    /* 安全检查：只给 sinit 发。比如在 WSL 里误运行 "sinit reboot"，
     * 1 号进程是 WSL 自己的 init，可不能给它发关机信号。 */
    char comm[32] = "?";
    if (sk_read_line("/proc/1/comm", comm, sizeof comm) != 0 || strcmp(comm, "sinit") != 0) {
        fprintf(stderr, "sinit: 1 号进程是 \"%s\" 而不是 sinit，不发送信号\n", comm);
        return 1;
    }
    /* 先打印再发信号：关机/重启时 sinit 会立刻给所有进程（也包括我们自己）发 SIGTERM，
     * 发完信号再打印的话，这句话可能永远没机会出现 */
    printf("请求 1 号进程%s（发送 %s）\n", what, sig_name(sig));
    fflush(stdout);
    if (kill(1, sig) != 0) {
        fprintf(stderr, "sinit: 无法给 1 号进程发信号: %s\n", sk_strerror(errno));
        return 1;
    }
    return 0;
}

static int cli_main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        fprintf(stderr, "\n（sinit 现在不是 1 号进程，pid = %d）\n", (int)getpid());
        return 1;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
        printf("sinit (%s) %s\n", SKETCHOS_NAME, SKETCHOS_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(cmd, "--check"))
        return svc_check_main(argc > 2 ? argv[2] : SINIT_CONF);
    if (!strcmp(cmd, "reload"))
        return send_to_init(SIGHUP, "重新读取服务表");
    if (!strcmp(cmd, "reboot"))
        return send_to_init(SIGTERM, "重启");
    if (!strcmp(cmd, "halt"))
        return send_to_init(SIGUSR1, "停机");
    if (!strcmp(cmd, "poweroff"))
        return send_to_init(SIGUSR2, "关机");
    fprintf(stderr, "sinit: 不认识的参数 \"%s\"\n\n", cmd);
    usage(stderr);
    return 1;
}

int main(int argc, char **argv)
{
    if (getpid() != 1)
        return cli_main(argc, argv);
    /* 1 号进程：内核传给 init 的命令行参数（如 "single"）我们不使用 */
    g_is_pid1 = 1;
    pid1_main();
}
