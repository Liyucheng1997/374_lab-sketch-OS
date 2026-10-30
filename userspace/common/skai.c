/*
 * skai.c — AI 服务客户端协议的实现（协议说明见 skai.h）
 */
#include "skai.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int skai_connect(void)
{
    const char *path = getenv(SKAI_SOCKET_ENV);
    if (!path || !*path)
        path = SKAI_SOCKET_PATH;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(addr.sun_path, path);
    /* 流式套接字（SOCK_STREAM）：像管道一样是一条字节流，消息边界由我们自己的换行符决定 */
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

char *skai_escape(const char *s)
{
    size_t n = 0;
    for (const char *p = s; *p; p++)
        n += (*p == '\\' || *p == '\n' || *p == '\r') ? 2 : 1;
    char *out = malloc(n + 1), *o = out;
    if (!out)
        return NULL;
    for (const char *p = s; *p; p++) {
        if (*p == '\\') { *o++ = '\\'; *o++ = '\\'; }
        else if (*p == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (*p == '\r') { *o++ = '\\'; *o++ = 'r'; }
        else *o++ = *p;
    }
    *o = 0;
    return out;
}

void skai_unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *o++ = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

/* write 可能只写了一部分（套接字缓冲区满了），要循环写完 */
static int write_all(int fd, const char *d, size_t n)
{
    while (n) {
        ssize_t w = write(fd, d, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        d += w;
        n -= (size_t)w;
    }
    return 0;
}

int skai_send(int fd, const char *cmd, const char *arg)
{
    char *esc = skai_escape(arg ? arg : "");
    if (!esc)
        return -1;
    size_t cl = strlen(cmd), el = strlen(esc);
    char *line = malloc(cl + el + 3);
    if (!line) {
        free(esc);
        return -1;
    }
    memcpy(line, cmd, cl);
    size_t n = cl;
    if (arg) {
        line[n++] = ' ';
        memcpy(line + n, esc, el);
        n += el;
    }
    line[n++] = '\n';
    int rc = write_all(fd, line, n);
    free(line);
    free(esc);
    return rc;
}

long skai_reader_fill(skai_reader *r, int fd)
{
    if (r->cap - r->len < 4096) {
        size_t cap = r->cap ? r->cap * 2 : 8192;
        char *d = realloc(r->d, cap);
        if (!d)
            return -1;
        r->d = d;
        r->cap = cap;
    }
    ssize_t n;
    do {
        n = read(fd, r->d + r->len, r->cap - r->len - 1);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
        r->len += (size_t)n;
    return (long)n;
}

char *skai_reader_next(skai_reader *r)
{
    char *nl = r->len ? memchr(r->d, '\n', r->len) : NULL;
    if (!nl) {
        if (r->len > SKAI_LINE_MAX)
            r->len = 0;          /* 一行长得离谱：对方大概出错了，丢掉，免得内存无限增长 */
        return NULL;
    }
    size_t n = (size_t)(nl - r->d);
    char *line = malloc(n + 1);
    if (!line)
        return NULL;
    memcpy(line, r->d, n);
    line[n] = 0;
    if (n && line[n - 1] == '\r')
        line[n - 1] = 0;
    memmove(r->d, nl + 1, r->len - n - 1);
    r->len -= n + 1;
    return line;
}

void skai_reader_free(skai_reader *r)
{
    free(r->d);
    r->d = NULL;
    r->len = r->cap = 0;
}

const char *skai_split(char *line, char **arg)
{
    char *sp = strchr(line, ' ');
    if (sp) {
        *sp = 0;
        *arg = sp + 1;
    } else {
        *arg = line + strlen(line);
    }
    skai_unescape(*arg);
    return line;
}
