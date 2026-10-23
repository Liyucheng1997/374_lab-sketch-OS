/*
 * tail — 输出文件的末尾部分；-f 时持续跟踪文件新增的内容
 *
 * 对普通文件，tail 不必从头读：用 lseek() 直接跳到文件末尾，向前一块一块地找换行符。
 * -f 用 inotify 让内核在文件被修改时通知我们（而不是每隔一会儿去问一次），
 * 没有新数据时进程在 poll() 里睡觉，不占 CPU。
 */
#include "skbox.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

SKBOX_APPLET(tail, "显示文件的最后几行，或持续跟踪文件")

static const char usage[] =
    "用法: tail [选项] [文件...]\n"
    "输出每个文件的最后 10 行。多个文件时，每个文件前面加上 ==> 文件名 <== 标题。\n\n"
    "  -n 数量   输出最后若干行；写成 -n +数量 时从第“数量”行开始输出到结尾\n"
    "  -c 数量   输出最后若干字节；-c +数量 表示从第“数量”个字节开始\n"
    "  -数量     等于 -n 数量\n"
    "  -f        输出完后继续等待，文件增长时显示新增的内容（Ctrl+C 结束）\n"
    "  -s 秒数   配合 -f：检查间隔（默认 1 秒，内核支持 inotify 时立即响应）\n"
    "  -q        从不输出文件名标题\n"
    "  -v        总是输出文件名标题\n";

static long long g_count = 10;
static int g_bytes, g_from_start;

static char *slurp(int fd, size_t *len)
{
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    for (;;) {
        if (n == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return NULL;
        }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    *len = n;
    return buf;
}

/* 在缓冲区里找出“最后 count 行”的起点 */
static size_t last_lines_start(const char *buf, size_t len, long long count)
{
    if (count == 0)
        return len;
    size_t pos = len;
    if (pos > 0 && buf[pos - 1] == '\n')
        pos--;                    /* 末尾的换行属于最后一行 */
    long long seen = 0;
    while (pos > 0) {
        if (buf[pos - 1] == '\n' && ++seen == count)
            return pos;
        pos--;
    }
    return 0;
}

/* 对能 seek 的普通文件：从末尾往前读，找到足够的换行就停 */
static int tail_seekable(int fd, off_t size)
{
    off_t start;
    if (g_bytes) {
        start = (off_t)g_count >= size ? 0 : size - (off_t)g_count;
    } else {
        const size_t blk = 8192;
        static char buf[8192];
        off_t pos = size;
        long long seen = 0;
        int first = 1;
        start = 0;
        while (pos > 0) {
            size_t n = pos >= (off_t)blk ? blk : (size_t)pos;
            pos -= (off_t)n;
            if (pread(fd, buf, n, pos) != (ssize_t)n)
                return -1;
            size_t i = n;
            if (first) {
                if (i > 0 && buf[i - 1] == '\n')
                    i--;
                first = 0;
            }
            int done = 0;
            while (i > 0) {
                if (buf[i - 1] == '\n' && ++seen == g_count) {
                    start = pos + (off_t)i;
                    done = 1;
                    break;
                }
                i--;
            }
            if (done)
                break;
        }
        if (g_count == 0)
            start = size;
    }
    if (lseek(fd, start, SEEK_SET) < 0)
        return -1;
    fflush(stdout);
    return sb_copy_fd(fd, STDOUT_FILENO);
}

static int tail_fd(int fd)
{
    struct stat st;
    if (g_from_start) {
        /* +N：跳过前 N-1 行（或字节），剩下的全部输出 */
        long long skip = g_count > 0 ? g_count - 1 : 0;
        static char buf[65536];
        fflush(stdout);
        for (;;) {
            ssize_t r = read(fd, buf, sizeof buf);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (r == 0)
                return 0;
            size_t off = 0;
            if (skip > 0) {
                if (g_bytes) {
                    off = (long long)r <= skip ? (size_t)r : (size_t)skip;
                    skip -= (long long)off;
                } else {
                    while (off < (size_t)r && skip > 0)
                        if (buf[off++] == '\n')
                            skip--;
                }
            }
            if (off < (size_t)r && sb_write_all(STDOUT_FILENO, buf + off, (size_t)r - off) != 0)
                return -1;
        }
    }
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && lseek(fd, 0, SEEK_CUR) == 0)
        return tail_seekable(fd, st.st_size);
    size_t len;
    char *buf = slurp(fd, &len);
    if (!buf)
        return -1;
    size_t start = g_bytes ? ((unsigned long long)g_count >= len ? 0 : len - (size_t)g_count)
                           : last_lines_start(buf, len, g_count);
    fflush(stdout);
    int r = sb_write_all(STDOUT_FILENO, buf + start, len - start);
    free(buf);
    return r;
}

typedef struct {
    const char *name;
    int fd;
    int wd;          /* inotify 监视号 */
    off_t pos;
} followed;

/* 把文件从 pos 开始新增的内容输出；文件变短了说明被截断，从头开始 */
static void follow_drain(followed *f, followed **last_shown, int headers)
{
    struct stat st;
    if (fstat(f->fd, &st) != 0)
        return;
    if (S_ISREG(st.st_mode) && st.st_size < f->pos) {
        sb_err("%s: 文件被截断", f->name);
        f->pos = 0;
    }
    if (S_ISREG(st.st_mode) && st.st_size == f->pos)
        return;
    static char buf[65536];
    for (;;) {
        ssize_t r = pread(f->fd, buf, sizeof buf, f->pos);
        if (r <= 0)
            break;
        if (headers && *last_shown != f) {
            printf("\n==> %s <==\n", f->name);
            *last_shown = f;
        }
        fflush(stdout);
        if (sb_write_all(STDOUT_FILENO, buf, (size_t)r) != 0)
            exit(1);
        f->pos += r;
    }
}

static int follow_loop(followed *fs, int n, double interval, int headers)
{
    int ino = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    for (int i = 0; i < n; i++) {
        fs[i].pos = lseek(fs[i].fd, 0, SEEK_END);
        fs[i].wd = ino >= 0 ? inotify_add_watch(ino, fs[i].name, IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE) : -1;
    }
    followed *last_shown = n ? &fs[n - 1] : NULL;
    int timeout = (int)(interval * 1000);
    if (timeout < 10)
        timeout = 10;
    for (;;) {
        fflush(stdout);
        if (ino >= 0) {
            /* 有 inotify 时也设一个超时：有些文件系统（如 /proc）不发通知，兜底用轮询 */
            struct pollfd p = { ino, POLLIN, 0 };
            int r = poll(&p, 1, timeout);
            if (r < 0 && errno != EINTR)
                return 1;
            if (r > 0) {
                char evbuf[4096];
                while (read(ino, evbuf, sizeof evbuf) > 0)
                    ;
            }
        } else {
            struct timespec ts = { timeout / 1000, (long)(timeout % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
        for (int i = 0; i < n; i++)
            follow_drain(&fs[i], &last_shown, headers);
    }
}

int applet_tail(int argc, char **argv)
{
    int follow = 0, headers = -1, c;
    double interval = 1.0;
    static char *nargv[1024];
    static char num[32];
    /* 兼容老写法 tail -20 / tail +5 */
    if (argc > 1 && (argv[1][0] == '-' || argv[1][0] == '+') && isdigit((unsigned char)argv[1][1]) &&
        argc < 1020) {
        int k = 0;
        nargv[k++] = argv[0];
        nargv[k++] = "-n";
        snprintf(num, sizeof num, "%s%s", argv[1][0] == '+' ? "+" : "", argv[1] + 1);
        nargv[k++] = num;
        for (int i = 2; i < argc; i++)
            nargv[k++] = argv[i];
        nargv[k] = NULL;
        argv = nargv;
        argc = k;
    }
    while ((c = sb_getopt(argc, argv, "n:c:fFs:qv", usage)) != -1) {
        switch (c) {
        case 'n':
        case 'c': {
            const char *s = optarg;
            g_from_start = *s == '+';
            if (*s == '+' || *s == '-')
                s++;
            if (sb_parse_size(s, &g_count) != 0 || g_count < 0) {
                sb_err("无效的%s数 '%s'", c == 'n' ? "行" : "字节", optarg);
                return 1;
            }
            g_bytes = c == 'c';
            break;
        }
        case 'f':
        case 'F': follow = 1; break;
        case 's': {
            char *end;
            interval = strtod(optarg, &end);
            if (*end || interval < 0) {
                sb_err("无效的秒数 '%s'", optarg);
                return 1;
            }
            break;
        }
        case 'q': headers = 0; break;
        case 'v': headers = 1; break;
        default:  return 1;
        }
    }
    int nfiles = argc - optind;
    char *dash[] = { "-", NULL };
    char **files = nfiles ? argv + optind : dash;
    if (!nfiles)
        nfiles = 1;
    if (headers < 0)
        headers = nfiles > 1;

    followed *fs = calloc((size_t)nfiles, sizeof *fs);
    if (!fs) {
        sb_err("内存不足");
        return 1;
    }
    int nf = 0, rc = 0;
    for (int i = 0; i < nfiles; i++) {
        int fd = sb_open_input(files[i]);
        if (fd < 0) {
            sb_perr("无法打开 '%s'", files[i]);
            rc = 1;
            continue;
        }
        const char *shown = strcmp(files[i], "-") ? files[i] : "标准输入";
        if (headers) {
            printf("%s==> %s <==\n", i ? "\n" : "", shown);
            fflush(stdout);
        }
        if (tail_fd(fd) != 0) {
            sb_perr("读取 '%s' 出错", files[i]);
            rc = 1;
        }
        struct stat st;
        /* 管道没法“等它变长”，跟踪只对普通文件有意义（和 GNU 一样，标准输入是管道时忽略 -f） */
        if (follow && fstat(fd, &st) == 0 && !S_ISFIFO(st.st_mode) && fd != STDIN_FILENO) {
            fs[nf].name = files[i];
            fs[nf].fd = fd;
            nf++;
        } else if (fd != STDIN_FILENO) {
            close(fd);
        }
    }
    if (follow && nf > 0)
        return follow_loop(fs, nf, interval, headers);
    free(fs);
    return rc;
}
