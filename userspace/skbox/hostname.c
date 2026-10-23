/*
 * hostname — 显示或设置主机名
 *
 * 主机名保存在内核里（UTS 命名空间），开机时是内核配置的默认值 "sketchos"。
 * sethostname() 只有 root 能调用，修改只在本次开机内有效。
 */
#include "skbox.h"

#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

SKBOX_APPLET(hostname, "显示或设置主机名")

static const char usage[] =
    "用法: hostname [-s] [新主机名]\n"
    "不带参数时显示主机名；给出参数时把主机名改成它（需要 root）。\n\n"
    "  -s    只显示第一个点号之前的部分（短主机名）\n";

int applet_hostname(int argc, char **argv)
{
    int shortname = 0, c;
    while ((c = sb_getopt(argc, argv, "s", usage)) != -1) {
        if (c != 's')
            return 1;
        shortname = 1;
    }
    if (optind < argc) {
        if (optind + 1 < argc) {
            sb_err("多余的操作数 '%s'", argv[optind + 1]);
            return 1;
        }
        const char *name = argv[optind];
        if (sethostname(name, strlen(name)) != 0) {
            sb_perr("无法设置主机名 '%s'", name);
            return 1;
        }
        return 0;
    }
    struct utsname u;
    if (uname(&u) != 0) {
        sb_perr("无法取得主机名");
        return 1;
    }
    if (shortname) {
        char *dot = strchr(u.nodename, '.');
        if (dot)
            *dot = 0;
    }
    puts(u.nodename);
    return 0;
}
