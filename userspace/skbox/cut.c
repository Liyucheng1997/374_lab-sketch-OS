/*
 * cut — 从每一行中截取指定的部分（字节、字符或字段）
 *
 * 例如 /etc/passwd 每行是用冒号分隔的字段：cut -d: -f1 /etc/passwd 取出所有用户名。
 */
#include "skbox.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

SKBOX_APPLET(cut, "截取每行中的指定字段或字符")

static const char usage[] =
    "用法: cut 选项 [文件...]\n"
    "输出每行中被选中的部分。必须且只能使用 -b、-c、-f 中的一个。\n\n"
    "  -b 列表   选择字节\n"
    "  -c 列表   选择字符（按 UTF-8 计算，一个汉字算一个字符）\n"
    "  -f 列表   选择字段（默认用制表符分隔）\n"
    "  -d 字符   字段分隔符（配合 -f）\n"
    "  -s        配合 -f：不输出不含分隔符的行\n"
    "  --output-delimiter=字符串   输出时字段之间用这个字符串连接\n\n"
    "列表由逗号分隔的范围组成：N 第 N 个，N-M 第 N 到 M 个，N- 第 N 个到最后，-M 第 1 到 M 个。\n"
    "例如: cut -d: -f1,6 /etc/passwd      cut -c1-10 文件\n";

typedef struct {
    long lo, hi;   /* 从 1 开始，闭区间 */
} range;

static range *g_ranges;
static int g_nranges;

static int parse_list(const char *s)
{
    char *copy = strdup(s), *save = NULL;
    if (!copy)
        return -1;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        range r = { 1, LONG_MAX };
        char *dash = strchr(tok, '-');
        char *end;
        if (!dash) {
            r.lo = r.hi = strtol(tok, &end, 10);
            if (*end || end == tok)
                goto bad;
        } else {
            if (dash != tok) {
                r.lo = strtol(tok, &end, 10);
                if (end != dash)
                    goto bad;
            }
            if (dash[1]) {
                r.hi = strtol(dash + 1, &end, 10);
                if (*end)
                    goto bad;
            }
            if (dash == tok && !dash[1])
                goto bad;
        }
        if (r.lo < 1 || r.hi < r.lo)
            goto bad;
        range *nr = realloc(g_ranges, sizeof *nr * (size_t)(g_nranges + 1));
        if (!nr)
            goto bad;
        g_ranges = nr;
        g_ranges[g_nranges++] = r;
    }
    free(copy);
    return g_nranges ? 0 : -1;
bad:
    free(copy);
    return -1;
}

static int selected(long i)
{
    for (int k = 0; k < g_nranges; k++)
        if (i >= g_ranges[k].lo && i <= g_ranges[k].hi)
            return 1;
    return 0;
}

int applet_cut(int argc, char **argv)
{
    static const struct option lo[] = {
        { "output-delimiter", required_argument, NULL, 'O' },
        { 0, 0, 0, 0 },
    };
    char mode = 0, delim = '\t';
    int only_delimited = 0, c;
    const char *list = NULL, *outdelim = NULL;
    while ((c = sb_getopt_long(argc, argv, "b:c:f:d:sn", lo, usage)) != -1) {
        switch (c) {
        case 'b':
        case 'c':
        case 'f':
            if (mode) {
                sb_err("只能指定一种列表（-b、-c 或 -f）");
                return 1;
            }
            mode = (char)c;
            list = optarg;
            break;
        case 'd':
            if (strlen(optarg) != 1) {
                sb_err("分隔符必须是单个字符");
                return 1;
            }
            delim = optarg[0];
            break;
        case 's': only_delimited = 1; break;
        case 'n': break;
        case 'O': outdelim = optarg; break;
        default:  return 1;
        }
    }
    if (!mode) {
        sb_err("必须指定 -b、-c 或 -f 中的一个");
        sb_hint();
        return 1;
    }
    if (parse_list(list) != 0) {
        sb_err("无效的列表 '%s'（字段和位置从 1 开始编号）", list);
        return 1;
    }
    char dstr[2] = { delim, 0 };
    if (!outdelim)
        outdelim = mode == 'f' ? dstr : NULL;

    int nfiles = argc - optind, rc = 0;
    char *dash[] = { "-", NULL };
    char **files = nfiles ? argv + optind : dash;
    if (!nfiles)
        nfiles = 1;
    char *line = NULL;
    size_t cap = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        FILE *f = strcmp(files[fi], "-") ? fopen(files[fi], "r") : stdin;
        if (!f) {
            sb_perr("%s", files[fi]);
            rc = 1;
            continue;
        }
        ssize_t len;
        while ((len = getline(&line, &cap, f)) >= 0) {
            int had_nl = len > 0 && line[len - 1] == '\n';
            if (had_nl)
                line[--len] = 0;
            if (mode == 'f') {
                if (!memchr(line, delim, (size_t)len)) {
                    if (!only_delimited) {
                        fwrite(line, 1, (size_t)len, stdout);
                        putchar('\n');
                    }
                    continue;
                }
                long field = 1;
                int first = 1;
                char *p = line, *end = line + len;
                while (p <= end) {
                    char *q = memchr(p, delim, (size_t)(end - p));
                    if (!q)
                        q = end;
                    if (selected(field)) {
                        if (!first)
                            fputs(outdelim, stdout);
                        fwrite(p, 1, (size_t)(q - p), stdout);
                        first = 0;
                    }
                    field++;
                    p = q + 1;
                }
                putchar('\n');
            } else {
                long idx = 1, last_sel = -10;
                for (ssize_t i = 0; i < len;) {
                    int k = 1;
                    if (mode == 'c') {
                        uint32_t cp;
                        k = sk_utf8_decode(line + i, (size_t)(len - i), &cp);
                    }
                    if (selected(idx)) {
                        /* 用了 --output-delimiter 时，不相邻的两段之间插入分隔符 */
                        if (outdelim && last_sel >= 0 && last_sel != idx - 1)
                            fputs(outdelim, stdout);
                        fwrite(line + i, 1, (size_t)k, stdout);
                        last_sel = idx;
                    }
                    i += k;
                    idx++;
                }
                putchar('\n');
            }
        }
        if (f != stdin)
            fclose(f);
    }
    free(line);
    return rc;
}
