/*
 * env — 显示环境变量，或者在修改过的环境里运行一个程序
 *
 * 环境变量是每个进程自带的一组 “名字=值” 字符串。fork() 时子进程得到父进程的一份拷贝；
 * execve() 时由调用者决定传给新程序什么环境。env 就是先改自己的环境，再 exec 目标程序。
 */
#include "skbox.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(env, "显示环境变量或在指定环境中运行程序")

static const char usage[] =
    "用法: env [-i] [-0] [-u 名字]... [名字=值]... [命令 [参数...]]\n"
    "设置环境变量后运行命令；没有给出命令时，列出当前的全部环境变量。\n\n"
    "  -i, -     从空的环境开始（不继承任何变量）\n"
    "  -u 名字   删除这个变量\n"
    "  -0        列出变量时用 NUL 字符而不是换行分隔\n";

extern char **environ;

int applet_env(int argc, char **argv)
{
    int ignore = 0, nul = 0, c;
    /* 先收集 -u，等 -i 处理完再执行（顺序不影响结果） */
    const char **unsets = calloc((size_t)argc, sizeof *unsets);
    int nunset = 0;
    if (!unsets) {
        sb_err("内存不足");
        return 125;
    }
    /* '+'：遇到第一个非选项参数（变量赋值或命令）就停止解析，后面的选项属于那个命令 */
    while ((c = sb_getopt(argc, argv, "+i0u:", usage)) != -1) {
        switch (c) {
        case 'i': ignore = 1; break;
        case '0': nul = 1; break;
        case 'u': unsets[nunset++] = optarg; break;
        default:  return 125;
        }
    }
    int i = optind;
    if (i < argc && strcmp(argv[i], "-") == 0) {
        ignore = 1;
        i++;
    }
    if (ignore)
        clearenv();
    for (int k = 0; k < nunset; k++) {
        if (unsetenv(unsets[k]) != 0) {
            sb_perr("无法删除变量 '%s'", unsets[k]);
            return 125;
        }
    }
    free(unsets);
    for (; i < argc && strchr(argv[i], '='); i++) {
        if (argv[i][0] == '=') {
            sb_err("无效的变量赋值 '%s'", argv[i]);
            return 125;
        }
        if (putenv(argv[i]) != 0) {
            sb_perr("无法设置变量 '%s'", argv[i]);
            return 125;
        }
    }
    if (i >= argc) {
        for (char **e = environ; e && *e; e++) {
            fputs(*e, stdout);
            putchar(nul ? '\0' : '\n');
        }
        return 0;
    }
    if (nul) {
        sb_err("运行命令时不能使用 -0");
        return 125;
    }
    fflush(stdout);
    /* execvp 会按 PATH 查找命令。成功的话它不会返回：当前进程的内存被新程序整个替换掉了 */
    execvp(argv[i], argv + i);
    int err = errno;
    sb_err("'%s': %s", argv[i], sk_strerror(err));
    return err == ENOENT ? 127 : 126;
}
