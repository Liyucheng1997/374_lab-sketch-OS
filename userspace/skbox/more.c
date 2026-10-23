/*
 * more — 分页查看文本
 *
 * 一屏显示不下时，显示满一屏就停下来等用户按键。有一个细节：
 *   cat 大文件 | more   这时 more 的标准输入是管道（数据从这里来），
 * 按键就不能从标准输入读了，要单独打开 /dev/tty —— 它永远代表“当前进程的控制终端”。
 */
#include "skbox.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SKBOX_APPLET(more, "分页显示文件内容")

static const char usage[] =
    "用法: more [文件...]\n"
    "一页一页地显示文件（默认读标准输入）。输出不是终端时，相当于 cat。\n\n"
    "按键：\n"
    "  空格 / f    下一页\n"
    "  回车 / j    下一行\n"
    "  d           下半页\n"
    "  q           退出\n"
    "  h / ?       显示按键帮助\n";

static sb_term g_tty;
static int g_tty_fd = -1;

static void restore(void)
{
    if (g_tty.active)
        sb_term_restore(&g_tty);
}

static void on_signal(int sig)
{
    /* 被 Ctrl+C 等打断时也要把终端恢复原样，否则 shell 会变得不回显 */
    restore();
    sb_write_all(STDOUT_FILENO, "\r\033[K", 4);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* 一行文字在终端上占几行（考虑汉字宽度、制表符和颜色转义序列） */
static int screen_rows(const char *s, size_t len, int cols)
{
    int col = 0, rows = 1;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\033' && i + 1 < len && s[i + 1] == '[') {
            /* ESC [ ... 字母：颜色等控制序列，不占位置 */
            i += 2;
            while (i < len && !(s[i] >= '@' && s[i] <= '~'))
                i++;
            i++;
            continue;
        }
        int w;
        if (c == '\t') {
            w = 8 - col % 8;
            i++;
        } else if (c == '\r') {
            col = 0;
            i++;
            continue;
        } else {
            uint32_t cp;
            int k = sk_utf8_decode(s + i, len - i, &cp);
            w = cp == 0xFFFD ? 1 : sk_wcwidth(cp);
            i += (size_t)k;
        }
        if (col + w > cols) {
            rows++;
            col = 0;
        }
        col += w;
    }
    return rows;
}

/* 显示提示并等待按键，返回下一步要显示的行数（0 = 退出） */
static int ask(int page, const char *suffix)
{
    for (;;) {
        char prompt[256];
        snprintf(prompt, sizeof prompt, "\033[7m--更多--%s\033[0m", suffix);
        sb_write_all(STDOUT_FILENO, prompt, strlen(prompt));
        unsigned char ch;
        ssize_t r;
        do {
            r = read(g_tty_fd, &ch, 1);
        } while (r < 0 && errno == EINTR);
        sb_write_all(STDOUT_FILENO, "\r\033[K", 4);
        if (r <= 0)
            return 0;
        switch (ch) {
        case ' ':
        case 'f':
            return page;
        case '\n':
        case '\r':
        case 'j':
            return 1;
        case 'd':
            return page / 2 > 0 ? page / 2 : 1;
        case 'q':
        case 'Q':
        case 4:   /* Ctrl+D */
            return 0;
        case 'h':
        case '?': {
            const char *help = "\033[7m空格=下一页  回车=下一行  d=半页  q=退出\033[0m  ";
            sb_write_all(STDOUT_FILENO, help, strlen(help));
            continue;
        }
        default:
            sb_write_all(STDOUT_FILENO, "\a", 1);   /* 不认识的键：响一下铃 */
        }
    }
}

int applet_more(int argc, char **argv)
{
    int c;
    while ((c = sb_getopt(argc, argv, "", usage)) != -1)
        return 1;
    int nfiles = argc - optind;
    char *dash[] = { "-", NULL };
    char **files = nfiles ? argv + optind : dash;
    if (!nfiles)
        nfiles = 1;

    int interactive = isatty(STDOUT_FILENO);
    if (interactive) {
        g_tty_fd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
        if (g_tty_fd < 0 || sb_term_raw(&g_tty, g_tty_fd) != 0)
            interactive = 0;   /* 没有控制终端：只能像 cat 一样直接输出 */
    }
    if (interactive) {
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        signal(SIGHUP, on_signal);
        signal(SIGQUIT, on_signal);
    }
    int cols, rows;
    sk_term_size(STDOUT_FILENO, &cols, &rows);
    int page = rows > 2 ? rows - 1 : 1;
    int budget = page;   /* 还能显示多少行就要停下来 */
    int rc = 0, quit = 0;
    char *line = NULL;
    size_t cap = 0;

    for (int i = 0; i < nfiles && !quit; i++) {
        FILE *f = strcmp(files[i], "-") ? fopen(files[i], "r") : stdin;
        if (!f) {
            sb_perr("%s", files[i]);
            rc = 1;
            continue;
        }
        struct stat st;
        long long size = fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
        if (S_ISDIR(st.st_mode)) {
            sb_err("%s: 是一个目录", files[i]);
            rc = 1;
            if (f != stdin)
                fclose(f);
            continue;
        }
        if (nfiles > 1) {
            printf("::::::::::::::\n%s\n::::::::::::::\n", files[i]);
            budget -= 3;
        }
        long long done = 0;
        ssize_t len;
        while (!quit && (len = getline(&line, &cap, f)) >= 0) {
            done += len;
            int need = interactive ? screen_rows(line, (size_t)(len && line[len - 1] == '\n' ? len - 1 : len), cols) : 1;
            if (interactive && budget - need < 0 && budget < page) {
                fflush(stdout);
                char suffix[64] = "";
                if (size > 0)
                    snprintf(suffix, sizeof suffix, "(%lld%%)", (done - len) * 100 / size);
                int more = ask(page, suffix);
                if (more == 0) {
                    quit = 1;
                    break;
                }
                budget = more;
            }
            fwrite(line, 1, (size_t)len, stdout);
            if (len == 0 || line[len - 1] != '\n')
                putchar('\n');
            budget -= need;
        }
        if (f != stdin)
            fclose(f);
        /* 换到下一个文件之前也停一下 */
        if (interactive && !quit && i + 1 < nfiles && budget <= 0) {
            fflush(stdout);
            char suffix[300];
            snprintf(suffix, sizeof suffix, "(下一个文件: %s)", files[i + 1]);
            int more = ask(page, suffix);
            if (more == 0)
                quit = 1;
            budget = more;
        }
    }
    free(line);
    fflush(stdout);
    restore();
    if (g_tty_fd >= 0)
        close(g_tty_fd);
    return rc;
}
