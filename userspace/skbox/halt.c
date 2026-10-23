/*
 * halt — 停机：停止所有服务、让 CPU 停下来，但不切断电源（通知 sinit：SIGUSR1）。实现见 reboot.c
 */
#include "skbox.h"

#include <signal.h>
#include <sys/reboot.h>

SKBOX_APPLET(halt, "停止系统运行（不断电）")

static const char usage[] =
    "用法: halt [-f] [-n]\n"
    "停机：通知 1 号进程 sinit（发送 SIGUSR1），由它停止服务后让 CPU 停下（不断电）。\n\n"
    "  -f    不通过 sinit，直接调用 reboot() 系统调用停机\n"
    "  -n    停机前不执行 sync\n";

int applet_halt(int argc, char **argv)
{
    return sb_power_main(argc, argv, SIGUSR1, RB_HALT_SYSTEM, "正在关机（停机，不断电）...", usage);
}
