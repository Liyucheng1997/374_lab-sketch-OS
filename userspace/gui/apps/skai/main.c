/*
 * skai — SketchOS 的 AI 助手（图形界面）
 *
 * 一个聊天窗口：下面输入你想做的事，上面是对话记录。AI 不只是“聊天”——它能在这台电脑上
 * 运行命令、读写文件、打开应用来完成任务，每一步都会显示在对话记录里（“▶ 运行命令：…”）。
 *
 * ┌──────────────── 这个窗口自己并不“智能” ────────────────┐
 *
 *   skai（这个程序）──Unix 域套接字──▶ skaid（AI 服务，守护进程）──串口──▶ 网页 ──▶ Claude
 *
 *  真正和模型打交道、执行工具的是系统服务 skaid（userspace/ai/）。skai 只是它的一个“前台”：
 *  把你输入的话发过去，再把一段段流回来的文字显示出来。命令行里的 ai 命令是另一个前台，
 *  两者说的是同一套协议（common/skai.h）。这种“服务 + 多个客户端”的结构在操作系统里
 *  到处都是：显示服务器 sketchd 和各个图形应用也是这样分工的。
 *
 *  套接字的 fd 交给 libsketch 的事件循环（sk_app_add_fd）：有数据来时回调我们，
 *  没有数据时整个程序睡在 poll() 里——窗口事件和 AI 的回应在同一个单线程循环里处理。
 *
 * └─────────────────────────────────────────────────────────┘
 *
 * 命令行：skai [问题]        带参数时，连上 AI 服务后立刻提问（桌面或别的程序可以这样把问题交给它）
 * 快捷键：回车 发送   Esc 停止回答   Ctrl+N 新对话   Ctrl+L 清屏
 *
 * 调试（自动化测试用）：SKAI_DEBUG=1 时向标准输出打印
 *   skai: status <状态> <模型>          收到 AI 服务的状态
 *   skai: busy <0|1>
 *   skai: send <问题>
 *   skai: done <结束原因> chars=<对话记录的字符数>
 */
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>
#include <sketch/rough.h>
#include <skai.h>
#include <skutil.h>

#include <errno.h>
#include <poll.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAD         10
#define TOOLBAR_H   48
#define INPUT_H     40
#define STATUS_H    28
#define BG_SEED     0xA1A1u
#define FLUSH_MS    150          /* 流式文字攒一攒再刷到屏幕上（网页版的 CPU 很慢） */
#define RETRY_MS    3000
#define LOG_MAX     (200 * 1024) /* 对话记录太长时丢掉最前面的部分 */

enum { SEG_NONE, SEG_USER, SEG_AI, SEG_TOOL, SEG_NOTE };

typedef struct {
    sk_app *app;
    sk_window *win;
    sk_ui *ui;
    sk_widget *btn_new, *btn_stop, *lbl_state;
    sk_widget *log_view, *input, *btn_send, *lbl_status;
    int debug;

    int fd;                     /* 和 skaid 的连接，-1 = 没连上 */
    skai_reader rd;
    int retry_timer, flush_timer;

    char *log;                  /* 对话记录的全部文字 */
    size_t len, cap;
    int seg;                    /* 记录的最后一段是谁说的 */
    int log_dirty;

    char state[16], model[64];
    int busy, thinking;
    long in_tok, out_tok;
    char *pending_ask;          /* 命令行参数给的问题：连上并就绪后发送 */
    int shown_hint;             /* “没有密钥/没有桥”的提示只在对话记录里写一次 */
} skai;

static void dbg(skai *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dbg(skai *a, const char *fmt, ...)
{
    if (!a->debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    printf("skai: ");
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    va_end(ap);
}

/* ================================================================ 对话记录 */

static void log_put(skai *a, const char *s)
{
    size_t n = strlen(s);
    if (a->len + n + 1 > a->cap) {
        size_t cap = a->cap ? a->cap : 4096;
        while (cap < a->len + n + 1)
            cap *= 2;
        char *p = realloc(a->log, cap);
        if (!p)
            return;
        a->log = p;
        a->cap = cap;
    }
    memcpy(a->log + a->len, s, n + 1);
    a->len += n;
    if (a->len > LOG_MAX) {
        /* 从一半的位置往后找一个换行，把前面的全部丢掉 */
        char *cut = strchr(a->log + a->len / 2, '\n');
        if (cut) {
            size_t drop = (size_t)(cut + 1 - a->log);
            memmove(a->log, cut + 1, a->len - drop + 1);
            a->len -= drop;
        }
    }
    a->log_dirty = 1;
}

static void log_printf(skai *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_printf(skai *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    if (vasprintf(&s, fmt, ap) >= 0) {
        log_put(a, s);
        free(s);
    }
    va_end(ap);
}

/* 开始新的一段：和上一段之间空一行（工具调用紧跟在文字后面，不空行） */
static void log_begin(skai *a, int seg)
{
    if (a->seg == seg)
        return;
    if (a->len && a->log[a->len - 1] != '\n')
        log_put(a, "\n");
    if (a->len && !(seg == SEG_TOOL && a->seg == SEG_AI) && !(seg == SEG_AI && a->seg == SEG_TOOL))
        log_put(a, "\n");
    if (seg == SEG_USER)
        log_put(a, "你：");
    else if (seg == SEG_AI)
        log_put(a, "AI：");
    a->seg = seg;
}

/* 把攒下的文字刷到编辑器里，滚到最后 */
static void flush_log(skai *a)
{
    if (!a->log_dirty)
        return;
    a->log_dirty = 0;
    sk_textarea_set_text(a->log_view, a->len ? a->log : "");
    char *t = sk_textarea_get_text(a->log_view);
    int end = t ? (int)strlen(t) : 0;
    free(t);
    sk_textarea_select(a->log_view, end, end);
}

static void on_flush_timer(sk_app *app, void *user)
{
    (void)app;
    skai *a = user;
    a->flush_timer = 0;
    flush_log(a);
}

static void schedule_flush(skai *a)
{
    if (!a->flush_timer)
        a->flush_timer = sk_app_add_timer(a->app, FLUSH_MS, 0, on_flush_timer, a);
}

static void welcome(skai *a)
{
    a->len = 0;
    if (a->log)
        a->log[0] = 0;
    log_put(a, "你好！我是 SketchOS 的 AI 助手。我不只会聊天——我能在这台电脑上运行命令、读写文件、"
               "打开应用，帮你把事情做完。每一步我都会写在这里。可以试试：\n"
               "  - 这台电脑现在的内存、进程和磁盘情况怎么样？\n"
               "  - 在“文档”文件夹里写一首关于秋天的诗，然后用记事本打开它\n"
               "  - 写一个 shell 脚本，打印 1 到 10 的平方，并运行它\n"
               "  - 解释一下 1 号进程 sinit 是做什么的，结合 ps 的输出讲讲\n");
    a->seg = SEG_NONE;          /* 欢迎词和后面的内容之间空一行 */
    flush_log(a);
}

/* ================================================================ 状态显示 */

static void update_status(skai *a)
{
    const char *s;
    char buf[160];
    if (a->fd < 0)
        s = "AI 服务没有运行";
    else if (a->busy)
        s = a->thinking ? "思考中……" : "正在回答……";
    else if (!strcmp(a->state, "ready"))
        s = "就绪";
    else if (!strcmp(a->state, "nokey"))
        s = "没有 API 密钥";
    else if (!strcmp(a->state, "nobridge"))
        s = "没有连接 Claude";
    else
        s = "连接中……";
    sk_widget_set_text(a->lbl_state, s);
    sk_label_set_color(a->lbl_state, a->busy ? SK_PURPLE : !strcmp(a->state, "ready") ? SK_GREEN : SK_RED);

    if (a->in_tok || a->out_tok)
        snprintf(buf, sizeof buf, "模型 %s · 本轮用量：输入 %ld，输出 %ld token", a->model[0] ? a->model : "-",
                 a->in_tok, a->out_tok);
    else
        snprintf(buf, sizeof buf, "模型 %s · 回车发送，Esc 停止，Ctrl+N 新对话", a->model[0] ? a->model : "-");
    sk_widget_set_text(a->lbl_status, buf);
    sk_widget_set_enabled(a->btn_stop, a->busy);
    sk_widget_set_enabled(a->btn_send, !a->busy && a->fd >= 0);
}

/* ================================================================ 和 skaid 通信 */

static void try_connect(skai *a);

static void retry_cb(sk_app *app, void *user)
{
    (void)app;
    try_connect(user);
}

static void send_ask(skai *a, const char *q)
{
    if (a->fd < 0 || a->busy || !*q)
        return;
    log_begin(a, SEG_USER);
    log_put(a, q);
    log_put(a, "\n");
    a->seg = SEG_USER;
    flush_log(a);
    dbg(a, "send %s", q);
    if (skai_send(a->fd, "ASK", q) < 0) {
        log_begin(a, SEG_NOTE);
        log_put(a, "【出错】发送失败：和 AI 服务的连接断了\n");
    }
    a->in_tok = a->out_tok = 0;
    a->busy = 1;            /* 不等 BUSY 1 回来就先锁住按钮，免得连按两次 */
    update_status(a);
}

static void on_line(skai *a, const char *cmd, const char *arg)
{
    if (!strcmp(cmd, "TEXT")) {
        a->thinking = 0;
        log_begin(a, SEG_AI);
        log_put(a, arg);
        schedule_flush(a);
        update_status(a);
    } else if (!strcmp(cmd, "THINK")) {
        if (!a->thinking) {
            a->thinking = 1;
            update_status(a);
        }
    } else if (!strcmp(cmd, "TOOL")) {
        a->thinking = 0;
        log_begin(a, SEG_TOOL);
        log_printf(a, "  ▶ %s\n", arg);
        schedule_flush(a);
    } else if (!strcmp(cmd, "RESULT")) {
        int ok = !strncmp(arg, "ok", 2);
        const char *sum = strchr(arg, ' ');
        sum = sum ? sum + 1 : "";
        log_begin(a, SEG_TOOL);
        log_printf(a, "    %s %s\n", ok ? "→" : "→ 失败：", *sum ? sum : (ok ? "完成" : ""));
        schedule_flush(a);
    } else if (!strcmp(cmd, "ERROR")) {
        log_begin(a, SEG_NOTE);
        log_printf(a, "【出错】%s\n", arg);
        schedule_flush(a);
    } else if (!strcmp(cmd, "USAGE")) {
        sscanf(arg, "%ld %ld", &a->in_tok, &a->out_tok);
        update_status(a);
    } else if (!strcmp(cmd, "BUSY")) {
        a->busy = atoi(arg) != 0;
        if (!a->busy)
            a->thinking = 0;
        dbg(a, "busy %d", a->busy);
        update_status(a);
    } else if (!strcmp(cmd, "DONE")) {
        if (!strcmp(arg, "cancelled")) {
            log_begin(a, SEG_NOTE);
            log_put(a, "（已停止）\n");
        }
        flush_log(a);
        dbg(a, "done %s chars=%zu", arg, a->len);
    } else if (!strcmp(cmd, "STATUS")) {
        char st[16] = "", model[64] = "";
        sscanf(arg, "%15s %63s", st, model);
        const char *note = arg;
        for (int k = 0; k < 2 && note; k++) {          /* 跳过前两个字段，剩下的是说明文字 */
            note = strchr(note, ' ');
            if (note)
                note++;
        }
        sk_strlcpy(a->state, st, sizeof a->state);
        sk_strlcpy(a->model, model, sizeof a->model);
        dbg(a, "status %s %s", st, model);
        if ((!strcmp(st, "nokey") || !strcmp(st, "nobridge")) && !a->shown_hint && note) {
            a->shown_hint = 1;
            log_begin(a, SEG_NOTE);
            log_printf(a, "【提示】%s\n", note);
            schedule_flush(a);
        }
        if (!strcmp(st, "ready")) {
            a->shown_hint = 0;
            if (a->pending_ask) {
                char *q = a->pending_ask;
                a->pending_ask = NULL;
                send_ask(a, q);
                free(q);
            }
        }
        update_status(a);
    }
}

static void disconnect(skai *a, const char *why)
{
    if (a->fd >= 0) {
        sk_app_remove_fd(a->app, a->fd);
        close(a->fd);
        a->fd = -1;
    }
    skai_reader_free(&a->rd);
    a->busy = a->thinking = 0;
    log_begin(a, SEG_NOTE);
    log_printf(a, "【出错】%s，%d 秒后重新连接……\n", why, RETRY_MS / 1000);
    flush_log(a);
    update_status(a);
    if (!a->retry_timer)
        a->retry_timer = sk_app_add_timer(a->app, RETRY_MS, 0, retry_cb, a);
}

static void on_socket(sk_app *app, int fd, short revents, void *user)
{
    (void)app;
    (void)fd;
    (void)revents;
    skai *a = user;
    long n = skai_reader_fill(&a->rd, a->fd);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        disconnect(a, "和 AI 服务 skaid 的连接断了");
        return;
    }
    char *line;
    while (a->fd >= 0 && (line = skai_reader_next(&a->rd))) {
        char *arg;
        const char *cmd = skai_split(line, &arg);
        on_line(a, cmd, arg);
        free(line);
    }
}

static void try_connect(skai *a)
{
    a->retry_timer = 0;
    a->fd = skai_connect();
    if (a->fd < 0) {
        a->fd = -1;
        if (!a->shown_hint) {
            a->shown_hint = 1;
            log_begin(a, SEG_NOTE);
            log_printf(a, "【出错】连不上 AI 服务 skaid（%s）。它是开机时由 sinit 启动的系统服务，"
                          "可以在终端里用 svc 命令看看它的状态。正在重试……\n", sk_strerror(errno));
            flush_log(a);
        }
        update_status(a);
        a->retry_timer = sk_app_add_timer(a->app, RETRY_MS, 0, retry_cb, a);
        return;
    }
    a->shown_hint = 0;
    sk_app_add_fd(a->app, a->fd, POLLIN, on_socket, a);
    update_status(a);
}

/* ================================================================ 界面事件 */

static void on_send(sk_widget *w, void *user)
{
    (void)w;
    skai *a = user;
    const char *t = sk_textbox_text(a->input);
    char *q = strdup(t ? t : "");
    if (!q)
        return;
    char *s = sk_trim(q);
    if (*s && !a->busy && a->fd >= 0) {
        send_ask(a, s);
        sk_textbox_set_text(a->input, "");
    }
    free(q);
    sk_ui_focus(a->ui, a->input);
}

static void on_stop(sk_widget *w, void *user)
{
    (void)w;
    skai *a = user;
    if (a->fd >= 0 && a->busy)
        skai_send(a->fd, "CANCEL", NULL);
}

static void on_new(sk_widget *w, void *user)
{
    (void)w;
    skai *a = user;
    if (a->fd >= 0)
        skai_send(a->fd, "RESET", NULL);
    a->in_tok = a->out_tok = 0;
    welcome(a);
    update_status(a);
    sk_ui_focus(a->ui, a->input);
}

static int on_event(sk_window *w, const sk_event *e, void *user)
{
    (void)w;
    skai *a = user;
    if (e->type == SK_EVENT_KEY && (e->key.state == 1)) {
        uint32_t mods = e->key.modifiers;
        if (e->key.keycode == KEY_ESC && !(mods & (SK_MOD_CTRL | SK_MOD_ALT))) {
            on_stop(NULL, a);
            return 1;
        }
        if ((mods & SK_MOD_CTRL) && !(mods & SK_MOD_ALT)) {
            if (e->key.keycode == KEY_N) {
                on_new(NULL, a);
                return 1;
            }
            if (e->key.keycode == KEY_L) {
                welcome(a);
                return 1;
            }
        }
    }
    return 0;
}

/* ================================================================ 布局与背景 */

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    skai *a = user;
    sk_widget_set_rect(a->btn_new, sk_rect_make(PAD, 6, 112, 36));
    sk_widget_set_rect(a->btn_stop, sk_rect_make(PAD + 118, 6, 82, 36));
    sk_widget_set_rect(a->lbl_state, sk_rect_make(PAD + 210, 10, W - PAD * 2 - 214, 28));

    int iy = H - STATUS_H - INPUT_H - 8;
    sk_widget_set_rect(a->log_view, sk_rect_make(PAD, TOOLBAR_H + 6, W - 2 * PAD, iy - TOOLBAR_H - 14));
    sk_widget_set_rect(a->input, sk_rect_make(PAD, iy, W - 2 * PAD - 92, INPUT_H));
    sk_widget_set_rect(a->btn_send, sk_rect_make(W - PAD - 84, iy + 1, 84, INPUT_H - 2));
    sk_widget_set_rect(a->lbl_status, sk_rect_make(PAD + 4, H - STATUS_H + 1, W - 2 * PAD - 8, STATUS_H - 2));
}

/* 和记事本一样：纸张背景，顶部工具栏和底部状态栏用深一点的纸，交界处各一条手绘分隔线 */
static void background(sk_ui *ui, sk_canvas *c, sk_rect dirty, void *user)
{
    (void)ui;
    skai *a = user;
    int W = sk_window_width(a->win), H = sk_window_height(a->win);
    sk_paper_fill(c, dirty, SK_PAPER, 3, BG_SEED);
    sk_rect bar = sk_rect_intersect(dirty, sk_rect_make(0, 0, W, TOOLBAR_H));
    if (!sk_rect_empty(bar))
        sk_paper_fill(c, bar, SK_PAPER_DARK, 3, BG_SEED);
    sk_rect st = sk_rect_intersect(dirty, sk_rect_make(0, H - STATUS_H - 2, W, STATUS_H + 2));
    if (!sk_rect_empty(st))
        sk_paper_fill(c, st, SK_PAPER_DARK, 3, BG_SEED);
    if (dirty.y <= TOOLBAR_H + 2 && dirty.y + dirty.h >= TOOLBAR_H - 4)
        sk_draw_separator(c, 0, W, TOOLBAR_H - 1, 0xA101u);
    if (dirty.y <= H - STATUS_H + 1 && dirty.y + dirty.h >= H - STATUS_H - 5)
        sk_draw_separator(c, 0, W, H - STATUS_H - 2, 0xA102u);
}

/* ================================================================ main */

int main(int argc, char **argv)
{
    skai a;
    memset(&a, 0, sizeof a);
    a.fd = -1;
    const char *dv = getenv("SKAI_DEBUG");
    a.debug = dv && *dv && strcmp(dv, "0") != 0;
    if (argc > 1 && argv[1][0]) {
        /* sketchd 把 %f 换成一个参数；从命令行运行时也可能是好几个词，拼起来 */
        size_t n = 1;
        for (int i = 1; i < argc; i++)
            n += strlen(argv[i]) + 1;
        a.pending_ask = calloc(1, n);
        for (int i = 1; i < argc && a.pending_ask; i++) {
            if (i > 1)
                strcat(a.pending_ask, " ");
            strcat(a.pending_ask, argv[i]);
        }
    }

    a.app = sk_app_connect("skai");
    if (!a.app)
        return 1;
    a.win = sk_window_create(a.app, "AI 助手", 720, 560, SK_WIN_RESIZABLE, SK_PURPLE);
    if (!a.win) {
        sk_app_disconnect(a.app);
        return 1;
    }
    sk_window_set_min_size(a.win, 520, 320);
    a.ui = sk_ui_new(a.win);
    if (!a.ui) {
        sk_app_disconnect(a.app);
        return 1;
    }

    sk_rect z = sk_rect_make(0, 0, 10, 10);
    a.btn_new = sk_icon_button_new(a.ui, z, SK_ICON_REFRESH, "新对话", on_new, &a);
    a.btn_stop = sk_button_new(a.ui, z, "停止", on_stop, &a);
    a.lbl_state = sk_label_new(a.ui, z, "", SK_FONT_UI, SK_ALIGN_RIGHT);
    a.log_view = sk_textarea_new(a.ui, z, "");
    a.input = sk_textbox_new(a.ui, z, "", NULL, &a);
    a.btn_send = sk_button_new(a.ui, z, "发送", on_send, &a);
    a.lbl_status = sk_label_new(a.ui, z, "", SK_FONT_UI, SK_ALIGN_LEFT);
    if (!a.btn_new || !a.btn_stop || !a.lbl_state || !a.log_view || !a.input || !a.btn_send || !a.lbl_status) {
        fprintf(stderr, "skai: 内存不足\n");
        return 1;
    }
    sk_widget_set_accent(a.btn_new, SK_PURPLE);
    sk_widget_set_accent(a.btn_stop, SK_RED);
    sk_widget_set_accent(a.btn_send, SK_PURPLE);
    sk_textarea_set_readonly(a.log_view, 1);
    sk_textbox_set_placeholder(a.input, "告诉 AI 你想做什么，回车发送");
    sk_widget_set_callbacks(a.input, on_send, NULL, &a);          /* 输入框里按回车 = 发送 */
    sk_label_set_color(a.lbl_status, SK_INK_SOFT);

    sk_ui_set_background(a.ui, background, &a);
    sk_ui_set_layout(a.ui, layout, &a);
    sk_ui_set_handler(a.ui, on_event, &a);
    sk_ui_focus(a.ui, a.input);

    welcome(&a);
    try_connect(&a);
    update_status(&a);

    int rc = sk_app_run(a.app);
    if (a.fd >= 0)
        close(a.fd);
    free(a.log);
    free(a.pending_ask);
    sk_ui_free(a.ui);
    sk_app_disconnect(a.app);
    return rc;
}
