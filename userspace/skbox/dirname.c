/*
 * dirname — 去掉路径的最后一部分，只留下目录
 */
#include "skbox.h"

#include <string.h>

SKBOX_APPLET(dirname, "取出路径中的目录部分")

static const char usage[] =
    "用法: dirname 路径...\n"
    "输出每个路径去掉最后一部分后剩下的目录；没有目录时输出 \".\"。\n"
    "例如: dirname /usr/bin/sksh  → /usr/bin\n"
    "      dirname note.txt       → .\n";

static void one(const char *path)
{
    char buf[4096];
    sk_strlcpy(buf, path, sizeof buf);
    size_t len = strlen(buf);
    /* 1. 去掉结尾的斜杠  2. 去掉最后一段  3. 再去掉分隔用的斜杠 */
    while (len > 1 && buf[len - 1] == '/')
        len--;
    while (len > 0 && buf[len - 1] != '/')
        len--;
    if (len == 0) {
        puts(".");
        return;
    }
    while (len > 1 && buf[len - 1] == '/')
        len--;
    buf[len] = 0;
    puts(buf);
}

int applet_dirname(int argc, char **argv)
{
    int c;
    while ((c = sb_getopt(argc, argv, "+", usage)) != -1)
        return 1;
    if (optind >= argc) {
        sb_err("缺少操作数");
        sb_hint();
        return 1;
    }
    for (int i = optind; i < argc; i++)
        one(argv[i]);
    return 0;
}
