/*
 * util.c — sksh 的通用小工具：内存分配、字符串缓冲、内存池、UTF-8 显示宽度、错误输出
 */
#include "sksh.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

/* ======================= 内存 ======================= */

static void out_of_memory(void)
{
    static const char msg[] = "sksh: 内存不足\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof msg - 1);
    (void)r;
    _exit(2);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        out_of_memory();
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        out_of_memory();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        out_of_memory();
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* ======================= strbuf ======================= */

void sb_init(strbuf *b)
{
    b->s = NULL;
    b->len = b->cap = 0;
}

static void sb_grow(strbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1)
        cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
}

void sb_putc(strbuf *b, int c)
{
    sb_grow(b, 1);
    b->s[b->len++] = (char)c;
    b->s[b->len] = 0;
}

void sb_putn(strbuf *b, const char *s, size_t n)
{
    if (n == 0) {
        if (!b->s) {
            sb_grow(b, 0);
            b->s[0] = 0;
        }
        return;
    }
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_puts(strbuf *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

void sb_printf(strbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    sb_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void sb_clear(strbuf *b)
{
    b->len = 0;
    if (b->s)
        b->s[0] = 0;
}

void sb_setlen(strbuf *b, size_t len)
{
    if (len < b->len) {
        b->len = len;
        b->s[len] = 0;
    }
}

void sb_free(strbuf *b)
{
    free(b->s);
    sb_init(b);
}

char *sb_take(strbuf *b)
{
    char *s = b->s ? b->s : xstrdup("");
    sb_init(b);
    return s;
}

const char *sb_str(const strbuf *b)
{
    return b->s ? b->s : "";
}

/* ======================= strvec ======================= */

void sv_init(strvec *v)
{
    v->v = NULL;
    v->n = v->cap = 0;
}

void sv_push(strvec *v, char *s)
{
    if (v->n + 2 > v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->v = xrealloc(v->v, sizeof(char *) * (size_t)v->cap);
    }
    v->v[v->n++] = s;
    v->v[v->n] = NULL;
}

void sv_free(strvec *v)
{
    for (int i = 0; i < v->n; i++)
        free(v->v[i]);
    free(v->v);
    sv_init(v);
}

char **sv_argv(strvec *v)
{
    if (!v->v) {
        v->cap = 8;
        v->v = xcalloc((size_t)v->cap, sizeof(char *));
    }
    return v->v;
}

/* ======================= arena（内存池） =======================
 * 语法树由很多小节点组成，逐个 malloc/free 既慢又容易漏。
 * 这里用“碰撞指针”分配：从一大块内存里顺序切出小块，整棵树用完后整池释放。 */

#define ARENA_BLOCK 8192

struct ablock {
    struct ablock *next;
    size_t used, cap;
    max_align_t data[];
};

struct arena {
    struct ablock *blocks;
    int refs;
};

arena *arena_new(void)
{
    arena *a = xcalloc(1, sizeof *a);
    a->refs = 1;
    return a;
}

void *arena_alloc(arena *a, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    struct ablock *b = a->blocks;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > ARENA_BLOCK / 2 ? n : ARENA_BLOCK;
        struct ablock *nb = xmalloc(sizeof *nb + cap);
        nb->used = 0;
        nb->cap = cap;
        if (b && n > ARENA_BLOCK / 2) {
            /* 大块单独挂在第二个位置，不浪费当前块剩余的空间 */
            nb->next = b->next;
            b->next = nb;
        } else {
            nb->next = b;
            a->blocks = nb;
        }
        b = nb;
    }
    void *p = (char *)b->data + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strndup(arena *a, const char *s, size_t n)
{
    char *d = arena_alloc(a, n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *arena_strdup(arena *a, const char *s)
{
    return arena_strndup(a, s, strlen(s));
}

void arena_ref(arena *a)
{
    if (a)
        a->refs++;
}

void arena_unref(arena *a)
{
    if (!a || --a->refs > 0)
        return;
    struct ablock *b = a->blocks;
    while (b) {
        struct ablock *n = b->next;
        free(b);
        b = n;
    }
    free(a);
}

/* ======================= UTF-8 =======================
 * 终端按“列”显示字符：ASCII 占 1 列，汉字等东亚宽字符占 2 列，组合用的附加符号占 0 列。
 * 行编辑器要知道光标在第几列，必须先把 UTF-8 字节解码成 Unicode 码点再查宽度。
 * 这里自己解码（不依赖 locale），宽度查询用 musl 的 wcwidth（它与 locale 无关）。 */

int utf8_seqlen(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

int utf8_decode(const char *s, size_t len, uint32_t *cp)
{
    unsigned char c = (unsigned char)s[0];
    int n = utf8_seqlen(c);
    if (n == 1) {
        *cp = c < 0x80 ? c : 0xFFFD;
        return 1;
    }
    if ((size_t)n > len) {
        *cp = 0xFFFD;
        return 1;
    }
    uint32_t v = c & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        unsigned char k = (unsigned char)s[i];
        if ((k & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        v = (v << 6) | (k & 0x3F);
    }
    *cp = v;
    return n;
}

int cp_width(uint32_t cp)
{
    if (cp < 0x20 || cp == 0x7F)
        return 0;
    if (cp < 0x7F)
        return 1;
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

/* 跳过一个 ANSI 转义序列（ESC [ ... 终止字节、ESC ] ... BEL、ESC x），返回下一个位置 */
static size_t skip_escape(const char *s, size_t len, size_t i)
{
    i++;                        /* ESC */
    if (i >= len)
        return i;
    if (s[i] == '[') {
        i++;
        while (i < len && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7E))
            i++;
        return i < len ? i + 1 : i;
    }
    if (s[i] == ']') {
        while (i < len && s[i] != '\a' && !(s[i] == 0x1b && i + 1 < len && s[i + 1] == '\\'))
            i++;
        if (i < len && s[i] == 0x1b)
            i++;
        return i < len ? i + 1 : i;
    }
    return i + 1;
}

int str_width(const char *s, size_t len)
{
    int w = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            i = skip_escape(s, len, i);
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            i++;
            continue;
        }
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        w += cp_width(cp);
        i += (size_t)n;
    }
    return w;
}

size_t utf8_count(const char *s)
{
    size_t n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80)
            n++;
    return n;
}

/* ======================= 读写 =======================
 * read/write 可能被信号打断（EINTR）或者只写了一部分（管道满、终端慢），
 * 所以要循环直到全部写完。这是系统编程里最常见的坑之一。 */

ssize_t full_write(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(fd, p + done, n - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)r;
    }
    return (ssize_t)done;
}

ssize_t full_read(int fd, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = read(fd, buf, n);
        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

/* ======================= 错误信息 ======================= */

static void verror(int with_loc, const char *fmt, va_list ap)
{
    fflush(stdout);
    strbuf b;
    sb_init(&b);
    sb_puts(&b, "sksh: ");
    if (with_loc && sh.script_name && sh.lineno > 0)
        sb_printf(&b, "%s: 第 %d 行: ", sh.script_name, sh.lineno);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        sb_grow(&b, (size_t)n);
        vsnprintf(b.s + b.len, (size_t)n + 1, fmt, ap);
        b.len += (size_t)n;
    }
    sb_putc(&b, '\n');
    full_write(STDERR_FILENO, b.s, b.len);
    sb_free(&b);
}

void sh_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verror(1, fmt, ap);
    va_end(ap);
}

void sh_error_noloc(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verror(0, fmt, ap);
    va_end(ap);
}

const char *sh_strerror(int err)
{
    switch (err) {
    case ENOENT:       return "没有那个文件或目录";
    case EACCES:       return "权限不够";
    case EPERM:        return "不允许的操作";
    case ENOTDIR:      return "不是目录";
    case EISDIR:       return "是一个目录";
    case EEXIST:       return "文件已存在";
    case ENOEXEC:      return "可执行文件格式错误";
    case E2BIG:        return "参数列表过长";
    case ENOMEM:       return "内存不足";
    case EBADF:        return "错误的文件描述符";
    case EINVAL:       return "无效的参数";
    case EMFILE:       return "打开的文件过多";
    case ENFILE:       return "系统中打开的文件过多";
    case ENOSPC:       return "设备上没有空间";
    case EROFS:        return "只读文件系统";
    case ELOOP:        return "符号链接的层数过多";
    case ENAMETOOLONG: return "文件名过长";
    case EBUSY:        return "设备或资源忙";
    case ECHILD:       return "没有子进程";
    case ESRCH:        return "没有那个进程";
    case EINTR:        return "被中断的系统调用";
    case EAGAIN:       return "资源暂时不可用";
    case EPIPE:        return "断开的管道";
    case ENOTTY:       return "对设备不适当的 ioctl 操作（不是终端）";
    case EXDEV:        return "无效的跨设备链接";
    case ETXTBSY:      return "文本文件忙";
    case EIO:          return "输入/输出错误";
    case ENOSYS:       return "函数未实现";
    case ENOTEMPTY:    return "目录非空";
    case EFBIG:        return "文件过大";
    default:           return strerror(err);
    }
}

/* 需要时给字符串加上单引号，让输出的内容可以原样粘贴回 shell 执行 */
void shell_quote(strbuf *out, const char *s)
{
    if (!*s) {
        sb_puts(out, "''");
        return;
    }
    int safe = 1;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr("_@%+=:,./-", c))) {
            safe = 0;
            break;
        }
    }
    if (safe) {
        sb_puts(out, s);
        return;
    }
    sb_putc(out, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'')
            sb_puts(out, "'\\''");
        else
            sb_putc(out, *p);
    }
    sb_putc(out, '\'');
}

/* ======================= 文件描述符 ======================= */

int fd_move_high(int fd)
{
    if (fd < 0 || fd >= SH_FD_BASE) {
        if (fd >= 0)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        return fd;
    }
    int nfd = fcntl(fd, F_DUPFD_CLOEXEC, SH_FD_BASE);
    if (nfd < 0)
        return fd;
    close(fd);
    return nfd;
}
