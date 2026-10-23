/*
 * skterm — SketchOS 的终端（图形窗口里的命令行）
 *
 * 用法（/etc/sketch/apps.conf：terminal … /bin/skterm %f；桌面上 Ctrl+Alt+T 也会打开它）：
 *   skterm            在家目录（$HOME）里打开一个交互式 shell（$SHELL -l，没有就用 /bin/sksh）
 *   skterm 目录       在这个目录里打开 shell
 *   skterm 命令行     用 “shell -c 命令行” 运行一条命令（文件管理器“运行程序”就是这样做的）；
 *                     命令结束后窗口不关，显示退出码，按任意键关闭
 *
 * ┌──────────────────────── 一个字符的旅程 ────────────────────────┐
 *
 *  你在窗口里按下 l、s、回车：
 *    sketchd 把按键事件发给 skterm → input.c 翻译成字节 "ls\r" → write() 写进 pty 主机端
 *    → 内核线路规程：规范模式下由它回显（"ls" 原样送回主机端）、把 \r 变成 \n、攒够一行交给程序
 *      （sksh 的行编辑器把终端切到了“原始模式”，回显、光标移动由它自己做，见 shell/lineedit.c）
 *    → sksh 从标准输入（pty 从机端）读到 "ls"、回车，fork + exec 运行 ls
 *    → ls 往标准输出写 "\033[1;34m文档\033[0m  欢迎.txt\n"
 *    → 线路规程把 \n 变成 \r\n → 我们在主机端 read() 到这些字节
 *    → vt.c 解析：设置颜色、把字写进格子、记下哪些格子变了
 *    → 本轮事件循环结束时 draw.c 只重画变了的格子，告诉 sketchd 这几块变了（damage）
 *
 * └──────────────────────────────────────────────────────────────────┘
 *
 * 子进程结束：shell 退出时内核给我们发 SIGCHLD；信号处理函数里不能做复杂的事，只往一个
 * “自管道”里写一个字节，事件循环看到管道可读，再在正常的代码里 waitpid 回收子进程（不留僵尸）。
 * 交互式 shell 退出（输入 exit）→ 关窗口；“运行命令”模式 → 显示退出码，按任意键关窗口。
 *
 * 调试钩子（自动化测试用，tests/apps/test_skterm.py）：
 *   环境变量 SKTERM_DEBUG=1 时，skterm 启动后在标准输出打印 “skterm: child pid=N cols=C rows=R”，
 *   子进程结束时打印 “skterm: child exited status=S”；
 *   收到 SIGUSR1 时把当前画面打印成一行 JSON（以 “SKTERM-DUMP ” 开头），字段：
 *     cols rows     终端的列数、行数          cx cy    光标位置（从 0 开始，屏幕坐标）
 *     cursor        光标是否显示（?25h/l）    alt      是否在备用屏幕
 *     view          往回翻了几行（0 = 最新）  sb       滚动历史的行数
 *     app_cursor    应用光标键模式（?1h）      title    窗口标题
 *     finished      “程序已结束”状态          focused  窗口是否有键盘焦点
 *     lines  显示的每一行文字（行尾空白去掉，宽字符的右半格不输出）
 *     cells  每一格一个字母：'.' 空格子  'a' 普通字符  'W' 宽字符左半  '-' 宽字符右半
 *     attrs  非默认样式的连续格子：[行, 起始列, 格数, 前景, 背景, 属性位]，
 *            颜色写成 "default"、调色板序号 "1" 或 "#rrggbb"，属性位见 term.h 的 TA_*
 */
#define _GNU_SOURCE
#include "skterm.h"

#include <skutil.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define READ_CHUNK   16384
#define READ_BUDGET  (256 * 1024)  /* 一次回调最多读这么多，然后先去画一帧、处理按键 */
#define READ_MS      30

static int g_sigpipe_w = -1;       /* 信号处理函数只能用全局变量 */

/* ======================================================================
 * 信号 → 自管道
 * ====================================================================== */

/* 信号处理函数会在任意时刻打断主程序，里面只能调用“异步信号安全”的函数（write 是，
 * malloc/printf 都不是）。所以只写一个字节记下“发生了什么”，真正的处理回到事件循环里做。 */
static void on_signal(int sig)
{
    int saved = errno;
    char c = sig == SIGCHLD ? 'C' : sig == SIGUSR1 ? 'D' : 'T';
    if (g_sigpipe_w >= 0) {
        ssize_t n = write(g_sigpipe_w, &c, 1);
        (void)n;
    }
    errno = saved;
}

static int setup_signals(skterm *s)
{
    if (pipe2(s->sigpipe, O_CLOEXEC | O_NONBLOCK) < 0)
        return -1;
    g_sigpipe_w = s->sigpipe[1];
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART：被信号打断的慢速系统调用自动重来；SA_NOCLDSTOP：子进程只是被暂停（Ctrl+Z）时不通知 */
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        return -1;
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGTERM, &sa, NULL) < 0 || sigaction(SIGHUP, &sa, NULL) < 0)
        return -1;
    if (s->debug && sigaction(SIGUSR1, &sa, NULL) < 0)
        return -1;
    /* 标准输出（调试信息）可能是一根已经没人读的管道：写它时别让 SIGPIPE 把终端杀掉。
     * （子进程在 exec 之前会把所有信号恢复成默认，shell 里的程序照常收到 SIGPIPE） */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* ======================================================================
 * 调试：把画面打印成 JSON
 * ====================================================================== */

static void json_str(FILE *f, const char *p, size_t n)
{
    fputc('"', f);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

static void json_color(FILE *f, uint32_t c)
{
    switch (TC_KIND(c)) {
    case TC_INDEXED: fprintf(f, "\"%u\"", c & 0xFF); break;
    case TC_RGB:     fprintf(f, "\"#%06x\"", c & 0xFFFFFF); break;
    default:         fputs("\"default\"", f); break;
    }
}

static void debug_dump(skterm *s)
{
    term *t = s->t;
    FILE *f = stdout;
    fprintf(f, "SKTERM-DUMP {\"cols\":%d,\"rows\":%d,\"cx\":%d,\"cy\":%d,\"cursor\":%d,\"alt\":%d,"
               "\"view\":%d,\"sb\":%d,\"finished\":%d,\"focused\":%d,\"app_cursor\":%d,\"title\":",
            t->cols, t->rows, t->cx, t->cy, t->cursor_visible, t->alt_on, s->view, t->sb_count,
            s->finished, s->focused, t->app_cursor);
    json_str(f, s->title_buf, strlen(s->title_buf));
    char *line = malloc((size_t)t->cols * 4 + 1);
    char *map = malloc((size_t)t->cols + 1);
    fputs(",\"lines\":[", f);
    for (int r = 0; r < t->rows && line; r++) {
        const tline *l = term_line(t, skt_view_line(s, r));
        size_t n = 0, keep = 0;
        for (int x = 0; l && x < l->len && x < t->cols; x++) {
            if (l->c[x].attr & TA_CONT)
                continue;
            n += (size_t)sk_utf8_encode(l->c[x].cp ? l->c[x].cp : ' ', line + n);
            if (l->c[x].cp && l->c[x].cp != ' ')
                keep = n;
        }
        fputs(r ? "," : "", f);
        json_str(f, line, keep);
    }
    fputs("],\"cells\":[", f);
    for (int r = 0; r < t->rows && map; r++) {
        const tline *l = term_line(t, skt_view_line(s, r));
        int n = 0, keep = 0;
        for (int x = 0; x < t->cols; x++) {
            const tcell *c = l && x < l->len ? &l->c[x] : NULL;
            char ch = '.';
            if (c && (c->attr & TA_CONT))
                ch = '-';
            else if (c && (c->attr & TA_WIDE))
                ch = 'W';
            else if (c && c->cp && c->cp != ' ')
                ch = 'a';
            map[n++] = ch;
            if (ch != '.')
                keep = n;
        }
        fputs(r ? "," : "", f);
        json_str(f, map, (size_t)keep);
    }
    fputs("],\"attrs\":[", f);
    int first = 1;
    for (int r = 0; r < t->rows; r++) {
        const tline *l = term_line(t, skt_view_line(s, r));
        for (int x = 0; l && x < l->len && x < t->cols;) {
            const tcell *c = &l->c[x];
            int a = c->attr & TA_PEN_MASK;
            if (!a && c->fg == TC_DEFAULT && c->bg == TC_DEFAULT) {
                x++;
                continue;
            }
            int e = x + 1;
            while (e < l->len && e < t->cols && l->c[e].fg == c->fg && l->c[e].bg == c->bg &&
                   (l->c[e].attr & TA_PEN_MASK) == a)
                e++;
            fprintf(f, "%s[%d,%d,%d,", first ? "" : ",", r, x, e - x);
            json_color(f, c->fg);
            fputc(',', f);
            json_color(f, c->bg);
            fprintf(f, ",%d]", a);
            first = 0;
            x = e;
        }
    }
    fputs("]}\n", f);
    fflush(f);
    free(line);
    free(map);
}

/* ======================================================================
 * 子进程结束
 * ====================================================================== */

void skt_close(skterm *s)
{
    skt_hangup(s);
    sk_app_quit(s->app, 0);
}

/* 读空 pty 里剩下的输出（子进程退出前写的最后几行） */
static void drain_pty(skterm *s);

static void show_exit_message(skterm *s, const char *msg)
{
    term *t = s->t;
    char buf[512];
    /* 用终端自己的序列写进画面：先换到新的一行，再用“淡色”写一行说明 */
    int n = snprintf(buf, sizeof buf, "%s\033[0m\033[2m%s\033[0m", (t->cx > 0 || t->wrap_pending) ? "\r\n" : "",
                     msg);
    if (n > 0)
        term_write(t, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
    s->finished = 1;
    skt_set_view(s, 0);
    sk_window_set_title(s->win, "终端（已结束）");
    snprintf(s->title_buf, sizeof s->title_buf, "%s", "终端（已结束）");
}

static void child_exited(skterm *s)
{
    int st = s->child_status;
    if (s->debug) {
        printf("skterm: child exited status=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
        fflush(stdout);
    }
    drain_pty(s);
    char msg[256];
    if (WIFSIGNALED(st)) {
        int sig = WTERMSIG(st);
        const char *name = sk_signal_name(sig);
        snprintf(msg, sizeof msg, "[程序被信号 %d（%s）终止，按任意键关闭窗口]", sig, name ? name : "?");
    } else {
        snprintf(msg, sizeof msg, "[程序已结束，退出码 %d，按任意键关闭窗口]", WEXITSTATUS(st));
    }
    /* 交互式 shell：用户输入 exit 就是想关掉这个窗口。
     * 只有 shell 根本没能启动（126/127：找不到程序、没有权限）时才留着窗口让人看见错误信息。 */
    if (!s->command_mode && !(WIFEXITED(st) && (WEXITSTATUS(st) == 126 || WEXITSTATUS(st) == 127) &&
                              sk_now_ms() - s->start_ms < 5000)) {
        sk_app_quit(s->app, 0);
        return;
    }
    show_exit_message(s, msg);
}

static void reap_children(skterm *s)
{
    int st;
    pid_t pid;
    /* 多个子进程同时结束时，内核可能只发一个 SIGCHLD，所以循环回收到没有为止。
     * 不回收的话，结束的子进程会一直以“僵尸”(Z) 状态留在进程表里。 */
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        if (pid == s->child && s->child_running) {
            s->child_running = 0;
            s->child_status = st;
            child_exited(s);
        }
    }
}

static void on_sigpipe(sk_app *app, int fd, short revents, void *user)
{
    (void)app;
    (void)revents;
    skterm *s = user;
    char buf[64];
    ssize_t n;
    int chld = 0, dump = 0, term_sig = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            chld |= buf[i] == 'C';
            dump |= buf[i] == 'D';
            term_sig |= buf[i] == 'T';
        }
    }
    if (dump)
        debug_dump(s);
    if (chld)
        reap_children(s);
    if (term_sig)
        skt_close(s);
}

/* ======================================================================
 * pty 主机端可读 / 可写
 * ====================================================================== */

static void on_pty(sk_app *app, int fd, short revents, void *user);

static void pty_closed(skterm *s)
{
    /* 从机端已经没有任何进程打开了（shell 和它的子孙都退出了）：read 返回 EIO。
     * 一定要把 fd 从 poll 里拿掉——否则 poll 会一直报告 POLLHUP，事件循环原地空转。
     * 子进程的退出状态由 SIGCHLD 那边处理。 */
    sk_app_remove_fd(s->app, s->master);
    close(s->master);
    s->master = -1;
    s->wq_len = 0;
}

static void read_pty(skterm *s, int budget, int ms)
{
    static char buf[READ_CHUNK];
    uint64_t t0 = sk_now_ms();
    int total = 0;
    while (s->master >= 0 && total < budget) {
        ssize_t n = read(s->master, buf, sizeof buf);
        if (n > 0) {
            term_write(s->t, buf, (size_t)n);
            total += (int)n;
            if (ms > 0 && sk_now_ms() - t0 >= (uint64_t)ms)
                break;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            break;
        pty_closed(s);           /* EIO 或 EOF */
        break;
    }
}

static void drain_pty(skterm *s)
{
    read_pty(s, 4 * 1024 * 1024, 0);
}

static void on_pty(sk_app *app, int fd, short revents, void *user)
{
    (void)app;
    (void)fd;
    skterm *s = user;
    if (revents & POLLOUT) {
        skt_flush_input(s);
        if (s->wq_len == 0 && s->master >= 0)
            sk_app_add_fd(s->app, s->master, POLLIN, on_pty, s);
    }
    if (revents & (POLLIN | POLLHUP | POLLERR))
        read_pty(s, READ_BUDGET, READ_MS);
}

/* ======================================================================
 * 终端仿真器的回调
 * ====================================================================== */

static void cb_reply(void *user, const char *data, size_t n)
{
    skterm *s = user;
    skt_send(s, data, n);
}

static void cb_title(void *user, const char *title)
{
    skterm *s = user;
    sk_strlcpy(s->title_buf, *title ? title : "终端", sizeof s->title_buf);
    if (s->win)
        sk_window_set_title(s->win, s->title_buf);
}

static void cb_bell(void *user)
{
    skt_bell(user);
}

/* ======================================================================
 * 窗口事件
 * ====================================================================== */

static void on_resize(skterm *s, int w, int h)
{
    int cols, rows;
    skt_layout(s, w, h, &cols, &rows);
    if (cols != s->t->cols || rows != s->t->rows) {
        if (term_resize(s->t, cols, rows) == 0 && skt_view_alloc(s, rows) == 0) {
            /* 告诉内核新尺寸 → 内核给前台进程组发 SIGWINCH */
            skt_set_winsize(s);
        } else {
            sk_log("改变终端尺寸失败：内存不足");
        }
        s->view = 0;
        s->vshift = 0;
        s->seen_pushed = s->t->sb_pushed;
        skt_clear_selection(s);
    }
    /* 新缓冲区的内容是未定义的：整窗重画（必须在这里同步画好，库随后就把它交给服务器） */
    skt_render_all(s);
}

static int on_event(sk_window *w, const sk_event *e, void *user)
{
    (void)w;
    skterm *s = user;
    switch (e->type) {
    case SK_EVENT_KEY:
        return skt_key(s, e);
    case SK_EVENT_MOUSE:
        return skt_mouse(s, e);
    case SK_EVENT_FOCUS:
        /* 光标样式（实心/空心）和选区颜色与焦点有关；光标由 skt_render 自己比较、重画 */
        s->focused = e->focused;
        skt_mark_selection(s);
        return 1;
    case SK_EVENT_RESIZE:
        on_resize(s, e->size.w, e->size.h);
        return 1;
    case SK_EVENT_CLIPBOARD:
        skt_paste(s, e->clip.text, e->clip.text_len);
        return 1;
    case SK_EVENT_CLOSE:
        /* 用户点了关闭按钮：挂断子进程，然后让库默认处理（销毁窗口 → 事件循环结束） */
        skt_hangup(s);
        return 0;
    default:
        return 0;
    }
}

static void on_idle(sk_app *app, void *user)
{
    (void)app;
    skterm *s = user;
    if (s->wq_len > 0 && s->master >= 0)
        sk_app_add_fd(s->app, s->master, POLLIN | POLLOUT, on_pty, s);
    skt_render(s);
}

/* ======================================================================
 * 启动
 * ====================================================================== */

/* 选 shell：$SHELL（必须是可执行的绝对路径），否则 /bin/sksh */
static const char *pick_shell(void)
{
    const char *sh = getenv("SHELL");
    if (sh && sh[0] == '/' && access(sh, X_OK) == 0)
        return sh;
    return "/bin/sksh";
}

static int is_dir(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void start_child(skterm *s, const char *arg)
{
    const char *shell = pick_shell();
    char *argv[4];
    const char *dir = NULL;
    argv[0] = (char *)shell;
    if (arg && *arg && !is_dir(arg)) {
        /* 参数不是目录：当作一条命令行交给 shell 执行（docs/开发规范.md 4.4） */
        s->command_mode = 1;
        argv[1] = "-c";
        argv[2] = (char *)arg;
        argv[3] = NULL;
    } else {
        /* 交互式登录 shell（-l：先执行 /etc/profile 和 ~/.skshrc，和串口控制台一样） */
        argv[1] = "-l";
        argv[2] = NULL;
        dir = arg && *arg ? arg : getenv("HOME");
        if (!is_dir(dir))
            dir = "/";
    }
    s->start_ms = sk_now_ms();
    if (skt_spawn(s, argv, dir) < 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "\033[31m无法启动 %s：%s\033[0m\r\n", shell, sk_strerror(errno));
        term_write(s->t, msg, strlen(msg));
        s->command_mode = 1;
        show_exit_message(s, "[按任意键关闭窗口]");
        return;
    }
    if (sk_app_add_fd(s->app, s->master, POLLIN, on_pty, s) < 0) {
        sk_log("无法监听 pty：内存不足");
        skt_close(s);
    }
    if (s->debug) {
        printf("skterm: child pid=%d cols=%d rows=%d\n", (int)s->child, s->t->cols, s->t->rows);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    static skterm s;
    sk_log_init("skterm");
    const char *dbg = getenv("SKTERM_DEBUG");
    s.debug = dbg && *dbg && strcmp(dbg, "0") != 0;
    s.master = -1;
    s.focused = 1;
    s.seed = sk_hash_str("skterm");
    sk_strlcpy(s.title_buf, "终端", sizeof s.title_buf);
    const char *arg = argc > 1 ? argv[1] : NULL;

    if (setup_signals(&s) < 0) {
        fprintf(stderr, "skterm: 设置信号处理失败：%s\n", sk_strerror(errno));
        return 1;
    }
    s.app = sk_app_connect("skterm");
    if (!s.app)
        return 1;

    /* 终端字体是等宽的：ASCII 占 1 格，汉字正好 2 格 */
    s.font = sk_font_get(SK_FONT_MONO);
    s.cw = sk_font_cell_width(s.font);
    s.lh = sk_font_line_height(s.font);
    s.asc = sk_font_ascent(s.font);
    if (s.cw <= 0)
        s.cw = 8;
    if (s.lh < s.asc + sk_font_descent(s.font))
        s.lh = s.asc + sk_font_descent(s.font);

    /* 窗口：默认 80×24 格；屏幕太小时缩小（留出标题栏和边框的位置） */
    int w = skt_win_w(&s, SKT_COLS), h = skt_win_h(&s, SKT_ROWS);
    int sw = sk_app_screen_w(s.app), sh = sk_app_screen_h(s.app);
    if (sw > 0 && w > sw - 2 * SK_BORDER - 8)
        w = sw - 2 * SK_BORDER - 8;
    if (sh > 0 && h > sh - SK_TITLE_H - 2 * SK_BORDER - 48)
        h = sh - SK_TITLE_H - 2 * SK_BORDER - 48;
    s.win = sk_window_create(s.app, "终端", w, h, SK_WIN_RESIZABLE, SK_BLUE);
    if (!s.win) {
        sk_app_disconnect(s.app);
        return 1;
    }
    sk_window_set_min_size(s.win, skt_win_w(&s, SKT_MIN_COLS), skt_win_h(&s, SKT_MIN_ROWS));
    sk_window_set_cursor(s.win, SK_CURSOR_TEXT);

    int cols, rows;
    skt_layout(&s, sk_window_width(s.win), sk_window_height(s.win), &cols, &rows);
    term_callbacks cb = { cb_reply, cb_title, cb_bell, &s };
    s.t = term_new(cols, rows, SKT_SCROLLBACK, &cb);
    if (!s.t || skt_view_alloc(&s, rows) < 0) {
        fprintf(stderr, "skterm: 内存不足\n");
        sk_app_disconnect(s.app);
        return 1;
    }
    sk_window_set_handler(s.win, on_event, &s);
    sk_app_add_fd(s.app, s.sigpipe[0], POLLIN, on_sigpipe, &s);
    sk_app_add_idle(s.app, on_idle, &s);
    skt_render_all(&s);

    start_child(&s, arg);

    int rc = sk_app_run(s.app);

    /* 收尾：挂断还在运行的子进程，关闭主机端（内核随之挂断从机端），稍等它退出并回收 */
    skt_hangup(&s);
    if (s.master >= 0) {
        close(s.master);
        s.master = -1;
    }
    for (int i = 0; i < 20 && s.child_running; i++) {
        int st;
        pid_t p = waitpid(s.child, &st, WNOHANG);
        if (p == s.child || (p < 0 && errno == ECHILD))
            s.child_running = 0;
        else {
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    sk_app_disconnect(s.app);
    term_free(s.t);
    free(s.vlo);
    free(s.vhi);
    free(s.wq);
    return rc;
}
