/*
 * yes — 不停地输出同一行（默认 "y"），直到被结束
 *
 * 常和管道一起用：yes | rm -i *。当读的一方退出后，再往管道里写会收到 SIGPIPE 信号，
 * 默认动作就是结束进程——yes 就这样“自然”停下来了。
 */
#include "skbox.h"

#include <string.h>
#include <unistd.h>

SKBOX_APPLET(yes, "重复输出一行文字，直到被结束")

static const char usage[] =
    "用法: yes [字符串...]\n"
    "不停地输出一行（参数用空格连接，默认 \"y\"），直到被结束（Ctrl+C）或管道另一端关闭。\n";

int applet_yes(int argc, char **argv)
{
    if (sb_help(argc, argv, usage))
        return 0;
    static char line[4096];
    size_t len = 0;
    if (argc < 2) {
        memcpy(line, "y\n", 2);
        len = 2;
    } else {
        for (int i = 1; i < argc; i++) {
            size_t l = strlen(argv[i]);
            if (len + l + 2 > sizeof line)
                break;
            if (i > 1)
                line[len++] = ' ';
            memcpy(line + len, argv[i], l);
            len += l;
        }
        line[len++] = '\n';
    }
    /* 攒成一大块再写：每次 write 都是一次系统调用，一次只写一行太慢 */
    static char buf[16384];
    size_t used = 0;
    while (used + len <= sizeof buf) {
        memcpy(buf + used, line, len);
        used += len;
    }
    for (;;) {
        if (sb_write_all(STDOUT_FILENO, buf, used) != 0) {
            sb_perr("写入失败");
            return 1;
        }
    }
}
