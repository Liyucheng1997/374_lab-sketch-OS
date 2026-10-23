/*
 * basename — 去掉路径中的目录部分（以及可选的后缀）
 */
#include "skbox.h"

#include <string.h>

SKBOX_APPLET(basename, "去掉路径的目录部分和后缀")

static const char usage[] =
    "用法: basename 路径 [后缀]\n"
    "  或: basename -a [-s 后缀] 路径...\n"
    "输出路径的最后一部分；给出后缀时一并去掉。\n"
    "例如: basename /usr/bin/sksh  → sksh\n"
    "      basename note.txt .txt  → note\n\n"
    "  -a        处理多个路径\n"
    "  -s 后缀   去掉后缀（隐含 -a）\n";

static void one(const char *path, const char *suffix)
{
    char buf[4096];
    sb_basename(path, buf, sizeof buf);
    if (suffix && *suffix) {
        size_t bl = strlen(buf), sl = strlen(suffix);
        if (bl > sl && strcmp(buf + bl - sl, suffix) == 0)
            buf[bl - sl] = 0;
    }
    puts(buf);
}

int applet_basename(int argc, char **argv)
{
    int multi = 0, c;
    const char *suffix = NULL;
    while ((c = sb_getopt(argc, argv, "+as:", usage)) != -1) {
        switch (c) {
        case 'a': multi = 1; break;
        case 's': suffix = optarg; multi = 1; break;
        default:  return 1;
        }
    }
    int n = argc - optind;
    if (n < 1) {
        sb_err("缺少操作数");
        sb_hint();
        return 1;
    }
    if (!multi) {
        if (n > 2) {
            sb_err("多余的操作数 '%s'", argv[optind + 2]);
            sb_hint();
            return 1;
        }
        one(argv[optind], n == 2 ? argv[optind + 1] : NULL);
        return 0;
    }
    for (int i = optind; i < argc; i++)
        one(argv[i], suffix);
    return 0;
}
