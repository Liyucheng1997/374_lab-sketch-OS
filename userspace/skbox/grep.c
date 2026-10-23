/*
 * grep — 在文件里搜索匹配模式的行
 *
 * 模式是“正则表达式”，用 C 库的 POSIX 正则：regcomp() 把模式编译成一个自动机，
 * regexec() 再用它去匹配每一行。编译只做一次，所以搜索大文件也很快。
 *   基本正则（BRE，默认）：.  *  ^  $  [abc]  \(分组\)  \{n,m\}
 *   扩展正则（ERE，-E）：  另外支持  +  ?  |  (分组)  {n,m}  不需要反斜杠
 */
#include "skbox.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

SKBOX_APPLET(grep, "在文件中搜索匹配的行（正则表达式）")

static const char usage[] =
    "用法: grep [选项] 模式 [文件...]\n"
    "  或: grep [选项] -e 模式... [文件...]\n"
    "在每个文件（默认标准输入）中搜索匹配“模式”的行并输出。\n\n"
    "  -E        模式是扩展正则表达式（支持 + ? | () {}）\n"
    "  -F        模式是普通字符串，不是正则表达式\n"
    "  -G        模式是基本正则表达式（默认）\n"
    "  -e 模式   指定模式（可以多次使用，匹配任意一个即可）\n"
    "  -f 文件   从文件中读取模式，每行一个\n"
    "  -i        忽略大小写\n"
    "  -v        反向选择：输出不匹配的行\n"
    "  -w        只匹配完整的单词\n"
    "  -x        只匹配完整的一行\n"
    "  -c        只输出匹配的行数\n"
    "  -l        只输出包含匹配的文件名\n"
    "  -L        只输出不包含匹配的文件名\n"
    "  -n        输出行号\n"
    "  -o        只输出匹配的部分，每个一行\n"
    "  -m 数量   每个文件最多输出这么多匹配行\n"
    "  -q        安静模式：不输出任何东西，只用退出状态表示是否找到\n"
    "  -s        不显示“文件不存在/不可读”的错误\n"
    "  -r        递归搜索目录（不跟随目录里的符号链接）\n"
    "  -R        递归搜索目录，并跟随所有符号链接\n"
    "  -H / -h   总是 / 从不在每行前显示文件名\n"
    "  -a        把二进制文件也当作文本处理\n"
    "  --color[=何时]  高亮匹配：always、never、auto（输出到终端时，默认）\n\n"
    "退出状态：找到 0，没找到 1，出错 2。\n";

typedef struct {
    regex_t re;
    char *text;       /* -F 时的字符串 */
    size_t len;
} pattern;

static pattern *g_pats;
static int g_npats;
static int opt_fixed, opt_extended, opt_icase, opt_invert, opt_word, opt_line, opt_count;
static int opt_list, opt_list_nomatch, opt_number, opt_only, opt_quiet, opt_silent;
static int opt_recursive, opt_deref, opt_text, opt_color, with_filename = -1;
static long long opt_max = -1;
static int any_selected, any_error;

/* ---------- 模式 ---------- */

static void add_pattern(const char *p)
{
    pattern *np = realloc(g_pats, sizeof *g_pats * (size_t)(g_npats + 1));
    if (!np) {
        sb_err("内存不足");
        exit(2);
    }
    g_pats = np;
    pattern *pt = &g_pats[g_npats++];
    memset(pt, 0, sizeof *pt);
    pt->text = strdup(p);
    pt->len = strlen(p);
}

static void compile_patterns(void)
{
    if (opt_fixed)
        return;
    int flags = (opt_extended ? REG_EXTENDED : 0) | (opt_icase ? REG_ICASE : 0) | REG_NEWLINE;
    for (int i = 0; i < g_npats; i++) {
        int r = regcomp(&g_pats[i].re, g_pats[i].text, flags);
        if (r != 0) {
            char msg[256];
            regerror(r, &g_pats[i].re, msg, sizeof msg);
            sb_err("无效的正则表达式 '%s': %s", g_pats[i].text, msg);
            exit(2);
        }
    }
}

static int is_word_char(unsigned char c)
{
    return c == '_' || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || c >= 0x80;
}

static const char *find_fixed(const char *hay, size_t hlen, const pattern *p)
{
    if (p->len == 0)
        return hay;
    if (p->len > hlen)
        return NULL;
    for (size_t i = 0; i + p->len <= hlen; i++) {
        if (opt_icase ? strncasecmp(hay + i, p->text, p->len) == 0 : memcmp(hay + i, p->text, p->len) == 0)
            return hay + i;
    }
    return NULL;
}

/* 在 line[from..len) 里找一个满足 -w/-x 条件的匹配，返回 1 并给出 [*so, *eo) */
static int find_one(const pattern *p, const char *line, size_t len, size_t from, size_t *so, size_t *eo)
{
    size_t pos = from;
    while (pos <= len) {
        size_t s, e;
        if (opt_fixed) {
            const char *hit = find_fixed(line + pos, len - pos, p);
            if (!hit)
                return 0;
            s = (size_t)(hit - line);
            e = s + p->len;
        } else {
            regmatch_t m;
            /* 从行中间开始找时告诉 regexec “这不是行首”，否则 ^ 会错误地匹配 */
            if (regexec(&p->re, line + pos, 1, &m, pos ? REG_NOTBOL : 0) != 0)
                return 0;
            s = pos + (size_t)m.rm_so;
            e = pos + (size_t)m.rm_eo;
        }
        int ok = 1;
        if (opt_line)
            ok = s == 0 && e == len;
        else if (opt_word)
            ok = (s == 0 || !is_word_char((unsigned char)line[s - 1])) &&
                 (e == len || !is_word_char((unsigned char)line[e])) && e > s;
        if (ok) {
            *so = s;
            *eo = e;
            return 1;
        }
        if (opt_line)
            return 0;
        pos = s + 1;   /* 这个位置不满足单词边界，往后接着找 */
    }
    return 0;
}

/* 所有模式里最靠前（同位置取最长）的匹配 */
static int find_any(const char *line, size_t len, size_t from, size_t *so, size_t *eo)
{
    int found = 0;
    for (int i = 0; i < g_npats; i++) {
        size_t s, e;
        if (find_one(&g_pats[i], line, len, from, &s, &e)) {
            if (!found || s < *so || (s == *so && e > *eo)) {
                *so = s;
                *eo = e;
                found = 1;
            }
        }
    }
    return found;
}

/* ---------- 按行读取（自己做缓冲，方便检查开头是否有 NUL 字节来判断二进制文件） ---------- */

typedef struct {
    int fd;
    char *buf;
    size_t cap, start, end;
    int eof;
} reader;

static int reader_fill(reader *r)
{
    if (r->start > 0) {
        memmove(r->buf, r->buf + r->start, r->end - r->start);
        r->end -= r->start;
        r->start = 0;
    }
    if (r->end == r->cap) {
        char *nb = realloc(r->buf, r->cap * 2 + 1);   /* +1：给行尾的 NUL 留位置 */
        if (!nb)
            return -1;
        r->buf = nb;
        r->cap *= 2;
    }
    for (;;) {
        ssize_t n = read(r->fd, r->buf + r->end, r->cap - r->end);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            r->eof = 1;
        r->end += (size_t)n;
        return 0;
    }
}

/* 取下一行（不含换行符）。返回 1 有数据，0 结束，-1 出错 */
static int reader_line(reader *r, char **line, size_t *len)
{
    for (;;) {
        char *nl = memchr(r->buf + r->start, '\n', r->end - r->start);
        if (nl) {
            *line = r->buf + r->start;
            *len = (size_t)(nl - *line);
            *nl = 0;
            r->start += *len + 1;
            return 1;
        }
        if (r->eof) {
            if (r->start == r->end)
                return 0;
            *line = r->buf + r->start;
            *len = r->end - r->start;
            r->buf[r->end] = 0;
            r->start = r->end;
            return 1;
        }
        if (reader_fill(r) != 0)
            return -1;
    }
}

/* ---------- 输出 ---------- */

static void put_prefix(const char *name, long long lineno, char sep)
{
    if (with_filename) {
        if (opt_color)
            printf("\033[35m%s\033[0m\033[36m%c\033[0m", name, sep);
        else
            printf("%s%c", name, sep);
    }
    if (opt_number) {
        if (opt_color)
            printf("\033[32m%lld\033[0m\033[36m%c\033[0m", lineno, sep);
        else
            printf("%lld%c", lineno, sep);
    }
}

/* C 库的 regexec 只认以 NUL 结尾的字符串。行里夹着 NUL 字节（二进制文件）时，
 * 复制一份把 NUL 换成换行再去匹配（和 GNU grep 一样把 NUL 当作行的分隔），输出时仍用原来的字节 */
static const char *match_text(const char *line, size_t len)
{
    static char *buf;
    static size_t cap;
    if (!memchr(line, 0, len))
        return line;
    if (len + 1 > cap) {
        char *nb = realloc(buf, len + 1);
        if (!nb)
            return line;
        buf = nb;
        cap = len + 1;
    }
    for (size_t i = 0; i < len; i++)
        buf[i] = line[i] ? line[i] : '\n';
    buf[len] = 0;
    return buf;
}

static void put_line(const char *line, const char *mt, size_t len)
{
    if (!opt_color || opt_invert) {
        fwrite(line, 1, len, stdout);
        putchar('\n');
        return;
    }
    size_t pos = 0, so, eo;
    while (pos <= len && find_any(mt, len, pos, &so, &eo)) {
        if (eo == so) {             /* 空匹配不高亮，避免死循环 */
            if (so >= len)
                break;
            fwrite(line + pos, 1, so + 1 - pos, stdout);
            pos = so + 1;
            continue;
        }
        fwrite(line + pos, 1, so - pos, stdout);
        fputs("\033[01;31m", stdout);
        fwrite(line + so, 1, eo - so, stdout);
        fputs("\033[0m", stdout);
        pos = eo;
    }
    if (pos < len)
        fwrite(line + pos, 1, len - pos, stdout);
    putchar('\n');
}

/* ---------- 搜索一个文件 ---------- */

static int grep_fd(int fd, const char *name)
{
    reader r = { fd, malloc(65536 + 1), 65536, 0, 0, 0 };
    if (!r.buf) {
        sb_err("内存不足");
        return -1;
    }
    r.cap = 65536;
    /* 先读一块：里面有 NUL 字节就认为是二进制文件（和 GNU grep 的判断方法一样） */
    if (reader_fill(&r) != 0) {
        sb_perr("%s", name);
        free(r.buf);
        return -1;
    }
    int binary = !opt_text && memchr(r.buf, 0, r.end) != NULL;

    long long lineno = 0, selected = 0;
    char *line;
    size_t len;
    int rr = 0;
    while (opt_max != 0 && (rr = reader_line(&r, &line, &len)) == 1) {
        lineno++;
        size_t so, eo;
        const char *mt = match_text(line, len);
        int m = find_any(mt, len, 0, &so, &eo);
        if (m == opt_invert)
            continue;
        selected++;
        any_selected = 1;
        if (opt_quiet) {
            free(r.buf);
            fflush(stdout);
            exit(0);   /* 找到一个就够了 */
        }
        if (opt_list || opt_list_nomatch)
            break;
        if (opt_count) {
            if (opt_max >= 0 && selected >= opt_max)
                break;
            continue;
        }
        if (binary) {
            printf("匹配到二进制文件 %s\n", name);
            break;
        }
        if (opt_only && !opt_invert) {
            size_t pos = 0;
            while (pos <= len && find_any(mt, len, pos, &so, &eo)) {
                if (eo == so) {
                    pos = so + 1;
                    continue;
                }
                put_prefix(name, lineno, ':');
                if (opt_color)
                    printf("\033[01;31m%.*s\033[0m\n", (int)(eo - so), line + so);
                else
                    printf("%.*s\n", (int)(eo - so), line + so);
                pos = eo;
            }
        } else {
            put_prefix(name, lineno, ':');
            put_line(line, mt, len);
        }
        if (opt_max >= 0 && selected >= opt_max)
            break;
    }
    if (rr < 0) {
        sb_perr("%s", name);
        free(r.buf);
        return -1;
    }
    free(r.buf);
    if (opt_count) {
        if (with_filename) {
            if (opt_color)
                printf("\033[35m%s\033[0m\033[36m:\033[0m", name);
            else
                printf("%s:", name);
        }
        printf("%lld\n", selected);
    }
    if (opt_list && selected)
        printf(opt_color ? "\033[35m%s\033[0m\n" : "%s\n", name);
    if (opt_list_nomatch && !selected)
        printf(opt_color ? "\033[35m%s\033[0m\n" : "%s\n", name);
    return 0;
}

static void grep_path(const char *path, const char *shown, int top);

static void grep_dir(const char *path, const char *shown)
{
    DIR *d = opendir(path);
    if (!d) {
        if (!opt_silent)
            sb_perr("%s", shown);
        any_error = 1;
        return;
    }
    /* 按目录里的原始顺序处理（和 GNU grep 一样不排序）；先把名字读完再处理，
     * 这样递归时不必同时打开很多层目录 */
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **nn = realloc(names, cap * sizeof *nn);
            if (!nn)
                break;
            names = nn;
        }
        names[n] = strdup(e->d_name);
        if (names[n])
            n++;
    }
    closedir(d);
    for (size_t i = 0; i < n; i++) {
        char *cp = sk_strdup_printf("%s/%s", path, names[i]);
        char *cs = shown[0] ? sk_strdup_printf("%s%s%s", shown,
                                               shown[strlen(shown) - 1] == '/' ? "" : "/", names[i])
                            : strdup(names[i]);
        if (cp && cs)
            grep_path(cp, cs, 0);
        free(cp);
        free(cs);
        free(names[i]);
    }
    free(names);
}

static void grep_path(const char *path, const char *shown, int top)
{
    struct stat st;
    int follow = top || opt_deref;
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) {
        if (!opt_silent)
            sb_perr("%s", shown);
        any_error = 1;
        return;
    }
    if (S_ISLNK(st.st_mode))
        return;   /* 递归时不跟随符号链接（-R 除外） */
    if (S_ISDIR(st.st_mode)) {
        if (opt_recursive) {
            grep_dir(path, shown);
        } else if (!opt_silent) {
            sb_err("%s: 是一个目录", shown);
        }
        return;
    }
    if (!top && !S_ISREG(st.st_mode))
        return;   /* 递归时跳过设备文件、管道等 */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (!opt_silent)
            sb_perr("%s", shown);
        any_error = 1;
        return;
    }
    if (grep_fd(fd, shown) != 0)
        any_error = 1;
    close(fd);
}

int applet_grep(int argc, char **argv)
{
    static const struct option lo[] = {
        { "color", optional_argument, NULL, 'K' },
        { "colour", optional_argument, NULL, 'K' },
        { "extended-regexp", no_argument, NULL, 'E' },
        { "fixed-strings", no_argument, NULL, 'F' },
        { "ignore-case", no_argument, NULL, 'i' },
        { "invert-match", no_argument, NULL, 'v' },
        { "recursive", no_argument, NULL, 'r' },
        { "count", no_argument, NULL, 'c' },
        { "quiet", no_argument, NULL, 'q' },
        { 0, 0, 0, 0 },
    };
    int color_mode = -1, c;
    while ((c = sb_getopt_long(argc, argv, "EFGe:f:ivwxclLnom:qsrRHha", lo, usage)) != -1) {
        switch (c) {
        case 'E': opt_extended = 1; opt_fixed = 0; break;
        case 'F': opt_fixed = 1; opt_extended = 0; break;
        case 'G': opt_fixed = 0; opt_extended = 0; break;
        case 'e': add_pattern(optarg); break;
        case 'f': {
            size_t len;
            char *s = sk_read_file(optarg, &len);
            if (!s) {
                sb_perr("%s", optarg);
                return 2;
            }
            for (char *line = s; line && *line;) {
                char *nl = strchr(line, '\n');
                if (nl)
                    *nl = 0;
                add_pattern(line);
                line = nl ? nl + 1 : NULL;
            }
            free(s);
            break;
        }
        case 'i': opt_icase = 1; break;
        case 'v': opt_invert = 1; break;
        case 'w': opt_word = 1; break;
        case 'x': opt_line = 1; break;
        case 'c': opt_count = 1; break;
        case 'l': opt_list = 1; break;
        case 'L': opt_list_nomatch = 1; break;
        case 'n': opt_number = 1; break;
        case 'o': opt_only = 1; break;
        case 'm':
            if (sb_parse_ll(optarg, &opt_max) != 0 || opt_max < 0) {
                sb_err("无效的数量 '%s'", optarg);
                return 2;
            }
            break;
        case 'q': opt_quiet = 1; break;
        case 's': opt_silent = 1; break;
        case 'r': opt_recursive = 1; break;
        case 'R': opt_recursive = 1; opt_deref = 1; break;
        case 'H': with_filename = 1; break;
        case 'h': with_filename = 0; break;
        case 'a': opt_text = 1; break;
        case 'K':
            if (!optarg || strcmp(optarg, "auto") == 0 || strcmp(optarg, "tty") == 0)
                color_mode = -1;
            else if (strcmp(optarg, "always") == 0 || strcmp(optarg, "yes") == 0)
                color_mode = 1;
            else if (strcmp(optarg, "never") == 0 || strcmp(optarg, "no") == 0)
                color_mode = 0;
            else {
                sb_err("--color 的参数无效: '%s'", optarg);
                return 2;
            }
            break;
        default:
            return 2;
        }
    }
    if (g_npats == 0) {
        if (optind >= argc) {
            sb_err("缺少搜索模式");
            sb_hint();
            return 2;
        }
        add_pattern(argv[optind++]);
    }
    compile_patterns();
    opt_color = color_mode == -1 ? sb_color_enabled(STDOUT_FILENO) : color_mode;
    if (opt_quiet)
        opt_color = 0;

    int nfiles = argc - optind;
    if (with_filename < 0)
        with_filename = nfiles > 1 || opt_recursive;
    if (nfiles == 0) {
        if (opt_recursive) {
            grep_dir(".", "");     /* grep -r 不给目录时搜索当前目录，输出的路径不带 "./" */
        } else {
            if (grep_fd(STDIN_FILENO, "(标准输入)") != 0)
                any_error = 1;
        }
    } else {
        for (int i = optind; i < argc; i++) {
            if (strcmp(argv[i], "-") == 0) {
                if (grep_fd(STDIN_FILENO, "(标准输入)") != 0)
                    any_error = 1;
            } else {
                grep_path(argv[i], argv[i], 1);
            }
        }
    }
    if (any_error && !(opt_quiet && any_selected))
        return 2;
    return any_selected ? 0 : 1;
}
