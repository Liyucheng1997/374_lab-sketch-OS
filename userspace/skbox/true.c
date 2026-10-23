/*
 * true — 什么也不做，以状态 0（成功）退出
 *
 * 程序的“退出状态”是 main() 的返回值（0~255），父进程用 wait() 拿到它。
 * shell 里 0 表示成功/真，非 0 表示失败/假。while true; do ...; done 就靠它。
 */
#include "skbox.h"

SKBOX_APPLET(true, "什么也不做，返回成功（0）")

static const char usage[] =
    "用法: true\n"
    "什么也不做，退出状态为 0（成功）。常用于 shell 的 while true 循环。\n";

int applet_true(int argc, char **argv)
{
    sb_help(argc, argv, usage);
    return 0;
}
