/*
 * xxd — 以十六进制显示文件内容（vim 附带的 xxd 的格式）
 *
 * 和 hexdump -C 类似，但字节按书写顺序两两成组（不做小端序转换），偏移量后面带冒号。
 */
#include "skbox.h"

#include <errno.h>
#include <ctype.h>
#include <unistd.h>

SKBOX_APPLET(xxd, "以十六进制显示文件内容（xxd 格式）")

static const char usage[] =
    "用法: xxd [选项] [文件]\n"
    "以十六进制显示文件内容：偏移量、每两个字节一组的十六进制、右侧的可打印字符。\n\n"
    "  -c 个数   每行显示的字节数（默认 16，-p 时默认 30）\n"
    "  -l 长度   只显示这么多字节\n"
    "  -s 偏移   从这个偏移量开始\n"
    "  -p        纯十六进制输出，不带偏移量和字符\n"
    "  -u        十六进制字母用大写\n";

int applet_xxd(int argc, char **argv)
{
    long long cols = -1, limit = -1, seek = 0;
    int plain = 0, upper = 0, c;
    while ((c = sb_getopt(argc, argv, "c:l:s:pu", usage)) != -1) {
        switch (c) {
        case 'p': plain = 1; break;
        case 'u': upper = 1; break;
        case 'c':
        case 'l':
        case 's': {
            long long v;
            if (sb_parse_size(optarg, &v) != 0 || v < 0) {
                sb_err("无效的数值 '%s'", optarg);
                return 1;
            }
            if (c == 'c')
                cols = v;
            else if (c == 'l')
                limit = v;
            else
                seek = v;
            break;
        }
        default:
            return 1;
        }
    }
    if (cols == 0 || cols > 256) {
        sb_err("每行字节数必须在 1~256 之间");
        return 1;
    }
    if (cols < 0)
        cols = plain ? 30 : 16;
    if (argc - optind > 1) {
        sb_err("多余的操作数 '%s'", argv[optind + 1]);
        return 1;
    }
    const char *file = optind < argc ? argv[optind] : "-";
    int fd = sb_open_input(file);
    if (fd < 0) {
        sb_perr("%s", file);
        return 1;
    }
    /* 能 seek 的文件直接跳过去，管道只能读掉 */
    if (seek > 0 && lseek(fd, (off_t)seek, SEEK_SET) < 0) {
        char tmp[4096];
        long long left = seek;
        while (left > 0) {
            ssize_t r = read(fd, tmp, left > (long long)sizeof tmp ? sizeof tmp : (size_t)left);
            if (r <= 0)
                break;
            left -= r;
        }
    }
    FILE *in = fdopen(fd, "r");    /* 用带缓冲的 stdio 逐字节读，避免每个字节一次系统调用 */
    if (!in) {
        sb_perr("%s", file);
        return 1;
    }
    const char *hex = upper ? "%02X" : "%02x";
    unsigned char line[256];
    unsigned long long off = (unsigned long long)seek, shown = 0;
    int rc = 0;
    for (;;) {
        size_t n = 0;
        while (n < (size_t)cols && (limit < 0 || (long long)shown < limit)) {
            int ch = getc(in);
            if (ch == EOF)
                break;
            line[n++] = (unsigned char)ch;
            shown++;
        }
        if (ferror(in)) {
            sb_perr("%s", file);
            rc = 1;
        }
        if (n == 0)
            break;
        if (plain) {
            for (size_t i = 0; i < n; i++)
                printf(hex, line[i]);
            putchar('\n');
        } else {
            printf("%08llx: ", off);
            int width = 0;
            for (size_t i = 0; i < n; i++) {
                printf(hex, line[i]);
                width += 2;
                if (i % 2 == 1) {
                    putchar(' ');
                    width++;
                }
            }
            /* 补齐到整行十六进制区域的宽度，让右边的字符列对齐 */
            int full = (int)cols * 2 + (int)(cols / 2);
            for (; width < full; width++)
                putchar(' ');
            putchar(' ');
            for (size_t i = 0; i < n; i++)
                putchar(isprint(line[i]) && line[i] < 0x7F ? line[i] : '.');
            putchar('\n');
        }
        off += n;
        if (n < (size_t)cols)
            break;
    }
    if (fd != STDIN_FILENO)
        fclose(in);
    return rc;
}
