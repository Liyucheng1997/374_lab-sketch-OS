/*
 * du — 统计文件和目录占用的磁盘空间
 *
 * 文件“大小”(st_size) 和“占用空间”不一样：磁盘按块分配，1 字节的文件也要占一整块（常见 4K）；
 * 而稀疏文件里没写过的“洞”不占空间。du 用的是 st_blocks（以 512 字节为单位的实际分配块数）。
 * 同一个 inode 的多个硬链接只算一次。
 */
#include "skbox.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(du, "统计目录占用的磁盘空间")

static const char usage[] =
    "用法: du [选项] [文件...]\n"
    "统计每个文件（默认当前目录）占用的磁盘空间，目录会递归统计并逐个列出子目录。\n"
    "默认单位是 KB（1024 字节，向上取整）。\n\n"
    "  -s        只显示每个参数的总计\n"
    "  -a        同时列出文件，而不只是目录\n"
    "  -h        用 K、M、G 显示\n"
    "  -c        最后显示所有参数的总计\n"
    "  -d 层数   只列出不超过这么多层的目录（-s 等于 -d 0）\n"
    "  -b        显示表观大小（字节数 st_size），而不是占用的块\n"
    "  -k / -m   以 KB / MB 为单位\n"
    "  -x        不进入其他文件系统\n";

static int opt_all, opt_human, opt_bytes, opt_onefs, opt_unit = 1024;
static long long opt_depth = -1;
static int g_status;
static dev_t g_root_dev;

/* 记录见过的 (设备号, inode)，硬链接只统计一次 */
typedef struct {
    dev_t dev;
    ino_t ino;
} seen_t;
static seen_t *g_seen;
static size_t g_nseen, g_capseen;

static int seen_before(dev_t dev, ino_t ino)
{
    for (size_t i = 0; i < g_nseen; i++)
        if (g_seen[i].dev == dev && g_seen[i].ino == ino)
            return 1;
    if (g_nseen == g_capseen) {
        g_capseen = g_capseen ? g_capseen * 2 : 64;
        seen_t *ns = realloc(g_seen, sizeof *ns * g_capseen);
        if (!ns)
            return 0;
        g_seen = ns;
    }
    g_seen[g_nseen].dev = dev;
    g_seen[g_nseen].ino = ino;
    g_nseen++;
    return 0;
}

static void print_size(unsigned long long bytes, const char *path)
{
    char buf[32];
    if (opt_human)
        sk_fmt_size_short(buf, sizeof buf, bytes);
    else if (opt_bytes && opt_unit == 1)
        snprintf(buf, sizeof buf, "%llu", bytes);
    else
        snprintf(buf, sizeof buf, "%llu", (bytes + (unsigned long long)opt_unit - 1) / (unsigned long long)opt_unit);
    printf("%s\t%s\n", buf, path);
}

/* 返回 path 占用的字节数 */
static unsigned long long walk(const char *path, int depth)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        sb_perr("无法访问 '%s'", path);
        g_status = 1;
        return 0;
    }
    if (depth == 0)
        g_root_dev = st.st_dev;
    unsigned long long size = 0;
    if (!S_ISDIR(st.st_mode) && st.st_nlink > 1 && seen_before(st.st_dev, st.st_ino))
        return 0;
    /* 表观大小（-b）不算目录本身：目录的 st_size 只是目录表占的空间，和 GNU du 一致 */
    if (opt_bytes)
        size = S_ISDIR(st.st_mode) ? 0 : (unsigned long long)st.st_size;
    else
        size = (unsigned long long)st.st_blocks * 512;

    if (S_ISDIR(st.st_mode) && !(opt_onefs && st.st_dev != g_root_dev)) {
        DIR *d = opendir(path);
        if (!d) {
            sb_perr("无法读取目录 '%s'", path);
            g_status = 1;
        } else {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                size_t pl = strlen(path);
                char *child = sk_strdup_printf("%s%s%s", path, (pl && path[pl - 1] == '/') ? "" : "/", e->d_name);
                if (child) {
                    size += walk(child, depth + 1);
                    free(child);
                }
            }
            closedir(d);
        }
    }
    int show = opt_depth < 0 || depth <= opt_depth;
    if (show && (S_ISDIR(st.st_mode) || opt_all || depth == 0))
        print_size(size, path);
    return size;
}

int applet_du(int argc, char **argv)
{
    int total = 0, summarize = 0, c;
    while ((c = sb_getopt(argc, argv, "sahcd:bkmx", usage)) != -1) {
        switch (c) {
        case 's': summarize = 1; break;
        case 'a': opt_all = 1; break;
        case 'h': opt_human = 1; break;
        case 'c': total = 1; break;
        case 'd':
            if (sb_parse_ll(optarg, &opt_depth) != 0 || opt_depth < 0) {
                sb_err("无效的层数 '%s'", optarg);
                return 1;
            }
            break;
        case 'b': opt_bytes = 1; opt_unit = 1; break;
        case 'k': opt_unit = 1024; break;
        case 'm': opt_unit = 1024 * 1024; break;
        case 'x': opt_onefs = 1; break;
        default:  return 1;
        }
    }
    if (summarize) {
        if (opt_all) {
            sb_err("不能同时使用 -s 和 -a");
            return 1;
        }
        opt_depth = 0;
    }
    char *dot[] = { ".", NULL };
    char **paths = optind < argc ? argv + optind : dot;
    int n = optind < argc ? argc - optind : 1;
    unsigned long long sum = 0;
    for (int i = 0; i < n; i++)
        sum += walk(paths[i], 0);
    if (total)
        print_size(sum, "总用量");
    return g_status;
}
