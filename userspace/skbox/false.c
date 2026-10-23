/*
 * false — 什么也不做，以状态 1（失败）退出
 */
#include "skbox.h"

SKBOX_APPLET(false, "什么也不做，返回失败（1）")

static const char usage[] =
    "用法: false\n"
    "什么也不做，退出状态为 1（失败）。\n";

int applet_false(int argc, char **argv)
{
    if (sb_help(argc, argv, usage))
        return 0;
    return 1;
}
