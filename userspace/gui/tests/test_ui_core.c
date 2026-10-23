/*
 * test_ui_core.c — 控件库核心（ui.c）和客户端（client.c）的单元测试，宿主机上运行
 *
 *   make -C userspace BUILD=... host-tests && $BUILD/host/test_ui_core
 *
 * 不需要真正的 sketchd：测试程序自己开一个线程扮演“显示服务器”，
 * 在临时目录里监听一个 SEQPACKET 套接字，回答 HELLO/CREATE_WINDOW，记录收到的
 * DAMAGE/SET_CURSOR/ATTACH_BUFFER 请求，还能向客户端注入鼠标、键盘、改变大小事件。
 * 主线程就是一个普通的 libsketch 应用。
 *
 * 检查的内容：UTF-8/单词/命中测试小工具、脏矩形合并、悬停只重画一个控件、
 * 光标形状只在变化时发送、点击语义、Tab 焦点顺序、禁用/隐藏控件、回调里销毁控件、
 * 剪贴板路由、改变大小。
 */
#define _GNU_SOURCE
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>
#include "../libsketch/ui_internal.h"

#include <linux/input-event-codes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                              printf(__VA_ARGS__); putchar('\n'); } } while (0)

/* ================================================================ 假的显示服务器 */

typedef struct { int type; sk_rect r; int value; } rec;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static rec logs[8192];
static int nlog;
static int listen_fd = -1, conn_fd = -1;
static uint32_t next_win = 1;
static char clip[SK_CLIP_MAX];
static int clip_len;
static char sock_path[108];

static void log_req(int type, sk_rect r, int value)
{
    pthread_mutex_lock(&mu);
    if (nlog < (int)(sizeof logs / sizeof logs[0]))
        logs[nlog++] = (rec){ type, r, value };
    pthread_mutex_unlock(&mu);
}

static void srv_send(const void *msg, size_t len) { send(conn_fd, msg, len, MSG_NOSIGNAL); }

static void *server_main(void *arg)
{
    (void)arg;
    conn_fd = accept(listen_fd, NULL, NULL);
    unsigned char buf[SK_MSG_MAX];
    for (;;) {
        char ctl[CMSG_SPACE(sizeof(int) * 4)];
        struct iovec iov = { buf, sizeof buf };
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
        ssize_t n = recvmsg(conn_fd, &mh, 0);
        if (n <= 0)
            break;
        int fd = -1;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
            if (cm->cmsg_type == SCM_RIGHTS)
                memcpy(&fd, CMSG_DATA(cm), sizeof fd);
        if (fd >= 0)
            close(fd);                 /* 测试里不需要真的映射像素 */
        const sk_msg_header *h = (const sk_msg_header *)buf;
        switch (h->type) {
        case SK_REQ_HELLO: {
            sk_ev_welcome w = { { SK_EV_WELCOME, sizeof w }, SK_PROTO_VERSION, 800, 600 };
            srv_send(&w, sizeof w);
            break;
        }
        case SK_REQ_CREATE_WINDOW: {
            const sk_req_create_window *c = (const void *)buf;
            sk_ev_window_created e = { { SK_EV_WINDOW_CREATED, sizeof e }, c->serial, next_win++ };
            srv_send(&e, sizeof e);
            sk_ev_focus f = { { SK_EV_FOCUS, sizeof f }, e.win, 1 };
            srv_send(&f, sizeof f);
            log_req(h->type, sk_rect_make(0, 0, c->w, c->h), fd >= 0);
            break;
        }
        case SK_REQ_DAMAGE: {
            const sk_req_damage *d = (const void *)buf;
            log_req(h->type, sk_rect_make(d->x, d->y, d->w, d->h), (int)d->win);
            break;
        }
        case SK_REQ_ATTACH_BUFFER: {
            const sk_req_attach_buffer *a = (const void *)buf;
            log_req(h->type, sk_rect_make(0, 0, a->w, a->h), fd >= 0);
            break;
        }
        case SK_REQ_SET_CURSOR:
            log_req(h->type, sk_rect_make(0, 0, 0, 0), (int)((const sk_req_set_cursor *)buf)->cursor);
            break;
        case SK_REQ_SET_CLIPBOARD: {
            const sk_req_clipboard *c = (const void *)buf;
            pthread_mutex_lock(&mu);
            clip_len = (int)c->len;
            memcpy(clip, c->text, c->len);
            pthread_mutex_unlock(&mu);
            break;
        }
        case SK_REQ_GET_CLIPBOARD: {
            static sk_ev_clipboard e;
            pthread_mutex_lock(&mu);
            e.hdr.type = SK_EV_CLIPBOARD;
            e.hdr.size = (uint16_t)(offsetof(sk_ev_clipboard, text) + clip_len);
            e.len = (uint32_t)clip_len;
            memcpy(e.text, clip, (size_t)clip_len);
            pthread_mutex_unlock(&mu);
            srv_send(&e, e.hdr.size);
            break;
        }
        default:
            log_req(h->type, sk_rect_make(0, 0, 0, 0), 0);
            break;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- 驱动客户端 */

static sk_app *app;
static volatile int pump_flag;

static void pump_done(sk_app *a, void *u) { (void)a; (void)u; pump_flag = 1; }

/* 让客户端的事件循环跑 ms 毫秒，然后用一次剪贴板往返确认服务器已经处理完之前的所有请求 */
static void pump(int ms)
{
    pump_flag = 0;
    sk_app_add_timer(app, ms, 0, pump_done, NULL);
    sk_app_run_until(app, &pump_flag);
    char *t = sk_clipboard_get_sync(app, 2000, NULL);
    free(t);
}

static int mark(void)
{
    pthread_mutex_lock(&mu);
    int m = nlog;
    pthread_mutex_unlock(&mu);
    return m;
}

/* 从 m 开始的、类型为 type 的记录 */
static int collect(int m, int type, rec *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&mu);
    for (int i = m; i < nlog && n < max; i++)
        if (logs[i].type == type)
            out[n++] = logs[i];
    pthread_mutex_unlock(&mu);
    return n;
}

static void mouse(uint32_t win, int kind, int x, int y, uint32_t buttons, uint32_t button)
{
    sk_ev_mouse e = { { SK_EV_MOUSE, sizeof e }, win, (uint32_t)kind, x, y, buttons, button, 0,
                      kind == SK_MOUSE_DOWN ? 1u : 0u, 0 };
    srv_send(&e, sizeof e);
}

static void click(uint32_t win, int x, int y)
{
    mouse(win, SK_MOUSE_MOVE, x, y, 0, 0);
    mouse(win, SK_MOUSE_DOWN, x, y, SK_BUTTON_LEFT, SK_BUTTON_LEFT);
    mouse(win, SK_MOUSE_UP, x, y, 0, SK_BUTTON_LEFT);
}

static void key(uint32_t win, uint32_t code, uint32_t mods, uint32_t cp)
{
    sk_ev_key e = { { SK_EV_KEY, sizeof e }, win, code, 1, mods, cp };
    srv_send(&e, sizeof e);
    e.state = 0;
    srv_send(&e, sizeof e);
}

static int rect_within(sk_rect r, sk_rect box)
{
    return r.x >= box.x && r.y >= box.y && r.x + r.w <= box.x + box.w && r.y + r.h <= box.y + box.h;
}

static sk_rect bleed(sk_rect r) { return sk_rect_inset(r, -SK_WIDGET_BLEED, -SK_WIDGET_BLEED); }

/* ================================================================ 测试 */

static void test_text_helpers(void)
{
    printf("[文本小工具]\n");
    const char *s = "a你b";                                       /* a=1 字节，你=3 字节 */
    int len = (int)strlen(s);
    CHECK(sk_ui_utf8_next(s, len, 0) == 1, "next(0)");
    CHECK(sk_ui_utf8_next(s, len, 1) == 4, "next(1) 跳过整个汉字");
    CHECK(sk_ui_utf8_prev(s, 4) == 1, "prev(4) 退回汉字开头");
    CHECK(sk_ui_utf8_prev(s, 0) == 0 && sk_ui_utf8_next(s, len, len) == len, "边界");

    const char *t = "hello  世界，foo_bar";
    int tl = (int)strlen(t);
    CHECK(sk_ui_word_right(t, tl, 0) == 5, "word_right 跳过 hello");
    CHECK(sk_ui_word_right(t, tl, 5) == 13, "word_right 跳过空格和“世界” → %d", sk_ui_word_right(t, tl, 5));
    CHECK(sk_ui_word_left(t, tl) == 16, "word_left 停在 foo_bar 开头 → %d", sk_ui_word_left(t, tl));
    CHECK(sk_ui_word_left(t, 16) == 7, "word_left 跳过“，”停在“世界”开头 → %d", sk_ui_word_left(t, 16));
    CHECK(!sk_ui_is_word_char(0xFF0C) && sk_ui_is_word_char(0x4E16) && sk_ui_is_word_char('_'), "is_word_char");

    const sk_font *f = sk_font_get(SK_FONT_UI);
    int a0 = sk_glyph_advance(f, 'a'), a1 = sk_glyph_advance(f, 0x4F60);
    CHECK(sk_ui_text_hit(f, s, len, -5) == 0, "hit 左边以外");
    CHECK(sk_ui_text_hit(f, s, len, a0 / 2 - 1) == 0, "hit 第一个字左半");
    CHECK(sk_ui_text_hit(f, s, len, a0 + a1 / 2 + 1) == 4, "hit 汉字右半 → 汉字之后");
    CHECK(sk_ui_text_hit(f, s, len, 10000) == len, "hit 右边以外");
}

static void noop_draw(sk_widget *w, sk_canvas *c, void *u) { (void)w; (void)c; (void)u; }

static void test_dirty(sk_window *win)
{
    printf("[脏矩形合并]\n");
    sk_ui *ui = sk_ui_new(win);
    pump(30);
    int m = mark();
    sk_ui_invalidate(ui, sk_rect_make(10, 10, 20, 20));
    sk_ui_invalidate(ui, sk_rect_make(30, 10, 20, 20));         /* 紧挨着：应该合并 */
    pump(20);
    rec d[64];
    int n = collect(m, SK_REQ_DAMAGE, d, 64);
    CHECK(n == 1 && d[0].r.x == 10 && d[0].r.w == 40 && d[0].r.h == 20, "相邻矩形合并成一块（%d 块）", n);

    m = mark();
    sk_ui_invalidate(ui, sk_rect_make(5, 5, 10, 10));
    sk_ui_invalidate(ui, sk_rect_make(250, 150, 10, 10));       /* 很远：分开 */
    sk_ui_invalidate(ui, sk_rect_make(6, 6, 4, 4));             /* 被包含：丢掉 */
    pump(20);
    n = collect(m, SK_REQ_DAMAGE, d, 64);
    CHECK(n == 2, "远处的两块分别 damage（%d 块）", n);

    /* 同一行上的零碎小块：互相合并，但每一个失效的像素都必须被某块 damage 覆盖 */
    m = mark();
    sk_rect small[12];
    for (int i = 0; i < 12; i++) {
        small[i] = sk_rect_make(i * 24, (i % 2) * 150, 4, 4);
        sk_ui_invalidate(ui, small[i]);
    }
    pump(20);
    n = collect(m, SK_REQ_DAMAGE, d, 64);
    int all_covered = 1;
    for (int i = 0; i < 12; i++) {
        int c = 0;
        for (int k = 0; k < n; k++)
            c |= rect_within(small[i], d[k].r);
        all_covered &= c;
    }
    CHECK(n >= 1 && n <= 8 && all_covered, "零碎小块合并成 %d 块，且没有漏掉任何一块", n);

    m = mark();
    pump(20);
    CHECK(collect(m, SK_REQ_DAMAGE, d, 64) == 0, "没有变化就没有 damage");
    sk_ui_free(ui);
}

/* 超过脏矩形上限（8 块）→ 合成一个包围盒。需要大窗口：块与块之间要隔得足够远才不会被提前合并 */
static void test_dirty_cap(void)
{
    printf("[脏矩形上限]\n");
    sk_window *big = sk_window_create(app, "大窗口", 1300, 1000, 0, 0);
    sk_ui *ui = sk_ui_new(big);
    pump(40);
    int m = mark();
    for (int i = 0; i < 9; i++)                                  /* 对角线上 9 块，两两相距很远 */
        sk_ui_invalidate(ui, sk_rect_make(10 + i * 140, 10 + i * 110, 6, 6));
    pump(20);
    rec d[16];
    int n = collect(m, SK_REQ_DAMAGE, d, 16);
    CHECK(n == 1 && d[0].r.x == 10 && d[0].r.y == 10 && d[0].r.w == 8 * 140 + 6 && d[0].r.h == 8 * 110 + 6,
          "第 9 块到来时合成包围盒（%d 块，%d,%d %dx%d）", n, n ? d[0].r.x : 0, n ? d[0].r.y : 0,
          n ? d[0].r.w : 0, n ? d[0].r.h : 0);
    m = mark();
    for (int i = 0; i < 8; i++)                                  /* 正好 8 块：保持分开 */
        sk_ui_invalidate(ui, sk_rect_make(10 + i * 140, 10 + i * 110, 6, 6));
    pump(20);
    n = collect(m, SK_REQ_DAMAGE, d, 16);
    CHECK(n == 4 || n == 8, "8 块以内保持分开（客户端 damage 最多攒 4 块，得到 %d 块）", n);
    sk_ui_free(ui);
    sk_window_destroy(big);
}

static int clicks_a, clicks_b, changes;
static void on_a(sk_widget *w, void *u) { (void)w; (void)u; clicks_a++; }
static void on_b(sk_widget *w, void *u) { (void)w; (void)u; clicks_b++; }
static void on_change(sk_widget *w, void *u) { (void)w; (void)u; changes++; }

static void test_hover_click(sk_window *win)
{
    printf("[悬停 / 光标 / 点击]\n");
    uint32_t id = sk_window_id(win);
    sk_ui *ui = sk_ui_new(win);
    sk_rect ra = sk_rect_make(20, 20, 100, 36), rb = sk_rect_make(160, 20, 100, 36);
    sk_button_new(ui, ra, "甲", on_a, NULL);
    sk_widget *b = sk_button_new(ui, rb, "乙", on_b, NULL);
    sk_widget *tb = sk_textbox_new(ui, sk_rect_make(20, 80, 200, 34), "", on_change, NULL);
    pump(30);
    mouse(id, SK_MOUSE_MOVE, 5, 150, 0, 0);
    pump(20);

    int m = mark();
    mouse(id, SK_MOUSE_MOVE, 50, 30, 0, 0);
    pump(20);
    rec d[64], cur[16];
    int n = collect(m, SK_REQ_DAMAGE, d, 64);
    int ok = n > 0;
    for (int i = 0; i < n; i++)
        ok &= rect_within(d[i].r, bleed(ra));
    CHECK(ok, "悬停只重画按钮甲（%d 块）", n);
    n = collect(m, SK_REQ_SET_CURSOR, cur, 16);
    CHECK(n == 1 && cur[0].value == SK_CURSOR_HAND, "光标变成小手（%d 次）", n);

    m = mark();
    mouse(id, SK_MOUSE_MOVE, 60, 31, 0, 0);
    mouse(id, SK_MOUSE_MOVE, 70, 32, 0, 0);
    pump(20);
    CHECK(collect(m, SK_REQ_DAMAGE, d, 64) == 0, "按钮内移动不重画");
    CHECK(collect(m, SK_REQ_SET_CURSOR, cur, 16) == 0, "按钮内移动不重复发送光标");

    m = mark();
    mouse(id, SK_MOUSE_MOVE, 200, 30, 0, 0);
    pump(20);
    n = collect(m, SK_REQ_DAMAGE, d, 64);
    ok = n > 0;
    for (int i = 0; i < n; i++)
        ok &= rect_within(d[i].r, bleed(ra)) || rect_within(d[i].r, bleed(rb)) ||
              rect_within(d[i].r, sk_rect_make(16, 16, 248, 44));     /* 两块可能被合并 */
    CHECK(ok, "从甲移到乙只重画这两个按钮");
    CHECK(collect(m, SK_REQ_SET_CURSOR, cur, 16) == 0, "两个按钮光标相同：不发送");

    m = mark();
    mouse(id, SK_MOUSE_MOVE, 100, 97, 0, 0);
    pump(20);
    n = collect(m, SK_REQ_SET_CURSOR, cur, 16);
    CHECK(n == 1 && cur[0].value == SK_CURSOR_TEXT, "输入框上是 I 形光标（%d 次）", n);

    click(id, 50, 30);
    pump(20);
    CHECK(clicks_a == 1, "点击甲 → on_click（%d）", clicks_a);
    mouse(id, SK_MOUSE_MOVE, 50, 30, 0, 0);
    mouse(id, SK_MOUSE_DOWN, 50, 30, SK_BUTTON_LEFT, SK_BUTTON_LEFT);
    mouse(id, SK_MOUSE_MOVE, 300, 150, SK_BUTTON_LEFT, 0);
    mouse(id, SK_MOUSE_UP, 300, 150, 0, SK_BUTTON_LEFT);
    pump(20);
    CHECK(clicks_a == 1, "按下后移出再松开不算点击");
    mouse(id, SK_MOUSE_DOWN, 190, 30, SK_BUTTON_RIGHT, SK_BUTTON_RIGHT);
    mouse(id, SK_MOUSE_UP, 190, 30, 0, SK_BUTTON_RIGHT);
    pump(20);
    CHECK(clicks_b == 0, "右键不算点击");
    sk_widget_set_enabled(b, 0);
    click(id, 190, 30);
    pump(20);
    CHECK(clicks_b == 0, "禁用的按钮不响应");
    sk_widget_set_enabled(b, 1);
    click(id, 190, 30);
    pump(20);
    CHECK(clicks_b == 1, "重新启用后响应");
    CHECK(sk_ui_focused(ui) == NULL, "点击按钮不夺取键盘焦点");

    /* 输入框：点击获得焦点、打字、剪贴板路由 */
    click(id, 100, 97);
    pump(20);
    CHECK(sk_ui_focused(ui) == tb, "点击输入框获得焦点");
    key(id, KEY_A, SK_MOD_SHIFT, 'A');
    key(id, 0, 0, 0x4F60);                                       /* “你”：输入法提交的字符 */
    pump(20);
    CHECK(strcmp(sk_textbox_text(tb), "A你") == 0 && changes == 2, "输入 ASCII 和中文（%s）", sk_textbox_text(tb));
    pthread_mutex_lock(&mu);
    strcpy(clip, "贴");
    clip_len = (int)strlen(clip);
    pthread_mutex_unlock(&mu);
    key(id, KEY_V, SK_MOD_CTRL, 'v');
    pump(40);
    CHECK(strcmp(sk_textbox_text(tb), "A你贴") == 0, "Ctrl+V 的回复交给发起请求的输入框（%s）", sk_textbox_text(tb));
    key(id, KEY_LEFT, 0, 0);
    key(id, KEY_BACKSPACE, 0, 0x08);
    pump(20);
    CHECK(strcmp(sk_textbox_text(tb), "A贴") == 0, "Backspace 删除整个汉字（%s）", sk_textbox_text(tb));
    sk_ui_free(ui);
}

static void test_focus(sk_window *win)
{
    printf("[Tab 焦点顺序]\n");
    uint32_t id = sk_window_id(win);
    sk_ui *ui = sk_ui_new(win);
    sk_widget *t1 = sk_textbox_new(ui, sk_rect_make(10, 10, 100, 30), "", NULL, NULL);
    sk_widget *bd = sk_button_new(ui, sk_rect_make(10, 50, 100, 30), "禁用", NULL, NULL);
    sk_widget *b1 = sk_button_new(ui, sk_rect_make(10, 90, 100, 30), "按钮", NULL, NULL);
    sk_widget *hid = sk_checkbox_new(ui, sk_rect_make(10, 130, 100, 30), "隐藏", 0, NULL, NULL);
    sk_widget *c1 = sk_checkbox_new(ui, sk_rect_make(10, 170, 100, 30), "复选", 0, NULL, NULL);
    sk_label_new(ui, sk_rect_make(10, 210, 100, 30), "标签不能获得焦点", SK_FONT_UI, SK_ALIGN_LEFT);
    sk_widget_set_enabled(bd, 0);
    sk_widget_set_visible(hid, 0);
    pump(20);
    sk_widget *order[4];
    for (int i = 0; i < 4; i++) {
        key(id, KEY_TAB, 0, '\t');
        pump(15);
        order[i] = sk_ui_focused(ui);
    }
    CHECK(order[0] == t1 && order[1] == b1 && order[2] == c1 && order[3] == t1,
          "Tab：输入框 → 按钮 → 复选框 → 回到输入框（跳过禁用/隐藏/标签）");
    CHECK(sk_ui_focus_visible(ui), "用 Tab 移动焦点后显示焦点框");
    key(id, KEY_TAB, SK_MOD_SHIFT, '\t');
    pump(15);
    CHECK(sk_ui_focused(ui) == c1, "Shift+Tab 反向");
    key(id, KEY_SPACE, 0, ' ');
    pump(15);
    CHECK(sk_widget_checked(c1), "空格切换焦点复选框");
    mouse(id, SK_MOUSE_DOWN, 300, 200, SK_BUTTON_LEFT, SK_BUTTON_LEFT);
    mouse(id, SK_MOUSE_UP, 300, 200, 0, SK_BUTTON_LEFT);
    pump(15);
    CHECK(!sk_ui_focus_visible(ui), "鼠标点击后隐藏焦点框");
    sk_widget_set_visible(c1, 0);
    CHECK(sk_ui_focused(ui) == NULL, "隐藏焦点控件 → 失去焦点");
    sk_ui_free(ui);
}

static sk_widget *victim, *replacement;
static sk_ui *dui;
static void destroy_me(sk_widget *w, void *u)
{
    (void)u;
    sk_widget_destroy(w);
    replacement = sk_button_new(dui, sk_rect_make(20, 20, 100, 36), "新的", NULL, NULL);
}

static int layouts, last_w, last_h;
static void on_layout(sk_ui *ui, int w, int h, void *u) { (void)ui; (void)u; layouts++; last_w = w; last_h = h; }

static void test_destroy_resize(sk_window *win)
{
    printf("[回调里销毁 / 改变大小]\n");
    uint32_t id = sk_window_id(win);
    dui = sk_ui_new(win);
    victim = sk_button_new(dui, sk_rect_make(20, 20, 100, 36), "自毁", destroy_me, NULL);
    sk_custom_new(dui, sk_rect_make(0, 0, 10, 10), noop_draw, NULL, 0, NULL);
    pump(20);
    click(id, 50, 30);
    pump(20);
    CHECK(replacement && sk_ui_widget_at(dui, 50, 30) == replacement, "按钮在回调里销毁自己并创建新按钮");
    CHECK(!sk_widget_alive(victim) || victim != replacement, "旧按钮已经死亡");

    sk_ui_set_layout(dui, on_layout, NULL);
    CHECK(layouts == 1, "set_layout 立即调用一次");
    pump(20);                                    /* set_layout 之后的整窗重画先完成 */
    int m = mark();
    sk_ev_configure cf = { { SK_EV_CONFIGURE, sizeof cf }, id, 333, 222 };
    srv_send(&cf, sizeof cf);
    pump(30);
    rec a[8];
    int n = collect(m, SK_REQ_ATTACH_BUFFER, a, 8);
    CHECK(n == 1 && a[0].r.w == 333 && a[0].r.h == 222 && a[0].value == 1, "CONFIGURE → ATTACH_BUFFER 带新缓冲区");
    CHECK(layouts == 2 && last_w == 333 && last_h == 222, "改变大小时调用 layout（%dx%d）", last_w, last_h);
    sk_canvas *c = sk_window_canvas(win);
    CHECK(c->w == 333 && c->h == 222 && c->stride == 333, "同一个画布对象换成了新尺寸");
    rec d[16];
    n = collect(m, SK_REQ_DAMAGE, d, 16);
    CHECK(n >= 1 && d[0].r.w == 333 && d[0].r.h == 222, "改变大小后整窗 damage");
    sk_ui_free(dui);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(sock_path, sizeof sock_path, "/tmp/sk-uicore-%d.sock", (int)getpid());
    unlink(sock_path);
    listen_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, sock_path);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(listen_fd, 1) < 0) {
        perror("fake server");
        return 1;
    }
    pthread_t th;
    pthread_create(&th, NULL, server_main, NULL);
    setenv(SK_SOCKET_ENV, sock_path, 1);

    test_text_helpers();
    app = sk_app_connect("test_ui_core");
    CHECK(app != NULL, "连接假服务器");
    if (!app)
        return 1;
    CHECK(sk_app_screen_w(app) == 800 && sk_app_screen_h(app) == 600, "WELCOME 里的屏幕尺寸");
    sk_window *win = sk_window_create(app, "单元测试", 300, 200, 0, 0);
    CHECK(win != NULL, "同步创建窗口");
    rec cw[2];
    CHECK(collect(0, SK_REQ_CREATE_WINDOW, cw, 2) == 1 && cw[0].value == 1, "CREATE_WINDOW 附带了 memfd");

    test_dirty(win);
    test_dirty_cap();
    test_hover_click(win);
    test_focus(win);
    test_destroy_resize(win);

    sk_app_disconnect(app);
    pthread_join(th, NULL);
    unlink(sock_path);
    printf("\n%d 项检查，%d 项失败\n", checks, failures);
    return failures ? 1 : 0;
}
