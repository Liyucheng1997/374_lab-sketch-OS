/*
 * reboot — 重新启动（poweroff、halt 也在这里实现共用部分）
 *
 * 为什么不直接调用 reboot() 系统调用？
 *   内核的 reboot() 会立刻重启，正在运行的程序来不及保存数据。正确的做法是通知 1 号进程
 *   （SketchOS 里是 sinit）：它会先给所有服务发 SIGTERM 让它们体面地退出、把缓存写回磁盘，
 *   最后才由它调用 reboot()。约定（和 busybox init 相同）：
 *     SIGTERM → 重启    SIGUSR2 → 关机断电    SIGUSR1 → 停机（停住但不断电）
 */
#include "skbox.h"

#include <signal.h>
#include <sys/reboot.h>
#include <unistd.h>

SKBOX_APPLET(reboot, "重新启动计算机")

static const char usage[] =
    "用法: reboot [-f] [-n]\n"
    "重新启动系统：通知 1 号进程 sinit（发送 SIGTERM），由它停止服务后重启。\n\n"
    "  -f    不通过 sinit，直接调用 reboot() 系统调用（服务不会被正常停止）\n"
    "  -n    重启前不执行 sync（不把文件缓存写回磁盘）\n";

int sb_power_main(int argc, char **argv, int sig, int reboot_cmd, const char *msg, const char *use)
{
    int force = 0, nosync = 0, c;
    while ((c = sb_getopt(argc, argv, "fn", use)) != -1) {
        switch (c) {
        case 'f': force = 1; break;
        case 'n': nosync = 1; break;
        default:  return 1;
        }
    }
    if (optind < argc) {
        sb_err("多余的参数 '%s'", argv[optind]);
        sb_hint();
        return 1;
    }
    printf("%s\n", msg);
    fflush(stdout);
    /* sync：让内核把还在内存里的“脏”文件数据写到磁盘上 */
    if (!nosync)
        sync();
    if (force) {
        if (reboot(reboot_cmd) != 0) {
            sb_perr("reboot() 系统调用失败（需要 root 权限）");
            return 1;
        }
        return 0;
    }
    if (kill(1, sig) != 0) {
        sb_perr("无法通知 1 号进程 sinit");
        return 1;
    }
    return 0;
}

int applet_reboot(int argc, char **argv)
{
    return sb_power_main(argc, argv, SIGTERM, RB_AUTOBOOT, "正在关机并重新启动...", usage);
}
