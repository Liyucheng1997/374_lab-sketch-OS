/*
 * pwd — 显示当前工作目录
 *
 * “当前目录”是内核为每个进程保存的一个属性（task_struct 里的 fs->pwd），相对路径都从这里开始找。
 * 所以 cd 只能是 shell 的内建命令：子进程改的是它自己的当前目录，影响不到父进程。
 */
#include "skbox.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(pwd, "显示当前所在的目录")

static const char usage[] =
    "用法: pwd [-L|-P]\n"
    "显示当前工作目录的完整路径。\n\n"
    "  -L    使用环境变量 PWD（可能包含符号链接），前提是它确实指向当前目录\n"
    "  -P    显示不含符号链接的真实路径（默认）\n";

/* $PWD 可信吗：必须是绝对路径、不含 . 和 ..、并且和 "." 是同一个目录 */
static int pwd_env_valid(const char *p)
{
    if (!p || p[0] != '/')
        return 0;
    for (const char *s = p; *s; s++) {
        if (s[0] == '/' && s[1] == '.' &&
            (s[2] == '/' || s[2] == 0 || (s[2] == '.' && (s[3] == '/' || s[3] == 0))))
            return 0;
    }
    struct stat a, b;
    return stat(p, &a) == 0 && stat(".", &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

int applet_pwd(int argc, char **argv)
{
    int logical = 0, c;
    while ((c = sb_getopt(argc, argv, "LP", usage)) != -1) {
        switch (c) {
        case 'L': logical = 1; break;
        case 'P': logical = 0; break;
        default:  return 1;
        }
    }
    if (logical) {
        const char *p = getenv("PWD");
        if (pwd_env_valid(p)) {
            puts(p);
            return 0;
        }
    }
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof buf)) {
        sb_perr("无法取得当前目录");
        return 1;
    }
    puts(buf);
    return 0;
}
