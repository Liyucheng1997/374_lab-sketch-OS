/*
 * skutil.c — 公共工具函数的实现，见 skutil.h
 */
#define _GNU_SOURCE
#include "skutil.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <signal.h>
#include <strings.h>
#include <sys/ioctl.h>

/* ======================= 日志 ======================= */

static char g_log_tag[32];
static int g_kmsg_fd = -2;      /* -2 = 还没尝试打开 */
static int g_console_fd = -2;

void sk_log_init(const char *tag)
{
    sk_strlcpy(g_log_tag, tag ? tag : "", sizeof g_log_tag);
}

static const char *log_tag(void)
{
    if (!g_log_tag[0]) {
        /* 默认用程序名（/proc/self/comm） */
        if (sk_read_line("/proc/self/comm", g_log_tag, sizeof g_log_tag) != 0)
            sk_strlcpy(g_log_tag, "sketchos", sizeof g_log_tag);
    }
    return g_log_tag;
}

static int lazy_open(int *fd, const char *path)
{
    if (*fd == -2)
        *fd = open(path, O_WRONLY | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    return *fd;
}

static void vlog(int to_console, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    size_t len = strlen(msg);
    while (len > 0 && msg[len - 1] == '\n')
        msg[--len] = 0;

    char line[600];
    int n = snprintf(line, sizeof line, "[%s] %s\n", log_tag(), msg);
    if (n > 0) {
        ssize_t r = write(STDERR_FILENO, line, (size_t)SK_MIN(n, (int)sizeof line - 1));
        (void)r;
    }

    /* 内核日志：<6> 表示 KERN_INFO 级别 */
    if (lazy_open(&g_kmsg_fd, "/dev/kmsg") >= 0) {
        n = snprintf(line, sizeof line, "<6>%s: %s\n", log_tag(), msg);
        if (n > 0) {
            ssize_t r = write(g_kmsg_fd, line, (size_t)SK_MIN(n, (int)sizeof line - 1));
            (void)r;
        }
    }
    if (to_console && lazy_open(&g_console_fd, "/dev/console") >= 0) {
        n = snprintf(line, sizeof line, "%s: %s\r\n", log_tag(), msg);
        if (n > 0) {
            ssize_t r = write(g_console_fd, line, (size_t)SK_MIN(n, (int)sizeof line - 1));
            (void)r;
        }
    }
}

void sk_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(0, fmt, ap);
    va_end(ap);
}

void sk_log_console(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(1, fmt, ap);
    va_end(ap);
}

/* ======================= 字符串 ======================= */

size_t sk_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len >= size ? size - 1 : len;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

size_t sk_strlcat(char *dst, const char *src, size_t size)
{
    size_t dl = strnlen(dst, size);
    if (dl == size)
        return size + strlen(src);
    return dl + sk_strlcpy(dst + dl, src, size - dl);
}

char *sk_trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

int sk_startswith(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int sk_endswith(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

int sk_split_args(char *s, char **argv, int max)
{
    int argc = 0;
    char *r = s, *w = s;
    while (*r && argc < max) {
        while (*r && isspace((unsigned char)*r))
            r++;
        if (!*r)
            break;
        argv[argc++] = w;
        char quote = 0;
        while (*r) {
            if (quote) {
                if (*r == quote) { quote = 0; r++; continue; }
                if (quote == '"' && *r == '\\' && r[1]) r++;
                *w++ = *r++;
            } else if (*r == '"' || *r == '\'') {
                quote = *r++;
            } else if (*r == '\\' && r[1]) {
                r++;
                *w++ = *r++;
            } else if (isspace((unsigned char)*r)) {
                r++;
                break;
            } else {
                *w++ = *r++;
            }
        }
        *w++ = 0;
    }
    return argc;
}

void sk_fmt_size(char *buf, size_t n, uint64_t bytes)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    if (bytes < 1024) {
        snprintf(buf, n, "%u B", (unsigned)bytes);
        return;
    }
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    snprintf(buf, n, v >= 100 ? "%.0f %s" : "%.1f %s", v, units[u]);
}

void sk_fmt_duration(char *buf, size_t n, uint64_t s)
{
    uint64_t d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60, sec = s % 60;
    if (d)
        snprintf(buf, n, "%u 天 %u 小时", (unsigned)d, (unsigned)h);
    else if (h)
        snprintf(buf, n, "%u 小时 %u 分", (unsigned)h, (unsigned)m);
    else if (m)
        snprintf(buf, n, "%u 分 %02u 秒", (unsigned)m, (unsigned)sec);
    else
        snprintf(buf, n, "%u 秒", (unsigned)sec);
}

char *sk_strdup_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *out = NULL;
    if (vasprintf(&out, fmt, ap) < 0)
        out = NULL;
    va_end(ap);
    return out;
}

/* ======================= 文件 ======================= */

char *sk_read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    size_t cap = 4096, used = 0;
    char *buf = malloc(cap + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    for (;;) {
        if (used == cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap + 1);
            if (!nb) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        ssize_t r = read(fd, buf + used, cap - used);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        used += (size_t)r;
    }
    close(fd);
    buf[used] = 0;
    if (len)
        *len = used;
    return buf;
}

int sk_read_line(const char *path, char *buf, size_t n)
{
    if (!n)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0)
        return -1;
    buf[r] = 0;
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = 0;
    return 0;
}

int sk_write_file(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    const char *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return close(fd);
}

int sk_mkdir_p(const char *path, mode_t mode)
{
    char tmp[1024];
    sk_strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

void sk_path_join(char *buf, size_t n, const char *dir, const char *name)
{
    if (name[0] == '/' || !dir || !dir[0]) {
        sk_strlcpy(buf, name, n);
        return;
    }
    size_t dl = strlen(dir);
    snprintf(buf, n, "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", name);
}

void sk_path_normalize(char *path)
{
    /* 逐个处理路径分量：跳过 "" 和 "."，".." 弹出上一个分量 */
    char out[1024];
    size_t ol = 0;
    const char *p = path;
    int absolute = path[0] == '/';
    out[0] = 0;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len == 1 && s[0] == '.')
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (ol > 0 && out[ol - 1] != '/')
                ol--;
            if (ol > 0)
                ol--;
            out[ol] = 0;
            continue;
        }
        if (ol + len + 2 >= sizeof out)
            break;
        out[ol++] = '/';
        memcpy(out + ol, s, len);
        ol += len;
        out[ol] = 0;
    }
    if (ol == 0) {
        strcpy(path, absolute ? "/" : ".");
        return;
    }
    strcpy(path, absolute ? out : out + 1);
}

/* ======================= 内核命令行 ======================= */

int sk_cmdline_get(const char *key, char *buf, size_t n)
{
    char *cmd = sk_read_file("/proc/cmdline", NULL);
    if (!cmd)
        return 0;
    size_t kl = strlen(key);
    int found = 0;
    char *save = NULL;
    for (char *tok = strtok_r(cmd, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
        if (strncmp(tok, key, kl) != 0)
            continue;
        if (tok[kl] == 0) {
            if (n) buf[0] = 0;
            found = 1;
        } else if (tok[kl] == '=') {
            if (n) sk_strlcpy(buf, tok + kl + 1, n);
            found = 1;
        }
        /* 同一参数出现多次时以最后一次为准（与内核行为一致），所以不 break */
    }
    free(cmd);
    return found;
}

/* ======================= 系统信息 ======================= */

int sk_read_meminfo(sk_meminfo *m)
{
    memset(m, 0, sizeof *m);
    char *s = sk_read_file("/proc/meminfo", NULL);
    if (!s)
        return -1;
    struct { const char *name; uint64_t *dst; } keys[] = {
        { "MemTotal:", &m->total_kb },     { "MemFree:", &m->free_kb },
        { "MemAvailable:", &m->available_kb }, { "Buffers:", &m->buffers_kb },
        { "Cached:", &m->cached_kb },      { "Shmem:", &m->shmem_kb },
    };
    for (char *line = s; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        for (size_t i = 0; i < SK_ARRAY_LEN(keys); i++) {
            size_t kl = strlen(keys[i].name);
            if (strncmp(line, keys[i].name, kl) == 0)
                *keys[i].dst = strtoull(line + kl, NULL, 10);
        }
        line = nl ? nl + 1 : NULL;
    }
    free(s);
    if (!m->available_kb)
        m->available_kb = m->free_kb + m->buffers_kb + m->cached_kb;
    return 0;
}

int sk_read_cpustat(sk_cpustat *c)
{
    memset(c, 0, sizeof *c);
    char line[256];
    if (sk_read_line("/proc/stat", line, sizeof line) != 0)
        return -1;
    unsigned long long v[7] = { 0 };
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) < 4)
        return -1;
    c->user = v[0]; c->nice = v[1]; c->system = v[2]; c->idle = v[3];
    c->iowait = v[4]; c->irq = v[5]; c->softirq = v[6];
    c->total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6];
    c->busy = c->total - c->idle - c->iowait;
    return 0;
}

int sk_cpu_usage_permille(const sk_cpustat *prev, const sk_cpustat *now)
{
    uint64_t dt = now->total - prev->total;
    if (now->total < prev->total || dt == 0)
        return 0;
    uint64_t db = now->busy >= prev->busy ? now->busy - prev->busy : 0;
    return (int)SK_MIN(1000, db * 1000 / dt);
}

int sk_read_uptime(double *uptime, double *idle)
{
    char line[128];
    if (sk_read_line("/proc/uptime", line, sizeof line) != 0)
        return -1;
    double u = 0, i = 0;
    if (sscanf(line, "%lf %lf", &u, &i) < 1)
        return -1;
    if (uptime) *uptime = u;
    if (idle) *idle = i;
    return 0;
}

int sk_read_loadavg(double load[3], int *running, int *total)
{
    char line[128];
    if (sk_read_line("/proc/loadavg", line, sizeof line) != 0)
        return -1;
    int r = 0, t = 0;
    if (sscanf(line, "%lf %lf %lf %d/%d", &load[0], &load[1], &load[2], &r, &t) < 3)
        return -1;
    if (running) *running = r;
    if (total) *total = t;
    return 0;
}

#define PF_KTHREAD_FLAG 0x00200000u   /* 内核里的 PF_KTHREAD：这是一个内核线程 */

int sk_read_proc(pid_t pid, sk_procinfo *p)
{
    char path[64];
    memset(p, 0, sizeof *p);
    p->pid = pid;

    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    char *s = sk_read_file(path, NULL);
    if (!s)
        return -1;
    /* 格式: pid (comm) state ppid ...；comm 里可能有空格和括号，所以找最后一个 ')' */
    char *lp = strchr(s, '('), *rp = strrchr(s, ')');
    if (!lp || !rp || rp < lp) {
        free(s);
        return -1;
    }
    size_t cl = (size_t)(rp - lp - 1);
    if (cl >= sizeof p->comm)
        cl = sizeof p->comm - 1;
    memcpy(p->comm, lp + 1, cl);
    p->comm[cl] = 0;

    /* 第 3 个字段开始；fields[0] 对应 state（第 3 字段） */
    char *fields[24] = { 0 };
    int nf = 0;
    char *save = NULL;
    for (char *t = strtok_r(rp + 2, " ", &save); t && nf < 24; t = strtok_r(NULL, " ", &save))
        fields[nf++] = t;
    if (nf < 22) {
        free(s);
        return -1;
    }
    p->state = fields[0][0];
    p->ppid = (pid_t)atoi(fields[1]);
    unsigned long flags = strtoul(fields[6], NULL, 10);
    p->utime = strtoull(fields[11], NULL, 10);
    p->stime = strtoull(fields[12], NULL, 10);
    p->priority = atoi(fields[15]);
    p->nice = atoi(fields[16]);
    p->threads = atoi(fields[17]);
    p->start_time = strtoull(fields[19], NULL, 10);
    p->vsize_kb = strtoull(fields[20], NULL, 10) / 1024;
    p->rss_kb = strtoull(fields[21], NULL, 10) * (uint64_t)(sysconf(_SC_PAGESIZE) / 1024);
    p->is_kernel_thread = (flags & PF_KTHREAD_FLAG) != 0;
    free(s);

    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    size_t len = 0;
    char *cmd = sk_read_file(path, &len);
    if (cmd) {
        /* 参数之间用 NUL 分隔，换成空格 */
        for (size_t i = 0; i < len; i++)
            if (cmd[i] == 0)
                cmd[i] = ' ';
        while (len > 0 && cmd[len - 1] == ' ')
            cmd[--len] = 0;
        sk_strlcpy(p->cmdline, cmd, sizeof p->cmdline);
        free(cmd);
    }
    return 0;
}

static int cmp_pid(const void *a, const void *b)
{
    const sk_procinfo *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

sk_procinfo *sk_list_procs(int *count)
{
    *count = 0;
    DIR *d = opendir("/proc");
    if (!d)
        return NULL;
    int cap = 64, n = 0;
    sk_procinfo *arr = malloc(sizeof *arr * (size_t)cap);
    if (!arr) {
        closedir(d);
        return NULL;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        if (n == cap) {
            cap *= 2;
            sk_procinfo *na = realloc(arr, sizeof *arr * (size_t)cap);
            if (!na)
                break;
            arr = na;
        }
        /* 进程可能在我们读的过程中退出，读失败就跳过 */
        if (sk_read_proc((pid_t)atoi(e->d_name), &arr[n]) == 0)
            n++;
    }
    closedir(d);
    qsort(arr, (size_t)n, sizeof *arr, cmp_pid);
    *count = n;
    return arr;
}

long sk_clk_tck(void)
{
    long t = sysconf(_SC_CLK_TCK);
    return t > 0 ? t : 100;
}

void sk_kernel_release(char *buf, size_t n)
{
    struct utsname u;
    if (uname(&u) == 0)
        sk_strlcpy(buf, u.release, n);
    else
        sk_strlcpy(buf, "?", n);
}

/* ======================================================================
 * 以下是 skbox 组追加的通用工具（只增加，不改动上面已有的函数）
 * ====================================================================== */

/* ======================= 终端 ======================= */

int sk_term_size(int fd, int *cols, int *rows)
{
    struct winsize ws;
    int c = 0, r = 0, from_kernel = -1;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        c = ws.ws_col;
        r = ws.ws_row;
        from_kernel = 0;
    }
    if (c <= 0) {
        const char *e = getenv("COLUMNS");
        c = e ? atoi(e) : 0;
    }
    if (r <= 0) {
        const char *e = getenv("LINES");
        r = e ? atoi(e) : 0;
    }
    if (c <= 0)
        c = 80;
    if (r <= 0)
        r = 24;
    if (cols)
        *cols = c;
    if (rows)
        *rows = r;
    return from_kernel;
}

int sk_term_width(int fd)
{
    int c = 80;
    sk_term_size(fd, &c, NULL);
    return c;
}

/* ======================= UTF-8 与显示宽度 ======================= */

int sk_utf8_decode(const char *str, size_t len, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)str;
    if (len == 0) {
        *cp = 0;
        return 0;
    }
    unsigned c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    /* 首字节决定总长度：110xxxxx=2 字节，1110xxxx=3 字节，11110xxx=4 字节 */
    int n;
    uint32_t v, min;
    if ((c & 0xE0) == 0xC0) { n = 2; v = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; min = 0x10000; }
    else goto bad;
    if ((size_t)n > len)
        goto bad;
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80)      /* 后续字节必须是 10xxxxxx */
            goto bad;
        v = (v << 6) | (s[i] & 0x3F);
    }
    /* 拒绝“超长编码”、UTF-16 代理区和超出 Unicode 范围的值 */
    if (v < min || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF)
        goto bad;
    *cp = v;
    return n;
bad:
    *cp = 0xFFFD;
    return 1;
}

struct sk_wrange { uint32_t lo, hi; };

/* 占 0 列的组合字符（常见的附加符号、零宽字符、变体选择符）。按区间排序，二分查找。 */
static const struct sk_wrange zero_width[] = {
    { 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD }, { 0x05BF, 0x05BF },
    { 0x05C1, 0x05C2 }, { 0x05C4, 0x05C5 }, { 0x05C7, 0x05C7 }, { 0x0610, 0x061A },
    { 0x064B, 0x065F }, { 0x0670, 0x0670 }, { 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 },
    { 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED }, { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A },
    { 0x0E47, 0x0E4E }, { 0x1AB0, 0x1AFF }, { 0x1DC0, 0x1DFF }, { 0x200B, 0x200F },
    { 0x202A, 0x202E }, { 0x2060, 0x2064 }, { 0x20D0, 0x20FF }, { 0x302A, 0x302D },
    { 0x3099, 0x309A }, { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F }, { 0xFEFF, 0xFEFF },
    { 0xE0100, 0xE01EF },
};

/* 占 2 列的“东亚宽字符”：汉字、假名、谚文、全角符号、大部分 emoji。
 * 这是 Unicode East Asian Width 表的简化版，覆盖了终端里实际会遇到的字符。 */
static const struct sk_wrange wide[] = {
    { 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A }, { 0x23E9, 0x23EC },
    { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 }, { 0x25FD, 0x25FE }, { 0x2614, 0x2615 },
    { 0x2648, 0x2653 }, { 0x267F, 0x267F }, { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 },
    { 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 }, { 0x26CE, 0x26CE },
    { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA }, { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 },
    { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD }, { 0x2705, 0x2705 }, { 0x270A, 0x270B },
    { 0x2728, 0x2728 }, { 0x274C, 0x274C }, { 0x274E, 0x274E }, { 0x2753, 0x2755 },
    { 0x2757, 0x2757 }, { 0x2795, 0x2797 }, { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF },
    { 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 }, { 0x2E80, 0x303E },
    { 0x3041, 0x33FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xA000, 0xA4CF },
    { 0xA960, 0xA97F }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 },
    { 0xFE30, 0xFE6F }, { 0xFF00, 0xFF60 }, { 0xFFE0, 0xFFE6 }, { 0x16FE0, 0x16FE4 },
    { 0x17000, 0x18CFF }, { 0x1B000, 0x1B2FF }, { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF },
    { 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A }, { 0x1F200, 0x1F251 }, { 0x1F300, 0x1F64F },
    { 0x1F680, 0x1F6FF }, { 0x1F7E0, 0x1F7EB }, { 0x1F900, 0x1F9FF }, { 0x1FA70, 0x1FAFF },
    { 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD },
};

static int in_ranges(uint32_t cp, const struct sk_wrange *t, size_t n)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < t[mid].lo)
            hi = mid;
        else if (cp > t[mid].hi)
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

int sk_wcwidth(uint32_t cp)
{
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return 0;                       /* 控制字符不占位置 */
    if (cp < 0x300)
        return 1;                       /* 快速路径：ASCII 和拉丁字母 */
    if (in_ranges(cp, zero_width, SK_ARRAY_LEN(zero_width)))
        return 0;
    if (in_ranges(cp, wide, SK_ARRAY_LEN(wide)))
        return 2;
    return 1;
}

int sk_utf8_nwidth(const char *s, size_t len)
{
    int w = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        int k = sk_utf8_decode(s + i, len - i, &cp);
        if (k <= 0)
            break;
        /* 非法字节会被显示成一个替换符号，算 1 列 */
        w += cp == 0xFFFD ? 1 : sk_wcwidth(cp);
        i += (size_t)k;
    }
    return w;
}

int sk_utf8_width(const char *s)
{
    return sk_utf8_nwidth(s, strlen(s));
}

size_t sk_utf8_fit(const char *s, int max_cols, int *cols)
{
    size_t len = strlen(s), i = 0;
    int w = 0;
    while (i < len) {
        uint32_t cp;
        int k = sk_utf8_decode(s + i, len - i, &cp);
        int cw = cp == 0xFFFD ? 1 : sk_wcwidth(cp);
        if (k <= 0 || w + cw > max_cols)
            break;
        w += cw;
        i += (size_t)k;
    }
    if (cols)
        *cols = w;
    return i;
}

/* ======================= 中文错误信息 ======================= */

const char *sk_strerror(int err)
{
    switch (err) {
    case 0:            return "成功";
    case EPERM:        return "不允许的操作";
    case ENOENT:       return "没有那个文件或目录";
    case ESRCH:        return "没有那个进程";
    case EINTR:        return "被信号中断";
    case EIO:          return "输入/输出错误";
    case ENXIO:        return "没有那个设备或地址";
    case E2BIG:        return "参数列表过长";
    case ENOEXEC:      return "可执行文件格式错误";
    case EBADF:        return "错误的文件描述符";
    case ECHILD:       return "没有子进程";
    case EAGAIN:       return "资源暂时不可用";
    case ENOMEM:       return "内存不足";
    case EACCES:       return "权限不够";
    case EFAULT:       return "错误的地址";
    case ENOTBLK:      return "需要块设备";
    case EBUSY:        return "设备或资源忙";
    case EEXIST:       return "文件已存在";
    case EXDEV:        return "不能跨文件系统链接";
    case ENODEV:       return "没有那个设备";
    case ENOTDIR:      return "不是目录";
    case EISDIR:       return "是一个目录";
    case EINVAL:       return "无效的参数";
    case ENFILE:       return "系统打开的文件过多";
    case EMFILE:       return "打开的文件过多";
    case ENOTTY:       return "不是终端（对设备不适当的 ioctl 操作）";
    case ETXTBSY:      return "文本文件忙（程序正在运行）";
    case EFBIG:        return "文件过大";
    case ENOSPC:       return "设备上没有空间";
    case ESPIPE:       return "非法的定位操作（管道不能 seek）";
    case EROFS:        return "只读文件系统";
    case EMLINK:       return "链接过多";
    case EPIPE:        return "管道已断开";
    case EDOM:         return "数学参数超出定义域";
    case ERANGE:       return "数值结果超出范围";
    case EDEADLK:      return "将发生资源死锁";
    case ENAMETOOLONG: return "文件名过长";
    case ENOSYS:       return "内核不支持这个功能（系统调用未实现）";
    case ENOTEMPTY:    return "目录非空";
    case ELOOP:        return "符号链接的层数过多";
    case EOVERFLOW:    return "数值太大，数据类型放不下";
    case ENOTSOCK:     return "不是套接字";
    case EADDRINUSE:   return "地址已被使用";
    case ECONNREFUSED: return "连接被拒绝";
    case ECONNRESET:   return "连接被对方重置";
    case ENOTCONN:     return "套接字没有连接";
    case ETIMEDOUT:    return "连接超时";
    case EOPNOTSUPP:   return "不支持的操作";
    default:           return strerror(err);
    }
}

/* ======================= 用户和组 ======================= */

/* 小缓存：ls -l 列出几百个文件时，属主几乎都相同，不必每次都重新读 /etc/passwd */
static struct { char kind; unsigned id; char name[32]; } g_id_cache[8];
static int g_id_cache_next;

static const char *id_to_name(char kind, const char *file, unsigned id, char *buf, size_t n)
{
    for (size_t i = 0; i < SK_ARRAY_LEN(g_id_cache); i++) {
        if (g_id_cache[i].kind == kind && g_id_cache[i].id == id) {
            sk_strlcpy(buf, g_id_cache[i].name, n);
            return buf;
        }
    }
    char name[32] = "";
    char *s = sk_read_file(file, NULL);
    if (s) {
        /* 每行形如  root:x:0:0:root:/root:/bin/sksh   （第 3 个字段是数字 id） */
        for (char *line = s; line && *line;) {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = 0;
            char *c1 = strchr(line, ':');
            char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (c1 && c2 && line[0] != '#') {
                char *end;
                unsigned long v = strtoul(c2 + 1, &end, 10);
                if (end != c2 + 1 && (*end == ':' || *end == 0) && v == id) {
                    *c1 = 0;
                    sk_strlcpy(name, line, sizeof name);
                    break;
                }
            }
            line = nl ? nl + 1 : NULL;
        }
        free(s);
    }
    if (!name[0]) {
        if (id == 0)
            sk_strlcpy(name, "root", sizeof name);
        else
            snprintf(name, sizeof name, "%u", id);
    }
    int slot = g_id_cache_next++ % (int)SK_ARRAY_LEN(g_id_cache);
    g_id_cache[slot].kind = kind;
    g_id_cache[slot].id = id;
    sk_strlcpy(g_id_cache[slot].name, name, sizeof g_id_cache[slot].name);
    sk_strlcpy(buf, name, n);
    return buf;
}

const char *sk_user_name(uid_t uid, char *buf, size_t n)
{
    return id_to_name('u', "/etc/passwd", (unsigned)uid, buf, n);
}

const char *sk_group_name(gid_t gid, char *buf, size_t n)
{
    return id_to_name('g', "/etc/group", (unsigned)gid, buf, n);
}

/* ======================= 信号 ======================= */

static const struct { int sig; const char *name; const char *desc; } g_signals[] = {
    { SIGHUP,    "HUP",    "挂起（终端断开）" },
    { SIGINT,    "INT",    "中断（Ctrl+C）" },
    { SIGQUIT,   "QUIT",   "退出并转储（Ctrl+\\）" },
    { SIGILL,    "ILL",    "非法指令" },
    { SIGTRAP,   "TRAP",   "跟踪/断点陷阱" },
    { SIGABRT,   "ABRT",   "中止（abort）" },
    { SIGBUS,    "BUS",    "总线错误" },
    { SIGFPE,    "FPE",    "算术异常（如除以零）" },
    { SIGKILL,   "KILL",   "强制杀死（不能捕获）" },
    { SIGUSR1,   "USR1",   "用户自定义信号 1" },
    { SIGSEGV,   "SEGV",   "段错误（非法内存访问）" },
    { SIGUSR2,   "USR2",   "用户自定义信号 2" },
    { SIGPIPE,   "PIPE",   "管道断开（读端已关闭）" },
    { SIGALRM,   "ALRM",   "闹钟定时器到时" },
    { SIGTERM,   "TERM",   "请求终止" },
    { SIGSTKFLT, "STKFLT", "协处理器栈错误" },
    { SIGCHLD,   "CHLD",   "子进程状态改变" },
    { SIGCONT,   "CONT",   "继续运行" },
    { SIGSTOP,   "STOP",   "暂停（不能捕获）" },
    { SIGTSTP,   "TSTP",   "终端暂停（Ctrl+Z）" },
    { SIGTTIN,   "TTIN",   "后台进程读终端" },
    { SIGTTOU,   "TTOU",   "后台进程写终端" },
    { SIGURG,    "URG",    "套接字有紧急数据" },
    { SIGXCPU,   "XCPU",   "超出 CPU 时间限制" },
    { SIGXFSZ,   "XFSZ",   "超出文件大小限制" },
    { SIGVTALRM, "VTALRM", "虚拟定时器到时" },
    { SIGPROF,   "PROF",   "性能分析定时器到时" },
    { SIGWINCH,  "WINCH",  "终端窗口大小改变" },
    { SIGIO,     "IO",     "可以进行 I/O 了" },
    { SIGPWR,    "PWR",    "电源故障" },
    { SIGSYS,    "SYS",    "错误的系统调用" },
};

int sk_signal_from_name(const char *name)
{
    if (!name || !*name)
        return -1;
    if (isdigit((unsigned char)name[0])) {
        char *end;
        long v = strtol(name, &end, 10);
        return (*end == 0 && v >= 0 && v < _NSIG) ? (int)v : -1;
    }
    if (strncasecmp(name, "SIG", 3) == 0)
        name += 3;
    for (size_t i = 0; i < SK_ARRAY_LEN(g_signals); i++)
        if (strcasecmp(name, g_signals[i].name) == 0)
            return g_signals[i].sig;
    /* 几个历史别名 */
    if (strcasecmp(name, "IOT") == 0)  return SIGABRT;
    if (strcasecmp(name, "CLD") == 0)  return SIGCHLD;
    if (strcasecmp(name, "POLL") == 0) return SIGIO;
    /* 实时信号：RTMIN、RTMIN+3、RTMAX-1 */
    if (strncasecmp(name, "RTMIN", 5) == 0 || strncasecmp(name, "RTMAX", 5) == 0) {
        int base = toupper((unsigned char)name[4]) == 'N' ? SIGRTMIN : SIGRTMAX;
        const char *p = name + 5;
        int off = 0;
        if (*p == '+' || *p == '-') {
            char *end;
            off = (int)strtol(p, &end, 10);
            if (*end)
                return -1;
        } else if (*p) {
            return -1;
        }
        int sig = base + off;
        return (sig >= SIGRTMIN && sig <= SIGRTMAX) ? sig : -1;
    }
    return -1;
}

const char *sk_signal_name(int sig)
{
    for (size_t i = 0; i < SK_ARRAY_LEN(g_signals); i++)
        if (g_signals[i].sig == sig)
            return g_signals[i].name;
    return NULL;
}

const char *sk_signal_desc(int sig)
{
    for (size_t i = 0; i < SK_ARRAY_LEN(g_signals); i++)
        if (g_signals[i].sig == sig)
            return g_signals[i].desc;
    if (sig >= SIGRTMIN && sig <= SIGRTMAX)
        return "实时信号";
    return NULL;
}

/* ======================= 紧凑大小 ======================= */

void sk_fmt_size_short(char *buf, size_t n, uint64_t bytes)
{
    static const char units[] = "KMGTPE";
    if (bytes < 1024) {
        snprintf(buf, n, "%llu", (unsigned long long)bytes);
        return;
    }
    /* 选单位：让整数部分落在 [1, 1024) */
    int u = 0;
    uint64_t div = 1024;
    while (u < 5 && bytes / div >= 1024) {
        div *= 1024;
        u++;
    }
    uint64_t ip = bytes / div, rem = bytes % div;
    if (ip < 10) {
        /* 小于 10 时保留一位小数，并且向上取整（GNU 的习惯：宁可多报，不少报） */
        uint64_t frac = (rem * 10 + div - 1) / div;   /* rem < div <= 2^60，乘 10 不会溢出 */
        uint64_t tenths = ip * 10 + frac;
        if (tenths < 100) {
            snprintf(buf, n, "%llu.%llu%c", (unsigned long long)(tenths / 10),
                     (unsigned long long)(tenths % 10), units[u]);
            return;
        }
        ip = 10;
        rem = 0;
    }
    uint64_t v = ip + (rem ? 1 : 0);
    if (v >= 1024 && u < 5) {
        snprintf(buf, n, "1.0%c", units[u + 1]);
        return;
    }
    snprintf(buf, n, "%llu%c", (unsigned long long)v, units[u]);
}
