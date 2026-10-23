/*
 * uniq — 合并相邻的重复行
 *
 * 注意只比较“相邻”的行，所以通常先 sort 再 uniq：sort | uniq -c 是统计词频的经典组合。
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

SKBOX_APPLET(uniq, "去掉或统计相邻的重复行")

static const char usage[] =
    "用法: uniq [选项] [输入文件 [输出文件]]\n"
    "把相邻的重复行合并成一行输出。\n\n"
    "  -c      在每行前面显示它重复的次数\n"
    "  -d      只输出有重复的行（每组一次）\n"
    "  -u      只输出没有重复的行\n"
    "  -i      比较时忽略大小写\n"
    "  -f N    比较时跳过前 N 个字段（字段由空白分隔）\n"
    "  -s N    比较时跳过前 N 个字符\n";

static int opt_icase;
static long long skip_fields, skip_chars;

static const char *cmp_start(const char *s, size_t len, size_t *rest)
{
    const char *p = s, *end = s + len;
    for (long long f = 0; f < skip_fields && p < end; f++) {
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        while (p < end && *p != ' ' && *p != '\t')
            p++;
    }
    for (long long c = 0; c < skip_chars && p < end; c++)
        p++;
    *rest = (size_t)(end - p);
    return p;
}

static int same(const char *a, size_t la, const char *b, size_t lb)
{
    size_t ra, rb;
    const char *pa = cmp_start(a, la, &ra), *pb = cmp_start(b, lb, &rb);
    if (ra != rb)
        return 0;
    return opt_icase ? strncasecmp(pa, pb, ra) == 0 : memcmp(pa, pb, ra) == 0;
}

int applet_uniq(int argc, char **argv)
{
    int count = 0, only_dup = 0, only_uniq = 0, c;
    while ((c = sb_getopt(argc, argv, "cduif:s:", usage)) != -1) {
        switch (c) {
        case 'c': count = 1; break;
        case 'd': only_dup = 1; break;
        case 'u': only_uniq = 1; break;
        case 'i': opt_icase = 1; break;
        case 'f':
        case 's': {
            long long v;
            if (sb_parse_ll(optarg, &v) != 0 || v < 0) {
                sb_err("无效的数量 '%s'", optarg);
                return 1;
            }
            if (c == 'f')
                skip_fields = v;
            else
                skip_chars = v;
            break;
        }
        default:
            return 1;
        }
    }
    FILE *in = stdin, *out = stdout;
    if (optind < argc && strcmp(argv[optind], "-") != 0) {
        in = fopen(argv[optind], "r");
        if (!in) {
            sb_perr("%s", argv[optind]);
            return 1;
        }
    }
    if (optind + 1 < argc) {
        out = fopen(argv[optind + 1], "w");
        if (!out) {
            sb_perr("%s", argv[optind + 1]);
            return 1;
        }
    }
    if (optind + 2 < argc) {
        sb_err("多余的操作数 '%s'", argv[optind + 2]);
        return 1;
    }

    char *prev = NULL, *cur = NULL;
    size_t pcap = 0, ccap = 0;
    ssize_t plen = -1, clen;
    unsigned long long n = 0;
    for (;;) {
        clen = getline(&cur, &ccap, in);
        if (clen > 0 && cur[clen - 1] == '\n')
            cur[--clen] = 0;
        if (clen >= 0 && plen >= 0 && same(prev, (size_t)plen, cur, (size_t)clen)) {
            n++;
            continue;
        }
        /* 一组结束：按选项决定是否输出上一组 */
        if (plen >= 0) {
            int dup = n > 1;
            if ((!only_dup || dup) && (!only_uniq || !dup)) {
                if (count)
                    fprintf(out, "%7llu ", n);
                fwrite(prev, 1, (size_t)plen, out);
                putc('\n', out);
            }
        }
        if (clen < 0)
            break;
        /* 当前行成为新一组的代表：交换两个缓冲区，避免复制 */
        char *t = prev;
        prev = cur;
        cur = t;
        size_t tc = pcap;
        pcap = ccap;
        ccap = tc;
        plen = clen;
        n = 1;
    }
    free(prev);
    free(cur);
    if (ferror(in)) {
        sb_perr("读取失败");
        return 1;
    }
    if (out != stdout && fclose(out) != 0) {
        sb_perr("写入失败");
        return 1;
    }
    return 0;
}
