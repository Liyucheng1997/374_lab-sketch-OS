/*
 * kill — 给进程发送信号
 *
 * 名字叫 kill，其实它只是调用 kill(pid, sig) 系统调用“发一个信号”。
 * 收到信号的进程可以捕获它（执行自己的处理函数）、忽略它，或者执行默认动作（多数是结束进程）。
 * SIGKILL(9) 和 SIGSTOP(19) 是例外：进程无法捕获或忽略，所以 kill -9 一定能杀死进程
 * （除非它处在 D 状态，正卡在内核里等硬件）。
 * pid 写成负数（如 -123）时，信号发给整个进程组 123。
 */
#include "skbox.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(kill, "给进程发送信号（默认结束进程）")

static const char usage[] =
    "用法: kill [-s 信号 | -信号] 进程号...\n"
    "  或: kill -l [信号]\n"
    "给进程发送信号，默认是 TERM（请求进程结束）。\n\n"
    "  -s 信号    要发送的信号：名字（TERM、KILL、SIGHUP……）或编号（15、9……）\n"
    "  -信号      同上，例如 -9、-KILL、-SIGKILL\n"
    "  -l         列出所有信号；给出编号时显示名字，给出名字时显示编号\n"
    "进程号写成负数时（如 -- -123）表示整个进程组。\n"
    "常用信号：HUP(1) 挂起  INT(2) 中断  KILL(9) 强制杀死  TERM(15) 请求结束  STOP(19) 暂停  CONT(18) 继续\n";

static void list_signals(void)
{
    for (int s = 1; s < 32; s++) {
        const char *name = sk_signal_name(s);
        if (!name)
            continue;
        printf("%2d) SIG%-8s %s\n", s, name, sk_signal_desc(s));
    }
    printf("%d~%d) 实时信号 SIGRTMIN ~ SIGRTMAX\n", SIGRTMIN, SIGRTMAX);
}

static int parse_signal(const char *s)
{
    int sig = sk_signal_from_name(s);
    if (sig < 0)
        sb_err("无效的信号 '%s'（用 kill -l 查看全部信号）", s);
    return sig;
}

int applet_kill(int argc, char **argv)
{
    int sig = SIGTERM, i = 1;
    if (i < argc && strcmp(argv[i], "--help") == 0) {
        fputs(usage, stdout);
        return 0;
    }
    /* 手工解析：-9、-KILL 这种写法 getopt 处理不了 */
    if (i < argc && (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-L") == 0)) {
        if (i + 1 >= argc) {
            list_signals();
            return 0;
        }
        int rc = 0;
        for (i++; i < argc; i++) {
            const char *a = argv[i];
            if (isdigit((unsigned char)a[0])) {
                /* 数字：显示名字。也接受“退出状态”写法（128+信号号） */
                int v = atoi(a);
                if (v > 128)
                    v -= 128;
                const char *name = sk_signal_name(v);
                if (name) {
                    puts(name);
                } else {
                    sb_err("无效的信号编号 '%s'", a);
                    rc = 1;
                }
            } else {
                int s = sk_signal_from_name(a);
                if (s < 0) {
                    sb_err("无效的信号名 '%s'", a);
                    rc = 1;
                } else {
                    printf("%d\n", s);
                }
            }
        }
        return rc;
    }
    if (i < argc && strcmp(argv[i], "-s") == 0) {
        if (i + 1 >= argc) {
            sb_err("选项需要一个参数 -- 's'");
            sb_hint();
            return 1;
        }
        if ((sig = parse_signal(argv[i + 1])) < 0)
            return 1;
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--") != 0) {
        if ((sig = parse_signal(argv[i] + 1)) < 0)
            return 1;
        i++;
    }
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        sb_err("缺少进程号");
        sb_hint();
        return 1;
    }
    int rc = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '%') {
            sb_err("%s: 作业号（%%N）只能在 shell 里使用", a);
            rc = 1;
            continue;
        }
        long long pid;
        if (sb_parse_ll(a, &pid) != 0) {
            sb_err("无效的进程号 '%s'", a);
            rc = 1;
            continue;
        }
        if (kill((pid_t)pid, sig) != 0) {
            sb_err("(%lld) - %s", pid, sk_strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
