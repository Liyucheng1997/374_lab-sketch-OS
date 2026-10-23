/*
 * ls — 列出目录内容
 *
 * 目录本身也是一种文件，内容是一张“名字 → inode 编号”的表。
 *   opendir()/readdir()  读出这张表（底层是 getdents64 系统调用），只有名字和 inode 号；
 *   lstat()              再按名字查 inode，拿到类型、权限、大小、时间等属性。
 * 所以 ls -l 要比 ls 慢：每个文件都要多一次系统调用。我们只在确实需要属性时才调用 lstat。
 */
#include "skbox.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sysmacros.h>
#include <unistd.h>

SKBOX_APPLET(ls, "列出目录内容")

static const char usage[] =
    "用法: ls [选项] [文件...]\n"
    "列出文件的信息；参数是目录时列出目录里的内容（默认是当前目录）。\n"
    "默认按名字排序。输出到终端时分栏显示并带颜色。\n\n"
    "  -l    长格式：权限、链接数、属主、组、大小、修改时间、名字\n"
    "  -a    显示所有文件，包括以 . 开头的隐藏文件和 . ..\n"
    "  -A    同 -a，但不显示 . 和 ..\n"
    "  -h    配合 -l：用 K、M、G 显示大小\n"
    "  -1    每行只显示一个文件\n"
    "  -C    分栏显示（输出不是终端时也分栏）\n"
    "  -R    递归列出子目录\n"
    "  -d    列出目录本身，而不是它的内容\n"
    "  -t    按修改时间排序，最新的在前\n"
    "  -S    按文件大小排序，最大的在前\n"
    "  -r    逆序排列\n"
    "  -U    不排序，按目录里的原始顺序列出\n"
    "  -F    在名字后面加类型标记：/ 目录  * 可执行  @ 符号链接  | 管道  = 套接字\n"
    "  -i    显示 inode 编号\n"
    "  -n    配合 -l：显示数字形式的用户和组 id\n"
    "  -w 列数        假定终端宽度\n"
    "  --color[=何时] 着色：always 总是、never 从不、auto 输出到终端时（默认）\n\n"
    "退出状态：0 正常，1 有小问题（如无法进入子目录），2 严重问题（如无法访问命令行参数）\n";

typedef struct {
    char *name;           /* 显示的名字 */
    char *path;           /* 实际访问用的路径 */
    struct stat st;
    int have_st;          /* st 有效 */
    unsigned char dtype;  /* 没有 stat 时用 readdir 给出的类型 */
    char *link;           /* 符号链接的内容 */
    int link_ok;          /* 链接目标存在 */
    mode_t link_mode;     /* 链接目标的类型 */
    int width;            /* 名字 + 类型标记的显示宽度 */
} entry;

enum { FMT_ONE, FMT_COLUMNS, FMT_LONG };
enum { SORT_NAME, SORT_TIME, SORT_SIZE, SORT_NONE };

static int fmt, sort_by, reverse, show_all, almost_all, human, recursive, list_dirs;
static int classify, show_inode, numeric_ids, use_color, line_width, sanitize;
static int status;          /* 退出状态 */
static time_t now_sec;

static void set_status(int s)
{
    if (s > status)
        status = s;
}

/* ---------- 颜色和类型标记 ---------- */

static mode_t entry_mode(const entry *e)
{
    if (e->have_st)
        return e->st.st_mode;
    switch (e->dtype) {
    case DT_DIR:  return S_IFDIR;
    case DT_LNK:  return S_IFLNK;
    case DT_FIFO: return S_IFIFO;
    case DT_SOCK: return S_IFSOCK;
    case DT_CHR:  return S_IFCHR;
    case DT_BLK:  return S_IFBLK;
    default:      return S_IFREG;
    }
}

static int has_ext(const char *name, const char *const *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return 0;
    for (; *exts; exts++)
        if (strcasecmp(dot + 1, *exts) == 0)
            return 1;
    return 0;
}

/* 和 GNU 的默认配色（dircolors）一致 */
static const char *color_for_mode(mode_t m, const char *name, int orphan)
{
    static const char *const archives[] = { "tar", "tgz", "gz", "xz", "bz2", "zst", "zip", "7z",
                                            "rar", "cpio", "deb", "rpm", "lz", "lzma", NULL };
    static const char *const images[] = { "png", "jpg", "jpeg", "gif", "bmp", "ppm", "pgm", "svg",
                                          "ico", "webp", "tif", "tiff", NULL };
    static const char *const audio[] = { "mp3", "wav", "ogg", "flac", "m4a", "aac", NULL };
    if (orphan)
        return "40;31;01";
    switch (m & S_IFMT) {
    case S_IFDIR:
        if ((m & S_ISVTX) && (m & S_IWOTH))
            return "30;42";
        if (m & S_IWOTH)
            return "34;42";
        if (m & S_ISVTX)
            return "37;44";
        return "01;34";
    case S_IFLNK:  return "01;36";
    case S_IFIFO:  return "40;33";
    case S_IFSOCK: return "01;35";
    case S_IFCHR:
    case S_IFBLK:  return "40;33;01";
    }
    if (m & S_ISUID)
        return "37;41";
    if (m & S_ISGID)
        return "30;43";
    if (m & (S_IXUSR | S_IXGRP | S_IXOTH))
        return "01;32";
    if (has_ext(name, archives))
        return "01;31";
    if (has_ext(name, images))
        return "01;35";
    if (has_ext(name, audio))
        return "00;36";
    return NULL;
}

static char indicator_for(mode_t m)
{
    switch (m & S_IFMT) {
    case S_IFDIR:  return '/';
    case S_IFLNK:  return '@';
    case S_IFIFO:  return '|';
    case S_IFSOCK: return '=';
    case S_IFREG:  return (m & (S_IXUSR | S_IXGRP | S_IXOTH)) ? '*' : 0;
    }
    return 0;
}

/* 输出名字：控制字符显示成 ?（防止文件名里的转义序列捣乱终端） */
static void put_name(const char *name)
{
    if (!sanitize) {
        fputs(name, stdout);
        return;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        putchar((*p < 0x20 || *p == 0x7F) ? '?' : *p);
}

static void print_name(const entry *e, int in_long)
{
    mode_t m = entry_mode(e);
    const char *col = NULL;
    if (use_color) {
        int orphan = S_ISLNK(m) && e->have_st && !e->link_ok && e->link;
        col = color_for_mode(m, e->name, orphan);
    }
    if (col)
        printf("\033[%sm", col);
    put_name(e->name);
    if (col)
        fputs("\033[0m", stdout);
    if (in_long && S_ISLNK(m) && e->link) {
        fputs(" -> ", stdout);
        const char *tcol = use_color ? color_for_mode(e->link_ok ? e->link_mode : 0, e->link, !e->link_ok) : NULL;
        if (tcol)
            printf("\033[%sm", tcol);
        put_name(e->link);
        if (tcol)
            fputs("\033[0m", stdout);
        if (classify && e->link_ok) {
            char ind = indicator_for(e->link_mode);
            if (ind && ind != '@')
                putchar(ind);
        }
        return;
    }
    if (classify) {
        char ind = indicator_for(m);
        if (ind)
            putchar(ind);
    }
}

/* ---------- 收集条目 ---------- */

static int need_stat(void)
{
    return fmt == FMT_LONG || sort_by == SORT_TIME || sort_by == SORT_SIZE || use_color ||
           classify || show_inode || recursive;
}

/* 读取条目的属性；follow_dir：命令行参数是指向目录的符号链接时跟随它 */
static int fill_entry(entry *e, int follow_dir)
{
    if (lstat(e->path, &e->st) != 0)
        return -1;
    e->have_st = 1;
    if (S_ISLNK(e->st.st_mode)) {
        struct stat t;
        if (stat(e->path, &t) == 0) {
            e->link_ok = 1;
            e->link_mode = t.st_mode;
            if (follow_dir && S_ISDIR(t.st_mode)) {
                e->st = t;
                return 0;
            }
        }
        if (fmt == FMT_LONG || use_color) {
            char buf[PATH_MAX];
            ssize_t n = readlink(e->path, buf, sizeof buf - 1);
            if (n >= 0) {
                buf[n] = 0;
                e->link = strdup(buf);
            }
        }
    }
    return 0;
}

static void compute_width(entry *e)
{
    e->width = sk_utf8_width(e->name);
    if (classify) {
        mode_t m = entry_mode(e);
        if (indicator_for(m))
            e->width++;
    }
}

static void free_entries(entry *v, int n)
{
    for (int i = 0; i < n; i++) {
        free(v[i].name);
        free(v[i].path);
        free(v[i].link);
    }
    free(v);
}

/* ---------- 排序 ---------- */

static int cmp_entries(const void *pa, const void *pb)
{
    const entry *a = pa, *b = pb;
    int r = 0;
    if (sort_by == SORT_TIME) {
        if (a->st.st_mtim.tv_sec != b->st.st_mtim.tv_sec)
            r = a->st.st_mtim.tv_sec > b->st.st_mtim.tv_sec ? -1 : 1;
        else if (a->st.st_mtim.tv_nsec != b->st.st_mtim.tv_nsec)
            r = a->st.st_mtim.tv_nsec > b->st.st_mtim.tv_nsec ? -1 : 1;
    } else if (sort_by == SORT_SIZE) {
        if (a->st.st_size != b->st.st_size)
            r = a->st.st_size > b->st.st_size ? -1 : 1;
    }
    if (r == 0)
        r = strcmp(a->name, b->name);
    return reverse ? -r : r;
}

static void sort_entries(entry *v, int n)
{
    if (sort_by != SORT_NONE)
        qsort(v, (size_t)n, sizeof *v, cmp_entries);
    else if (reverse)
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            entry t = v[i];
            v[i] = v[j];
            v[j] = t;
        }
}

/* ---------- 输出 ---------- */

static int num_width(unsigned long long v)
{
    int w = 1;
    while (v >= 10) {
        v /= 10;
        w++;
    }
    return w;
}

static void format_time(char *buf, size_t n, const struct timespec *ts)
{
    struct tm tm;
    time_t t = ts->tv_sec;
    localtime_r(&t, &tm);
    /* 半年之内的文件显示时分，更早（或者在未来）的显示年份，和 GNU ls 一样 */
    const time_t six_months = 31556952 / 2;
    int recent = t > now_sec - six_months && t <= now_sec;
    if (recent)
        snprintf(buf, n, "%2d月 %2d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    else
        snprintf(buf, n, "%2d月 %2d  %d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
}

static void print_long(entry *v, int n, int is_dir_listing)
{
    int w_ino = 1, w_nlink = 1, w_user = 1, w_group = 1, w_size = 1, w_maj = 0, w_min = 0;
    unsigned long long total_blocks = 0;
    char buf[64];
    for (int i = 0; i < n; i++) {
        entry *e = &v[i];
        if (!e->have_st)
            continue;
        total_blocks += (unsigned long long)e->st.st_blocks;
        w_ino = SK_MAX(w_ino, num_width((unsigned long long)e->st.st_ino));
        w_nlink = SK_MAX(w_nlink, num_width((unsigned long long)e->st.st_nlink));
        if (numeric_ids) {
            w_user = SK_MAX(w_user, num_width(e->st.st_uid));
            w_group = SK_MAX(w_group, num_width(e->st.st_gid));
        } else {
            w_user = SK_MAX(w_user, sk_utf8_width(sk_user_name(e->st.st_uid, buf, sizeof buf)));
            w_group = SK_MAX(w_group, sk_utf8_width(sk_group_name(e->st.st_gid, buf, sizeof buf)));
        }
        if (S_ISCHR(e->st.st_mode) || S_ISBLK(e->st.st_mode)) {
            w_maj = SK_MAX(w_maj, num_width(major(e->st.st_rdev)));
            w_min = SK_MAX(w_min, num_width(minor(e->st.st_rdev)));
        } else {
            if (human)
                sk_fmt_size_short(buf, sizeof buf, (uint64_t)e->st.st_size);
            else
                snprintf(buf, sizeof buf, "%lld", (long long)e->st.st_size);
            w_size = SK_MAX(w_size, (int)strlen(buf));
        }
    }
    if (w_maj)
        w_size = SK_MAX(w_size, w_maj + 2 + w_min);

    if (is_dir_listing) {
        /* st_blocks 以 512 字节为单位；“总用量”按 1K 块计（向上取整） */
        if (human)
            sk_fmt_size_short(buf, sizeof buf, total_blocks * 512);
        else
            snprintf(buf, sizeof buf, "%llu", (total_blocks + 1) / 2);
        printf("总用量 %s\n", buf);
    }

    for (int i = 0; i < n; i++) {
        entry *e = &v[i];
        if (!e->have_st) {
            if (show_inode)
                printf("%*s ", w_ino, "?");
            printf("?????????? %*s %-*s %-*s %*s %12s ", w_nlink, "?", w_user, "?", w_group, "?",
                   w_size, "?", "?");
            print_name(e, 1);
            putchar('\n');
            continue;
        }
        char mode[12], tbuf[64];
        sb_mode_string(e->st.st_mode, mode);
        if (show_inode)
            printf("%*llu ", w_ino, (unsigned long long)e->st.st_ino);
        printf("%s %*llu ", mode, w_nlink, (unsigned long long)e->st.st_nlink);
        if (numeric_ids) {
            printf("%-*u %-*u ", w_user, (unsigned)e->st.st_uid, w_group, (unsigned)e->st.st_gid);
        } else {
            sb_put_padded(stdout, sk_user_name(e->st.st_uid, buf, sizeof buf), w_user, 1);
            putchar(' ');
            sb_put_padded(stdout, sk_group_name(e->st.st_gid, buf, sizeof buf), w_group, 1);
            putchar(' ');
        }
        if (S_ISCHR(e->st.st_mode) || S_ISBLK(e->st.st_mode)) {
            /* 设备文件没有“大小”，显示“主设备号, 次设备号” */
            int pad = SK_MAX(0, w_size - (w_maj + 2 + w_min));
            printf("%*u, %*u ", w_maj + pad, major(e->st.st_rdev), w_min, minor(e->st.st_rdev));
        } else {
            if (human)
                sk_fmt_size_short(buf, sizeof buf, (uint64_t)e->st.st_size);
            else
                snprintf(buf, sizeof buf, "%lld", (long long)e->st.st_size);
            printf("%*s ", w_size, buf);
        }
        format_time(tbuf, sizeof tbuf, &e->st.st_mtim);
        printf("%s ", tbuf);
        print_name(e, 1);
        putchar('\n');
    }
}

static void print_one_entry(const entry *e, int w_ino)
{
    if (show_inode) {
        if (e->have_st)
            printf("%*llu ", w_ino, (unsigned long long)e->st.st_ino);
        else
            printf("%*s ", w_ino, "?");
    }
    print_name(e, 0);
}

/* 分栏：先竖着排（第一列从上到下，再第二列），找出能放下的最多列数。
 * 每一列的宽度取这一列里最长的名字，列之间至少空 2 格（和 GNU ls 的算法相同）。 */
static void print_columns(entry *v, int n)
{
    int w_ino = 0;
    if (show_inode) {
        for (int i = 0; i < n; i++)
            if (v[i].have_st)
                w_ino = SK_MAX(w_ino, num_width((unsigned long long)v[i].st.st_ino));
        w_ino = SK_MAX(w_ino, 1);
    }
    int extra = show_inode ? w_ino + 1 : 0;
    int max_cols = line_width / 3;
    if (max_cols < 1)
        max_cols = 1;
    if (max_cols > n)
        max_cols = n;

    /* col_w[c-1] 是“分成 c 列”这种方案的各列宽度 */
    int **col_w = calloc((size_t)max_cols, sizeof *col_w);
    int *line_len = calloc((size_t)max_cols, sizeof *line_len);
    int *valid = calloc((size_t)max_cols, sizeof *valid);
    if (!col_w || !line_len || !valid) {
        free(col_w);
        free(line_len);
        free(valid);
        for (int i = 0; i < n; i++) {
            print_one_entry(&v[i], w_ino);
            putchar('\n');
        }
        return;
    }
    for (int c = 0; c < max_cols; c++) {
        col_w[c] = calloc((size_t)c + 1, sizeof(int));
        valid[c] = col_w[c] != NULL;
        line_len[c] = (c + 1) * 3;
        for (int k = 0; col_w[c] && k <= c; k++)
            col_w[c][k] = 3;
    }
    for (int i = 0; i < n; i++) {
        int len = v[i].width + extra;
        for (int c = 0; c < max_cols; c++) {
            if (!valid[c])
                continue;
            int rows = (n + c) / (c + 1);
            int idx = i / rows;
            int real = len + (idx == c ? 0 : 2);
            if (col_w[c][idx] < real) {
                line_len[c] += real - col_w[c][idx];
                col_w[c][idx] = real;
                valid[c] = line_len[c] < line_width;
            }
        }
    }
    int cols = max_cols;
    while (cols > 1 && !valid[cols - 1])
        cols--;
    int rows = (n + cols - 1) / cols;
    int *widths = col_w[cols - 1];
    for (int r = 0; r < rows; r++) {
        int pos = 0;
        for (int c = 0, i = r; i < n; c++, i += rows) {
            print_one_entry(&v[i], w_ino);
            int w = v[i].width + extra;
            if (i + rows >= n)
                break;
            for (int k = pos + w; k < pos + widths[c]; k++)
                putchar(' ');
            pos += widths[c];
        }
        putchar('\n');
    }
    for (int c = 0; c < max_cols; c++)
        free(col_w[c]);
    free(col_w);
    free(line_len);
    free(valid);
}

static void print_entries(entry *v, int n, int is_dir_listing)
{
    if (fmt == FMT_LONG) {
        print_long(v, n, is_dir_listing);
    } else if (fmt == FMT_COLUMNS && n > 0) {
        print_columns(v, n);
    } else {
        int w_ino = 1;
        for (int i = 0; i < n; i++)
            if (show_inode && v[i].have_st)
                w_ino = SK_MAX(w_ino, num_width((unsigned long long)v[i].st.st_ino));
        for (int i = 0; i < n; i++) {
            print_one_entry(&v[i], w_ino);
            putchar('\n');
        }
    }
}

/* ---------- 列目录 ---------- */

static char *join_path(const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    return sk_strdup_printf("%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", name);
}

static void list_dir(const char *path, const char *shown, int print_header, int top, int *printed)
{
    DIR *d = opendir(path);
    if (!d) {
        sb_perr("无法打开目录 '%s'", shown);
        set_status(top ? 2 : 1);
        return;
    }
    if (*printed)
        putchar('\n');
    if (print_header) {
        put_name(shown);
        fputs(":\n", stdout);
    }
    *printed = 1;

    int cap = 64, n = 0;
    entry *v = calloc((size_t)cap, sizeof *v);
    struct dirent *de;
    int with_stat = need_stat();
    while (v && (de = readdir(d))) {
        const char *nm = de->d_name;
        if (nm[0] == '.') {
            if (!show_all && !almost_all)
                continue;
            if (almost_all && !show_all && (!nm[1] || (nm[1] == '.' && !nm[2])))
                continue;
        }
        if (n == cap) {
            cap *= 2;
            entry *nv = realloc(v, sizeof *v * (size_t)cap);
            if (!nv)
                break;
            v = nv;
        }
        entry *e = &v[n];
        memset(e, 0, sizeof *e);
        e->name = strdup(nm);
        e->path = join_path(path, nm);
        e->dtype = de->d_type;
        if (!e->name || !e->path) {
            free(e->name);
            free(e->path);
            continue;
        }
        if (with_stat && fill_entry(e, 0) != 0) {
            /* 列目录和 stat 之间文件可能已经消失（比如 /proc 下的进程目录） */
            sb_perr("无法访问 '%s'", e->path);
            set_status(1);
        }
        compute_width(e);
        n++;
    }
    closedir(d);
    if (!v) {
        sb_err("内存不足");
        set_status(2);
        return;
    }
    sort_entries(v, n);
    print_entries(v, n, 1);

    if (recursive) {
        for (int i = 0; i < n; i++) {
            entry *e = &v[i];
            if (!e->have_st || !S_ISDIR(e->st.st_mode))
                continue;
            if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0)
                continue;
            char *sub_shown = join_path(shown, e->name);
            list_dir(e->path, sub_shown ? sub_shown : e->path, 1, 0, printed);
            free(sub_shown);
        }
    }
    free_entries(v, n);
}

int applet_ls(int argc, char **argv)
{
    static const struct option lo[] = {
        { "color", optional_argument, NULL, 'K' },
        { "all", no_argument, NULL, 'a' },
        { "almost-all", no_argument, NULL, 'A' },
        { "human-readable", no_argument, NULL, 'h' },
        { "recursive", no_argument, NULL, 'R' },
        { "reverse", no_argument, NULL, 'r' },
        { "directory", no_argument, NULL, 'd' },
        { "classify", no_argument, NULL, 'F' },
        { "inode", no_argument, NULL, 'i' },
        { 0, 0, 0, 0 },
    };
    int color_mode = -1;   /* -1 自动 */
    int c;
    line_width = 0;
    fmt = -1;
    while ((c = sb_getopt_long(argc, argv, "laAh1CRdtSrUFinw:", lo, usage)) != -1) {
        switch (c) {
        case 'l': fmt = FMT_LONG; break;
        case 'a': show_all = 1; almost_all = 0; break;
        case 'A': almost_all = 1; show_all = 0; break;
        case 'h': human = 1; break;
        case '1': fmt = FMT_ONE; break;
        case 'C': fmt = FMT_COLUMNS; break;
        case 'R': recursive = 1; break;
        case 'd': list_dirs = 1; break;
        case 't': sort_by = SORT_TIME; break;
        case 'S': sort_by = SORT_SIZE; break;
        case 'r': reverse = 1; break;
        case 'U': sort_by = SORT_NONE; break;
        case 'F': classify = 1; break;
        case 'i': show_inode = 1; break;
        case 'n': numeric_ids = 1; fmt = FMT_LONG; break;
        case 'w': {
            long long w;
            if (sb_parse_ll(optarg, &w) != 0 || w < 0) {
                sb_err("无效的行宽 '%s'", optarg);
                return 2;
            }
            line_width = w ? (int)w : INT_MAX;
            break;
        }
        case 'K':
            if (!optarg || strcmp(optarg, "always") == 0 || strcmp(optarg, "yes") == 0 ||
                strcmp(optarg, "force") == 0)
                color_mode = !optarg ? -1 : 1;
            else if (strcmp(optarg, "never") == 0 || strcmp(optarg, "no") == 0 || strcmp(optarg, "none") == 0)
                color_mode = 0;
            else if (strcmp(optarg, "auto") == 0 || strcmp(optarg, "tty") == 0 || strcmp(optarg, "if-tty") == 0)
                color_mode = -1;
            else {
                sb_err("--color 的参数无效: '%s'（可用 always、never、auto）", optarg);
                return 2;
            }
            break;
        default:
            return 2;
        }
    }
    int tty = isatty(STDOUT_FILENO);
    if (fmt == -1)
        fmt = tty ? FMT_COLUMNS : FMT_ONE;
    use_color = color_mode == -1 ? sb_color_enabled(STDOUT_FILENO) : color_mode;
    sanitize = tty;
    if (!line_width)
        line_width = sk_term_width(STDOUT_FILENO);
    now_sec = time(NULL);

    int nops = argc - optind;
    char *dot[] = { ".", NULL };
    char **ops = nops ? argv + optind : dot;
    if (!nops)
        nops = 1;

    entry *files = calloc((size_t)nops, sizeof *files);
    entry *dirs = calloc((size_t)nops, sizeof *dirs);
    if (!files || !dirs) {
        sb_err("内存不足");
        return 2;
    }
    int nf = 0, nd = 0;
    /* 命令行上指向目录的符号链接：除非用了 -l/-d/-F，否则进入它指向的目录 */
    int follow = !(fmt == FMT_LONG || list_dirs || classify);
    for (int i = 0; i < nops; i++) {
        entry e;
        memset(&e, 0, sizeof e);
        e.name = strdup(ops[i]);
        e.path = strdup(ops[i]);
        if (!e.name || !e.path || fill_entry(&e, follow) != 0) {
            sb_perr("无法访问 '%s'", ops[i]);
            set_status(2);
            free(e.name);
            free(e.path);
            free(e.link);
            continue;
        }
        compute_width(&e);
        if (S_ISDIR(e.st.st_mode) && !list_dirs)
            dirs[nd++] = e;
        else
            files[nf++] = e;
    }

    int printed = 0;
    if (nf) {
        sort_entries(files, nf);
        print_entries(files, nf, 0);
        printed = 1;
    }
    sort_entries(dirs, nd);
    int header = nops > 1 || recursive;
    for (int i = 0; i < nd; i++)
        list_dir(dirs[i].path, dirs[i].name, header, 1, &printed);
    free_entries(files, nf);
    free_entries(dirs, nd);
    return status;
}
