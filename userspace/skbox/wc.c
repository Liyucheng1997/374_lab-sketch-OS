/*
 * wc — 统计行数、单词数、字符数和字节数
 *
 * “行数”其实就是换行符 '\n' 的个数；“字符数”和“字节数”在 UTF-8 下不同：
 * 一个汉字占 3 个字节，但只算 1 个字符。
 */
#include "skbox.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(wc, "统计行数、单词数和字节数")

static const char usage[] =
    "用法: wc [选项] [文件...]\n"
    "统计每个文件的换行符个数、单词数和字节数；多个文件时最后输出总计。\n"
    "不带选项时相当于 -lwc。没有文件或文件是 - 时读标准输入。\n\n"
    "  -l    行数（换行符个数）\n"
    "  -w    单词数（由空白分隔的非空白序列）\n"
    "  -m    字符数（按 UTF-8 解码）\n"
    "  -c    字节数\n"
    "  -L    最长一行的显示宽度\n";

typedef struct {
    unsigned long long lines, words, chars, bytes, maxlen;
} counts;

static int is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static int count_fd(int fd, counts *c, int need_chars)
{
    static unsigned char buf[65536 + 4];
    int in_word = 0;
    size_t carry = 0;           /* 上一块末尾不完整的 UTF-8 字节数 */
    unsigned long long linew = 0;
    for (;;) {
        ssize_t r = read(fd, buf + carry, sizeof buf - 4 - carry);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        size_t len = carry + (size_t)r;
        c->bytes += (unsigned long long)r;
        size_t i = 0;
        while (i < len) {
            unsigned char ch = buf[i];
            if (ch < 0x80) {
                if (ch == '\n') {
                    c->lines++;
                    if (linew > c->maxlen)
                        c->maxlen = linew;
                    linew = 0;
                } else if (ch == '\t') {
                    linew = (linew / 8 + 1) * 8;
                } else if (ch >= 0x20 && ch != 0x7F) {
                    linew++;
                }
                if (is_space(ch)) {
                    in_word = 0;
                } else if (!in_word) {
                    in_word = 1;
                    c->words++;
                }
                c->chars++;
                i++;
                continue;
            }
            if (!need_chars) {
                /* 不需要字符数时跳过解码，非 ASCII 字节一律算作单词的一部分 */
                if (!in_word) {
                    in_word = 1;
                    c->words++;
                }
                i++;
                continue;
            }
            /* 一个多字节字符可能被切在两次 read 之间：留到下一块再处理 */
            size_t need = (ch & 0xE0) == 0xC0 ? 2 : (ch & 0xF0) == 0xE0 ? 3 : (ch & 0xF8) == 0xF0 ? 4 : 1;
            if (r > 0 && i + need > len) {
                break;
            }
            uint32_t cp;
            int k = sk_utf8_decode((const char *)buf + i, len - i, &cp);
            if (cp != 0xFFFD || k > 1) {
                c->chars++;
                linew += (unsigned long long)sk_wcwidth(cp);
            }
            if (!in_word) {
                in_word = 1;
                c->words++;
            }
            i += (size_t)k;
        }
        carry = len - i;
        if (carry)
            memmove(buf, buf + i, carry);
        if (r == 0)
            break;   /* 文件末尾残缺的字节已经在上面按非法字节处理，不计入字符数 */
    }
    if (linew > c->maxlen)
        c->maxlen = linew;
    return 0;
}

static int show_l, show_w, show_m, show_c, show_L, width;

static void print_counts(const counts *c, const char *name)
{
    int first = 1;
#define OUT(flag, v)                                                     \
    do {                                                                 \
        if (flag) {                                                      \
            printf(first ? "%*llu" : " %*llu", width, (unsigned long long)(v)); \
            first = 0;                                                   \
        }                                                                \
    } while (0)
    OUT(show_l, c->lines);
    OUT(show_w, c->words);
    OUT(show_m, c->chars);
    OUT(show_c, c->bytes);
    OUT(show_L, c->maxlen);
#undef OUT
    if (name)
        printf(" %s", name);
    putchar('\n');
}

int applet_wc(int argc, char **argv)
{
    int c;
    while ((c = sb_getopt(argc, argv, "lwmcL", usage)) != -1) {
        switch (c) {
        case 'l': show_l = 1; break;
        case 'w': show_w = 1; break;
        case 'm': show_m = 1; break;
        case 'c': show_c = 1; break;
        case 'L': show_L = 1; break;
        default:  return 1;
        }
    }
    if (!show_l && !show_w && !show_m && !show_c && !show_L)
        show_l = show_w = show_c = 1;

    int nfiles = argc - optind;
    char *dash[] = { "-", NULL };
    char **files = nfiles ? argv + optind : dash;
    int named = nfiles > 0;
    if (!nfiles)
        nfiles = 1;

    /* 先 fstat 所有文件来决定数字列的宽度（和 GNU wc 的规则一致）：
     * 宽度 = 普通文件总大小的位数；有管道/终端等无法预知大小的输入时至少 7 位；
     * 只输出一个数字时不补空格（方便在脚本里用 $(wc -l < 文件)）。 */
    int *fds = calloc((size_t)nfiles, sizeof *fds);
    int *errs = calloc((size_t)nfiles, sizeof *errs);
    if (!fds || !errs) {
        sb_err("内存不足");
        return 1;
    }
    unsigned long long regular_total = 0;
    int min_width = 1, any_ok = 0;
    for (int i = 0; i < nfiles; i++) {
        fds[i] = sb_open_input(files[i]);
        errs[i] = errno;
        struct stat st;
        if (fds[i] >= 0 && fstat(fds[i], &st) == 0) {
            any_ok = 1;
            if (S_ISREG(st.st_mode))
                regular_total += (unsigned long long)st.st_size;
            else
                min_width = 7;
        }
    }
    int ncounts = show_l + show_w + show_m + show_c + show_L;
    width = 1;
    if (any_ok) {
        for (unsigned long long t = regular_total; t >= 10; t /= 10)
            width++;
        if (width < min_width)
            width = min_width;
    }
    if (ncounts == 1 && nfiles == 1)
        width = 1;

    counts total = { 0, 0, 0, 0, 0 };
    int rc = 0;
    for (int i = 0; i < nfiles; i++) {
        if (fds[i] < 0) {
            errno = errs[i];
            sb_perr("%s", files[i]);
            rc = 1;
            continue;
        }
        counts cnt = { 0, 0, 0, 0, 0 };
        if (count_fd(fds[i], &cnt, show_m || show_L) != 0) {
            sb_perr("%s", files[i]);
            rc = 1;
        }
        if (fds[i] != STDIN_FILENO)
            close(fds[i]);
        print_counts(&cnt, named ? files[i] : NULL);
        total.lines += cnt.lines;
        total.words += cnt.words;
        total.chars += cnt.chars;
        total.bytes += cnt.bytes;
        if (cnt.maxlen > total.maxlen)
            total.maxlen = cnt.maxlen;
    }
    if (nfiles > 1)
        print_counts(&total, "总计");
    free(fds);
    free(errs);
    return rc;
}
