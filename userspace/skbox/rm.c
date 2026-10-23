/*
 * rm — 删除文件或目录
 *
 * “删除文件”其实是 unlink()：从目录里去掉这个名字。inode 有一个链接计数，
 * 当计数变成 0、而且没有进程还开着这个文件时，内核才真正释放它占用的空间。
 * 所以正在运行的程序文件也可以被 rm，程序照样能继续运行。递归删除的实现见 lib_fs.c。
 */
#include "skbox.h"

#include <string.h>
#include <unistd.h>

SKBOX_APPLET(rm, "删除文件或目录")

static const char usage[] =
    "用法: rm [选项] 文件...\n"
    "删除文件。默认不删除目录。\n\n"
    "  -r, -R    递归删除目录及其中的全部内容\n"
    "  -f        忽略不存在的文件，从不询问\n"
    "  -i        每次删除前询问\n"
    "  -d        删除空目录\n"
    "  -v        显示删除了什么\n"
    "注意：被删除的文件不会进入回收站，无法恢复。\n";

int applet_rm(int argc, char **argv)
{
    int recursive = 0, force = 0, interactive = 0, empty_dirs = 0, verbose = 0, c;
    while ((c = sb_getopt(argc, argv, "rRfidv", usage)) != -1) {
        switch (c) {
        case 'r':
        case 'R': recursive = 1; break;
        case 'f': force = 1; interactive = 0; break;
        case 'i': interactive = 1; force = 0; break;
        case 'd': empty_dirs = 1; break;
        case 'v': verbose = 1; break;
        default:  return 1;
        }
    }
    if (optind >= argc) {
        if (force)
            return 0;
        sb_err("缺少操作数");
        sb_hint();
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        const char *p = argv[i];
        char base[4096];
        sb_basename(p, base, sizeof base);
        if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
            sb_err("拒绝删除 '.' 或 '..' 目录: 跳过 '%s'", p);
            rc = 1;
            continue;
        }
        if (recursive && strcmp(base, "/") == 0) {
            sb_err("在 '/' 上递归删除十分危险，已拒绝");
            rc = 1;
            continue;
        }
        struct stat st;
        if (!recursive && empty_dirs && lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (interactive) {
                fprintf(stderr, "%s: 是否删除目录 '%s'？", sb_applet, p);
                if (!sb_ask_yes())
                    continue;
            }
            if (rmdir(p) != 0) {
                sb_perr("无法删除 '%s'", p);
                rc = 1;
            } else if (verbose) {
                printf("已删除目录 '%s'\n", p);
            }
            continue;
        }
        if (sb_remove_path(p, recursive, force, interactive, verbose) != 0)
            rc = 1;
    }
    return rc;
}
