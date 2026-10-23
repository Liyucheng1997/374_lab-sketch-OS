/*
 * ln — 创建链接
 *
 * 硬链接（ln a b）：给同一个 inode 再起一个名字，两个名字完全平等，删掉一个另一个照样能用。
 *   硬链接不能跨文件系统，也不能指向目录（否则目录树会出现环）。
 * 符号链接（ln -s a b）：新建一个小文件，内容就是字符串 "a"。访问 b 时内核读出这个路径再去找 a。
 *   它可以指向任何地方，甚至指向不存在的东西（“悬空链接”）。
 */
#include "skbox.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(ln, "创建硬链接或符号链接")

static const char usage[] =
    "用法: ln [选项] 目标 [链接名]\n"
    "  或: ln [选项] 目标... 目录\n"
    "创建指向“目标”的链接。省略链接名时在当前目录下用目标的文件名。\n\n"
    "  -s    创建符号链接（不加时创建硬链接）\n"
    "  -f    链接名已存在时先删除它\n"
    "  -n    链接名是指向目录的符号链接时，把它当普通文件（配合 -f 替换它）\n"
    "  -v    显示创建了什么\n";

static int symbolic, force, no_deref, verbose;

static int link_one(const char *target, const char *linkname)
{
    if (force) {
        struct stat st;
        if (lstat(linkname, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                sb_err("'%s': 无法覆盖目录", linkname);
                return -1;
            }
            if (unlink(linkname) != 0) {
                sb_perr("无法删除 '%s'", linkname);
                return -1;
            }
        }
    }
    int r = symbolic ? symlink(target, linkname) : link(target, linkname);
    if (r != 0) {
        if (!symbolic && errno == EPERM) {
            struct stat st;
            if (stat(target, &st) == 0 && S_ISDIR(st.st_mode)) {
                sb_err("'%s': 不允许为目录创建硬链接", target);
                return -1;
            }
        }
        sb_perr("无法创建%s链接 '%s'", symbolic ? "符号" : "硬", linkname);
        return -1;
    }
    if (verbose)
        printf("'%s' -> '%s'\n", linkname, target);
    return 0;
}

int applet_ln(int argc, char **argv)
{
    int c;
    while ((c = sb_getopt(argc, argv, "sfnvT", usage)) != -1) {
        switch (c) {
        case 's': symbolic = 1; break;
        case 'f': force = 1; break;
        case 'n':
        case 'T': no_deref = 1; break;
        case 'v': verbose = 1; break;
        default:  return 1;
        }
    }
    int n = argc - optind;
    char **ops = argv + optind;
    if (n < 1) {
        sb_err("缺少文件操作数");
        sb_hint();
        return 1;
    }
    char base[4096], path[8192];
    if (n == 1) {
        sb_basename(ops[0], base, sizeof base);
        return link_one(ops[0], base) == 0 ? 0 : 1;
    }
    const char *dir = ops[n - 1];
    struct stat st, lst;
    int is_dir = stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
    /* -n：如果“目录”其实是一个指向目录的符号链接，就不进去，而是把它当作要替换的链接名 */
    if (is_dir && no_deref && lstat(dir, &lst) == 0 && S_ISLNK(lst.st_mode))
        is_dir = 0;
    if (!is_dir) {
        if (n > 2) {
            sb_err("目标 '%s' 不是目录", dir);
            return 1;
        }
        return link_one(ops[0], dir) == 0 ? 0 : 1;
    }
    int rc = 0;
    for (int i = 0; i < n - 1; i++) {
        sb_basename(ops[i], base, sizeof base);
        sk_path_join(path, sizeof path, dir, base);
        if (link_one(ops[i], path) != 0)
            rc = 1;
    }
    return rc;
}
