/*
 * chmod — 修改文件的权限（模式的解析见 lib.c 的 sb_mode_parse）
 *
 * 权限是 inode 的一部分。每次打开文件时，内核按“属主 → 同组 → 其他人”的顺序
 * 找到适用于当前进程的那一组 rwx 位来决定允许与否（root 例外，几乎不受限制）。
 */
#include "skbox.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(chmod, "修改文件的访问权限")

static const char usage[] =
    "用法: chmod [选项] 模式 文件...\n"
    "修改文件的权限。模式可以是：\n"
    "  八进制    如 755（rwxr-xr-x）、644（rw-r--r--）、4755（加 setuid）\n"
    "  符号      [ugoa][+-=][rwxXst]，多个子句用逗号分隔，如 u+x、go-w、a=r、u=rwx,g=rx,o=\n"
    "            u 属主  g 同组  o 其他人  a 所有人；X 只给目录和已可执行的文件加 x\n\n"
    "  -R    递归修改目录里的所有文件（不跟随符号链接）\n"
    "  -v    显示每个文件的处理结果\n"
    "  -c    只在权限确实改变时显示\n"
    "  -f    不显示大多数错误信息\n";

static int recursive, verbose, changes_only, quiet;
static const char *g_spec;
static mode_t g_umask;

static int chmod_one(const char *path, int top)
{
    struct stat st;
    /* 命令行上的符号链接跟随到目标；递归时遇到的符号链接跳过（链接本身没有权限可言） */
    if ((top ? stat(path, &st) : lstat(path, &st)) != 0) {
        if (!quiet)
            sb_perr("无法访问 '%s'", path);
        return -1;
    }
    if (S_ISLNK(st.st_mode))
        return 0;
    mode_t newmode;
    if (sb_mode_parse(g_spec, st.st_mode & 07777, S_ISDIR(st.st_mode), g_umask, &newmode) != 0) {
        sb_err("无效的模式 '%s'", g_spec);
        exit(1);
    }
    int rc = 0;
    mode_t old = st.st_mode & 07777;
    if (newmode != old || !changes_only) {
        if (chmod(path, newmode) != 0) {
            if (!quiet)
                sb_perr("无法修改 '%s' 的权限", path);
            rc = -1;
        } else if (verbose || (changes_only && newmode != old)) {
            char a[12], b[12];
            sb_mode_string(old | (st.st_mode & S_IFMT), a);
            sb_mode_string(newmode | (st.st_mode & S_IFMT), b);
            if (newmode != old)
                printf("'%s' 的权限已从 %04o (%s) 改为 %04o (%s)\n", path,
                       (unsigned)old, a + 1, (unsigned)newmode, b + 1);
            else
                printf("'%s' 的权限保持为 %04o (%s)\n", path, (unsigned)old, a + 1);
        }
    }
    if (recursive && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) {
            if (!quiet)
                sb_perr("无法打开目录 '%s'", path);
            return -1;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char *child = sk_strdup_printf("%s/%s", path, e->d_name);
            if (!child || chmod_one(child, 0) != 0)
                rc = -1;
            free(child);
        }
        closedir(d);
    }
    return rc;
}

int applet_chmod(int argc, char **argv)
{
    /* "chmod -w file" 里的 -w 是模式而不是选项：只把我们认识的选项字母当选项 */
    int c;
    while (optind < argc) {
        const char *a = argv[optind];
        if (a[0] == '-' && a[1] && strspn(a + 1, "Rvcf") != strlen(a + 1) && strcmp(a, "--help") != 0 &&
            strcmp(a, "--") != 0)
            break;   /* 形如 -w、-x 的参数：当作模式 */
        c = sb_getopt(argc, argv, "+Rvcf", usage);
        if (c == -1)
            break;
        switch (c) {
        case 'R': recursive = 1; break;
        case 'v': verbose = 1; break;
        case 'c': changes_only = 1; break;
        case 'f': quiet = 1; break;
        default:  return 1;
        }
    }
    if (argc - optind < 2) {
        if (argc - optind < 1)
            sb_err("缺少操作数");
        else
            sb_err("'%s' 后面缺少操作数", argv[argc - 1]);
        sb_hint();
        return 1;
    }
    g_spec = argv[optind++];
    g_umask = umask(0);
    umask(g_umask);
    mode_t test;
    if (sb_mode_parse(g_spec, 0, 0, g_umask, &test) != 0) {
        sb_err("无效的模式 '%s'", g_spec);
        return 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; i++)
        if (chmod_one(argv[i], 1) != 0)
            rc = 1;
    return rc;
}
