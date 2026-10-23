/*
 * sktest.c — sinit 开机测试用的“探针”程序（只放进测试用的根文件系统，不进正式系统）
 *
 * 它既可以当作服务被 sinit 启动（sleep / crash / stamp / info-sleep），
 * 也可以在串口 shell 里运行，检查系统状态（status / zombies / orphans / kill ...）。
 * 输出格式简单固定，方便 tests/boot/boot_test.py 用正则匹配。
 *
 * 用法：sktest <子命令> [参数...]，见 usage()。
 */
#define _GNU_SOURCE
#include "skutil.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static double uptime(void)
{
    double u = 0;
    sk_read_uptime(&u, NULL);
    return u;
}

/* 读 /proc/<pid>/stat 里 ')' 之后的字段：state ppid pgrp session tty_nr ... */
static int proc_ids(pid_t pid, int *ppid, int *pgrp, int *sid, int *tty)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    char *s = sk_read_file(path, NULL);
    if (!s)
        return -1;
    char *rp = strrchr(s, ')');
    char st;
    int r = rp ? sscanf(rp + 2, "%c %d %d %d %d", &st, ppid, pgrp, sid, tty) : 0;
    free(s);
    return r == 5 ? 0 : -1;
}

static void status_field(pid_t pid, const char *key, char *out, size_t n)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    sk_strlcpy(out, "?", n);
    char *s = sk_read_file(path, NULL);
    if (!s)
        return;
    size_t kl = strlen(key);
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n"))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':')
            sk_strlcpy(out, sk_trim(line + kl + 1), n);
    free(s);
}

static void print_ids(const char *label, pid_t pid)
{
    int ppid = 0, pgrp = 0, sid = 0, tty = 0;
    if (proc_ids(pid, &ppid, &pgrp, &sid, &tty) != 0) {
        printf("%s pid=%d 读取失败\n", label, (int)pid);
        return;
    }
    char blk[32], ign[32];
    status_field(pid, "SigBlk", blk, sizeof blk);
    status_field(pid, "SigIgn", ign, sizeof ign);
    printf("%s pid=%d ppid=%d pgrp=%d sid=%d tty_nr=%d sigblk=%s sigign=%s\n",
           label, (int)pid, ppid, pgrp, sid, tty, blk, ign);
}

static void cmd_info(void)
{
    print_ids("info", getpid());
    char cwd[256] = "?";
    if (!getcwd(cwd, sizeof cwd))
        sk_strlcpy(cwd, "?", sizeof cwd);
    mode_t m = umask(0);
    umask(m);
    printf("info cwd=%s umask=%04o\n", cwd, (unsigned)m);
    for (char **e = environ; e && *e; e++)
        printf("env %s\n", *e);
    for (int fd = 0; fd <= 2; fd++) {
        char p[32], t[256];
        snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(p, t, sizeof t - 1);
        t[n > 0 ? n : 0] = 0;
        printf("fd%d=%s\n", fd, t);
    }
    /* 列出所有打开的文件描述符：sinit 不应该把自己的 signalfd、控制台等“泄漏”给服务 */
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        char list[256] = "";
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!isdigit((unsigned char)e->d_name[0]) || atoi(e->d_name) == dirfd(d))
                continue;
            if (list[0])
                sk_strlcat(list, ",", sizeof list);
            sk_strlcat(list, e->d_name, sizeof list);
        }
        closedir(d);
        printf("info openfds=%s\n", list);
    }
    fflush(stdout);
}

/* 从 /run/sinit/status 找服务的 pid */
static pid_t svc_pid(const char *name)
{
    char *s = sk_read_file("/run/sinit/status", NULL);
    if (!s)
        return -1;
    pid_t pid = -1;
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
        char *f[5];
        int n = 0;
        for (char *p = line; n < 5;) {
            f[n++] = p;
            char *tab = strchr(p, '\t');
            if (!tab)
                break;
            *tab = 0;
            p = tab + 1;
        }
        if (n >= 3 && strcmp(f[0], name) == 0)
            pid = (pid_t)atoi(f[2]);
    }
    free(s);
    return pid;
}

static int sig_from_name(const char *s)
{
    static const struct { const char *n; int v; } t[] = {
        { "HUP", SIGHUP }, { "INT", SIGINT }, { "TERM", SIGTERM }, { "KILL", SIGKILL },
        { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "PWR", SIGPWR }, { "SEGV", SIGSEGV },
        { "STOP", SIGSTOP }, { "CONT", SIGCONT },
    };
    if (isdigit((unsigned char)s[0]))
        return atoi(s);
    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    for (size_t i = 0; i < SK_ARRAY_LEN(t); i++)
        if (strcmp(s, t[i].n) == 0)
            return t[i].v;
    return -1;
}

/* 制造 n 个孤儿：子进程 fork 出孙进程后立刻退出，孙进程于是被过继给 1 号进程；
 * 孙进程确认自己被过继后报告新的父进程号，再睡 secs 秒后退出——
 * 那时它的父进程就是 sinit，如果 sinit 不回收，它就会变成僵尸。 */
static int cmd_orphans(int n, int secs)
{
    int pfd[2];
    if (pipe(pfd) != 0)
        return 1;
    for (int i = 0; i < n; i++) {
        pid_t c = fork();
        if (c == 0) {
            pid_t g = fork();
            if (g == 0) {
                close(pfd[0]);
                pid_t orig = getppid();
                for (int k = 0; k < 500 && getppid() == orig; k++)
                    msleep(10);
                dprintf(pfd[1], "orphan %d ppid=%d\n", (int)getpid(), (int)getppid());
                close(pfd[1]);
                msleep(secs * 1000L);
                _exit(7);
            }
            _exit(0);
        }
        if (c > 0)
            waitpid(c, NULL, 0);
    }
    close(pfd[1]);
    char buf[4096];
    size_t used = 0;
    ssize_t r;
    while ((r = read(pfd[0], buf + used, sizeof buf - 1 - used)) > 0)
        used += (size_t)r;
    buf[used] = 0;
    fputs(buf, stdout);
    printf("orphans-created %d\n", n);
    return 0;
}

static int cmd_zombies(void)
{
    int n = 0, z = 0;
    sk_procinfo *list = sk_list_procs(&n);
    for (int i = 0; i < n; i++)
        if (list[i].state == 'Z') {
            printf("zombie %d ppid=%d comm=%s\n", (int)list[i].pid, (int)list[i].ppid, list[i].comm);
            z++;
        }
    free(list);
    printf("zombies=%d\n", z);
    return 0;
}

static void usage(void)
{
    fputs("sktest — sinit 开机测试探针\n"
          "  sleep 秒 | crash [退出码] | true | stamp 标签 毫秒 | args 参数... |\n"
          "  info | info-sleep | ignore-term | parentinfo | ids pid或服务名 |\n"
          "  orphans 个数 秒 | zombies | status |\n"
          "  svcpid 服务名 | kill 信号 pid或服务名 | append 文件 文本 | cat 文件 |\n"
          "  readlink 路径 | mode 路径 | exists pid | hostname\n", stderr);
}

int main(int argc, char **argv)
{
    /* 以 "closefds-init" 的名字被内核当作 1 号进程运行（rdinit=/bin/closefds-init）：
     * 模拟最糟糕的开机环境——0/1/2 都没打开、根目录里也没有 /dev/console——然后把 1 号进程
     * 的位置交给 sinit（execve 不改变 pid，sinit 依然是 1 号进程）。 */
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (!strcmp(base, "closefds-init")) {
        close(0);
        close(1);
        close(2);
        unlink("/dev/console");
        unlink("/dev/null");
        char *nargv[] = { "sinit", NULL };
        execv("/sbin/sinit", nargv);
        for (;;)
            pause();
    }
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *c = argv[1];
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!strcmp(c, "sleep") && argc > 2) {
        long s = atol(argv[2]);
        for (;;) {
            if (s <= 0)
                pause();
            msleep(SK_MIN(s, 100000L) * 1000L);
            s -= SK_MIN(s, 100000L);
            if (s <= 0)
                return 0;
        }
    }
    if (!strcmp(c, "crash"))
        return argc > 2 ? atoi(argv[2]) : 1;
    if (!strcmp(c, "true"))
        return 0;
    if (!strcmp(c, "stamp") && argc > 3) {
        printf("%s start %.2f pid=%d\n", argv[2], uptime(), (int)getpid());
        msleep(atol(argv[3]));
        printf("%s end %.2f\n", argv[2], uptime());
        return 0;
    }
    if (!strcmp(c, "args")) {
        for (int i = 2; i < argc; i++)
            printf("argv[%d]=%s\n", i - 1, argv[i]);
        return 0;
    }
    if (!strcmp(c, "info")) {
        cmd_info();
        return 0;
    }
    if (!strcmp(c, "info-sleep")) {
        cmd_info();
        for (;;)
            pause();
    }
    if (!strcmp(c, "ignore-term")) {
        /* 故意不理 SIGTERM 的“顽固进程”：测试关机时 sinit 会不会用 SIGKILL 收场 */
        signal(SIGTERM, SIG_IGN);
        for (;;)
            pause();
    }
    if (!strcmp(c, "parentinfo")) {
        print_ids("parent", getppid());
        return 0;
    }
    if (!strcmp(c, "ids") && argc > 2) {
        pid_t pid = isdigit((unsigned char)argv[2][0]) ? (pid_t)atoi(argv[2]) : svc_pid(argv[2]);
        print_ids("info", pid);
        return 0;
    }
    if (!strcmp(c, "orphans") && argc > 3)
        return cmd_orphans(atoi(argv[2]), atoi(argv[3]));
    if (!strcmp(c, "zombies"))
        return cmd_zombies();
    if (!strcmp(c, "status") || (!strcmp(c, "cat") && argc > 2)) {
        char *s = sk_read_file(!strcmp(c, "status") ? "/run/sinit/status" : argv[2], NULL);
        if (!s) {
            printf("cat: %s\n", strerror(errno));
            return 1;
        }
        fputs(s, stdout);
        free(s);
        return 0;
    }
    if (!strcmp(c, "svcpid") && argc > 2) {
        printf("svcpid %s=%d\n", argv[2], (int)svc_pid(argv[2]));
        return 0;
    }
    if (!strcmp(c, "kill") && argc > 3) {
        int sig = sig_from_name(argv[2]);
        pid_t pid = isdigit((unsigned char)argv[3][0]) ? (pid_t)atoi(argv[3]) : svc_pid(argv[3]);
        if (sig < 0 || pid <= 0) {
            printf("kill: 参数不对\n");
            return 1;
        }
        if (kill(pid, sig) != 0) {
            printf("kill: %s\n", strerror(errno));
            return 1;
        }
        printf("killed %d with %d\n", (int)pid, sig);
        return 0;
    }
    if (!strcmp(c, "append") && argc > 3) {
        FILE *f = fopen(argv[2], "a");
        if (!f) {
            printf("append: %s\n", strerror(errno));
            return 1;
        }
        fprintf(f, "%s\n", argv[3]);
        fclose(f);
        return 0;
    }
    if (!strcmp(c, "readlink") && argc > 2) {
        char t[256];
        ssize_t n = readlink(argv[2], t, sizeof t - 1);
        t[n > 0 ? n : 0] = 0;
        printf("link %s -> %s\n", argv[2], n > 0 ? t : "(不是符号链接)");
        return 0;
    }
    if (!strcmp(c, "mode") && argc > 2) {
        struct stat st;
        if (stat(argv[2], &st) != 0) {
            printf("mode %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        printf("mode %s=%04o\n", argv[2], (unsigned)(st.st_mode & 07777));
        return 0;
    }
    if (!strcmp(c, "exists") && argc > 2) {
        char p[64];
        snprintf(p, sizeof p, "/proc/%s", argv[2]);
        printf("exists %s=%s\n", argv[2], access(p, F_OK) == 0 ? "yes" : "no");
        return 0;
    }
    if (!strcmp(c, "hostname")) {
        char h[128] = "?";
        gethostname(h, sizeof h);
        printf("hostname=%s\n", h);
        return 0;
    }
    usage();
    return 2;
}
