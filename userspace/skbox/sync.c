/*
 * sync — 把内存里缓存的文件数据写回磁盘
 *
 * 为了快，内核写文件时先写进内存里的“页缓存”，过一会儿才真正写到磁盘（延迟写）。
 * 如果这时突然断电，数据就丢了。sync() 让内核立刻把所有脏数据写回去。
 */
#include "skbox.h"

#include <fcntl.h>
#include <unistd.h>

SKBOX_APPLET(sync, "把缓存的文件数据写回磁盘")

static const char usage[] =
    "用法: sync [文件...]\n"
    "把内核缓存中尚未写入磁盘的数据写回去。\n"
    "给出文件时只同步这些文件（fsync）。\n";

int applet_sync(int argc, char **argv)
{
    int c;
    while ((c = sb_getopt(argc, argv, "", usage)) != -1)
        return 1;
    if (optind >= argc) {
        sync();
        return 0;
    }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            sb_perr("无法打开 '%s'", argv[i]);
            rc = 1;
            continue;
        }
        if (fsync(fd) != 0) {
            sb_perr("同步 '%s' 失败", argv[i]);
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
