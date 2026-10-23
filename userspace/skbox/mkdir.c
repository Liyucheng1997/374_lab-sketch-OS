/*
 * mkdir — 创建目录
 *
 * mkdir() 系统调用一次只能建一层；-p 就是从最上层开始逐层检查、缺哪层建哪层。
 * 新目录的权限 = 给定模式去掉 umask 里的位（umask 是进程属性，默认 022：别人不能写）。
 */
#include "skbox.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

SKBOX_APPLET(mkdir, "创建目录")

static const char usage[] =
    "用法: mkdir [选项] 目录...\n"
    "创建目录（如果还不存在）。\n\n"
    "  -p        需要时创建上层目录；目录已存在也不报错\n"
    "  -m 模式   设置新目录的权限（如 755 或 u=rwx,go=rx），而不是 a=rwx 减去 umask\n"
    "  -v        每创建一个目录就打印一条消息\n";

static int verbose;

static int make_one(const char *path, mode_t mode, int quiet_exist)
{
    if (mkdir(path, mode) == 0) {
        if (verbose)
            printf("%s: 已创建目录 '%s'\n", sb_applet, path);
        return 0;
    }
    int err = errno;
    struct stat st;
    if (quiet_exist && err == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    errno = err;
    sb_perr("无法创建目录 '%s'", path);
    return -1;
}

static int make_parents(const char *path, mode_t mode)
{
    char buf[4096];
    sk_strlcpy(buf, path, sizeof buf);
    /* 上层目录用 u+wx 的默认权限，保证我们能继续往里面建 */
    mode_t um = umask(0);
    umask(um);
    mode_t parent_mode = (0777 & ~um) | S_IWUSR | S_IXUSR;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/' || p[-1] == '/')
            continue;
        *p = 0;
        struct stat st;
        if (stat(buf, &st) != 0) {
            if (make_one(buf, parent_mode, 1) != 0)
                return -1;
        } else if (!S_ISDIR(st.st_mode)) {
            sb_err("无法创建目录 '%s': '%s' 已存在但不是目录", path, buf);
            return -1;
        }
        *p = '/';
    }
    return make_one(path, mode, 1);
}

int applet_mkdir(int argc, char **argv)
{
    int parents = 0, c;
    const char *mode_str = NULL;
    while ((c = sb_getopt(argc, argv, "pm:v", usage)) != -1) {
        switch (c) {
        case 'p': parents = 1; break;
        case 'm': mode_str = optarg; break;
        case 'v': verbose = 1; break;
        default:  return 1;
        }
    }
    if (optind >= argc) {
        sb_err("缺少操作数");
        sb_hint();
        return 1;
    }
    mode_t mode = 0777;
    int explicit_mode = 0;
    if (mode_str) {
        mode_t um = umask(0);
        umask(um);
        /* 符号模式是在 a=rwx 的基础上修改的 */
        if (sb_mode_parse(mode_str, 0777, 1, um, &mode) != 0) {
            sb_err("无效的模式 '%s'", mode_str);
            return 1;
        }
        explicit_mode = 1;
    }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        int r = parents ? make_parents(argv[i], mode) : make_one(argv[i], mode, 0);
        if (r != 0) {
            rc = 1;
            continue;
        }
        /* mkdir() 会被 umask 过滤，用 -m 明确指定时要再 chmod 一次才准确 */
        if (explicit_mode && chmod(argv[i], mode) != 0) {
            sb_perr("无法设置 '%s' 的权限", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
