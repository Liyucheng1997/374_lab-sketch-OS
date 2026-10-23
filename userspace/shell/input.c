/*
 * input.c — shell 的输入来源
 *
 * 同一个“读-解析-执行”循环可以从三种地方读命令：
 *   字符串   sksh -c '...'、eval、trap
 *   文件描述符  脚本文件、source、从管道读脚本（echo 'ls' | sksh）
 *   终端     交互式使用：用行编辑器读，显示 PS1 / PS2 提示符
 *
 * 一个微妙的问题：从 stdin（fd 0）读脚本时，脚本里的命令也会读 stdin。比如
 *     printf 'read x\nhello\necho $x\n' | sksh
 * 第二行 "hello" 是给 read 命令的数据，不是命令。如果 shell 一次 read() 读走 4KB，
 * 子命令就读不到了。所以：
 *   - stdin 是管道（不能回退）时，shell 每次只读 1 个字节，读到换行就停；
 *   - stdin 是普通文件时，可以多读，执行命令前用 lseek 把多读的部分“退回去”。
 * 文件偏移量属于“打开文件描述”，父子进程共享它，所以退回之后子进程就能接着读。
 */
#include "sksh.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { SRC_STRING, SRC_FD, SRC_TTY };

struct source {
    int    kind;
    char  *name;
    /* 字符串 */
    char  *str;
    size_t slen, spos;
    /* 文件描述符 */
    int    fd, own_fd;
    int    seekable, bytewise, need_sync;
    int    eof;
    size_t bpos, blen;
    char   buf[4096];
};

static source *source_alloc(int kind, const char *name)
{
    source *s = xcalloc(1, sizeof *s);
    s->kind = kind;
    s->name = xstrdup(name ? name : "sksh");
    s->fd = -1;
    return s;
}

source *source_string(const char *str, const char *name)
{
    source *s = source_alloc(SRC_STRING, name);
    s->slen = strlen(str);
    s->str = xstrndup(str, s->slen);
    return s;
}

source *source_fd(int fd, const char *name, int own_fd)
{
    source *s = source_alloc(SRC_FD, name);
    s->fd = fd;
    s->own_fd = own_fd;
    s->seekable = lseek(fd, 0, SEEK_CUR) != (off_t)-1;
    if (fd == STDIN_FILENO) {
        s->bytewise = !s->seekable;
        s->need_sync = s->seekable;
    }
    return s;
}

source *source_tty(void)
{
    source *s = source_alloc(SRC_TTY, "sksh");
    s->fd = STDIN_FILENO;
    s->bytewise = 1;        /* 没有行编辑器时的后备方式 */
    return s;
}

void source_free(source *s)
{
    if (!s)
        return;
    if (s->own_fd && s->fd >= 0)
        close(s->fd);
    free(s->str);
    free(s->name);
    free(s);
}

const char *source_name(const source *s)
{
    return s->name;
}

static int fd_fill(source *s)
{
    if (s->eof)
        return 0;
    ssize_t r = full_read(s->fd, s->buf, s->bytewise ? 1 : sizeof s->buf);
    if (r <= 0) {
        if (r < 0 && errno != EIO)
            sh_error_noloc("读取输入失败: %s", sh_strerror(errno));
        s->eof = 1;
        return 0;
    }
    s->bpos = 0;
    s->blen = (size_t)r;
    return 1;
}

static int fd_getline(source *s, strbuf *out)
{
    int got = 0;
    for (;;) {
        if (s->bpos >= s->blen && !fd_fill(s))
            return got;
        char  *start = s->buf + s->bpos;
        size_t avail = s->blen - s->bpos;
        char  *nl = memchr(start, '\n', avail);
        size_t take = nl ? (size_t)(nl - start) + 1 : avail;
        for (size_t i = 0; i < take; i++)
            if (start[i])           /* NUL 字节在 shell 里没有意义，丢掉 */
                sb_putc(out, start[i]);
        s->bpos += take;
        got = 1;
        if (nl)
            return 1;
    }
}

static int string_getline(source *s, strbuf *out)
{
    if (s->spos >= s->slen)
        return 0;
    const char *start = s->str + s->spos;
    const char *nl = memchr(start, '\n', s->slen - s->spos);
    size_t take = nl ? (size_t)(nl - start) + 1 : s->slen - s->spos;
    sb_putn(out, start, take);
    s->spos += take;
    return 1;
}

static int tty_getline(source *s, strbuf *out, int continuation)
{
    const char *ps = var_get(continuation ? "PS2" : "PS1");
    if (!ps)
        ps = continuation ? "> " : "# ";
    char *prompt = expand_prompt(ps);
    int r;
    if (lineedit_available()) {
        strbuf line;
        sb_init(&line);
        r = lineedit_read(prompt, &line);
        if (r == LE_OK) {
            sb_putn(out, sb_str(&line), line.len);
            sb_putc(out, '\n');
        }
        sb_free(&line);
        free(prompt);
        return r == LE_OK ? 1 : r == LE_INTR ? -1 : 0;
    }
    /* stdin 不是终端（例如 sksh -i < 文件）：照样打印提示符，按行读 */
    full_write(STDERR_FILENO, prompt, strlen(prompt));
    free(prompt);
    g_sigint = 0;
    r = fd_getline(s, out);
    if (g_sigint)
        return -1;
    return r;
}

int source_getline(source *s, strbuf *out, int continuation)
{
    size_t before = out->len;
    int r;
    switch (s->kind) {
    case SRC_STRING: r = string_getline(s, out); break;
    case SRC_FD:     r = fd_getline(s, out); break;
    default:         r = tty_getline(s, out, continuation); break;
    }
    if (r > 0 && sh.opt_verbose)        /* set -v：把读到的输入原样显示出来 */
        full_write(STDERR_FILENO, out->s + before, out->len - before);
    return r;
}

void source_sync(source *s)
{
    if (s->kind != SRC_FD || !s->need_sync || s->bpos >= s->blen)
        return;
    if (lseek(s->fd, -(off_t)(s->blen - s->bpos), SEEK_CUR) != (off_t)-1) {
        s->bpos = s->blen = 0;
        s->eof = 0;
    }
}
