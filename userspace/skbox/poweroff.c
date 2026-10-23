/*
 * poweroff — 关机并断电（通知 sinit：SIGUSR2）。实现见 reboot.c
 */
#include "skbox.h"

#include <signal.h>
#include <sys/reboot.h>

SKBOX_APPLET(poweroff, "关机并切断电源")

static const char usage[] =
    "用法: poweroff [-f] [-n]\n"
    "关机并切断电源：通知 1 号进程 sinit（发送 SIGUSR2），由它停止服务后关机。\n"
    "在 QEMU 里关机后虚拟机窗口会关闭；在网页版里模拟器会停止。\n\n"
    "  -f    不通过 sinit，直接调用 reboot() 系统调用关机\n"
    "  -n    关机前不执行 sync\n";

int applet_poweroff(int argc, char **argv)
{
    return sb_power_main(argc, argv, SIGUSR2, RB_POWER_OFF, "正在关机...", usage);
}
