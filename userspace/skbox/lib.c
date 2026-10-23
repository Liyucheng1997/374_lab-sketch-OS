/*
 * lib.c — skbox 各个工具共用的小函数：错误信息、选项解析、数字解析、输出、权限模式、终端模式
 */
#include "skbox.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *sb_applet = "skbox";

/* ======================= 错误信息 ======================= */

static void verr(int with_errno, int saved, const char *fmt, va_list ap)
{
    /* 先拼成一整行再一次写出：多个进程同时往终端写错误时不容易交错 */
    char msg[1024];
    int n = snprintf(msg, sizeof msg, "%s: ", sb_applet);
    if (n < 0)
        n = 0;
    if ((size_t)n < sizeof msg)
        vsnprintf(msg + n, sizeof msg - (size_t)n, fmt, ap);
    if (with_errno) {
        sk_strlcat(msg, ": ", sizeof msg);
        sk_strlcat(msg, sk_strerror(saved), sizeof msg);
    }
    sk_strlcat(msg, "\n", sizeof msg);
    fflush(stdout);   /* 让错误信息出现在已输出内容的后面 */
    fputs(msg, stderr);
}

void sb_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verr(0, 0, fmt, ap);
    va_end(ap);
}

void sb_perr(const char *fmt, ...)
{
    int saved = errno;
    va_list ap;
    va_start(ap, fmt);
    verr(1, saved, fmt, ap);
    va_end(ap);
    errno = saved;
}

void sb_hint(void)
{
    fprintf(stderr, "请用 '%s --help' 查看用法。\n", sb_applet);
}

/* ======================= 选项解析 ======================= */

int sb_getopt_long(int argc, char **argv, const char *optstring,
                   const struct option *longopts, const char *usage)
{
    /* 在调用者的选项表后面自动加上 --help；optstring 前面加 ':'，让 getopt
     * 在“缺少参数”时返回 ':' 而不是自己打印英文错误 */
    static char opts[160];
    static const char *opts_src;
    static struct option *merged;
    static const struct option *merged_src;
    static int merged_ready;

    if (opts_src != optstring) {
        size_t k = 0;
        const char *p = optstring;
        if (*p == '+')
            opts[k++] = *p++;
        opts[k++] = ':';
        sk_strlcpy(opts + k, p, sizeof opts - k);
        opts_src = optstring;
    }
    if (!merged_ready || merged_src != longopts) {
        size_t n = 0;
        while (longopts && longopts[n].name)
            n++;
        struct option *m = realloc(merged, sizeof *m * (n + 2));
        if (!m) {
            sb_err("内存不足");
            exit(1);
        }
        merged = m;
        for (size_t i = 0; i < n; i++)
            merged[i] = longopts[i];
        merged[n] = (struct option){ "help", no_argument, NULL, SB_OPT_HELP };
        merged[n + 1] = (struct option){ 0, 0, 0, 0 };
        merged_src = longopts;
        merged_ready = 1;
    }

    opterr = 0;
    int c = getopt_long(argc, argv, opts, merged, NULL);
    if (c == SB_OPT_HELP) {
        fputs(usage, stdout);
        exit(fflush(stdout) == 0 ? 0 : 1);
    }
    if (c == '?') {
        if (optopt > 0 && optopt < 128)
            sb_err("无效的选项 -- '%c'", optopt);
        else
            sb_err("无法识别的选项 '%s'", argv[optind - 1]);
        sb_hint();
        return '?';
    }
    if (c == ':') {
        if (optopt > 0 && optopt < 128 && strchr(optstring, optopt))
            sb_err("选项需要一个参数 -- '%c'", optopt);
        else
            sb_err("选项 '%s' 需要一个参数", argv[optind - 1]);
        sb_hint();
        return '?';
    }
    return c;
}

int sb_getopt(int argc, char **argv, const char *optstring, const char *usage)
{
    return sb_getopt_long(argc, argv, optstring, NULL, usage);
}

int sb_help(int argc, char **argv, const char *usage)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        fputs(usage, stdout);
        return 1;
    }
    return 0;
}

/* ======================= 数字解析 ======================= */

int sb_parse_ll(const char *s, long long *out)
{
    if (!s || !*s)
        return -1;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || *end || end == s || isspace((unsigned char)s[0]))
        return -1;
    *out = v;
    return 0;
}

int sb_parse_size(const char *s, long long *out)
{
    if (!s || !*s || isspace((unsigned char)s[0]))
        return -1;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || end == s)
        return -1;
    long long mul = 1;
    if (*end) {
        static const char units[] = "KMGTPE";
        char u = (char)toupper((unsigned char)*end);
        const char *pos = strchr(units, u);
        if (*end == 'b' && end[1] == 0) {
            mul = 512;
        } else if (pos && u) {
            int power = (int)(pos - units) + 1;
            long long base = 1024;
            if (end[1] == 'B' && end[2] == 0)
                base = 1000;                      /* kB、MB：十进制单位 */
            else if (end[1] == 'i' && end[2] == 'B' && end[3] == 0)
                base = 1024;                      /* KiB、MiB */
            else if (end[1] != 0)
                return -1;
            for (int i = 0; i < power; i++)
                mul *= base;
        } else {
            return -1;
        }
    }
    if (mul > 1 && (v > INT64_MAX / mul || v < INT64_MIN / mul))
        return -1;
    *out = v * mul;
    return 0;
}

/* ======================= 输出 ======================= */

int sb_write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int sb_copy_fd(int in, int out)
{
    static char buf[65536];
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return 0;
        if (sb_write_all(out, buf, (size_t)r) != 0)
            return -1;
    }
}

int sb_open_input(const char *path)
{
    if (strcmp(path, "-") == 0)
        return STDIN_FILENO;
    return open(path, O_RDONLY | O_CLOEXEC);
}

int sb_color_enabled(int fd)
{
    if (!isatty(fd) || getenv("NO_COLOR"))
        return 0;
    const char *term = getenv("TERM");
    return !(term && strcmp(term, "dumb") == 0);
}

void sb_put_padded(FILE *f, const char *s, int width, int left)
{
    int pad = width - sk_utf8_width(s);
    if (!left)
        for (int i = 0; i < pad; i++)
            fputc(' ', f);
    fputs(s, f);
    if (left)
        for (int i = 0; i < pad; i++)
            fputc(' ', f);
}

/* ======================= 文件属性 ======================= */

void sb_mode_string(mode_t m, char *out)
{
    char t = '?';
    switch (m & S_IFMT) {
    case S_IFREG:  t = '-'; break;
    case S_IFDIR:  t = 'd'; break;
    case S_IFLNK:  t = 'l'; break;
    case S_IFCHR:  t = 'c'; break;
    case S_IFBLK:  t = 'b'; break;
    case S_IFIFO:  t = 'p'; break;
    case S_IFSOCK: t = 's'; break;
    }
    out[0] = t;
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    /* setuid 位：执行这个程序时，进程获得文件属主的身份（比如 passwd 命令）。有 x 显示 s，没有显示 S */
    out[3] = (m & S_ISUID) ? ((m & S_IXUSR) ? 's' : 'S') : ((m & S_IXUSR) ? 'x' : '-');
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_ISGID) ? ((m & S_IXGRP) ? 's' : 'S') : ((m & S_IXGRP) ? 'x' : '-');
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    /* 粘滞位（sticky）：用在 /tmp 这类公共目录上，只有文件的主人才能删除自己的文件 */
    out[9] = (m & S_ISVTX) ? ((m & S_IXOTH) ? 't' : 'T') : ((m & S_IXOTH) ? 'x' : '-');
    out[10] = 0;
}

const char *sb_file_type_name(mode_t m, off_t size)
{
    switch (m & S_IFMT) {
    case S_IFREG:  return size == 0 ? "普通空文件" : "普通文件";
    case S_IFDIR:  return "目录";
    case S_IFLNK:  return "符号链接";
    case S_IFCHR:  return "字符设备";
    case S_IFBLK:  return "块设备";
    case S_IFIFO:  return "命名管道";
    case S_IFSOCK: return "套接字";
    }
    return "未知类型";
}

/*
 * 权限模式。Linux 用 12 个二进制位描述文件权限：
 *   setuid setgid sticky | 属主 rwx | 同组 rwx | 其他人 rwx
 * 八进制写法 "755" 正好每一位对应 3 个二进制位。符号写法 "u+x,go-w" 则是“在原来的基础上修改”。
 */
int sb_mode_parse(const char *spec, mode_t old, int is_dir, mode_t umask_val, mode_t *out)
{
    const mode_t ALL = S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO;
    if (!spec || !*spec)
        return -1;

    if (spec[0] >= '0' && spec[0] <= '7') {
        mode_t m = 0;
        size_t len = 0;
        for (const char *p = spec; *p; p++, len++) {
            if (*p < '0' || *p > '7')
                return -1;
            m = m * 8 + (mode_t)(*p - '0');
            if (m > 07777)
                return -1;
        }
        /* 与 GNU 一致：对目录，少于 5 位的八进制不清除 setuid/setgid（要清除请写 00755） */
        if (is_dir && len < 5)
            m |= old & (S_ISUID | S_ISGID);
        *out = m;
        return 0;
    }

    mode_t m = old & ALL;
    const char *p = spec;
    for (;;) {
        mode_t who = 0;
        for (;; p++) {
            if (*p == 'u')      who |= S_ISUID | S_IRWXU;
            else if (*p == 'g') who |= S_ISGID | S_IRWXG;
            else if (*p == 'o') who |= S_ISVTX | S_IRWXO;
            else if (*p == 'a') who |= ALL;
            else break;
        }
        int who_given = who != 0;
        if (!who_given)
            who = ALL;
        if (*p != '+' && *p != '-' && *p != '=')
            return -1;
        while (*p == '+' || *p == '-' || *p == '=') {
            char op = *p++;
            mode_t perm = 0;
            if (*p == 'u' || *p == 'g' || *p == 'o') {
                /* "g=u"：把属主的 rwx 复制给同组 */
                mode_t src;
                if (*p == 'u')      src = (m & S_IRWXU) >> 6;
                else if (*p == 'g') src = (m & S_IRWXG) >> 3;
                else                src = m & S_IRWXO;
                perm = (src << 6) | (src << 3) | src;
                p++;
            } else {
                for (; *p && strchr("rwxXst", *p); p++) {
                    switch (*p) {
                    case 'r': perm |= S_IRUSR | S_IRGRP | S_IROTH; break;
                    case 'w': perm |= S_IWUSR | S_IWGRP | S_IWOTH; break;
                    case 'x': perm |= S_IXUSR | S_IXGRP | S_IXOTH; break;
                    case 'X':   /* 只对目录、或已经有人可执行的文件加 x（chmod -R a+X 的常见用法） */
                        if (is_dir || (old & (S_IXUSR | S_IXGRP | S_IXOTH)))
                            perm |= S_IXUSR | S_IXGRP | S_IXOTH;
                        break;
                    case 's': perm |= S_ISUID | S_ISGID; break;
                    case 't': perm |= S_ISVTX; break;
                    }
                }
            }
            mode_t eff = perm & who;
            if (!who_given)
                eff &= ~umask_val;   /* 没写 ugoa 时，受 umask 保护的位不变 */
            if (op == '+') {
                m |= eff;
            } else if (op == '-') {
                m &= ~eff;
            } else {
                mode_t clear = who;
                if (is_dir && !(perm & (S_ISUID | S_ISGID)))
                    clear &= ~(S_ISUID | S_ISGID);
                m = (m & ~clear) | eff;
            }
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == 0)
            break;
        return -1;
    }
    *out = m;
    return 0;
}

const char *sb_basename(const char *path, char *buf, size_t n)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    if (len == 1 && path[0] == '/') {
        sk_strlcpy(buf, "/", n);
        return buf;
    }
    size_t start = len;
    while (start > 0 && path[start - 1] != '/')
        start--;
    size_t k = len - start;
    if (k >= n)
        k = n - 1;
    memcpy(buf, path + start, k);
    buf[k] = 0;
    return buf;
}

int sb_ask_yes(void)
{
    char line[64];
    fflush(stderr);
    if (!fgets(line, sizeof line, stdin))
        return 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == 'y' || *p == 'Y' || strncmp(p, "是", strlen("是")) == 0;
}

/* ======================= 终端原始模式 ======================= */

int sb_term_raw(sb_term *t, int fd)
{
    t->fd = fd;
    t->active = 0;
    if (tcgetattr(fd, &t->saved) != 0)
        return -1;
    struct termios r = t->saved;
    /* 关掉规范模式和回显；保留 ISIG，这样 Ctrl+C 仍然会产生 SIGINT */
    r.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    r.c_cc[VMIN] = 1;
    r.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &r) != 0)
        return -1;
    t->active = 1;
    return 0;
}

void sb_term_restore(sb_term *t)
{
    if (t->active) {
        tcsetattr(t->fd, TCSANOW, &t->saved);
        t->active = 0;
    }
}
