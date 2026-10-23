/*
 * umount — 卸载文件系统
 *
 * 只要还有进程在使用这个文件系统（打开着里面的文件、当前目录在里面），内核就拒绝卸载，
 * 返回 EBUSY。-l（懒卸载）会先把它从目录树上摘下来，等最后一个使用者离开后再真正释放。
 */
#include "skbox.h"

#include <errno.h>
#include <sys/mount.h>

SKBOX_APPLET(umount, "卸载文件系统")

static const char usage[] =
    "用法: umount [选项] 目录...\n"
    "卸载挂载在这些目录上的文件系统。\n\n"
    "  -l    懒卸载：立即从目录树上摘下，等不再使用时再清理\n"
    "  -f    强制卸载（主要用于网络文件系统）\n";

int applet_umount(int argc, char **argv)
{
    int flags = 0, c;
    while ((c = sb_getopt(argc, argv, "lfvn", usage)) != -1) {
        switch (c) {
        case 'l': flags |= MNT_DETACH; break;
        case 'f': flags |= MNT_FORCE; break;
        case 'v':
        case 'n': break;
        default:  return 1;
        }
    }
    if (optind >= argc) {
        sb_err("缺少要卸载的目录");
        sb_hint();
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        if (umount2(argv[i], flags) != 0) {
            int err = errno;
            if (err == EBUSY)
                sb_err("无法卸载 '%s': 设备忙（有进程正在使用它，可以用 lsof 查看，或用 -l 懒卸载）", argv[i]);
            else if (err == EINVAL)
                sb_err("无法卸载 '%s': 它不是挂载点", argv[i]);
            else
                sb_err("无法卸载 '%s': %s", argv[i], sk_strerror(err));
            rc = 1;
        }
    }
    return rc;
}
