/*
 * killall — 按名字给进程发信号
 *
 * 内核不提供“按名字找进程”的系统调用，所以 killall 先遍历 /proc 找出名字相同的进程，
 * 再逐个调用 kill()。进程名（comm）最长 15 个字符，超长的名字只比较前 15 个字符。
 */
#include "skbox.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(killall, "按名字结束进程")

static const char usage[] =
    "用法: killall [-信号 | -s 信号] [-q] [-v] [-w] 名字...\n"
    "给所有叫这个名字的进程发送信号（默认 TERM）。\n\n"
    "  -s 信号  要发送的信号（名字或编号），也可以写成 -9、-KILL\n"
    "  -q       找不到进程时不报错\n"
    "  -v       显示给哪些进程发了信号\n"
    "  -w       等到这些进程全部退出后再返回\n"
    "  -l       列出所有信号名\n";

static int name_matches(const sk_procinfo *p, const char *name)
{
    if (strcmp(p->comm, name) == 0)
        return 1;
    /* comm 被截断到 15 个字符 */
    if (strlen(name) > 15 && strlen(p->comm) == 15 && strncmp(p->comm, name, 15) == 0)
        return 1;
    /* 也比较命令行第一个参数的文件名部分（脚本、改过名的程序） */
    if (p->cmdline[0]) {
        char first[256], base[256];
        sk_strlcpy(first, p->cmdline, sizeof first);
        char *sp = strchr(first, ' ');
        if (sp)
            *sp = 0;
        sb_basename(first, base, sizeof base);
        if (strcmp(base, name) == 0)
            return 1;
    }
    return 0;
}

int applet_killall(int argc, char **argv)
{
    int sig = SIGTERM, quiet = 0, verbose = 0, wait_exit = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "--help") == 0) {
            fputs(usage, stdout);
            return 0;
        }
        if (strcmp(a, "-l") == 0) {
            for (int s = 1; s < 32; s++)
                if (sk_signal_name(s))
                    printf("%s%c", sk_signal_name(s), s % 8 == 0 ? '\n' : ' ');
            putchar('\n');
            return 0;
        }
        if (strcmp(a, "-s") == 0) {
            if (i + 1 >= argc || (sig = sk_signal_from_name(argv[i + 1])) < 0) {
                sb_err("无效的信号 '%s'", i + 1 < argc ? argv[i + 1] : "");
                return 1;
            }
            i++;
            continue;
        }
        if (strspn(a + 1, "qvw") == strlen(a + 1)) {
            for (const char *p = a + 1; *p; p++) {
                if (*p == 'q') quiet = 1;
                if (*p == 'v') verbose = 1;
                if (*p == 'w') wait_exit = 1;
            }
            continue;
        }
        if ((sig = sk_signal_from_name(a + 1)) < 0) {
            sb_err("无效的选项或信号 '%s'", a);
            sb_hint();
            return 1;
        }
    }
    if (i >= argc) {
        sb_err("缺少进程名");
        sb_hint();
        return 1;
    }
    int n;
    sb_proc *procs = sb_list_procs(&n);
    if (!procs) {
        sb_perr("无法读取 /proc");
        return 1;
    }
    pid_t self = getpid();
    pid_t *killed = calloc((size_t)(n ? n : 1), sizeof *killed);
    int nkilled = 0, rc = 0;
    for (; i < argc; i++) {
        int hit = 0;
        for (int k = 0; k < n; k++) {
            sk_procinfo *p = &procs[k].info;
            if (p->pid == self || p->is_kernel_thread || !name_matches(p, argv[i]))
                continue;
            if (kill(p->pid, sig) != 0) {
                if (!quiet)
                    sb_err("%s(%d): %s", p->comm, (int)p->pid, sk_strerror(errno));
                continue;
            }
            hit = 1;
            if (verbose)
                printf("已向 %s(%d) 发送信号 %s\n", p->comm, (int)p->pid, sk_signal_name(sig) ? sk_signal_name(sig) : "?");
            if (killed)
                killed[nkilled++] = p->pid;
        }
        if (!hit) {
            if (!quiet)
                sb_err("没有找到进程 '%s'", argv[i]);
            rc = 1;
        }
    }
    free(procs);
    /* -w：每 0.1 秒检查一次，直到这些进程都消失。kill(pid, 0) 不发信号，只检查进程是否存在；
     * 已经退出、但父进程还没 wait() 的僵尸进程也算已结束 */
    while (wait_exit && nkilled > 0) {
        int alive = 0;
        for (int k = 0; k < nkilled; k++) {
            sk_procinfo pi;
            if (killed[k] && kill(killed[k], 0) == 0 && sk_read_proc(killed[k], &pi) == 0 && pi.state != 'Z')
                alive = 1;
            else
                killed[k] = 0;
        }
        if (!alive)
            break;
        struct timespec ts = { 0, 100000000L };
        nanosleep(&ts, NULL);
    }
    free(killed);
    return rc;
}
