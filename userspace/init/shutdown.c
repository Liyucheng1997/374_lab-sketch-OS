/*
 * shutdown.c — 关机、重启、停机
 *
 * 关机不是“直接断电”，而是一个有礼貌的过程：
 *   1. 不再重启任何服务
 *   2. 给所有进程发 SIGTERM（“请收拾好东西退出”），程序可以趁机保存数据、恢复终端
 *   3. 等一会儿（最多约 2 秒），一边等一边回收退出的进程
 *   4. 还不走的，发 SIGKILL 强制结束（SIGKILL 不能被捕获或忽略）
 *   5. sync() 把内核缓存里还没写到磁盘的数据写回去，卸载文件系统
 *   6. 调用 reboot() 系统调用，请内核重启 / 停机 / 关闭电源
 */
#define _GNU_SOURCE
#include "sinit.h"
#include "skutil.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

/* 回收子进程，直到一个子进程都不剩（返回 1）或者超时（返回 0）。
 * 为什么“没有子进程”就等于“所有进程都退出了”？因为任何进程的父进程退出后，
 * 它都会被过继给 1 号进程——所以系统里每一个用户进程，最终都是 sinit 的子孙；
 * 而 kill(-1, ...) 已经通知了全部进程，剩下的只要还活着，就一定会以“孩子”的身份回到我们这里。 */
static int reap_wait(int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        for (;;) {
            int st;
            pid_t p = waitpid(-1, &st, WNOHANG);
            if (p > 0) {
                svc_child_exited(p, st);
                continue;
            }
            if (p < 0 && errno == EINTR)
                continue;
            if (p < 0 && errno == ECHILD)
                return 1;
            break;                       /* p == 0：还有孩子活着 */
        }
        int64_t left = deadline - now_ms();
        if (left <= 0)
            return 0;
        int fd = sig_fd();
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        poll(fd >= 0 ? &pfd : NULL, fd >= 0 ? 1 : 0, left > 50 ? 50 : (int)left);
        sig_event ev[16];
        sig_read(ev, 16);                /* 关机过程中再来的关机请求就忽略了 */
    }
}

/* “挂断”服务用的终端，就像拔掉调制解调器的电话线：
 * 内核会给这个终端上的会话发 SIGHUP，并让它们之后对终端的读写都失败。
 * 为什么需要这一步？交互式 shell 习惯忽略 SIGTERM（免得 kill 0 把自己杀掉），
 * 但终端挂断时它一定会退出——终端都没了，再等用户输入也没有意义。
 * 挂断会让所有直接打开这个终端的文件失效，所以之后要重新打开控制台（con_init）。
 * （通过 /dev/console 打开的文件不受影响，这是内核为控制台特意留的后门。） */
static void hangup_ttys(void)
{
    const char *ttys[SVC_MAX];
    int n = svc_list_ttys(ttys, SVC_MAX);
    for (int i = 0; i < n; i++) {
        int fd = open(ttys[i], O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (isatty(fd) && ioctl(fd, TIOCVHANGUP, 0) != 0)
            sk_log("挂断 %s 失败: %s", ttys[i], sk_strerror(errno));
        close(fd);
    }
    con_init();
}

static int count_user_processes(void)
{
    int n = 0, count = 0;
    sk_procinfo *list = sk_list_procs(&n);
    for (int i = 0; i < n; i++)
        if (list[i].pid != 1 && !list[i].is_kernel_thread)
            count++;
    free(list);
    return count;
}

/* 让一个子进程去调用 reboot()。
 * 原因：如果内核的关机代码“执行完了却没真的断电”（比如模拟器没有实现 ACPI 关机），
 * reboot() 系统调用最后会让调用者退出 (do_exit)。调用者要是 1 号进程，内核就会 panic，
 * 屏幕上打出一大段吓人的崩溃信息。让子进程去调用，失败了也只是子进程退出，sinit 还能善后。 */
static const char *call_reboot(int cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        reboot(cmd);
        _exit(errno > 0 && errno < 256 ? errno : 255);   /* 用退出码把错误号带回给父进程 */
    }
    if (pid < 0) {
        reboot(cmd);
        return sk_strerror(errno);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(st) && WEXITSTATUS(st) != 0 && WEXITSTATUS(st) != 255)
        return sk_strerror(WEXITSTATUS(st));
    return "内核返回了，但机器没有响应";
}

void sinit_shutdown(sd_kind how)
{
    const char *word = how == SD_REBOOT ? "重启" : how == SD_HALT ? "停机" : "关机";
    const char *word_en = how == SD_REBOOT ? "rebooting" : how == SD_HALT ? "halting" : "powering off";

    svc_shutdown_begin();
    sk_log("开始%s", word);
    char zh[256], en[256];
    snprintf(zh, sizeof zh, "\n   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"
             "    sinit: 正在%s\n"
             "     - 通知所有进程退出 (SIGTERM)，挂断终端……\n", word);
    snprintf(en, sizeof en, "\n   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n"
             "    sinit: %s\n"
             "     - asking all processes to exit (SIGTERM), hanging up terminals ...\n", word_en);
    con_write_split(zh, en);

    /* kill(-1, 信号)：发给除了 1 号进程自己以外的所有进程。
     * 再补一个 SIGCONT：被 Ctrl+Z 暂停的进程要先“醒过来”，才能处理 SIGTERM。 */
    kill(-1, SIGTERM);
    kill(-1, SIGCONT);
    hangup_ttys();
    if (reap_wait(2000)) {
        CON_SAY("     - 所有进程都已退出\n", "     - all processes have exited\n");
    } else {
        int left = count_user_processes();
        CON_SAY("     - 还有 %d 个进程没有退出，强制结束 (SIGKILL)\n",
                "     - %d process(es) still running, killing them (SIGKILL)\n", left);
        kill(-1, SIGKILL);
        if (!reap_wait(2000)) {
            left = count_user_processes();
            CON_SAY("     - 仍有 %d 个进程卡在内核里无法结束，不等了\n",
                    "     - %d process(es) stuck in the kernel, not waiting any longer\n", left);
        }
    }
    svc_write_status();

    CON_SAY("     - 同步缓存、卸载内存文件系统\n", "     - syncing caches, unmounting memory file systems\n");
    sync();
    mount_teardown();
    sync();

    switch (how) {
    case SD_REBOOT:
        CON_SAY("    再见！正在重启……\n\n", "    Bye! Rebooting ...\n\n");
        {
            const char *why = call_reboot(RB_AUTOBOOT);
            CON_SAY("    重启失败: %s\n", "    reboot failed (%s)\n", why);
        }
        break;
    case SD_HALT:
        /* 停机：CPU 停下来，但不断电。内核停机后就不会再运行任何程序了，所以提示要先打出来 */
        CON_SAY("    系统已停止，可以关闭窗口了\n\n",
                "    The system has stopped. You can close the window now.\n\n");
        call_reboot(RB_HALT_SYSTEM);
        sinit_idle_forever();
    case SD_POWEROFF:
    default:
        /* 关闭电源靠 ACPI（主板的电源管理接口）。没有 ACPI 时内核会退化成“停机”，
         * 那就干脆不调用了，留在这里显示提示（网页版的 v86 模拟器就没有 ACPI 关机）。 */
        if (access("/sys/firmware/acpi", F_OK) != 0) {
            CON_SAY("    这台（虚拟）电脑没有 ACPI，无法用软件切断电源。\n",
                    "    This (virtual) machine has no ACPI, so software cannot switch the power off.\n");
            break;
        }
        CON_SAY("    再见！正在关闭电源……\n\n", "    Bye! Powering off ...\n\n");
        sk_log("关闭电源失败: %s", call_reboot(RB_POWER_OFF));
        break;
    }
    CON_SAY("    系统已停止，可以关闭窗口了\n\n",
            "    The system has stopped. You can close the window now.\n\n");
    sinit_idle_forever();
}
