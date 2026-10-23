/*
 * ui_test_app.c — libsketch 客户端库 + 控件核心的测试程序（配合 tests/ui/test_ui.py 使用）
 *
 * 在 WSL 里直接运行（32 位静态程序），连接 tools/mock_display.py 模拟的显示服务器：
 *   ui_test_app gallery   所有标准控件在各种状态下的样子（截图人工检查）
 *   ui_test_app form      一个可交互的表单；把回调写到 stdout，Python 脚本据此断言
 *   ui_test_app loop      事件循环：定时器精度、fd 回调、空闲回调、嵌套循环、quit 逐层退出
 *   ui_test_app misuse    各种误用（NULL 参数、在回调里销毁自己……）不能崩溃
 * stdout 每行一个事件，例如 "CLICK greet text=小明"。
 */
#define _GNU_SOURCE
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sk_app *app;

#define OUT(...) do { printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

/* ================================================================ gallery */

static void label_at(sk_ui *ui, int x, int y, int w, const char *t, sk_color color)
{
    sk_widget *l = sk_label_new(ui, sk_rect_make(x, y, w, 24), t, SK_FONT_UI, SK_ALIGN_LEFT);
    sk_label_set_color(l, color);
}

static void draw_demo(sk_widget *w, sk_canvas *c, void *user)
{
    (void)user;
    sk_rough_style s;
    sk_style_hatched(&s, SK_TEAL, w->seed);
    sk_rough_ellipse(c, w->r.x + w->r.w / 2.0f, w->r.y + w->r.h / 2.0f, w->r.w - 10, w->r.h - 10, &s);
    sk_draw_text_in_rect(c, sk_font_get(SK_FONT_UI), w->r, "自定义", SK_INK, SK_ALIGN_CENTER);
}

static int run_gallery(void)
{
    sk_window *win = sk_window_create(app, "控件陈列室", 940, 700, SK_WIN_CENTER, SK_PURPLE);
    if (!win)
        return 1;
    sk_ui *ui = sk_ui_new(win);
    static const char *states[] = { "普通", "悬停", "按下", "焦点", "禁用", "选中" };
    static const unsigned bits[] = { 0, SK_STATE_HOVER, SK_STATE_HOVER | SK_STATE_PRESSED, SK_STATE_FOCUSED,
                                     SK_STATE_DISABLED, SK_STATE_CHECKED };
    int col0 = 110, colw = 135;
    sk_label_new(ui, sk_rect_make(16, 10, 400, 28), "控件陈列室：每一列是一种状态", SK_FONT_TITLE, SK_ALIGN_LEFT);
    for (int s = 0; s < 6; s++)
        label_at(ui, col0 + s * colw + 10, 44, colw - 10, states[s], SK_INK_SOFT);

    const char *rows[] = { "按钮", "图标按钮", "复选框", "输入框", "色块", "滑块", "占用率" };
    for (int r = 0; r < 7; r++)
        label_at(ui, 16, 78 + r * 56 + 6, 90, rows[r], SK_INK);

    for (int s = 0; s < 6; s++) {
        int x = col0 + s * colw;
        sk_widget *w;
        w = sk_button_new(ui, sk_rect_make(x, 78, colw - 18, 36), "确定", NULL, NULL);
        w->state = bits[s];
        w = sk_icon_button_new(ui, sk_rect_make(x, 78 + 56, colw - 18, 36), SK_ICON_SAVE, "保存", NULL, NULL);
        w->state = bits[s];
        w = sk_checkbox_new(ui, sk_rect_make(x, 78 + 112, colw - 18, 30), "记住我", s == 5, NULL, NULL);
        w->state |= bits[s];
        w = sk_textbox_new(ui, sk_rect_make(x, 78 + 168, colw - 18, 34), s == 0 ? "" : "你好 hello", NULL, NULL);
        sk_textbox_set_placeholder(w, "请输入…");
        w->state = bits[s];
        w = sk_swatch_new(ui, sk_rect_make(x + 30, 78 + 224, 40, 40), s % 2 ? SK_RED : SK_GREEN, NULL, NULL);
        w->state = bits[s];
        w = sk_slider_new(ui, sk_rect_make(x, 78 + 280, colw - 18, 30), 0, 100, 20 + s * 15, NULL, NULL);
        w->state = bits[s];
        w = sk_meter_new(ui, sk_rect_make(x, 78 + 340, colw - 18, 24), s % 2 ? SK_ORANGE : SK_BLUE);
        sk_meter_set(w, 150 + s * 160, s == 3 ? "63%" : NULL);
        w->state = bits[s] & SK_STATE_DISABLED;
    }
    static const char *tabs[] = { "常规", "高级", "关于本机" };
    sk_widget *t = sk_tabs_new(ui, sk_rect_make(16, 486, 420, 34), tabs, 3, NULL, NULL);
    sk_tabs_set_active(t, 1);
    sk_paragraph_new(ui, sk_rect_make(16, 532, 420, 120),
                     "段落控件会自动换行：SketchOS 的界面全部是手绘风格，线条像铅笔、颜色像蜡笔，"
                     "背景是一张带颗粒的纸。English words wrap by word.", SK_FONT_UI);
    sk_custom_new(ui, sk_rect_make(470, 486, 160, 100), draw_demo, NULL, 0, NULL);
    sk_widget *icons = sk_icon_button_new(ui, sk_rect_make(660, 486, 44, 40), SK_ICON_FOLDER, NULL, NULL, NULL);
    (void)icons;
    sk_widget *ib2 = sk_icon_button_new(ui, sk_rect_make(710, 486, 44, 40), SK_ICON_TRASH, NULL, NULL, NULL);
    ib2->state = SK_STATE_HOVER;
    sk_label_new(ui, sk_rect_make(470, 600, 440, 24), "右下角：截断很长很长很长很长很长很长很长很长的标签文字", SK_FONT_UI,
                 SK_ALIGN_LEFT);
    /* 弹出菜单也放进陈列室 */
    static const char *items[] = { "新建", "打开…", "-", "另存为…", NULL, "退出" };
    sk_menu_popup(ui, 680, 540, items, 6, NULL, NULL);
    return sk_app_run(app);
}

/* ================================================================ form */

typedef struct {
    sk_ui *ui;
    sk_widget *name, *greet, *hello, *check, *slider, *tabs, *meter, *swatch[4], *save, *area, *status, *off;
} form_t;

static form_t F;

static void on_greet(sk_widget *w, void *u)
{
    (void)u;
    const char *name = sk_textbox_text(F.name);
    char buf[256];
    snprintf(buf, sizeof buf, "你好，%s！", *name ? name : "陌生人");
    sk_widget_set_text(F.hello, buf);
    OUT("CLICK %s text=%s", w == F.greet ? "greet" : "enter", name);
}

static void on_name_change(sk_widget *w, void *u)
{
    (void)u;
    OUT("CHANGE name=%s", sk_textbox_text(w));
}

static void on_check(sk_widget *w, void *u) { (void)u; OUT("CHANGE check=%d", sk_widget_checked(w)); }

static void on_slider(sk_widget *w, void *u)
{
    (void)u;
    char cap[32];
    snprintf(cap, sizeof cap, "%d%%", sk_slider_value(w));
    sk_meter_set(F.meter, sk_slider_value(w) * 10, cap);
    OUT("CHANGE slider=%d", sk_slider_value(w));
}

static void on_tabs(sk_widget *w, void *u) { (void)u; OUT("CHANGE tabs=%d", sk_tabs_active(w)); }

static void on_swatch(sk_widget *w, void *u)
{
    (void)u;
    for (int i = 0; i < 4; i++)
        sk_widget_set_checked(F.swatch[i], F.swatch[i] == w);
    OUT("CLICK swatch=%06x", sk_swatch_color(w) & 0xFFFFFF);
}

static void on_save(sk_widget *w, void *u) { (void)w; (void)u; OUT("CLICK save"); }

static void on_menu(sk_ui *ui, int index, void *user)
{
    (void)ui; (void)user;
    OUT("MENU %d", index);
}

static int area_event(sk_widget *w, const sk_event *e, void *user)
{
    (void)user;
    if (e->type == SK_EVENT_MOUSE && e->mouse.kind == SK_MOUSE_DOWN && e->mouse.button == SK_BUTTON_RIGHT) {
        static const char *items[] = { "剪切", "复制", "-", "粘贴", "全选" };
        sk_menu_popup(w->ui, e->mouse.x, e->mouse.y, items, 5, on_menu, NULL);
        return 1;
    }
    if (e->type == SK_EVENT_MOUSE && e->mouse.kind == SK_MOUSE_ENTER) {
        sk_window_set_cursor(sk_ui_window(w->ui), SK_CURSOR_PENCIL);   /* 应用自己控制光标 */
        return 1;
    }
    return 0;
}

static void area_draw(sk_widget *w, sk_canvas *c, void *user)
{
    (void)user;
    sk_rough_style s;
    sk_style_pencil(&s, w->seed);
    s.stroke = SK_INK_FAINT;
    sk_rough_rect(c, w->r.x + 2, w->r.y + 2, w->r.w - 4, w->r.h - 4, &s);
    sk_draw_text_in_rect(c, sk_font_get(SK_FONT_UI), w->r, "右键点这里弹出菜单", SK_INK_SOFT, SK_ALIGN_CENTER);
}

static void form_layout(sk_ui *ui, int w, int h, void *user)
{
    (void)ui; (void)user;
    /* 右边贴边的控件跟着窗口宽度走；自定义区域铺满剩下的空间 */
    sk_widget_set_rect(F.greet, sk_rect_make(w - 120, 20, 100, 36));
    sk_widget_set_rect(F.name, sk_rect_make(90, 21, w - 230, 34));
    sk_widget_set_rect(F.area, sk_rect_make(20, 300, w - 40, h - 340));
    sk_widget_set_rect(F.status, sk_rect_make(20, h - 32, w - 40, 24));
    OUT("LAYOUT %d %d", w, h);
}

static int form_handler(sk_window *w, const sk_event *e, void *user)
{
    (void)user;
    switch (e->type) {
    case SK_EVENT_KEY:
        if (e->key.state == 1)
            OUT("APPKEY %u mods=%u", e->key.keycode, e->key.modifiers);
        return 1;
    case SK_EVENT_CLOSE:
        OUT("CLOSE");
        return 0;                       /* 默认：销毁窗口 → sk_app_run 返回 0 */
    case SK_EVENT_FOCUS:
        OUT("FOCUS %d", e->focused);
        return 1;
    case SK_EVENT_RESIZE:
        OUT("RESIZE %d %d canvas=%dx%d", e->size.w, e->size.h, sk_window_canvas(w)->w, sk_window_canvas(w)->h);
        return 1;
    default:
        return 0;
    }
}

static int run_form(void)
{
    sk_window *win = sk_window_create(app, "表单测试", 560, 460, SK_WIN_RESIZABLE | SK_WIN_CENTER, SK_GREEN);
    if (!win)
        return 1;
    sk_window_set_min_size(win, 400, 380);
    sk_ui *ui = sk_ui_new(win);
    F.ui = ui;
    sk_label_new(ui, sk_rect_make(20, 26, 70, 24), "名字：", SK_FONT_UI, SK_ALIGN_LEFT);
    F.name = sk_textbox_new(ui, sk_rect_make(90, 21, 330, 34), "", on_name_change, NULL);
    sk_textbox_set_placeholder(F.name, "在这里输入名字");
    F.name->on_click = on_greet;              /* 回车也问好 */
    F.greet = sk_button_new(ui, sk_rect_make(440, 20, 100, 36), "问好", on_greet, NULL);
    F.hello = sk_label_new(ui, sk_rect_make(20, 66, 400, 26), "", SK_FONT_TITLE, SK_ALIGN_LEFT);
    sk_label_set_color(F.hello, SK_BLUE);
    F.check = sk_checkbox_new(ui, sk_rect_make(20, 104, 140, 30), "记住我", 0, on_check, NULL);
    F.slider = sk_slider_new(ui, sk_rect_make(180, 104, 200, 30), 0, 100, 30, on_slider, NULL);
    F.meter = sk_meter_new(ui, sk_rect_make(400, 108, 140, 22), SK_ORANGE);
    sk_meter_set(F.meter, 300, "30%");
    static const char *tabs[] = { "常规", "高级", "关于" };
    F.tabs = sk_tabs_new(ui, sk_rect_make(20, 146, 300, 34), tabs, 3, on_tabs, NULL);
    sk_color colors[4] = { SK_RED, SK_GREEN, SK_BLUE, SK_YELLOW };
    for (int i = 0; i < 4; i++)
        F.swatch[i] = sk_swatch_new(ui, sk_rect_make(20 + i * 44, 196, 40, 40), colors[i], on_swatch, NULL);
    F.save = sk_icon_button_new(ui, sk_rect_make(210, 196, 90, 40), SK_ICON_SAVE, "保存", on_save, NULL);
    F.off = sk_button_new(ui, sk_rect_make(310, 198, 100, 36), "禁用的", on_save, NULL);
    sk_widget_set_enabled(F.off, 0);
    sk_paragraph_new(ui, sk_rect_make(20, 244, 520, 50), "Tab 键可以在控件之间切换焦点；按回车也会问好。", SK_FONT_UI);
    F.area = sk_custom_new(ui, sk_rect_make(20, 300, 520, 120), area_draw, area_event, 0, NULL);
    F.status = sk_label_new(ui, sk_rect_make(20, 428, 520, 24), "状态栏", SK_FONT_UI, SK_ALIGN_RIGHT);
    sk_label_set_color(F.status, SK_INK_SOFT);
    sk_ui_set_handler(ui, form_handler, NULL);
    sk_ui_set_layout(ui, form_layout, NULL);
    OUT("READY");
    int rc = sk_app_run(app);
    OUT("EXIT %d", rc);
    return rc;
}

/* ================================================================ loop */

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); OUT(__VA_ARGS__); } } while (0)

static uint64_t t0;
static int ticks, max_dev, tick_timer;

static void tick(sk_app *a, void *u)
{
    (void)u;
    /* 不累积误差 = 每次触发都落在 t0 + k*40 的“网格”上（误差只来自调度延迟，不会越积越大） */
    uint64_t now = sk_now_ms();
    int expect = 40 * (ticks + 1);
    int dev = (int)(now - t0) - expect;
    if (dev < 0)
        dev = -dev;
    if (dev > max_dev)
        max_dev = dev;
    if (++ticks == 25)
        sk_app_remove_timer(a, tick_timer);        /* 在自己的回调里注销自己 */
}

static int pipefd[2], fd_calls, idle_calls, idle_id, oneshot_calls, victim_calls, victim_id;

static void on_fd(sk_app *a, int fd, short rev, void *u)
{
    (void)u; (void)rev;
    char buf[16];
    ssize_t n = read(fd, buf, sizeof buf);
    fd_calls++;
    CHECK(n == 3, "fd read %d", (int)n);
    sk_app_remove_fd(a, fd);                       /* 删除自己 */
}

static void write_pipe(sk_app *a, void *u) { (void)a; (void)u; if (write(pipefd[1], "abc", 3) != 3) fails++; }

static void on_idle(sk_app *a, void *u)
{
    (void)u;
    if (++idle_calls == 5)
        sk_app_remove_idle(a, idle_id);
}

static void victim(sk_app *a, void *u) { (void)a; (void)u; victim_calls++; }

static void oneshot(sk_app *a, void *u)
{
    (void)u;
    oneshot_calls++;
    sk_app_remove_timer(a, victim_id);             /* 删除另一个还没触发的定时器 */
}

static volatile int nested_flag;
static void set_flag(sk_app *a, void *u) { (void)a; (void)u; nested_flag = 1; }
static int nested_ok, nested_depth_ticks;
static void nested_tick(sk_app *a, void *u) { (void)a; (void)u; nested_depth_ticks++; }

static void start_nested(sk_app *a, void *u)
{
    (void)u;
    uint64_t s = sk_now_ms();
    int tid = sk_app_add_timer(a, 10, 1, nested_tick, NULL);
    sk_app_add_timer(a, 150, 0, set_flag, NULL);
    int r = sk_app_run_until(a, &nested_flag);
    sk_app_remove_timer(a, tid);
    int el = (int)(sk_now_ms() - s);
    CHECK(r == 0, "run_until returned %d", r);
    CHECK(el >= 145 && el < 400, "nested elapsed %d", el);
    CHECK(nested_depth_ticks >= 8, "timers in nested loop: %d", nested_depth_ticks);
    nested_ok = 1;
    OUT("NESTED r=%d elapsed=%d inner_ticks=%d", r, el, nested_depth_ticks);
}

/* quit 在两层嵌套里调用：两层都要退出，sk_app_run 返回 quit 的代码 */
static volatile int never;
static int inner_ret = 99;
static void do_quit(sk_app *a, void *u) { (void)u; sk_app_quit(a, 7); }
static void quit_test(sk_app *a, void *u)
{
    (void)u;
    sk_app_add_timer(a, 30, 0, do_quit, NULL);
    inner_ret = sk_app_run_until(a, &never);
}

static void finish_phase1(sk_app *a, void *u)
{
    (void)u;
    OUT("TIMER ticks=%d max_dev=%d", ticks, max_dev);
    CHECK(ticks == 25, "ticks %d", ticks);
    CHECK(max_dev <= 15, "timer drift %d ms", max_dev);
    CHECK(fd_calls == 1, "fd calls %d", fd_calls);
    CHECK(idle_calls == 5, "idle calls %d", idle_calls);
    CHECK(oneshot_calls == 1 && victim_calls == 0, "oneshot %d victim %d", oneshot_calls, victim_calls);
    CHECK(nested_ok, "nested not run");
    sk_app_add_timer(a, 10, 0, quit_test, NULL);
}

/* 跳过错过的周期：回调里故意卡 130ms，周期 40ms → 不应该补发一串 */
static int slow_count;
static uint64_t slow_last[8];
static int slow_id;
static void slow_tick(sk_app *a, void *u)
{
    (void)u;
    slow_last[slow_count] = sk_now_ms();
    if (slow_count == 0)
        usleep(130 * 1000);
    if (++slow_count == 4)
        sk_app_remove_timer(a, slow_id);
}

static void start_slow(sk_app *a, void *u)
{
    (void)u;
    slow_id = sk_app_add_timer(a, 40, 1, slow_tick, NULL);
}

static int run_loop(void)
{
    sk_window *win = sk_window_create(app, "循环测试", 200, 100, 0, 0);
    if (!win)
        return 1;
    if (pipe(pipefd) < 0)
        return 1;
    t0 = sk_now_ms();
    tick_timer = sk_app_add_timer(app, 40, 1, tick, NULL);
    sk_app_add_fd(app, pipefd[0], POLLIN, on_fd, NULL);
    sk_app_add_timer(app, 60, 0, write_pipe, NULL);
    sk_app_add_timer(app, 400, 0, write_pipe, NULL);    /* 第二次写：fd 已删除，不应再回调 */
    idle_id = sk_app_add_idle(app, on_idle, NULL);
    victim_id = sk_app_add_timer(app, 300, 0, victim, NULL);
    sk_app_add_timer(app, 100, 0, oneshot, NULL);
    sk_app_add_timer(app, 200, 0, start_nested, NULL);
    sk_app_add_timer(app, 1500, 0, finish_phase1, NULL);
    sk_app_add_timer(app, 1060, 0, start_slow, NULL);   /* 等 40ms 节拍测试结束后再测“卡顿” */
    /* 无效参数 */
    CHECK(sk_app_add_timer(app, 10, 0, NULL, NULL) == 0, "null timer accepted");
    sk_app_remove_timer(app, 12345);
    sk_app_remove_fd(app, 999);
    sk_app_remove_idle(app, -1);

    int rc = sk_app_run(app);
    OUT("RUN rc=%d inner=%d", rc, inner_ret);
    CHECK(rc == 7, "sk_app_run returned %d (want 7)", rc);
    CHECK(inner_ret == -1, "inner run_until returned %d (want -1)", inner_ret);
    if (slow_count >= 2) {
        int gap = (int)(slow_last[1] - slow_last[0]);
        OUT("SLOW gap=%d", gap);
        CHECK(gap >= 120, "missed ticks were burst-fired (gap %d)", gap);
        if (slow_count >= 3) {
            int gap2 = (int)(slow_last[2] - slow_last[1]);
            CHECK(gap2 >= 20, "second gap %d", gap2);
        }
    }
    /* 第二次 sk_app_run：窗口还在，quit 标志已清除，再用 quit 退出 */
    sk_app_add_timer(app, 20, 0, do_quit, NULL);
    rc = sk_app_run(app);
    CHECK(rc == 7, "second run rc=%d", rc);
    /* 销毁最后一个窗口 → sk_app_run 返回 0 */
    sk_window_destroy(win);
    rc = sk_app_run(app);
    CHECK(rc == 0, "run with no windows rc=%d", rc);
    OUT(fails ? "LOOP FAIL %d" : "LOOP PASS", fails);
    return fails ? 1 : 0;
}

/* ================================================================ misuse */

static sk_ui *mui;
static sk_widget *self_destroy_btn, *menu_btn;
static sk_window *mwin2;
static sk_ui *mui2;

static void destroy_self(sk_widget *w, void *u)
{
    (void)u;
    sk_widget_destroy(w);
    sk_widget_destroy(w);                         /* 第二次：空操作 */
    sk_widget_set_text(w, "dead");                /* 对已死控件的操作：空操作 */
    sk_widget_invalidate(w);
    OUT("SELFDESTROY ok");
}

static void free_ui_in_cb(sk_widget *w, void *u)
{
    (void)w; (void)u;
    sk_ui_free(mui2);                            /* 在自己 sk_ui 的回调里释放它 */
    sk_window_destroy(mwin2);                    /* 再销毁窗口 */
    OUT("UIFREE ok");
}

static void pick_destroy(sk_ui *ui, int idx, void *u)
{
    (void)ui; (void)u;
    OUT("MENU %d", idx);
    sk_widget_destroy(menu_btn);
}

static void open_menu(sk_widget *w, void *u)
{
    (void)u;
    static const char *items[] = { "一", "二" };
    sk_menu_popup(w->ui, w->r.x, w->r.y + w->r.h, items, 2, pick_destroy, NULL);
    sk_menu_popup(w->ui, w->r.x, w->r.y + w->r.h, items, 2, pick_destroy, NULL);   /* 重复弹出：替换旧的 */
}

static int misuse_handler(sk_window *w, const sk_event *e, void *user)
{
    (void)user;
    if (e->type == SK_EVENT_CLOSE) {
        OUT("CLOSE");
        sk_window_destroy(w);                     /* 在自己的事件回调里销毁窗口 */
        return 1;
    }
    return 0;
}

static int run_misuse(void)
{
    /* NULL 参数全部安全 */
    sk_window_destroy(NULL);
    sk_window_damage(NULL, sk_rect_make(0, 0, 1, 1));
    sk_window_flush(NULL);
    sk_window_set_title(NULL, "x");
    sk_window_set_cursor(NULL, 1);
    sk_clipboard_set(NULL, "x", 1);
    sk_clipboard_request(app, NULL);
    sk_app_launch(app, NULL, NULL);
    sk_ui_invalidate(NULL, sk_rect_make(0, 0, 1, 1));
    sk_ui_focus(NULL, NULL);
    sk_widget_destroy(NULL);
    sk_widget_set_text(NULL, "x");
    sk_widget_set_visible(NULL, 0);
    sk_textbox_set_text(NULL, "x");
    sk_meter_set(NULL, 1, NULL);
    sk_tabs_set_active(NULL, 1);
    if (sk_ui_new(NULL) != NULL)
        OUT("FAIL ui_new(NULL)");
    sk_window *big = sk_window_create(app, "big", 100000, -5, 0, 0);   /* 尺寸会被夹到合法范围 */
    if (!big || sk_window_width(big) != SK_MAX_WIN_SIZE || sk_window_height(big) != 1)
        OUT("FAIL clamp");
    sk_window_destroy(big);

    sk_window *win = sk_window_create(app, "误用测试", 400, 300, 0, 0);
    mui = sk_ui_new(win);
    sk_ui_set_handler(mui, misuse_handler, NULL);
    self_destroy_btn = sk_button_new(mui, sk_rect_make(20, 20, 120, 36), "自毁", destroy_self, NULL);
    menu_btn = sk_button_new(mui, sk_rect_make(160, 20, 120, 36), "菜单", open_menu, NULL);
    sk_widget *tb = sk_textbox_new(mui, sk_rect_make(20, 80, 200, 34), "abc", NULL, NULL);
    sk_widget_set_text(tb, "通过 set_text 设置");   /* 走 textbox 的钩子 */
    if (strcmp(sk_textbox_text(tb), "通过 set_text 设置") != 0)
        OUT("FAIL textbox hook");
    sk_textbox_text(self_destroy_btn);              /* 类型不对：返回 "" */
    sk_slider_value(tb);

    /* 第二个窗口：在按钮回调里 sk_ui_free + 销毁窗口 */
    mwin2 = sk_window_create(app, "第二个窗口", 300, 200, 0, 0);
    mui2 = sk_ui_new(mwin2);
    sk_button_new(mui2, sk_rect_make(20, 20, 160, 36), "释放 UI", free_ui_in_cb, NULL);

    /* 第三个窗口：先销毁窗口，再操作它的 ui 和控件 */
    sk_window *w3 = sk_window_create(app, "第三个窗口", 200, 100, 0, 0);
    sk_ui *ui3 = sk_ui_new(w3);
    sk_widget *b3 = sk_button_new(ui3, sk_rect_make(10, 10, 80, 30), "x", NULL, NULL);
    sk_window_destroy(w3);
    sk_widget_set_text(b3, "after window gone");
    sk_ui_invalidate_all(ui3);
    sk_ui_redraw_now(ui3);
    sk_ui_focus(ui3, b3);
    sk_ui_free(ui3);

    OUT("READY");
    int rc = sk_app_run(app);
    OUT("EXIT %d", rc);
    return rc;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "gallery";
    app = sk_app_connect("ui_test_app");
    if (!app)
        return 2;
    OUT("SCREEN %dx%d", sk_app_screen_w(app), sk_app_screen_h(app));
    int rc;
    if (!strcmp(mode, "gallery"))
        rc = run_gallery();
    else if (!strcmp(mode, "form"))
        rc = run_form();
    else if (!strcmp(mode, "loop"))
        rc = run_loop();
    else if (!strcmp(mode, "misuse"))
        rc = run_misuse();
    else {
        fprintf(stderr, "未知模式 %s\n", mode);
        rc = 2;
    }
    sk_app_disconnect(app);
    return rc;
}
