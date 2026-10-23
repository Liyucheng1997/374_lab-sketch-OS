/*
 * tr — 替换、删除或压缩字符
 *
 * 实现方法是一张 256 项的查找表：table[c] 表示字节 c 要变成什么。
 * 建好表之后，处理每个字节只需要查一次表，非常快。（tr 按字节工作，不处理多字节的汉字。）
 */
#include "skbox.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(tr, "替换、删除或压缩字符")

static const char usage[] =
    "用法: tr [选项] 集合1 [集合2]\n"
    "从标准输入读取，把集合1里的字符替换成集合2里对应的字符，写到标准输出。\n\n"
    "  -d    删除集合1中的字符\n"
    "  -s    把连续重复的字符压缩成一个（替换时看集合2，否则看集合1）\n"
    "  -c    使用集合1的补集（所有不在集合1里的字符）\n"
    "  -t    先把集合1截短到和集合2一样长\n\n"
    "集合的写法：\n"
    "  a-z           范围\n"
    "  \\n \\t \\\\ \\NNN  转义字符（NNN 是八进制）\n"
    "  [:alpha:] [:digit:] [:alnum:] [:upper:] [:lower:] [:space:] [:blank:] [:punct:]\n"
    "  [:cntrl:] [:print:] [:graph:] [:xdigit:]   字符类\n"
    "  [c*n]         （只用于集合2）把字符 c 重复 n 次；[c*] 重复到和集合1一样长\n"
    "例如: tr a-z A-Z        小写转大写\n"
    "      tr -d '\\r'        删除 Windows 换行里的回车\n"
    "      tr -s ' '         把连续空格压成一个\n";

typedef struct {
    unsigned char c[4096];
    int n;
    int star_pos;     /* [c*] 的位置（-1 表示没有） */
    int star_char;
} charset;

static int parse_escape(const char **ps)
{
    const char *s = *ps;   /* s 指向反斜杠后面 */
    int v;
    switch (*s) {
    case 'n': v = '\n'; s++; break;
    case 't': v = '\t'; s++; break;
    case 'r': v = '\r'; s++; break;
    case 'a': v = '\a'; s++; break;
    case 'b': v = '\b'; s++; break;
    case 'f': v = '\f'; s++; break;
    case 'v': v = '\v'; s++; break;
    case '\\': v = '\\'; s++; break;
    case 0: v = '\\'; break;
    default:
        if (*s >= '0' && *s <= '7') {
            v = 0;
            for (int i = 0; i < 3 && *s >= '0' && *s <= '7'; i++)
                v = v * 8 + (*s++ - '0');
            v &= 0xFF;
        } else {
            v = (unsigned char)*s++;
        }
    }
    *ps = s;
    return v;
}

static int class_test(const char *name, int c)
{
    if (!strcmp(name, "alpha")) return isalpha(c);
    if (!strcmp(name, "digit")) return isdigit(c);
    if (!strcmp(name, "alnum")) return isalnum(c);
    if (!strcmp(name, "upper")) return isupper(c);
    if (!strcmp(name, "lower")) return islower(c);
    if (!strcmp(name, "space")) return isspace(c);
    if (!strcmp(name, "blank")) return c == ' ' || c == '\t';
    if (!strcmp(name, "punct")) return ispunct(c);
    if (!strcmp(name, "cntrl")) return iscntrl(c);
    if (!strcmp(name, "print")) return isprint(c);
    if (!strcmp(name, "graph")) return isgraph(c);
    if (!strcmp(name, "xdigit")) return isxdigit(c);
    return -1;
}

static void add(charset *cs, int c)
{
    if (cs->n < (int)sizeof cs->c)
        cs->c[cs->n++] = (unsigned char)c;
}

static int parse_set(const char *s, charset *cs, int is_set2)
{
    cs->n = 0;
    cs->star_pos = -1;
    while (*s) {
        /* [:类名:] */
        if (s[0] == '[' && s[1] == ':') {
            const char *e = strstr(s + 2, ":]");
            if (e) {
                char name[16];
                size_t len = (size_t)(e - s - 2);
                if (len < sizeof name) {
                    memcpy(name, s + 2, len);
                    name[len] = 0;
                    if (class_test(name, 'a') < 0) {
                        sb_err("无效的字符类 '%s'", name);
                        return -1;
                    }
                    for (int c = 0; c < 256; c++)
                        if (class_test(name, c) > 0)
                            add(cs, c);
                    s = e + 2;
                    continue;
                }
            }
        }
        /* [=c=]：等价类，在 C 语言环境里就是这个字符本身 */
        if (s[0] == '[' && s[1] == '=' && s[2] && s[3] == '=' && s[4] == ']') {
            add(cs, (unsigned char)s[2]);
            s += 5;
            continue;
        }
        /* [c*n] 或 [c*] */
        if (is_set2 && s[0] == '[' && s[1] && s[2] == '*') {
            const char *p = s + 3;
            int c = (unsigned char)s[1];
            char *end;
            long n = strtol(p, &end, p[0] == '0' ? 8 : 10);
            if (*end == ']') {
                if (end == p || n == 0) {
                    cs->star_pos = cs->n;
                    cs->star_char = c;
                } else {
                    for (long i = 0; i < n; i++)
                        add(cs, c);
                }
                s = end + 1;
                continue;
            }
        }
        int c;
        if (*s == '\\') {
            s++;
            c = parse_escape(&s);
        } else {
            c = (unsigned char)*s++;
        }
        /* 范围 a-z */
        if (s[0] == '-' && s[1]) {
            const char *t = s + 1;
            int hi;
            if (*t == '\\') {
                t++;
                hi = parse_escape(&t);
            } else {
                hi = (unsigned char)*t++;
            }
            if (hi < c) {
                sb_err("范围 '%c-%c' 的顺序反了", c, hi);
                return -1;
            }
            for (int x = c; x <= hi; x++)
                add(cs, x);
            s = t;
            continue;
        }
        add(cs, c);
    }
    return 0;
}

int applet_tr(int argc, char **argv)
{
    int del = 0, squeeze = 0, comp = 0, trunc = 0, c;
    while ((c = sb_getopt(argc, argv, "dscCt", usage)) != -1) {
        switch (c) {
        case 'd': del = 1; break;
        case 's': squeeze = 1; break;
        case 'c':
        case 'C': comp = 1; break;
        case 't': trunc = 1; break;
        default:  return 1;
        }
    }
    int nsets = argc - optind;
    int need = (del && !squeeze) ? 1 : (del && squeeze) ? 2 : (squeeze ? 1 : 2);
    if (nsets < need || nsets > 2 || (del && !squeeze && nsets != 1)) {
        if (nsets < need)
            sb_err("缺少操作数");
        else
            sb_err("多余的操作数 '%s'", argv[argc - 1]);
        sb_hint();
        return 1;
    }
    static charset s1, s2;
    if (parse_set(argv[optind], &s1, 0) != 0)
        return 1;
    if (nsets == 2 && parse_set(argv[optind + 1], &s2, 1) != 0)
        return 1;

    unsigned char in1[256] = { 0 };
    for (int i = 0; i < s1.n; i++)
        in1[s1.c[i]] = 1;
    if (comp) {
        /* 补集：按从小到大的顺序列出所有不在集合1里的字节 */
        s1.n = 0;
        for (int x = 0; x < 256; x++)
            if (!in1[x])
                s1.c[s1.n++] = (unsigned char)x;
        memset(in1, 0, sizeof in1);
        for (int i = 0; i < s1.n; i++)
            in1[s1.c[i]] = 1;
    }

    int translate = !del && nsets == 2;
    int map[256];
    for (int x = 0; x < 256; x++)
        map[x] = x;
    if (translate) {
        /* [c*]：把 c 填到合适的个数，使集合2和集合1一样长 */
        if (s2.star_pos >= 0) {
            int fill = s1.n - s2.n;
            if (fill > 0) {
                memmove(s2.c + s2.star_pos + fill, s2.c + s2.star_pos, (size_t)(s2.n - s2.star_pos));
                memset(s2.c + s2.star_pos, s2.star_char, (size_t)fill);
                s2.n += fill;
            }
        }
        if (s2.n == 0) {
            sb_err("替换时集合2不能为空");
            return 1;
        }
        int n1 = trunc ? SK_MIN(s1.n, s2.n) : s1.n;
        for (int i = 0; i < n1; i++)
            map[s1.c[i]] = s2.c[i < s2.n ? i : s2.n - 1];   /* 集合2 不够长时重复最后一个字符 */
    }
    /* 压缩时看的集合：替换或“删除+压缩”时是集合2，只压缩时是集合1 */
    unsigned char sq[256] = { 0 };
    if (squeeze) {
        const charset *src = nsets == 2 ? &s2 : &s1;
        if (nsets == 1)
            memcpy(sq, in1, sizeof sq);
        else
            for (int i = 0; i < src->n; i++)
                sq[src->c[i]] = 1;
    }

    static unsigned char ibuf[65536], obuf[65536];
    int last = -1;
    for (;;) {
        ssize_t r = read(STDIN_FILENO, ibuf, sizeof ibuf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            sb_perr("读取失败");
            return 1;
        }
        if (r == 0)
            break;
        size_t o = 0;
        for (ssize_t i = 0; i < r; i++) {
            int ch = ibuf[i];
            if (del && in1[ch])
                continue;
            ch = map[ch];
            if (squeeze && sq[ch] && ch == last)
                continue;
            obuf[o++] = (unsigned char)ch;
            last = ch;
        }
        if (sb_write_all(STDOUT_FILENO, obuf, o) != 0) {
            sb_perr("写入失败");
            return 1;
        }
    }
    return 0;
}
