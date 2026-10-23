/*
 * sigtest.c — userspace/init/signals.c 的单元测试（在 WSL 里直接运行，不需要 QEMU）
 *
 * signals.c 有两条路：优先用 signalfd，内核不支持时退回“自管道”。正常的内核都支持
 * signalfd，后备路径平时根本走不到。这里用 seccomp 过滤器让 signalfd 系统调用返回 ENOSYS
 * （假装内核没有这个功能），逼 sig_setup() 走后备路径，再检查两条路的行为一致。
 *
 * 编译（boot_test.py 的 unit 场景会自动做）：
 *   sk-cc -I userspace/common -I userspace/init tests/boot/helpers/sigtest.c userspace/init/signals.c
 * 输出每项检查一行 "ok ..." 或 "FAIL ..."，最后 "sigtest: N passed, M failed"。
 */
#define _GNU_SOURCE
#include "sinit.h"
#include "skutil.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* signals.c 用到的日志函数：这里换成写 stderr 的替身，免得往宿主机的内核日志里写东西 */
void sk_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("  [log] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void sk_log_console(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("  [console] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

const char *sk_strerror(int err)
{
    return strerror(err);
}

static int g_pass, g_fail;

static void check(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (cond)
        g_pass++;
    else
        g_fail++;
}

/* 让 signalfd / signalfd4 返回 ENOSYS，其它系统调用照常 */
static int block_signalfd(void)
{
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_I386, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_signalfd4, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_signalfd, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
    };
    struct sock_fprog prog = { .len = (unsigned short)SK_ARRAY_LEN(f), .filter = f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return -1;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}

static int has_signal(const sig_event *ev, int n, int sig)
{
    for (int i = 0; i < n; i++)
        if (ev[i].signo == sig)
            return 1;
    return 0;
}

/* 等信号到达后取出来（最多等 2 秒） */
static int collect(sig_event *ev, int max, int want)
{
    int n = 0;
    for (int tries = 0; tries < 20 && n < want; tries++) {
        struct pollfd p = { .fd = sig_fd(), .events = POLLIN };
        poll(&p, 1, 100);
        n += sig_read(ev + n, max - n);
    }
    return n;
}

static void status_line(const char *key, char *out, size_t n)
{
    snprintf(out, n, "?");
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return;
    char line[256];
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':') {
            char *v = line + kl + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            v[strcspn(v, "\n")] = 0;
            snprintf(out, n, "%s", v);
        }
    fclose(f);
}

/* 在一个子进程里测一种模式；fallback=1 时先禁用 signalfd */
static int run_mode(int fallback)
{
    pid_t pid = fork();
    if (pid != 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
    }
    const char *mode = fallback ? "自管道" : "signalfd";
    char what[160];
    if (fallback && block_signalfd() != 0) {
        printf("FAIL 无法安装 seccomp 过滤器: %s\n", strerror(errno));
        _exit(1);
    }
    int fd = sig_setup();
    snprintf(what, sizeof what, "[%s] sig_setup 返回有效的描述符", mode);
    check(fd >= 0 && fd == sig_fd(), what);

    char link[128] = "";
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    ssize_t ln = readlink(path, link, sizeof link - 1);
    link[ln > 0 ? ln : 0] = 0;
    snprintf(what, sizeof what, "[%s] 描述符类型正确 (%s)", mode, link);
    check(fallback ? strncmp(link, "pipe:", 5) == 0 : strstr(link, "signalfd") != NULL, what);
    snprintf(what, sizeof what, "[%s] 描述符带 FD_CLOEXEC（不会泄漏给服务）", mode);
    check((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, what);

    /* 自己给自己发两个信号 + 制造一次 SIGCHLD */
    kill(getpid(), SIGHUP);
    kill(getpid(), SIGUSR2);
    pid_t c = fork();
    if (c == 0)
        _exit(0);
    sig_event ev[16];
    int n = collect(ev, 16, 3);
    snprintf(what, sizeof what, "[%s] 收到 SIGHUP、SIGUSR2、SIGCHLD（共 %d 个事件）", mode, n);
    check(has_signal(ev, n, SIGHUP) && has_signal(ev, n, SIGUSR2) && has_signal(ev, n, SIGCHLD), what);
    waitpid(c, NULL, 0);
    if (!fallback) {
        int ok = 0;
        for (int i = 0; i < n; i++)
            if (ev[i].signo == SIGHUP && ev[i].sender == getpid())
                ok = 1;
        check(ok, "[signalfd] 能知道是谁发的信号 (ssi_pid)");
    }
    n = sig_read(ev, 16);
    snprintf(what, sizeof what, "[%s] 信号读完后再读返回 0（不阻塞）", mode);
    check(n == 0, what);

    /* sig_reset_in_child：子进程里恢复默认，屏蔽字清空 */
    int pfd[2];
    if (pipe(pfd) != 0)
        _exit(1);
    c = fork();
    if (c == 0) {
        sig_reset_in_child();
        char blk[64], cgt[64], ign[64], buf[256];
        status_line("SigBlk", blk, sizeof blk);
        status_line("SigCgt", cgt, sizeof cgt);
        status_line("SigIgn", ign, sizeof ign);
        int len = snprintf(buf, sizeof buf, "%s %s %s", blk, cgt, ign);
        if (write(pfd[1], buf, (size_t)len) < 0)
            _exit(1);
        _exit(0);
    }
    close(pfd[1]);
    char got[256] = "";
    ssize_t r = read(pfd[0], got, sizeof got - 1);
    got[r > 0 ? r : 0] = 0;
    close(pfd[0]);
    waitpid(c, NULL, 0);
    snprintf(what, sizeof what, "[%s] sig_reset_in_child 后屏蔽字、处理函数、忽略集合都清空 (%s)", mode, got);
    check(strcmp(got, "0000000000000000 0000000000000000 0000000000000000") == 0, what);

    printf("sigtest[%s]: %d passed, %d failed\n", mode, g_pass, g_fail);
    _exit(g_fail ? 1 : 0);
}

int main(void)
{
    /* 不缓冲：下面会 fork 和 _exit，缓冲区里没写出去的内容会被复制或丢掉 */
    setvbuf(stdout, NULL, _IONBF, 0);
    int a = run_mode(0);
    int b = run_mode(1);
    printf("sigtest: %s\n", a == 0 && b == 0 ? "ALL PASSED" : "SOME FAILED");
    return a == 0 && b == 0 ? 0 : 1;
}
