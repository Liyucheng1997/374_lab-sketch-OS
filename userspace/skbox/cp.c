/*
 * cp — 复制文件和目录（递归复制的实现见 lib_fs.c）
 */
#include "skbox.h"

#include <string.h>

SKBOX_APPLET(cp, "复制文件和目录")

static const char usage[] =
    "用法: cp [选项] 源 目标\n"
    "  或: cp [选项] 源... 目录\n"
    "  或: cp [选项] -t 目录 源...\n"
    "复制文件。目标是已存在的目录时，复制到该目录里面。\n\n"
    "  -r, -R    递归复制目录（符号链接按链接本身复制）\n"
    "  -a        归档模式：等于 -dR -p，尽量原样保留一切\n"
    "  -p        保留权限、属主和时间戳\n"
    "  -d, -P    遇到符号链接时复制链接本身，而不是它指向的文件\n"
    "  -L        总是跟随符号链接\n"
    "  -H        跟随命令行上给出的符号链接\n"
    "  -f        目标文件无法打开时，先删除再重试\n"
    "  -i        覆盖前询问\n"
    "  -n        不覆盖已存在的文件\n"
    "  -v        显示复制了什么\n"
    "  -t 目录   把所有源复制到这个目录里\n"
    "  -T        把目标当作普通文件，即使它是目录\n";

int applet_cp(int argc, char **argv)
{
    sb_copy_opts o;
    memset(&o, 0, sizeof o);
    int deref_all = 0, no_target_dir = 0, explicit_nodrf = 0, c;
    const char *target_dir = NULL;
    while ((c = sb_getopt(argc, argv, "rRapdPLHfinvt:T", usage)) != -1) {
        switch (c) {
        case 'r':
        case 'R': o.recursive = 1; break;
        case 'a': o.recursive = 1; o.preserve = 1; o.no_deref = 1; explicit_nodrf = 1; break;
        case 'p': o.preserve = 1; break;
        case 'd':
        case 'P': o.no_deref = 1; explicit_nodrf = 1; deref_all = 0; break;
        case 'L': deref_all = 1; o.no_deref = 0; break;
        case 'H': o.deref_args = 1; break;
        case 'f': o.force = 1; break;
        case 'i': o.interactive = 1; o.no_clobber = 0; break;
        case 'n': o.no_clobber = 1; o.interactive = 0; break;
        case 'v': o.verbose = 1; break;
        case 't': target_dir = optarg; break;
        case 'T': no_target_dir = 1; break;
        default:  return 1;
        }
    }
    /* 和 GNU 一致：递归复制时默认不跟随符号链接（否则可能复制出环或者复制到目录树外面去） */
    if (o.recursive && !deref_all && !explicit_nodrf)
        o.no_deref = 1;

    int n = argc - optind;
    char **ops = argv + optind;
    if (target_dir) {
        if (n < 1) {
            sb_err("缺少源文件");
            sb_hint();
            return 1;
        }
    } else {
        if (n < 1) {
            sb_err("缺少文件操作数");
            sb_hint();
            return 1;
        }
        if (n < 2) {
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
            return sb_copy_path(ops[0], target_dir, &o) == 0 ? 0 : 1;
        }
        if (!is_dir) {
            sb_err("目标 '%s' 不是目录", target_dir);
            return 1;
        }
    }

    int rc = 0;
    for (int i = 0; i < n; i++) {
        char base[4096], dst[8192];
        sb_basename(ops[i], base, sizeof base);
        sk_path_join(dst, sizeof dst, target_dir, base);
        if (sb_copy_path(ops[i], dst, &o) != 0)
            rc = 1;
    }
    return rc;
}
