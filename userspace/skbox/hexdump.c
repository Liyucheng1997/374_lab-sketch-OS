/*
 * hexdump — 以十六进制显示文件内容
 *
 * 文件说到底就是一串字节（0~255）。文本文件的字节恰好都是可打印字符的编码，
 * 而程序、图片等“二进制文件”里什么值都有。hexdump 让我们看到每个字节的真实数值。
 * 例如 x86 是“小端”CPU：默认格式把相邻两个字节当成一个 16 位数显示，低字节在后，
 * 所以 "he" 会显示成 6568（'e'=0x65，'h'=0x68）。
 */
#include "skbox.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(hexdump, "以十六进制显示文件内容")

static const char usage[] =
    "用法: hexdump [选项] [文件...]\n"
    "以十六进制显示文件内容。多个文件会被连在一起显示。\n\n"
    "  -C        规范格式：偏移量、16 个十六进制字节、右侧的可打印字符（最常用）\n"
    "  -n 长度   只显示这么多字节\n"
    "  -s 偏移   跳过开头这么多字节\n"
    "  -v        显示所有行（默认把重复的行压缩成一个 *）\n"
    "不加 -C 时，每 2 个字节按小端序显示成一个 16 位数。\n";

static int canonical, verbose;

static void print_line(unsigned long long off, const unsigned char *b, size_t n)
{
    if (canonical) {
        printf("%08llx  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (i < n)
                printf("%02x ", b[i]);
            else
                fputs("   ", stdout);
            if (i == 7)
                putchar(' ');
        }
        fputs(" |", stdout);
        for (size_t i = 0; i < n; i++)
            putchar(b[i] >= 0x20 && b[i] < 0x7F ? b[i] : '.');
        fputs("|\n", stdout);
    } else {
        printf("%07llx", off);
        for (size_t i = 0; i < 16; i += 2) {
            if (i < n) {
                unsigned v = b[i] | (i + 1 < n ? (unsigned)b[i + 1] << 8 : 0);
                printf(" %04x", v);
            } else {
                fputs("     ", stdout);
            }
        }
        putchar('\n');
    }
}

int applet_hexdump(int argc, char **argv)
{
    long long limit = -1, skip = 0;
    int c;
    while ((c = sb_getopt(argc, argv, "Cn:s:v", usage)) != -1) {
        switch (c) {
        case 'C': canonical = 1; break;
        case 'v': verbose = 1; break;
        case 'n':
        case 's': {
            long long v;
            if (sb_parse_size(optarg, &v) != 0 || v < 0) {
                sb_err("无效的数值 '%s'", optarg);
                return 1;
            }
            if (c == 'n')
                limit = v;
            else
                skip = v;
            break;
        }
        default:
            return 1;
        }
    }
    int nfiles = argc - optind, rc = 0;
    char *dash[] = { "-", NULL };
    char **files = nfiles ? argv + optind : dash;
    if (!nfiles)
        nfiles = 1;

    unsigned char line[16], prev[16];
    size_t have = 0;
    int have_prev = 0, starred = 0;
    unsigned long long off = (unsigned long long)skip, shown = 0;
    static unsigned char buf[65536];
    for (int i = 0; i < nfiles && limit != 0; i++) {
        int fd = sb_open_input(files[i]);
        if (fd < 0) {
            sb_perr("%s", files[i]);
            rc = 1;
            continue;
        }
        while (limit < 0 || (long long)shown < limit) {
            ssize_t r = read(fd, buf, sizeof buf);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                sb_perr("%s", files[i]);
                rc = 1;
                break;
            }
            if (r == 0)
                break;
            size_t k = 0;
            /* -s：跳过的字节横跨多个文件也算数 */
            while (skip > 0 && k < (size_t)r) {
                k++;
                skip--;
            }
            for (; k < (size_t)r && (limit < 0 || (long long)shown < limit); k++, shown++) {
                line[have++] = buf[k];
                if (have < 16)
                    continue;
                /* 和上一行完全一样的行只输出一个 *，节省屏幕（-v 关闭这个功能） */
                if (!verbose && have_prev && memcmp(line, prev, 16) == 0) {
                    if (!starred)
                        puts("*");
                    starred = 1;
                } else {
                    print_line(off, line, 16);
                    starred = 0;
                }
                memcpy(prev, line, 16);
                have_prev = 1;
                off += 16;
                have = 0;
            }
        }
        if (fd != STDIN_FILENO)
            close(fd);
    }
    if (have) {
        print_line(off, line, have);
        off += have;
    }
    if (off > 0 || shown > 0)
        printf(canonical ? "%08llx\n" : "%07llx\n", off);
    return rc;
}
