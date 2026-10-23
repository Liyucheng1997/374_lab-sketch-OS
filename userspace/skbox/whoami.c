/*
 * whoami — 显示当前的用户名
 *
 * 内核只记录进程的数字身份（有效用户 id，euid）；名字要到 /etc/passwd 里查。
 */
#include "skbox.h"

#include <unistd.h>

SKBOX_APPLET(whoami, "显示当前用户名")

static const char usage[] =
    "用法: whoami\n"
    "显示当前进程的有效用户名（相当于 id -un）。\n";

int applet_whoami(int argc, char **argv)
{
    int c;
    while ((c = sb_getopt(argc, argv, "", usage)) != -1)
        return 1;
    if (optind < argc) {
        sb_err("多余的操作数 '%s'", argv[optind]);
        sb_hint();
        return 1;
    }
    char name[64];
    puts(sk_user_name(geteuid(), name, sizeof name));
    return 0;
}
