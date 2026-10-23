/*
 * df — 显示各个文件系统的空间使用情况
 *
 * /proc/mounts 列出了当前挂载的所有文件系统；对每个挂载点调用 statfs()，
 * 内核就会告诉我们这个文件系统一共多少块、空闲多少块、普通用户还能用多少块。
 * （“空闲”和“可用”的差别：ext 系列文件系统会给 root 预留一部分空间。）
 */
#include "skbox.h"

#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>

SKBOX_APPLET(df, "显示文件系统的磁盘空间使用情况")

static const char usage[] =
    "用法: df [选项] [文件...]\n"
    "显示每个已挂载文件系统的总空间、已用、可用和使用率。给出文件时只显示它所在的文件系统。\n"
    "默认单位是 KB（1024 字节）；总块数为 0 的虚拟文件系统（proc、sysfs……）默认不显示。\n\n"
    "  -h    用 K、M、G 显示\n"
    "  -k    以 KB 为单位（默认）\n"
    "  -m    以 MB 为单位\n"
    "  -T    显示文件系统类型\n"
    "  -a    显示所有文件系统，包括虚拟文件系统\n"
    "  -i    显示 inode 的使用情况而不是块\n";

typedef struct {
    char *src, *dir, *type;
} mount_ent;

/* /proc/mounts 里的空格等字符写成八进制转义（\040），这里还原 */
static void unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r;) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static mount_ent *read_mounts(int *n, char **buf)
{
    *n = 0;
    *buf = sk_read_file("/proc/self/mounts", NULL);
    if (!*buf)
        *buf = sk_read_file("/proc/mounts", NULL);
    if (!*buf)
        return NULL;
    int cap = 32;
    mount_ent *v = malloc(sizeof *v * (size_t)cap);
    if (!v)
        return NULL;
    for (char *line = *buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *save = NULL;
        char *src = strtok_r(line, " ", &save), *dir = strtok_r(NULL, " ", &save), *type = strtok_r(NULL, " ", &save);
        if (src && dir && type) {
            if (*n == cap) {
                cap *= 2;
                mount_ent *nv = realloc(v, sizeof *v * (size_t)cap);
                if (!nv)
                    break;
                v = nv;
            }
            unescape(src);
            unescape(dir);
            v[*n].src = src;
            v[*n].dir = dir;
            v[*n].type = type;
            (*n)++;
        }
        line = nl ? nl + 1 : NULL;
    }
    return v;
}

typedef struct {
    char cols[7][64];
} row_t;

int applet_df(int argc, char **argv)
{
    int human = 0, show_type = 0, all = 0, inodes = 0, c;
    unsigned long long unit = 1024;
    while ((c = sb_getopt(argc, argv, "hkmTai", usage)) != -1) {
        switch (c) {
        case 'h': human = 1; break;
        case 'k': unit = 1024; human = 0; break;
        case 'm': unit = 1024 * 1024; human = 0; break;
        case 'T': show_type = 1; break;
        case 'a': all = 1; break;
        case 'i': inodes = 1; break;
        default:  return 1;
        }
    }
    char *buf;
    int nm;
    mount_ent *m = read_mounts(&nm, &buf);
    if (!m) {
        sb_perr("无法读取 /proc/mounts（/proc 挂载了吗？）");
        return 1;
    }
    /* 选出要显示的挂载点：给了文件时，找包含它的那个（设备号相同、路径最长的挂载点） */
    int *pick = calloc((size_t)nm + 1, sizeof *pick);
    int npick = 0, rc = 0;
    if (!pick)
        return 1;
    if (optind < argc) {
        for (int i = optind; i < argc; i++) {
            struct stat st;
            if (stat(argv[i], &st) != 0) {
                sb_perr("%s", argv[i]);
                rc = 1;
                continue;
            }
            int best = -1;
            size_t best_len = 0;
            for (int k = 0; k < nm; k++) {
                struct stat ms;
                if (stat(m[k].dir, &ms) != 0 || ms.st_dev != st.st_dev)
                    continue;
                size_t l = strlen(m[k].dir);
                if (best < 0 || l >= best_len) {
                    best = k;
                    best_len = l;
                }
            }
            if (best >= 0)
                pick[npick++] = best;
        }
    } else {
        for (int k = 0; k < nm; k++)
            pick[npick++] = k;
    }

    row_t *rows = calloc((size_t)npick + 1, sizeof *rows);
    if (!rows)
        return 1;
    int ncols = show_type ? 7 : 6;
    const char *hdr_size = inodes ? "Inode" : human ? "大小" : unit == 1024 ? "1K-块" : "1M-块";
    const char *hdr[7] = { "文件系统", "类型", hdr_size, "已用", "可用", "已用%", "挂载点" };
    int nrows = 0;
    for (int col = 0, k = 0; col < 7; col++) {
        if (col == 1 && !show_type)
            continue;
        sk_strlcpy(rows[0].cols[k++], hdr[col], sizeof rows[0].cols[0]);
    }
    nrows = 1;
    for (int i = 0; i < npick; i++) {
        mount_ent *e = &m[pick[i]];
        struct statfs sf;
        if (statfs(e->dir, &sf) != 0) {
            if (optind < argc) {
                sb_perr("%s", e->dir);
                rc = 1;
            }
            continue;
        }
        if (!all && optind >= argc && (inodes ? sf.f_files == 0 : sf.f_blocks == 0))
            continue;
        unsigned long long bs = sf.f_frsize ? (unsigned long long)sf.f_frsize : (unsigned long long)sf.f_bsize;
        unsigned long long total, used, avail;
        if (inodes) {
            total = sf.f_files;
            used = sf.f_files - sf.f_ffree;
            avail = sf.f_ffree;
        } else {
            total = (unsigned long long)sf.f_blocks * bs;
            used = ((unsigned long long)sf.f_blocks - sf.f_bfree) * bs;
            avail = (unsigned long long)sf.f_bavail * bs;
        }
        row_t *r = &rows[nrows++];
        int k = 0;
        sk_strlcpy(r->cols[k++], e->src, sizeof r->cols[0]);
        if (show_type)
            sk_strlcpy(r->cols[k++], e->type, sizeof r->cols[0]);
        unsigned long long vals[3] = { total, used, avail };
        for (int v = 0; v < 3; v++) {
            if (inodes)
                snprintf(r->cols[k++], sizeof r->cols[0], "%llu", vals[v]);
            else if (human)
                sk_fmt_size_short(r->cols[k++], sizeof r->cols[0], vals[v]);
            else
                snprintf(r->cols[k++], sizeof r->cols[0], "%llu", (vals[v] + unit - 1) / unit);
        }
        /* 使用率 = 已用 / (已用 + 可用)，向上取整（和 GNU df 一样，不算 root 预留的部分） */
        unsigned long long denom = used + avail;
        if (denom)
            snprintf(r->cols[k++], sizeof r->cols[0], "%llu%%", (used * 100 + denom - 1) / denom);
        else
            sk_strlcpy(r->cols[k++], "-", sizeof r->cols[0]);
        sk_strlcpy(r->cols[k++], e->dir, sizeof r->cols[0]);
    }

    /* 按显示宽度对齐：文字列左对齐，数字列右对齐 */
    int width[7] = { 0 };
    for (int i = 0; i < nrows; i++)
        for (int k = 0; k < ncols; k++)
            width[k] = SK_MAX(width[k], sk_utf8_width(rows[i].cols[k]));
    for (int i = 0; i < nrows; i++) {
        for (int k = 0; k < ncols; k++) {
            int text = k == 0 || k == ncols - 1 || (show_type && k == 1);
            if (k)
                putchar(' ');
            if (k == ncols - 1)
                fputs(rows[i].cols[k], stdout);
            else
                sb_put_padded(stdout, rows[i].cols[k], width[k], text);
        }
        putchar('\n');
    }
    free(rows);
    free(pick);
    free(m);
    free(buf);
    return rc;
}
