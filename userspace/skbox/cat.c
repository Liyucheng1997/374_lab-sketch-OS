/*
 * cat — 连接文件并输出
 *
 * 最简单的 Unix 程序之一：open() 打开文件，循环 read() 到内存，再 write() 到标准输出（文件描述符 1）。
 * 不带选项时直接按大块复制字节，速度最快；带 -n 等选项时才逐个字符处理。
 */
#include "skbox.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(cat, "连接文件并显示内容")

static const char usage[] =
    "用法: cat [选项] [文件...]\n"
    "把文件内容依次输出到标准输出。没有文件或文件是 - 时读标准输入。\n\n"
    "  -n    给所有行编号\n"
    "  -b    只给非空行编号（覆盖 -n）\n"
    "  -s    把连续的多个空行压缩成一个\n"
    "  -E    在每行末尾显示 $\n";

typedef struct {
    int number, number_nonblank, squeeze, show_ends;
    long long line_no;     /* 编号在多个文件之间是连续的 */
    int at_line_start;
    int blank_run;         /* 连续空行计数（用于 -s） */
} cat_state;

static int cat_fancy(int fd, cat_state *s)
{
    static char in[65536];
    for (;;) {
        ssize_t r = read(fd, in, sizeof in);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return 0;
        for (ssize_t i = 0; i < r; i++) {
            char ch = in[i];
            if (s->at_line_start) {
                if (ch == '\n') {
                    /* 空行 */
                    if (s->squeeze && s->blank_run >= 1)
                        continue;
                    s->blank_run++;
                    if (s->number && !s->number_nonblank)
                        printf("%6lld\t", ++s->line_no);
                } else {
                    s->blank_run = 0;
                    if (s->number || s->number_nonblank)
                        printf("%6lld\t", ++s->line_no);
                }
            }
            if (ch == '\n') {
                if (s->show_ends)
                    putchar('$');
                s->at_line_start = 1;
            } else {
                s->at_line_start = 0;
            }
            putchar(ch);
        }
    }
}

int applet_cat(int argc, char **argv)
{
    cat_state s;
    memset(&s, 0, sizeof s);
    s.at_line_start = 1;
    int c;
    while ((c = sb_getopt(argc, argv, "nbsEu", usage)) != -1) {
        switch (c) {
        case 'n': s.number = 1; break;
        case 'b': s.number_nonblank = 1; break;
        case 's': s.squeeze = 1; break;
        case 'E': s.show_ends = 1; break;
        case 'u': break;   /* POSIX 的“不缓冲”，我们本来就不缓冲 */
        default:  return 1;
        }
    }
    int fancy = s.number || s.number_nonblank || s.squeeze || s.show_ends;
    char *dash[] = { "-", NULL };
    char **files = optind < argc ? argv + optind : dash;
    int nfiles = optind < argc ? argc - optind : 1;
    int rc = 0;
    for (int i = 0; i < nfiles; i++) {
        int fd = sb_open_input(files[i]);
        if (fd < 0) {
            sb_perr("%s", files[i]);
            rc = 1;
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            sb_err("%s: 是一个目录", files[i]);
            rc = 1;
        } else {
            int r;
            if (fancy) {
                r = cat_fancy(fd, &s);
            } else {
                fflush(stdout);
                r = sb_copy_fd(fd, STDOUT_FILENO);
            }
            if (r != 0) {
                sb_perr("%s", files[i]);
                rc = 1;
            }
        }
        if (fd != STDIN_FILENO)
            close(fd);
    }
    return rc;
}
