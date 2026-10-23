/*
 * mv — 移动或重命名文件
 *
 * 在同一个文件系统里，移动只是 rename() 系统调用：把“名字 → inode”这条记录从一个目录
 * 搬到另一个目录，文件内容一个字节都不用动，所以再大的文件也是瞬间完成。
 * 跨文件系统（比如从 /tmp 的 tmpfs 移到别处）时 rename 会失败并返回 EXDEV，
 * 这时只能老老实实地“复制一份，再删掉原来的”。
 */
#include "skbox.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(mv, "移动或重命名文件")

static const char usage[] =
    "用法: mv [选项] 源 目标\n"
    "  或: mv [选项] 源... 目录\n"
    "  或: mv [选项] -t 目录 源...\n"
    "重命名源文件，或者把源文件移动到目录里。\n\n"
    "  -f        覆盖前不询问（默认）\n"
    "  -i        覆盖前询问\n"
    "  -n        不覆盖已存在的文件\n"
    "  -v        显示移动了什么\n"
    "  -t 目录   把所有源移动到这个目录里\n"
    "  -T        把目标当作普通文件，即使它是目录\n";

static int interactive, no_clobber, verbose;

static int move_one(const char *src, const char *dst)
{
    struct stat ss, ds;
    if (lstat(src, &ss) != 0) {
        sb_perr("无法获取 '%s' 的文件信息", src);
        return -1;
    }
    int dst_exists = lstat(dst, &ds) == 0;
    if (dst_exists) {
        if (ss.st_dev == ds.st_dev && ss.st_ino == ds.st_ino) {
            sb_err("'%s' 和 '%s' 是同一个文件", src, dst);
            return -1;
        }
        if (no_clobber)
            return 0;
        if (interactive) {
            fprintf(stderr, "%s: 是否覆盖 '%s'？", sb_applet, dst);
            if (!sb_ask_yes())
                return 0;
        }
        if (S_ISDIR(ds.st_mode) && !S_ISDIR(ss.st_mode)) {
            sb_err("无法用非目录 '%s' 覆盖目录 '%s'", src, dst);
            return -1;
        }
        if (!S_ISDIR(ds.st_mode) && S_ISDIR(ss.st_mode)) {
            sb_err("无法用目录 '%s' 覆盖非目录 '%s'", src, dst);
            return -1;
        }
    }
    if (rename(src, dst) == 0) {
        if (verbose)
            printf("已重命名 '%s' -> '%s'\n", src, dst);
        return 0;
    }
    if (errno == EINVAL) {
        sb_err("无法把 '%s' 移动到它自己的子目录 '%s' 里", src, dst);
        return -1;
    }
    if (errno != EXDEV) {
        sb_perr("无法把 '%s' 移动到 '%s'", src, dst);
        return -1;
    }

    /* 跨文件系统：复制（保留全部属性、不跟随链接）+ 删除 */
    if (dst_exists) {
        int r = S_ISDIR(ds.st_mode) ? rmdir(dst) : unlink(dst);
        if (r != 0) {
            sb_perr("无法覆盖 '%s'", dst);
            return -1;
        }
    }
    sb_copy_opts o;
    memset(&o, 0, sizeof o);
    o.recursive = 1;
    o.no_deref = 1;
    o.preserve = 1;
    if (sb_copy_path(src, dst, &o) != 0)
        return -1;
    if (sb_remove_path(src, 1, 1, 0, 0) != 0)
        return -1;
    if (verbose)
        printf("已复制 '%s' -> '%s'，并删除了 '%s'\n", src, dst, src);
    return 0;
}

int applet_mv(int argc, char **argv)
{
    int no_target_dir = 0, c;
    const char *target_dir = NULL;
    while ((c = sb_getopt(argc, argv, "finvt:T", usage)) != -1) {
        switch (c) {
        case 'f': interactive = 0; no_clobber = 0; break;
        case 'i': interactive = 1; no_clobber = 0; break;
        case 'n': no_clobber = 1; interactive = 0; break;
        case 'v': verbose = 1; break;
        case 't': target_dir = optarg; break;
        case 'T': no_target_dir = 1; break;
        default:  return 1;
        }
    }
    int n = argc - optind;
    char **ops = argv + optind;
    if (!target_dir) {
        if (n < 2) {
            if (n < 1)
                sb_err("缺少文件操作数");
            else
                sb_err("'%s' 后面缺少目标文件操作数", ops[0]);
            sb_hint();
            return 1;
        }
        n--;
        target_dir = ops[n];
        struct stat st;
        int is_dir = stat(target_dir, &st) == 0 && S_ISDIR(st.st_mode);
        if (no_target_dir || (n == 1 && !is_dir)) {
            if (n > 1) {
                sb_err("多余的操作数 '%s'", ops[2]);
                return 1;
            }
            return move_one(ops[0], target_dir) == 0 ? 0 : 1;
        }
        if (!is_dir) {
            sb_err("目标 '%s' 不是目录", target_dir);
            return 1;
        }
    } else if (n < 1) {
        sb_err("缺少源文件");
        return 1;
    }
    int rc = 0;
    for (int i = 0; i < n; i++) {
        char base[4096], dst[8192];
        sb_basename(ops[i], base, sizeof base);
        sk_path_join(dst, sizeof dst, target_dir, base);
        if (move_one(ops[i], dst) != 0)
            rc = 1;
    }
    return rc;
}
