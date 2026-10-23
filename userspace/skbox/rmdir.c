/*
 * rmdir — 删除空目录
 *
 * 内核只允许删除空目录（rmdir 系统调用遇到非空目录返回 ENOTEMPTY），
 * 这是一道保险：要删除整棵目录树必须用 rm -r 明确地逐个删除。
 */
#include "skbox.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(rmdir, "删除空目录")

static const char usage[] =
    "用法: rmdir [选项] 目录...\n"
    "删除空目录。\n\n"
    "  -p                         删除目录后，再依次删除它变空的上层目录（a/b/c → a/b → a）\n"
    "  -v                         每删除一个目录就打印一条消息\n"
    "  --ignore-fail-on-non-empty 忽略“目录非空”的错误\n";

int applet_rmdir(int argc, char **argv)
{
    int parents = 0, verbose = 0, ignore_ne = 0, c;
    static const struct option lo[] = {
        { "ignore-fail-on-non-empty", no_argument, NULL, 'I' },
        { "parents", no_argument, NULL, 'p' },
        { "verbose", no_argument, NULL, 'v' },
        { 0, 0, 0, 0 },
    };
    while ((c = sb_getopt_long(argc, argv, "pv", lo, usage)) != -1) {
        switch (c) {
        case 'p': parents = 1; break;
        case 'v': verbose = 1; break;
        case 'I': ignore_ne = 1; break;
        default:  return 1;
        }
    }
    if (optind >= argc) {
        sb_err("缺少操作数");
        sb_hint();
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        char buf[4096];
        sk_strlcpy(buf, argv[i], sizeof buf);
        for (;;) {
            size_t len = strlen(buf);
            while (len > 1 && buf[len - 1] == '/')
                buf[--len] = 0;
            if (rmdir(buf) != 0) {
                if (!(ignore_ne && (errno == ENOTEMPTY || errno == EEXIST))) {
                    sb_perr("删除 '%s' 失败", buf);
                    rc = 1;
                }
                break;
            }
            if (verbose)
                printf("%s: 已删除目录 '%s'\n", sb_applet, buf);
            if (!parents)
                break;
            char *slash = strrchr(buf, '/');
            if (!slash || slash == buf)
                break;
            *slash = 0;
        }
    }
    return rc;
}
