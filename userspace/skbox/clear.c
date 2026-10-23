/*
 * clear — 清屏
 *
 * 并没有“清屏”的系统调用：程序只是输出一串特殊字节（ANSI 转义序列），
 * 终端（串口另一头的终端程序、skterm、Linux 控制台）看到后自己把屏幕擦掉。
 *   ESC[H   光标移到左上角      ESC[2J   擦除整个屏幕      ESC[3J   清掉回滚缓冲区
 */
#include "skbox.h"

SKBOX_APPLET(clear, "清除终端屏幕")

static const char usage[] =
    "用法: clear\n"
    "清除终端屏幕（输出 ANSI 转义序列 ESC[H ESC[2J ESC[3J）。\n";

int applet_clear(int argc, char **argv)
{
    if (sb_help(argc, argv, usage))
        return 0;
    fputs("\033[H\033[2J\033[3J", stdout);
    return 0;
}
