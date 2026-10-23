/*
 * which — 查找命令对应的程序文件
 *
 * 在 shell 里输入 ls 时，shell 会按环境变量 PATH 列出的目录（用冒号分隔）依次查找
 * 有没有一个叫 ls 的可执行文件，找到第一个就执行它。which 模拟的就是这个查找过程。
 */
#include "skbox.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(which, "查找命令所在的位置")

static const char usage[] =
    "用法: which [-a] 命令...\n"
    "按照环境变量 PATH 中的目录顺序，找出每个命令对应的可执行文件。\n\n"
    "  -a    列出所有匹配的文件，而不只是第一个\n";

static int is_exec_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

int applet_which(int argc, char **argv)
{
    int all = 0, c;
    while ((c = sb_getopt(argc, argv, "a", usage)) != -1) {
        if (c != 'a')
            return 1;
        all = 1;
    }
    if (optind >= argc) {
        sb_err("缺少操作数");
        sb_hint();
        return 1;
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin:/sbin";
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        const char *name = argv[i];
        int found = 0;
        if (strchr(name, '/')) {
            /* 名字里带斜杠就不搜索 PATH，直接检查这个路径 */
            if (is_exec_file(name)) {
                puts(name);
                found = 1;
            }
        } else {
            const char *p = path;
            while (!found || all) {
                const char *colon = strchr(p, ':');
                size_t dl = colon ? (size_t)(colon - p) : strlen(p);
                char full[PATH_MAX];
                /* PATH 里的空项表示当前目录 */
                if (dl == 0)
                    snprintf(full, sizeof full, "./%s", name);
                else
                    snprintf(full, sizeof full, "%.*s/%s", (int)dl, p, name);
                if (is_exec_file(full)) {
                    puts(full);
                    found = 1;
                }
                if (!colon)
                    break;
                p = colon + 1;
            }
        }
        if (!found)
            rc = 1;     /* 和 GNU which 一样：找不到时不输出，只返回 1 */
    }
    return rc;
}
