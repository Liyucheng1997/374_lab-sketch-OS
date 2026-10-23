/*
 * head — 输出文件的开头部分
 *
 * 读够了就立刻退出，不会把整个文件读完：所以 "yes | head -3" 能马上结束——
 * head 退出后管道的读端关闭，yes 再写时收到 SIGPIPE 也跟着结束。
 */
#include "skbox.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(head, "显示文件的开头几行")

static const char usage[] =
    "用法: head [选项] [文件...]\n"
    "输出每个文件的前 10 行。多个文件时，每个文件前面加上 ==> 文件名 <== 标题。\n\n"
    "  -n 数量   输出前若干行；写成 -n -数量 时输出除最后若干行以外的全部\n"
    "  -c 数量   输出前若干字节（可带 K、M 后缀）；-c -数量 表示除最后若干字节外的全部\n"
    "  -数量     等于 -n 数量\n"
    "  -q        从不输出文件名标题\n"
    "  -v        总是输出文件名标题\n";

/* 读到 EOF，返回 malloc 的缓冲区 */
static char *slurp(int fd, size_t *len)
{
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    for (;;) {
        if (n == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return NULL;
        }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    *len = n;
    return buf;
}

static int head_fd(int fd, long long count, int bytes, int all_but)
{
    fflush(stdout);
    if (all_but) {
        size_t len;
        char *buf = slurp(fd, &len);
        if (!buf)
            return -1;
        size_t end;
        if (bytes) {
            end = (unsigned long long)count >= len ? 0 : len - (size_t)count;
        } else if (count == 0 || len == 0) {
            end = len;
        } else {
            /* 从末尾往前数 count 行（最后一行没有换行也算一行），输出它们之前的部分 */
            size_t pos = len;
            if (buf[pos - 1] == '\n')
                pos--;
            long long seen = 0;
            end = 0;
            while (pos > 0) {
                if (buf[pos - 1] == '\n' && ++seen == count) {
                    end = pos;
                    break;
                }
                pos--;
            }
        }
        int r = sb_write_all(STDOUT_FILENO, buf, end);
        free(buf);
        return r;
    }
    static char buf[65536];
    long long left = count;
    while (left > 0) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        size_t take = (size_t)r;
        if (bytes) {
            if ((long long)take > left)
                take = (size_t)left;
            left -= (long long)take;
        } else {
            for (size_t i = 0; i < (size_t)r; i++) {
                if (buf[i] == '\n' && --left == 0) {
                    take = i + 1;
                    break;
                }
            }
        }
        if (sb_write_all(STDOUT_FILENO, buf, take) != 0)
            return -1;
    }
    return 0;
}

int applet_head(int argc, char **argv)
{
    long long count = 10;
    int bytes = 0, all_but = 0, headers = -1, c;
    /* 兼容老写法 head -5：把 "-5" 改写成 "-n" "5"，再统一解析 */
    static char *nargv[1024];
    if (argc > 1 && argc < 1020 && argv[1][0] == '-' && isdigit((unsigned char)argv[1][1])) {
        int k = 0;
        nargv[k++] = argv[0];
        nargv[k++] = "-n";
        nargv[k++] = argv[1] + 1;
        for (int i = 2; i < argc; i++)
            nargv[k++] = argv[i];
        nargv[k] = NULL;
        argv = nargv;
        argc = k;
    }
    while ((c = sb_getopt(argc, argv, "n:c:qv", usage)) != -1) {
        switch (c) {
        case 'n':
        case 'c': {
            const char *s = optarg;
            all_but = *s == '-';
            if (*s == '-' || *s == '+')
                s++;
            if (sb_parse_size(s, &count) != 0 || count < 0) {
                sb_err("无效的%s数 '%s'", c == 'n' ? "行" : "字节", optarg);
                return 1;
            }
            bytes = c == 'c';
            break;
        }
        case 'q': headers = 0; break;
        case 'v': headers = 1; break;
        default:  return 1;
        }
    }
    int nfiles = argc - optind;
    char *dash[] = { "-", NULL };
    char **files = nfiles ? argv + optind : dash;
    if (!nfiles)
        nfiles = 1;
    if (headers < 0)
        headers = nfiles > 1;
    int rc = 0;
    for (int i = 0; i < nfiles; i++) {
        int fd = sb_open_input(files[i]);
        if (fd < 0) {
            sb_perr("无法打开 '%s'", files[i]);
            rc = 1;
            continue;
        }
        if (headers) {
            printf("%s==> %s <==\n", i ? "\n" : "", strcmp(files[i], "-") ? files[i] : "标准输入");
            fflush(stdout);
        }
        if (head_fd(fd, count, bytes, all_but) != 0) {
            sb_perr("读取 '%s' 出错", files[i]);
            rc = 1;
        }
        if (fd != STDIN_FILENO)
            close(fd);
    }
    return rc;
}
