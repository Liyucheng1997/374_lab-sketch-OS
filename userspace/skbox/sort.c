/*
 * sort — 对文本行排序
 *
 * 默认按整行的字节值比较（和 C 语言 strcmp 一样）。-k 指定“键”：只比较行中的某几个字段。
 * 我们用归并排序：它是“稳定”的（相等的行保持原来的先后顺序），-u 去重时需要这个性质。
 */
#include "skbox.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(sort, "对文本行排序")

static const char usage[] =
    "用法: sort [选项] [文件...]\n"
    "把所有文件（默认标准输入）的行合在一起排序后输出。\n\n"
    "  -n          按数值大小比较（\"10\" 排在 \"9\" 后面）\n"
    "  -r          逆序\n"
    "  -u          相等的行只输出第一行\n"
    "  -f          忽略大小写\n"
    "  -b          忽略字段开头的空白\n"
    "  -s          稳定排序：键相等时保持原来的顺序（不再比较整行）\n"
    "  -t 字符     字段分隔符（默认：空白和非空白的交界处）\n"
    "  -k 起[,止]  按键排序。位置写成 字段号[.字符号]，可以在后面加 n r f b，例如：\n"
    "              -k2  从第 2 个字段到行尾    -k2,2  只用第 2 个字段    -k3,3nr  第 3 字段按数值逆序\n"
    "  -c          只检查是否已经排好序\n"
    "  -o 文件     输出到文件（可以和输入是同一个文件）\n";

typedef struct {
    size_t sword, schar, eword, echar;   /* 从 0 开始；eword=SIZE_MAX 表示到行尾 */
    int numeric, reverse, fold, skip_sblanks, skip_eblanks;
    int has_opts;
} sortkey;

typedef struct {
    char *text;
    size_t len;
} line_t;

static sortkey *g_keys;
static int g_nkeys;
static sortkey g_global;
static int g_tab = -1;        /* -1：默认的空白分隔 */
static int g_unique, g_stable;

static int is_blank(unsigned char c)
{
    return c == ' ' || c == '\t';
}

/* 找到键的起点（与 GNU sort 的 begfield 相同的规则） */
static const char *key_begin(const line_t *l, const sortkey *k)
{
    const char *p = l->text, *lim = l->text + l->len;
    size_t sword = k->sword;
    if (g_tab >= 0) {
        while (p < lim && sword--) {
            while (p < lim && *p != (char)g_tab)
                p++;
            if (p < lim)
                p++;
        }
    } else {
        /* 默认：字段 = 前导空白 + 非空白，空白属于后面那个字段 */
        while (p < lim && sword--) {
            while (p < lim && is_blank((unsigned char)*p))
                p++;
            while (p < lim && !is_blank((unsigned char)*p))
                p++;
        }
    }
    if (k->skip_sblanks)
        while (p < lim && is_blank((unsigned char)*p))
            p++;
    return (size_t)(lim - p) < k->schar ? lim : p + k->schar;
}

static const char *key_end(const line_t *l, const sortkey *k)
{
    const char *p = l->text, *lim = l->text + l->len;
    if (k->eword == SIZE_MAX)
        return lim;
    size_t eword = k->eword, echar = k->echar;
    if (echar == 0)
        eword++;        /* 没写字符位置：包含整个结束字段 */
    if (g_tab >= 0) {
        while (p < lim && eword--) {
            while (p < lim && *p != (char)g_tab)
                p++;
            if (p < lim && (eword || echar))
                p++;
        }
    } else {
        while (p < lim && eword--) {
            while (p < lim && is_blank((unsigned char)*p))
                p++;
            while (p < lim && !is_blank((unsigned char)*p))
                p++;
        }
    }
    if (echar) {
        if (k->skip_eblanks)
            while (p < lim && is_blank((unsigned char)*p))
                p++;
        p = (size_t)(lim - p) < echar ? lim : p + echar;
    }
    return p;
}

/* 数值比较：直接比较数字串，不转成浮点数，所以多长的数都不会丢精度 */
static int num_compare(const char *a, const char *alim, const char *b, const char *blim)
{
    while (a < alim && is_blank((unsigned char)*a))
        a++;
    while (b < blim && is_blank((unsigned char)*b))
        b++;
    int na = a < alim && *a == '-', nb = b < blim && *b == '-';
    if (na)
        a++;
    if (nb)
        b++;
    while (a < alim && *a == '0')
        a++;
    while (b < blim && *b == '0')
        b++;
    const char *ai = a, *bi = b;
    while (a < alim && *a >= '0' && *a <= '9')
        a++;
    while (b < blim && *b >= '0' && *b <= '9')
        b++;
    size_t la = (size_t)(a - ai), lb = (size_t)(b - bi);
    const char *af = a, *bf = b, *afe = a, *bfe = b;
    if (af < alim && *af == '.') {
        afe = ++af;
        while (afe < alim && *afe >= '0' && *afe <= '9')
            afe++;
    }
    if (bf < blim && *bf == '.') {
        bfe = ++bf;
        while (bfe < blim && *bfe >= '0' && *bfe <= '9')
            bfe++;
    }
    /* 判断是不是 0（-0 和 0 相等） */
    int za = la == 0, zb = lb == 0;
    for (const char *q = af; za && q < afe; q++)
        if (*q != '0')
            za = 0;
    for (const char *q = bf; zb && q < bfe; q++)
        if (*q != '0')
            zb = 0;
    if (za)
        na = 0;
    if (zb)
        nb = 0;
    if (na != nb)
        return na ? -1 : 1;
    int r = 0;
    if (la != lb) {
        r = la < lb ? -1 : 1;
    } else {
        int c = memcmp(ai, bi, la);
        if (c)
            r = c < 0 ? -1 : 1;
        else {
            /* 小数部分逐位比较，短的一方补 0 */
            const char *x = af, *y = bf;
            while (!r && (x < afe || y < bfe)) {
                char cx = x < afe ? *x++ : '0';
                char cy = y < bfe ? *y++ : '0';
                if (cx != cy)
                    r = cx < cy ? -1 : 1;
            }
        }
    }
    return na ? -r : r;
}

static int text_compare(const char *a, size_t la, const char *b, size_t lb, int fold)
{
    if (!fold) {
        int c = memcmp(a, b, SK_MIN(la, lb));
        if (c)
            return c;
        return la < lb ? -1 : la > lb;
    }
    for (size_t i = 0; i < la && i < lb; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return la < lb ? -1 : la > lb;
}

static int key_compare(const line_t *a, const line_t *b)
{
    for (int i = 0; i < g_nkeys; i++) {
        const sortkey *k = &g_keys[i];
        const char *ta = key_begin(a, k), *la = key_end(a, k);
        const char *tb = key_begin(b, k), *lb = key_end(b, k);
        if (la < ta)
            la = ta;
        if (lb < tb)
            lb = tb;
        int d;
        if (k->numeric)
            d = num_compare(ta, la, tb, lb);
        else
            d = text_compare(ta, (size_t)(la - ta), tb, (size_t)(lb - tb), k->fold);
        if (d)
            return k->reverse ? -d : d;
    }
    return 0;
}

static int compare(const line_t *a, const line_t *b)
{
    if (g_nkeys) {
        int d = key_compare(a, b);
        if (d || g_unique || g_stable)
            return d;
    }
    /* 所有键都相等：最后按整行字节比较，结果才是确定的 */
    int d = text_compare(a->text, a->len, b->text, b->len, 0);
    return g_global.reverse ? -d : d;
}

static void merge_sort(line_t *v, line_t *tmp, size_t n)
{
    if (n < 2)
        return;
    size_t mid = n / 2;
    merge_sort(v, tmp, mid);
    merge_sort(v + mid, tmp, n - mid);
    size_t i = 0, j = mid, k = 0;
    while (i < mid && j < n)
        tmp[k++] = compare(&v[j], &v[i]) < 0 ? v[j++] : v[i++];   /* 相等时取左边：稳定 */
    while (i < mid)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof *v);
}

/* 解析 -k 后面的 n r f b 等排序选项 */
static const char *parse_key_opts(const char *s, sortkey *k, int is_end)
{
    for (;; s++) {
        switch (*s) {
        case 'n': k->numeric = 1; k->has_opts = 1; break;
        case 'r': k->reverse = 1; k->has_opts = 1; break;
        case 'f': k->fold = 1; k->has_opts = 1; break;
        case 'b':
            if (is_end)
                k->skip_eblanks = 1;
            else
                k->skip_sblanks = 1;
            k->has_opts = 1;
            break;
        default:
            return s;
        }
    }
}

static int parse_count(const char **s, size_t *out)
{
    const char *p = *s;
    if (*p < '0' || *p > '9')
        return -1;
    size_t v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (size_t)(*p++ - '0');
    *out = v;
    *s = p;
    return 0;
}

static int parse_key(const char *spec, sortkey *k)
{
    memset(k, 0, sizeof *k);
    const char *s = spec;
    if (parse_count(&s, &k->sword) != 0 || k->sword == 0)
        return -1;
    k->sword--;
    if (*s == '.') {
        s++;
        if (parse_count(&s, &k->schar) != 0 || k->schar == 0)
            return -1;
        k->schar--;
    }
    s = parse_key_opts(s, k, 0);
    if (*s != ',') {
        k->eword = SIZE_MAX;
        k->echar = 0;
    } else {
        s++;
        if (parse_count(&s, &k->eword) != 0 || k->eword == 0)
            return -1;
        k->eword--;
        if (*s == '.') {
            s++;
            if (parse_count(&s, &k->echar) != 0)
                return -1;
        }
        s = parse_key_opts(s, k, 1);
    }
    return *s ? -1 : 0;
}

static int read_lines(int fd, line_t **v, size_t *n, size_t *cap)
{
    FILE *f = fdopen(fd, "r");
    if (!f)
        return -1;
    char *buf = NULL;
    size_t bcap = 0;
    ssize_t len;
    while ((len = getline(&buf, &bcap, f)) >= 0) {
        if (len > 0 && buf[len - 1] == '\n')
            buf[--len] = 0;
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 1024;
            line_t *nv = realloc(*v, sizeof **v * *cap);
            if (!nv) {
                free(buf);
                return -1;
            }
            *v = nv;
        }
        char *copy = malloc((size_t)len + 1);
        if (!copy) {
            free(buf);
            return -1;
        }
        memcpy(copy, buf, (size_t)len + 1);
        (*v)[*n].text = copy;
        (*v)[*n].len = (size_t)len;
        (*n)++;
    }
    int err = ferror(f);
    free(buf);
    if (fd == STDIN_FILENO)
        clearerr(f);
    else
        fclose(f);
    return err ? -1 : 0;
}

int applet_sort(int argc, char **argv)
{
    const char *outfile = NULL;
    int check = 0, c;
    while ((c = sb_getopt(argc, argv, "nrufbst:k:co:", usage)) != -1) {
        switch (c) {
        case 'n': g_global.numeric = 1; break;
        case 'r': g_global.reverse = 1; break;
        case 'f': g_global.fold = 1; break;
        case 'b': g_global.skip_sblanks = g_global.skip_eblanks = 1; break;
        case 'u': g_unique = 1; break;
        case 's': g_stable = 1; break;
        case 'c': check = 1; break;
        case 'o': outfile = optarg; break;
        case 't':
            if (strcmp(optarg, "\\0") == 0)
                g_tab = 0;
            else if (strlen(optarg) != 1) {
                sb_err("分隔符必须是单个字符: '%s'", optarg);
                return 2;
            } else
                g_tab = (unsigned char)optarg[0];
            break;
        case 'k': {
            sortkey *nk = realloc(g_keys, sizeof *nk * (size_t)(g_nkeys + 1));
            if (!nk)
                return 2;
            g_keys = nk;
            if (parse_key(optarg, &g_keys[g_nkeys]) != 0) {
                sb_err("无效的键 '%s'", optarg);
                sb_hint();
                return 2;
            }
            g_nkeys++;
            break;
        }
        default:
            return 2;
        }
    }
    /* 没有单独写选项的键，继承全局选项（-n -r -f -b） */
    for (int i = 0; i < g_nkeys; i++) {
        sortkey *k = &g_keys[i];
        if (!k->has_opts) {
            k->numeric = g_global.numeric;
            k->reverse = g_global.reverse;
            k->fold = g_global.fold;
            k->skip_sblanks = g_global.skip_sblanks;
            k->skip_eblanks = g_global.skip_eblanks;
        }
    }
    /* 没有 -k 但有 -n/-f/-b：把整行当作一个键 */
    if (!g_nkeys && (g_global.numeric || g_global.fold || g_global.skip_sblanks)) {
        g_keys = malloc(sizeof *g_keys);
        if (!g_keys)
            return 2;
        g_keys[0] = g_global;
        g_keys[0].sword = 0;
        g_keys[0].schar = 0;
        g_keys[0].eword = SIZE_MAX;
        g_nkeys = 1;
    }

    line_t *v = NULL;
    size_t n = 0, cap = 0;
    int rc = 0;
    int nfiles = argc - optind;
    char *dash[] = { "-", NULL };
    char **files = nfiles ? argv + optind : dash;
    if (!nfiles)
        nfiles = 1;
    for (int i = 0; i < nfiles; i++) {
        int fd = sb_open_input(files[i]);
        if (fd < 0 || read_lines(fd, &v, &n, &cap) != 0) {
            sb_perr("读取 '%s' 失败", files[i]);
            return 2;
        }
    }

    if (check) {
        for (size_t i = 1; i < n; i++) {
            int d = compare(&v[i - 1], &v[i]);
            if (d > 0 || (g_unique && d == 0)) {
                sb_err("%s:%zu: 无序: %s", nfiles == 1 ? files[0] : "-", i + 1, v[i].text);
                return 1;
            }
        }
        return 0;
    }

    line_t *tmp = malloc(sizeof *tmp * (n ? n : 1));
    if (!tmp) {
        sb_err("内存不足");
        return 2;
    }
    merge_sort(v, tmp, n);
    free(tmp);

    FILE *out = stdout;
    if (outfile && strcmp(outfile, "-") != 0) {
        out = fopen(outfile, "w");
        if (!out) {
            sb_perr("无法打开输出文件 '%s'", outfile);
            return 2;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (g_unique && i > 0 && compare(&v[i - 1], &v[i]) == 0)
            continue;
        fwrite(v[i].text, 1, v[i].len, out);
        putc('\n', out);
    }
    if (out != stdout && fclose(out) != 0) {
        sb_perr("写入 '%s' 失败", outfile);
        rc = 2;
    }
    for (size_t i = 0; i < n; i++)
        free(v[i].text);
    free(v);
    return rc;
}
