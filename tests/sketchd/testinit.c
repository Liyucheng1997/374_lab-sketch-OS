/*
 * testinit — 测试 sketchd 用的极简 1 号进程（只用于 tests/sketchd，真正的系统用 sinit）
 *
 * 做的事：挂载 /proc /sys /dev /tmp /run；在 tty1 上启动 /bin/sketchd（退出后自动重启，
 * 和 services.conf 里的 respawn 一样）；回收僵尸进程；把收到的关机/重启信号打印出来
 * （内核命令行带 testinit.noreboot 时只打印、不真的重启，方便测试继续）；
 * 在串口 ttyS0 上提供几个调试命令（qemu_harness 的 run_serial 能用）：
 *     ls 目录 | cat 文件 | ps | kill 信号 进程名或pid | run 命令 参数… | vt | log
 * 命令可以带 qemu_harness 追加的 “; echo 标记”。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int   g_ser = -1;
static pid_t g_skd = -1;
static int   g_noreboot;
static int   g_respawn_ms = 1000;
static long long g_respawn_at = -1;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1)
        n = sizeof buf - 1;
    if (n > 0 && write(g_ser >= 0 ? g_ser : 1, buf, (size_t)n) < 0) {
    }
}

static int cmdline_has(const char *key, char *val, size_t n)
{
    char buf[4096];
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0)
        return 0;
    buf[r] = 0;
    size_t kl = strlen(key);
    for (char *p = strtok(buf, " \n"); p; p = strtok(NULL, " \n"))
        if (!strncmp(p, key, kl) && (p[kl] == 0 || p[kl] == '=')) {
            if (val)
                snprintf(val, n, "%s", p[kl] == '=' ? p + kl + 1 : "");
            return 1;
        }
    return 0;
}

static void start_sketchd(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setsid();
        int tty = open("/dev/tty1", O_RDWR);
        if (tty >= 0) {
            ioctl(tty, TIOCSCTTY, 1);
            dup2(tty, 0);
            if (tty > 0)
                close(tty);
        }
        int log = open("/var/log/sketchd.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log >= 0) {
            dup2(log, 1);
            dup2(log, 2);
            if (log > 2)
                close(log);
        }
        execl("/bin/sketchd", "sketchd", (char *)NULL);
        _exit(127);
    }
    g_skd = pid;
    out("testinit: started sketchd pid=%d\n", (int)pid);
}

static void spawn(char **argv)
{
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setsid();
        int con = open("/dev/console", O_RDWR | O_NOCTTY);
        if (con >= 0) {
            dup2(con, 0);
            dup2(con, 1);
            dup2(con, 2);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    out("spawned pid=%d\n", (int)pid);
}

static pid_t pid_of(const char *name)
{
    if (name[0] >= '0' && name[0] <= '9')
        return (pid_t)atoi(name);
    DIR *d = opendir("/proc");
    struct dirent *de;
    pid_t found = -1;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9')
            continue;
        char p[300], comm[64] = "";
        snprintf(p, sizeof p, "/proc/%s/comm", de->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t r = read(fd, comm, sizeof comm - 1);
        close(fd);
        if (r > 0) {
            comm[r] = 0;
            comm[strcspn(comm, "\n")] = 0;
            if (!strcmp(comm, name))
                found = (pid_t)atoi(de->d_name);
        }
    }
    if (d)
        closedir(d);
    return found;
}

static void run_command(char *line)
{
    char *marker = NULL;
    char *e = strstr(line, "; echo ");
    if (e) {
        *e = 0;
        marker = e + 7;
    }
    char *argv[32];
    int argc = 0;
    for (char *p = strtok(line, " \t\r\n"); p && argc < 31; p = strtok(NULL, " \t\r\n"))
        argv[argc++] = p;
    argv[argc] = NULL;
    if (argc == 0) {
    } else if (!strcmp(argv[0], "ls") && argc > 1) {
        DIR *d = opendir(argv[1]);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.')
                continue;
            char p[512];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", argv[1], de->d_name);
            if (stat(p, &st) == 0)
                out("%s %lld\n", de->d_name, (long long)st.st_size);
        }
        if (d)
            closedir(d);
        else
            out("ls: %s\n", strerror(errno));
    } else if (!strcmp(argv[0], "cat") && argc > 1) {
        int fd = open(argv[1], O_RDONLY);
        char buf[1024];
        ssize_t r;
        while (fd >= 0 && (r = read(fd, buf, sizeof buf)) > 0)
            if (write(g_ser, buf, (size_t)r) < 0)
                break;
        if (fd >= 0)
            close(fd);
        else
            out("cat: %s\n", strerror(errno));
    } else if (!strcmp(argv[0], "ps")) {
        DIR *d = opendir("/proc");
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] < '1' || de->d_name[0] > '9')
                continue;
            char p[300], comm[64] = "";
            snprintf(p, sizeof p, "/proc/%s/comm", de->d_name);
            int fd = open(p, O_RDONLY);
            if (fd < 0)
                continue;
            ssize_t r = read(fd, comm, sizeof comm - 1);
            close(fd);
            if (r > 0) {
                comm[r] = 0;
                out("%s %s", de->d_name, comm);
            }
        }
        if (d)
            closedir(d);
    } else if (!strcmp(argv[0], "kill") && argc > 2) {
        pid_t p = pid_of(argv[2]);
        int sig = atoi(argv[1]);
        if (p > 0 && kill(p, sig) == 0)
            out("killed %d with %d\n", (int)p, sig);
        else
            out("kill failed\n");
    } else if (!strcmp(argv[0], "run") && argc > 1) {
        spawn(argv + 1);
    } else if (!strcmp(argv[0], "vt")) {
        int fd = open("/dev/tty1", O_RDWR | O_NOCTTY);
        int mode = -1, kb = -1;
        if (fd >= 0) {
            ioctl(fd, KDGETMODE, &mode);
            ioctl(fd, KDGKBMODE, &kb);
            close(fd);
        }
        out("vt mode=%s kbmode=%d\n", mode == KD_TEXT ? "text" : mode == KD_GRAPHICS ? "graphics" : "?", kb);
    } else if (!strcmp(argv[0], "log")) {
        char cmd[] = "cat /var/log/sketchd.log";
        run_command(cmd);
    } else {
        out("unknown command: %s\n", argv[0]);
    }
    if (marker) {
        char clean[128];
        size_t k = 0;
        for (const char *p = marker; *p && *p != '\n' && *p != '\r' && k + 1 < sizeof clean; p++)
            if (*p != '\'')
                clean[k++] = *p;
        clean[k] = 0;
        out("%s\n", clean);
    }
}

int main(void)
{
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mkdir("/tmp", 01777);
    mkdir("/run", 0755);
    mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
    mount("tmpfs", "/run", "tmpfs", 0, "mode=0755");
    mkdir("/var", 0755);
    mkdir("/var/log", 0755);
    int con = open("/dev/console", O_RDWR);
    if (con >= 0) {
        dup2(con, 0);
        dup2(con, 1);
        dup2(con, 2);
    }
    setenv("PATH", "/bin:/sbin", 1);
    setenv("HOME", "/root", 1);
    setenv("USER", "root", 1);
    setenv("TERM", "linux", 1);
    setenv("LANG", "C.UTF-8", 1);
    char tz[64] = "CST-8";
    cmdline_has("sketch.tz", tz, sizeof tz);
    setenv("TZ", tz, 1);
    g_noreboot = cmdline_has("testinit.noreboot", NULL, 0);
    char v[32];
    if (cmdline_has("testinit.respawn", v, sizeof v))
        g_respawn_ms = atoi(v);

    g_ser = open("/dev/ttyS0", O_RDWR | O_NOCTTY);
    out("testinit: SketchOS sketchd test init starting\n");

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGHUP);
    sigprocmask(SIG_BLOCK, &set, NULL);
    int sfd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);   /* 非阻塞：下面的 while 读到 EAGAIN 就停 */
    /* 让内核把 Ctrl+Alt+Del 变成发给 1 号进程的 SIGINT（sketchd 也会自己发） */
    reboot(RB_DISABLE_CAD);

    if (!cmdline_has("sketch.nogui", NULL, 0))
        start_sketchd();
    out("testinit# ");

    char line[1024];
    size_t ll = 0;
    for (;;) {
        struct pollfd p[2] = { { sfd, POLLIN, 0 }, { g_ser, POLLIN, 0 } };
        int timeout = -1;
        if (g_respawn_at >= 0) {
            long long d = g_respawn_at - now_ms();
            timeout = d < 0 ? 0 : (int)d;
        }
        int r = poll(p, g_ser >= 0 ? 2 : 1, timeout);
        if (r < 0 && errno != EINTR)
            continue;
        if (g_respawn_at >= 0 && now_ms() >= g_respawn_at) {
            g_respawn_at = -1;
            start_sketchd();
        }
        if (p[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                if (si.ssi_signo == SIGCHLD) {
                    int st;
                    pid_t pid;
                    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
                        if (pid == g_skd) {
                            out("testinit: sketchd exited (status %d), respawning in %d ms\n",
                                WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st), g_respawn_ms);
                            g_skd = -1;
                            g_respawn_at = now_ms() + g_respawn_ms;
                        }
                    }
                } else {
                    const char *what = si.ssi_signo == SIGUSR2 ? "poweroff" :
                                       si.ssi_signo == SIGTERM ? "reboot" :
                                       si.ssi_signo == SIGINT ? "reboot (ctrl-alt-del)" : "hup";
                    out("testinit: got signal %u from pid %u: %s request\n", si.ssi_signo, si.ssi_pid, what);
                    if (!g_noreboot && si.ssi_signo != SIGHUP) {
                        sync();
                        reboot(si.ssi_signo == SIGUSR2 ? RB_POWER_OFF : RB_AUTOBOOT);
                    }
                }
            }
        }
        if (g_ser >= 0 && (p[1].revents & POLLIN)) {
            char c;
            while (read(g_ser, &c, 1) == 1) {
                if (c == '\n' || c == '\r') {
                    line[ll] = 0;
                    if (ll > 0)
                        run_command(line);
                    ll = 0;
                    out("testinit# ");
                    break;
                }
                if (ll + 1 < sizeof line)
                    line[ll++] = c;
            }
        }
    }
}
