/*
 * tee — 把标准输入同时复制到标准输出和文件
 *
 * 名字来自水管的 T 形接头：数据从一头流进，分成两路流出。
 * 典型用法：make 2>&1 | tee build.log（一边看输出一边保存）。
 */
#include "skbox.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

SKBOX_APPLET(tee, "把输入同时写到屏幕和文件")

static const char usage[] =
    "用法: tee [-ai] [文件...]\n"
    "从标准输入读数据，写到标准输出，同时写进每个文件。\n\n"
    "  -a    追加到文件末尾，而不是覆盖\n"
    "  -i    忽略中断信号（Ctrl+C）\n";

int applet_tee(int argc, char **argv)
{
    int append = 0, c;
    while ((c = sb_getopt(argc, argv, "ai", usage)) != -1) {
        switch (c) {
        case 'a': append = 1; break;
        case 'i': signal(SIGINT, SIG_IGN); break;
        default:  return 1;
        }
    }
    int nfiles = argc - optind + 1;
    int *fds = calloc((size_t)nfiles, sizeof *fds);
    const char **names = calloc((size_t)nfiles, sizeof *names);
    if (!fds || !names) {
        sb_err("内存不足");
        return 1;
    }
    int rc = 0;
    fds[0] = STDOUT_FILENO;
    names[0] = "标准输出";
    for (int i = 1; i < nfiles; i++) {
        names[i] = argv[optind + i - 1];
        fds[i] = open(names[i], O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0666);
        if (fds[i] < 0) {
            sb_perr("%s", names[i]);
            rc = 1;
        }
    }
    static char buf[65536];
    for (;;) {
        ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            sb_perr("读取标准输入失败");
            rc = 1;
            break;
        }
        if (r == 0)
            break;
        for (int i = 0; i < nfiles; i++) {
            if (fds[i] < 0)
                continue;
            if (sb_write_all(fds[i], buf, (size_t)r) != 0) {
                sb_perr("%s", names[i]);
                if (i > 0)
                    close(fds[i]);
                fds[i] = -1;    /* 一个文件写失败了，其它的照常继续 */
                rc = 1;
            }
        }
    }
    for (int i = 1; i < nfiles; i++) {
        if (fds[i] >= 0 && close(fds[i]) != 0) {
            sb_perr("%s", names[i]);
            rc = 1;
        }
    }
    free(fds);
    free(names);
    return rc;
}
