/*
 * ui.c — 保留模式控件库的核心（sketch/ui.h）
 *
 * “保留模式”（retained mode）的意思是：应用不自己画界面，而是告诉库“这里有一个按钮、
 * 那里有一个输入框”，库记住（保留）这些控件，并负责在需要的时候把它们画出来。
 * 与之相对的是“立即模式”：应用每一帧自己从头画一遍。
 *
 * 本文件实现三件事：
 *
 * 1. 局部重绘（性能的关键，网页版的 CPU 比真机慢几十倍）
 *    控件状态一变，只把它的矩形记为“脏”（sk_ui_invalidate）。脏矩形攒在一个小列表里，
 *    互相靠近的会合并，太多了就合成一个包围盒。每轮事件循环结束前（空闲回调）统一重画：
 *        对每块脏矩形：裁剪区 = 这块矩形
 *                      画背景（默认是带颗粒的纸）
 *                      按从下到上的顺序，画所有与它相交的可见控件
 *                            （裁剪区 = 脏矩形 ∩ 控件矩形外扩 SK_WIDGET_BLEED）
 *                      把这块矩形 damage 给服务器（服务器也只合成这一块）
 *    鼠标在按钮上移动时，只有“悬停的控件变了”才会重画，而且只重画那两个控件。
 *
 * 2. 事件路由
 *    鼠标 → 鼠标下最上层的可交互控件；按下后该控件“捕获”鼠标直到所有按钮松开
 *    （所以拖动滑块时鼠标移出滑块也能继续拖）。键盘 → 焦点控件；Tab/Shift+Tab 切换焦点。
 *    控件不要的事件交给应用回调。
 *
 * 3. 公共服务：光标形状、焦点框、输入光标闪烁、控件定时器、剪贴板路由、弹出菜单的“抓取”。
 *
 * 安全性：任何回调里都可以销毁任何控件（甚至 sk_ui 本身），销毁会被推迟到
 * 没有 sk_ui 代码在调用栈上时（depth 归零）才真正释放内存。
 */
#include "ui_internal.h"
#include "sketch/rough.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>

#define DIRTY_MAX          8       /* 脏矩形列表上限，超过就合成一个包围盒 */
#define DIRTY_MERGE_SLACK  4096    /* 合并时允许多画的像素数 */
#define PAPER_STRENGTH     3       /* 窗口纸张颗粒强度（很淡） */
#define CARET_BLINK_MS     530     /* 光标闪烁半周期（和大多数系统一样） */
#define CARET_BLINK_LIMIT  20      /* 闪这么多次（约 10 秒）没有输入就停下 */
#define CLIP_REQ_MAX       8
#define FOCUS_RING_GAP     3       /* 焦点框在控件矩形外 3 像素（< SK_WIDGET_BLEED） */

/* 每个控件实际分配的是这个结构：公共部分在最前面，所以 sk_widget* 和 widget_impl* 可以互相转换 */
typedef struct widget_impl {
    sk_widget pub;
    unsigned  flags;
    int       cursor;
    int       dead;
    sk_widget_text_hook text_hook;
} widget_impl;

#define IMPL(w) ((widget_impl *)(w))
#define CIMPL(w) ((const widget_impl *)(w))

/* 控件定时器：把 client.c 的 (app, user) 回调转换成 (widget) 回调 */
typedef struct wtimer {
    struct wtimer *next;
    sk_ui      *ui;
    sk_widget  *w;
    sk_widget_timer_fn fn;
    int         id;            /* client.c 的定时器 id（也是返回给控件的 id） */
    int         repeat;
} wtimer;

struct sk_ui {
    sk_window *win;            /* 窗口销毁后为 NULL，所有绘制变成空操作 */
    sk_app    *app;
    sk_widget *first, *last;   /* 扁平列表：first 在最下层 */

    sk_rect    dirty[DIRTY_MAX];
    int        ndirty;

    sk_paint_fn  bg;     void *bg_user;
    sk_layout_fn layout; void *layout_user;
    sk_event_fn  handler; void *handler_user;

    sk_widget *hover;          /* 鼠标下的控件 */
    sk_widget *pressed;        /* 正在捕获鼠标的控件 */
    sk_widget *focus;          /* 键盘焦点控件 */
    sk_widget *grab;           /* 抓取了所有输入的控件（弹出菜单） */
    int        window_focused;
    int        focus_visible;  /* 用 Tab 移动过焦点 → 显示焦点框 */
    int        mouse_x, mouse_y, mouse_inside;

    int        idle_id;
    uint32_t   seed;
    uint32_t   widget_counter;

    /* 输入光标 */
    sk_widget *caret_owner;
    sk_rect    caret_rect;
    int        caret_on;
    int        caret_timer;
    int        caret_blinks;

    sk_widget *clip_req[CLIP_REQ_MAX];
    int        nclip;

    wtimer    *timers;

    int        depth;          /* sk_ui 代码在调用栈上的层数 */
    int        has_dead;       /* 有等待释放的控件 */
    int        free_pending;   /* 在回调里调用了 sk_ui_free */
};

static void ui_reap(sk_ui *ui);
static void ui_free_now(sk_ui *ui);
static void update_cursor(sk_ui *ui);
static void caret_stop(sk_ui *ui);

/* ------------------------------------------------------------ 小工具 */

static int alive(const sk_widget *w) { return w && !CIMPL(w)->dead; }
static int enabled(const sk_widget *w) { return !(w->state & SK_STATE_DISABLED); }
static int interactive(const sk_widget *w) { return w->cls->event != NULL; }

static sk_rect bleed_rect(const sk_widget *w) { return sk_rect_inset(w->r, -SK_WIDGET_BLEED, -SK_WIDGET_BLEED); }

static long long area(sk_rect r) { return (r.w > 0 && r.h > 0) ? (long long)r.w * r.h : 0; }

static sk_rect bbox(sk_rect a, sk_rect b)
{
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return sk_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static int inside(sk_rect outer, sk_rect in)
{
    return in.x >= outer.x && in.y >= outer.y &&
           in.x + in.w <= outer.x + outer.w && in.y + in.h <= outer.y + outer.h;
}

static int rect_eq(sk_rect a, sk_rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

/* 向控件投递事件 */
static int send(sk_widget *w, const sk_event *e)
{
    if (!alive(w) || !w->cls->event)
        return 0;
    return w->cls->event(w, e);
}

static int call_handler(sk_ui *ui, const sk_event *e)
{
    if (!ui->handler)
        return 0;
    return ui->handler(ui->win ? ui->win : e->win, e, ui->handler_user);
}

/* 合成一个简单的事件（ENTER/LEAVE/FOCUS）发给控件 */
static void send_mouse_kind(sk_ui *ui, sk_widget *w, uint32_t kind)
{
    sk_event e;
    memset(&e, 0, sizeof e);
    e.type = SK_EVENT_MOUSE;
    e.win = ui->win;
    e.mouse.kind = kind;
    e.mouse.x = ui->mouse_x;
    e.mouse.y = ui->mouse_y;
    send(w, &e);
}

static void send_focus(sk_ui *ui, sk_widget *w, int focused)
{
    sk_event e;
    memset(&e, 0, sizeof e);
    e.type = SK_EVENT_FOCUS;
    e.win = ui->win;
    e.focused = focused;
    send(w, &e);
}

/* ------------------------------------------------------------ 脏矩形 */

void sk_ui_invalidate(sk_ui *ui, sk_rect r)
{
    if (!ui || !ui->win)
        return;
    sk_canvas *c = sk_window_canvas(ui->win);
    r = sk_rect_intersect(r, sk_rect_make(0, 0, c->w, c->h));
    if (sk_rect_empty(r))
        return;
    for (;;) {
        int best = -1;
        long long best_cost = 0;
        for (int i = 0; i < ui->ndirty; i++) {
            if (inside(ui->dirty[i], r))
                return;                              /* 已经包含在某块里了 */
            if (inside(r, ui->dirty[i])) {           /* 新的盖住了旧的：删掉旧的 */
                ui->dirty[i--] = ui->dirty[--ui->ndirty];
                continue;
            }
            long long cost = area(bbox(ui->dirty[i], r)) - area(ui->dirty[i]) - area(r);
            if (best < 0 || cost < best_cost) {
                best = i;
                best_cost = cost;
            }
        }
        if (best >= 0 && best_cost <= DIRTY_MERGE_SLACK) {
            r = bbox(ui->dirty[best], r);            /* 相邻/重叠：合并后重新检查 */
            ui->dirty[best] = ui->dirty[--ui->ndirty];
            continue;
        }
        if (ui->ndirty < DIRTY_MAX) {
            ui->dirty[ui->ndirty++] = r;
            return;
        }
        /* 太多零碎的块：干脆合成一个包围盒（多画一点，但省掉很多次遍历控件） */
        for (int i = 0; i < ui->ndirty; i++)
            r = bbox(r, ui->dirty[i]);
        ui->dirty[0] = r;
        ui->ndirty = 1;
        return;
    }
}

void sk_ui_invalidate_all(sk_ui *ui)
{
    if (!ui || !ui->win)
        return;
    sk_canvas *c = sk_window_canvas(ui->win);
    ui->ndirty = 0;
    sk_ui_invalidate(ui, sk_rect_make(0, 0, c->w, c->h));
}

/* 让控件（含出血区域）失效，不管它可见与否（隐藏控件时也要擦掉它） */
static void inval_widget(sk_widget *w)
{
    if (w->ui)
        sk_ui_invalidate(w->ui, bleed_rect(w));
}

void sk_widget_invalidate(sk_widget *w)
{
    if (!alive(w) || !w->visible)
        return;
    inval_widget(w);
}

void sk_widget_invalidate_rect(sk_widget *w, sk_rect r)
{
    if (!alive(w) || !w->visible)
        return;
    r = sk_rect_intersect(r, bleed_rect(w));
    if (!sk_rect_empty(r))
        sk_ui_invalidate(w->ui, r);
}

/* ------------------------------------------------------------ 重绘 */

static int ring_wanted(sk_ui *ui, const sk_widget *w)
{
    return w == ui->focus && (w->state & SK_STATE_FOCUSED) && ui->focus_visible &&
           !(CIMPL(w)->flags & SK_WF_NO_FOCUS_RING);
}

static void ui_redraw(sk_ui *ui)
{
    if (!ui->win || ui->ndirty == 0)
        return;
    sk_canvas *c = sk_window_canvas(ui->win);
    sk_rect todo[DIRTY_MAX];
    int n = ui->ndirty;
    memcpy(todo, ui->dirty, sizeof todo[0] * (size_t)n);
    ui->ndirty = 0;                    /* 画的过程中如果又有失效，留到下一轮 */

    ui->depth++;
    for (int i = 0; i < n && ui->win; i++) {
        sk_rect d = sk_rect_intersect(todo[i], sk_rect_make(0, 0, c->w, c->h));
        if (sk_rect_empty(d))
            continue;
        sk_set_clip(c, d);
        if (ui->bg)
            ui->bg(ui, c, d, ui->bg_user);
        else
            sk_paper_fill(c, d, SK_PAPER, PAPER_STRENGTH, ui->seed);
        for (sk_widget *w = ui->first; w && ui->win; w = w->next) {
            if (!w->visible || IMPL(w)->dead)
                continue;
            sk_rect cl = sk_rect_intersect(d, bleed_rect(w));
            if (sk_rect_empty(cl))
                continue;
            sk_set_clip(c, cl);
            if (w->cls->draw)
                w->cls->draw(w, c);
            if (ring_wanted(ui, w)) {
                sk_set_clip(c, cl);
                sk_draw_focus_ring(c, sk_rect_inset(w->r, -FOCUS_RING_GAP, -FOCUS_RING_GAP), w->seed ^ 0xF0C5u);
            }
        }
        if (ui->win)
            sk_window_damage(ui->win, d);
    }
    if (ui->win)
        sk_reset_clip(c);
    ui->depth--;
}

void sk_ui_redraw_now(sk_ui *ui)
{
    if (!ui || !ui->win)
        return;
    ui_redraw(ui);
    if (ui->win)
        sk_window_flush(ui->win);
    if (ui->depth == 0)
        ui_reap(ui);
}

/* 空闲回调：每轮事件循环 poll 之前执行 */
static void ui_idle(sk_app *app, void *user)
{
    (void)app;
    sk_ui *ui = user;
    ui_redraw(ui);
    if (ui->depth == 0) {
        ui_reap(ui);
        if (ui->free_pending)
            ui_free_now(ui);
    }
}

/* ------------------------------------------------------------ 悬停、捕获、光标 */

/* 鼠标坐标处最上层的可交互可见控件（标签这类不处理事件的控件对鼠标是“透明”的） */
static sk_widget *pick(sk_ui *ui, int x, int y)
{
    sk_widget *hit = NULL;
    for (sk_widget *w = ui->first; w; w = w->next)
        if (w->visible && !IMPL(w)->dead && interactive(w) && sk_rect_contains(w->r, x, y))
            hit = w;
    return hit;
}

/* 此刻鼠标应该“悬停”在哪个控件上（考虑抓取和禁用） */
static sk_widget *hover_target(sk_ui *ui, int x, int y)
{
    if (!ui->mouse_inside)
        return NULL;
    if (ui->grab)
        return sk_rect_contains(ui->grab->r, x, y) ? ui->grab : NULL;
    sk_widget *t = pick(ui, x, y);
    return (t && enabled(t)) ? t : NULL;
}

static void set_hover(sk_ui *ui, sk_widget *w)
{
    if (w == ui->hover)
        return;
    sk_widget *old = ui->hover;
    ui->hover = w;
    if (alive(old)) {
        old->state &= ~SK_STATE_HOVER;
        if (!(IMPL(old)->flags & SK_WF_NO_AUTO_REDRAW))
            sk_widget_invalidate(old);
    }
    if (w) {
        w->state |= SK_STATE_HOVER;
        if (!(IMPL(w)->flags & SK_WF_NO_AUTO_REDRAW))
            sk_widget_invalidate(w);
    }
    /* 先设置光标再发 ENTER：控件可以在 ENTER 里改成自己想要的光标 */
    update_cursor(ui);
    if (alive(old))
        send_mouse_kind(ui, old, SK_MOUSE_LEAVE);
    if (alive(w) && ui->hover == w)
        send_mouse_kind(ui, w, SK_MOUSE_ENTER);
}

static void update_cursor(sk_ui *ui)
{
    if (!ui->win)
        return;
    sk_widget *t = ui->pressed ? ui->pressed : ui->hover;
    int cur = (alive(t) && enabled(t)) ? IMPL(t)->cursor : SK_CURSOR_ARROW;
    /* sk_window_set_cursor 自己会判断“和上次一样就不发报文” */
    sk_window_set_cursor(ui->win, cur);
}

/* 结束捕获 */
static void release_capture(sk_ui *ui)
{
    sk_widget *p = ui->pressed;
    ui->pressed = NULL;
    if (alive(p) && (p->state & SK_STATE_PRESSED)) {
        p->state &= ~SK_STATE_PRESSED;
        if (!(IMPL(p)->flags & SK_WF_NO_AUTO_REDRAW))
            sk_widget_invalidate(p);
    }
}

static void refresh_hover(sk_ui *ui)
{
    if (ui->pressed)
        return;
    set_hover(ui, hover_target(ui, ui->mouse_x, ui->mouse_y));
    update_cursor(ui);
}

static void set_focus_visible(sk_ui *ui, int on)
{
    if (ui->focus_visible == on)
        return;
    ui->focus_visible = on;
    if (alive(ui->focus) && !(IMPL(ui->focus)->flags & SK_WF_NO_FOCUS_RING))
        sk_widget_invalidate(ui->focus);       /* 画上/擦掉焦点框 */
}

static int ui_mouse(sk_ui *ui, const sk_event *e)
{
    int x = e->mouse.x, y = e->mouse.y;
    uint32_t kind = e->mouse.kind;
    ui->mouse_x = x;
    ui->mouse_y = y;

    if (kind == SK_MOUSE_LEAVE) {
        ui->mouse_inside = 0;
        if (!ui->pressed)
            set_hover(ui, NULL);
        if (ui->grab)
            send(ui->grab, e);
        return call_handler(ui, e);
    }
    ui->mouse_inside = 1;
    if (kind == SK_MOUSE_DOWN)
        set_focus_visible(ui, 0);

    /* ---- 抓取：弹出菜单打开时，一切鼠标事件都给它 ---- */
    if (ui->grab) {
        sk_widget *g = ui->grab;
        set_hover(ui, hover_target(ui, x, y));
        send(g, e);
        return 1;
    }

    /* 丢失的松开事件（例如拖动途中窗口失去焦点，服务器没发 UP）：
     * 出现“没有按钮按着的移动”或“只按着这一个按钮的按下”就说明旧的捕获已经失效 */
    if (ui->pressed) {
        if ((kind == SK_MOUSE_MOVE && e->mouse.buttons == 0) ||
            (kind == SK_MOUSE_DOWN && (e->mouse.buttons & ~e->mouse.button) == 0))
            release_capture(ui);
    }

    int r = 0;
    switch (kind) {
    case SK_MOUSE_ENTER:
        /* 鼠标进入窗口：只更新悬停（控件会收到 set_hover 合成的 ENTER），原始事件交给应用 */
        if (!ui->pressed)
            set_hover(ui, hover_target(ui, x, y));
        break;
    case SK_MOUSE_MOVE:
        if (ui->pressed) {
            sk_widget *p = ui->pressed;
            /* 捕获期间：只有按下的那个控件可以“悬停”（按钮按下后移出去会弹起来） */
            set_hover(ui, sk_rect_contains(p->r, x, y) ? p : NULL);
            r = send(p, e);
        } else {
            set_hover(ui, hover_target(ui, x, y));
            if (ui->hover)
                r = send(ui->hover, e);
        }
        break;

    case SK_MOUSE_DOWN: {
        if (ui->pressed) {                  /* 已经按着一个键，又按了另一个：继续给捕获者 */
            r = send(ui->pressed, e);
            break;
        }
        sk_widget *t = pick(ui, x, y);
        if (!t || !enabled(t))
            break;                          /* 点在空白处或禁用的控件上 */
        set_hover(ui, t);
        ui->pressed = t;
        if (e->mouse.button == SK_BUTTON_LEFT) {
            t->state |= SK_STATE_PRESSED;
            if (!(IMPL(t)->flags & SK_WF_NO_AUTO_REDRAW))
                sk_widget_invalidate(t);
        }
        if (t->cls->focusable && !(IMPL(t)->flags & SK_WF_NO_CLICK_FOCUS))
            sk_ui_focus(ui, t);
        update_cursor(ui);
        r = send(t, e);
        break;
    }

    case SK_MOUSE_UP: {
        sk_widget *p = ui->pressed;
        if (!p)
            break;                          /* 在别处按下、在这里松开：不属于任何控件 */
        r = send(p, e);                     /* 投递时 PRESSED 还在，控件据此判断“点击” */
        if (e->mouse.buttons == 0) {
            if (ui->pressed == p)
                release_capture(ui);
            refresh_hover(ui);
        } else if (e->mouse.button == SK_BUTTON_LEFT && alive(p)) {
            p->state &= ~SK_STATE_PRESSED;
            if (!(IMPL(p)->flags & SK_WF_NO_AUTO_REDRAW))
                sk_widget_invalidate(p);
        }
        break;
    }

    case SK_MOUSE_WHEEL: {
        sk_widget *t = ui->pressed ? ui->pressed : pick(ui, x, y);
        if (t && enabled(t))
            r = send(t, e);
        break;
    }
    default:
        break;
    }
    return r ? 1 : call_handler(ui, e);
}

/* ------------------------------------------------------------ 焦点 */

static int can_focus(const sk_widget *w)
{
    return alive(w) && w->cls->focusable && w->visible && enabled(w);
}

void sk_ui_focus(sk_ui *ui, sk_widget *w)
{
    if (!ui)
        return;
    if (w && (w->ui != ui || !can_focus(w)))
        return;
    if (w == ui->focus)
        return;
    sk_widget *old = ui->focus;
    ui->focus = w;
    if (alive(old)) {
        int was = (old->state & SK_STATE_FOCUSED) != 0;
        old->state &= ~SK_STATE_FOCUSED;
        if (ui->caret_owner == old)
            caret_stop(ui);
        if (!(IMPL(old)->flags & SK_WF_NO_AUTO_REDRAW) || ring_wanted(ui, old) || ui->focus_visible)
            sk_widget_invalidate(old);
        if (was)
            send_focus(ui, old, 0);
    }
    if (w && ui->focus == w && ui->window_focused) {
        w->state |= SK_STATE_FOCUSED;
        if (!(IMPL(w)->flags & SK_WF_NO_AUTO_REDRAW) || ring_wanted(ui, w))
            sk_widget_invalidate(w);
        send_focus(ui, w, 1);
    }
}

sk_widget *sk_ui_focused(sk_ui *ui) { return ui && alive(ui->focus) ? ui->focus : NULL; }

/* 窗口焦点变化 → 焦点控件的“有效焦点”跟着变 */
static void update_effective_focus(sk_ui *ui)
{
    sk_widget *f = ui->focus;
    if (!alive(f))
        return;
    int want = ui->window_focused != 0;
    int has = (f->state & SK_STATE_FOCUSED) != 0;
    if (want == has)
        return;
    if (want)
        f->state |= SK_STATE_FOCUSED;
    else {
        f->state &= ~SK_STATE_FOCUSED;
        if (ui->caret_owner == f)
            caret_stop(ui);
    }
    if (!(IMPL(f)->flags & SK_WF_NO_AUTO_REDRAW) ||
        (ui->focus_visible && !(IMPL(f)->flags & SK_WF_NO_FOCUS_RING)))
        sk_widget_invalidate(f);
    send_focus(ui, f, want);
}

/* Tab / Shift+Tab：在可获得焦点的控件之间循环 */
static void focus_step(sk_ui *ui, int dir)
{
    sk_widget *list[256];
    int n = 0, cur = -1;
    for (sk_widget *w = ui->first; w && n < (int)(sizeof list / sizeof list[0]); w = w->next) {
        if (!can_focus(w))
            continue;
        if (w == ui->focus)
            cur = n;
        list[n++] = w;
    }
    if (n == 0)
        return;
    int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
    set_focus_visible(ui, 1);
    sk_ui_focus(ui, list[next]);
    if (alive(list[next]))
        sk_widget_invalidate(list[next]);
}

static int is_tab(const sk_event *e)
{
    return e->key.keycode == KEY_TAB || (e->key.keycode == 0 && e->key.codepoint == '\t');
}

static int ui_key(sk_ui *ui, const sk_event *e)
{
    if (ui->grab) {
        send(ui->grab, e);
        return 1;
    }
    sk_widget *f = ui->focus;
    if (can_focus(f) && send(f, e))
        return 1;
    if (is_tab(e) && !(e->key.modifiers & (SK_MOD_CTRL | SK_MOD_ALT | SK_MOD_SUPER))) {
        if (e->key.state != 0)
            focus_step(ui, (e->key.modifiers & SK_MOD_SHIFT) ? -1 : 1);
        return 1;                          /* 松开 Tab 也吞掉 */
    }
    return call_handler(ui, e);
}

/* ------------------------------------------------------------ 剪贴板 */

static int ui_clipboard(sk_ui *ui, const sk_event *e)
{
    int had = 0;
    sk_widget *req = NULL;
    if (ui->nclip > 0) {
        had = 1;
        req = ui->clip_req[0];
        memmove(ui->clip_req, ui->clip_req + 1, sizeof ui->clip_req[0] * (size_t)(ui->nclip - 1));
        ui->nclip--;
    }
    if (req && send(req, e))
        return 1;
    /* 请求者已经没了：内容不该“粘贴”到别的控件里去，只交给应用 */
    if (!had && can_focus(ui->focus) && send(ui->focus, e))
        return 1;
    return call_handler(ui, e);
}

void sk_widget_request_clipboard(sk_widget *w)
{
    if (!alive(w) || !w->ui->win)
        return;
    sk_ui *ui = w->ui;
    if (ui->nclip >= CLIP_REQ_MAX)
        return;
    ui->clip_req[ui->nclip++] = w;
    sk_clipboard_request(ui->app, ui->win);
}

void sk_widget_set_clipboard(sk_widget *w, const char *text, int len)
{
    if (!alive(w))
        return;
    sk_clipboard_set(w->ui->app, text, len);
}

/* ------------------------------------------------------------ 窗口事件入口 */

static int ui_on_event(sk_window *win, const sk_event *e, void *user)
{
    (void)win;
    sk_ui *ui = user;
    if (ui->free_pending)
        return 0;
    int r = 0;
    ui->depth++;
    switch (e->type) {
    case SK_EVENT_MOUSE:
        r = ui_mouse(ui, e);
        break;
    case SK_EVENT_KEY:
        r = ui_key(ui, e);
        break;
    case SK_EVENT_FOCUS:
        ui->window_focused = e->focused;
        if (!e->focused) {
            release_capture(ui);           /* 失去焦点时可能收不到松开事件了 */
            if (ui->grab)
                send(ui->grab, e);         /* 弹出菜单借此关闭 */
        }
        update_effective_focus(ui);
        r = call_handler(ui, e);
        break;
    case SK_EVENT_RESIZE:
        if (ui->grab)
            send(ui->grab, e);
        if (ui->layout)
            ui->layout(ui, e->size.w, e->size.h, ui->layout_user);
        call_handler(ui, e);
        /* 新缓冲区的内容是未定义的：整窗重画，而且要在 client.c 把缓冲区交给服务器之前画好 */
        sk_ui_invalidate_all(ui);
        ui_redraw(ui);
        refresh_hover(ui);
        r = 1;
        break;
    case SK_EVENT_CLOSE:
        r = call_handler(ui, e);
        break;
    case SK_EVENT_CLIPBOARD:
        r = ui_clipboard(ui, e);
        break;
    default:
        r = call_handler(ui, e);
        break;
    }
    ui->depth--;
    if (ui->depth == 0) {
        ui_reap(ui);
        if (ui->free_pending) {
            ui_free_now(ui);
            return r;
        }
    }
    return r;
}

/* 窗口被销毁：断开（之后所有操作都变成空操作，内存由 sk_ui_free 释放） */
static void ui_window_gone(sk_window *win, void *user)
{
    (void)win;
    sk_ui *ui = user;
    caret_stop(ui);
    while (ui->timers) {
        wtimer *t = ui->timers;
        ui->timers = t->next;
        sk_app_remove_timer(ui->app, t->id);
        free(t);
    }
    if (ui->idle_id)
        sk_app_remove_idle(ui->app, ui->idle_id);
    ui->idle_id = 0;
    ui->win = NULL;
    ui->ndirty = 0;
    ui->hover = ui->pressed = ui->grab = NULL;
}

/* ------------------------------------------------------------ sk_ui 生命周期 */

sk_ui *sk_ui_new(sk_window *win)
{
    if (!win)
        return NULL;
    sk_ui *ui = calloc(1, sizeof *ui);
    if (!ui)
        return NULL;
    ui->win = win;
    ui->app = sk_window_app(win);
    /* 纸张颗粒的种子：同一个窗口每次都一样（局部重绘不会出现“补丁”） */
    ui->seed = sk_hash_u32(sk_hash_str(sk_window_title(win)), 0x5EEDu);
    ui->window_focused = sk_window_is_focused(win);
    sk_window_set_handler(win, ui_on_event, ui);
    sk_window_set_destroy_hook(win, ui_window_gone, ui);
    ui->idle_id = sk_app_add_idle(ui->app, ui_idle, ui);
    sk_ui_invalidate_all(ui);
    return ui;
}

static void widget_free(sk_widget *w)
{
    if (w->cls->destroy)
        w->cls->destroy(w);
    free(w->text);
    free(IMPL(w));
}

/* 释放所有已死亡的控件。只在 depth==0（没有 sk_ui 代码在栈上）时调用 */
static void ui_reap(sk_ui *ui)
{
    while (ui->has_dead) {
        ui->has_dead = 0;
        sk_widget *dead = NULL, *prev = NULL;
        sk_widget *w = ui->first;
        while (w) {                         /* 先全部摘下来，再释放（释放时可能又创建/销毁控件） */
            sk_widget *next = w->next;
            if (IMPL(w)->dead) {
                if (prev)
                    prev->next = next;
                else
                    ui->first = next;
                if (ui->last == w)
                    ui->last = prev;
                w->next = dead;
                dead = w;
            } else {
                prev = w;
            }
            w = next;
        }
        ui->depth++;
        while (dead) {
            sk_widget *next = dead->next;
            widget_free(dead);
            dead = next;
        }
        ui->depth--;
    }
}

static void ui_free_now(sk_ui *ui)
{
    ui->depth++;
    for (sk_widget *w = ui->first; w; w = w->next)
        IMPL(w)->dead = 1;
    ui->depth--;
    while (ui->first) {
        sk_widget *w = ui->first;
        ui->first = w->next;
        widget_free(w);
    }
    caret_stop(ui);
    while (ui->timers) {
        wtimer *t = ui->timers;
        ui->timers = t->next;
        sk_app_remove_timer(ui->app, t->id);
        free(t);
    }
    if (ui->idle_id)
        sk_app_remove_idle(ui->app, ui->idle_id);
    if (ui->win) {
        sk_window_set_handler(ui->win, NULL, NULL);
        sk_window_set_destroy_hook(ui->win, NULL, NULL);
    }
    free(ui);
}

void sk_ui_free(sk_ui *ui)
{
    if (!ui || ui->free_pending)
        return;
    if (ui->depth > 0) {
        /* 在自己的回调里被释放（例如对话框按钮的回调）：先断开窗口，等回调返回再释放内存 */
        ui->free_pending = 1;
        if (ui->win) {
            sk_window_set_handler(ui->win, NULL, NULL);
            sk_window_set_destroy_hook(ui->win, NULL, NULL);
            ui->win = NULL;
        }
        caret_stop(ui);
        while (ui->timers) {
            wtimer *t = ui->timers;
            ui->timers = t->next;
            sk_app_remove_timer(ui->app, t->id);
            free(t);
        }
        ui->ndirty = 0;
        ui->hover = ui->pressed = ui->grab = ui->focus = NULL;
        /* 空闲回调还留着：等调用栈退干净后，由它（或事件入口）完成真正的释放 */
        return;
    }
    ui_free_now(ui);
}

sk_window *sk_ui_window(sk_ui *ui) { return ui ? ui->win : NULL; }
sk_app *sk_ui_app(sk_ui *ui) { return ui ? ui->app : NULL; }
sk_canvas *sk_ui_canvas(sk_ui *ui) { return ui && ui->win ? sk_window_canvas(ui->win) : NULL; }

void sk_ui_set_background(sk_ui *ui, sk_paint_fn fn, void *user)
{
    if (!ui)
        return;
    ui->bg = fn;
    ui->bg_user = user;
    sk_ui_invalidate_all(ui);
}

void sk_ui_set_layout(sk_ui *ui, sk_layout_fn fn, void *user)
{
    if (!ui)
        return;
    ui->layout = fn;
    ui->layout_user = user;
    if (fn && ui->win) {
        ui->depth++;
        fn(ui, sk_window_width(ui->win), sk_window_height(ui->win), user);
        ui->depth--;
        sk_ui_invalidate_all(ui);
        if (ui->depth == 0)
            ui_reap(ui);
    }
}

void sk_ui_set_handler(sk_ui *ui, sk_event_fn fn, void *user)
{
    if (!ui)
        return;
    ui->handler = fn;
    ui->handler_user = user;
}

sk_widget *sk_ui_widget_at(sk_ui *ui, int x, int y)
{
    if (!ui)
        return NULL;
    sk_widget *hit = NULL;
    for (sk_widget *w = ui->first; w; w = w->next)
        if (w->visible && !IMPL(w)->dead && sk_rect_contains(w->r, x, y))
            hit = w;
    return hit;
}

/* ------------------------------------------------------------ 控件通用操作 */

sk_widget *sk_widget_new(sk_ui *ui, const sk_widget_class *cls, sk_rect r)
{
    if (!ui || !cls || ui->free_pending)
        return NULL;
    widget_impl *im = calloc(1, sizeof *im);
    if (!im)
        return NULL;
    sk_widget *w = &im->pub;
    w->cls = cls;
    w->ui = ui;
    w->r = r;
    w->visible = 1;
    /* 种子 = 窗口种子 + 创建序号：同一个应用每次运行，同一个控件的手绘抖动都一样 */
    w->seed = sk_hash_u32(ui->seed, ++ui->widget_counter);
    w->accent = SK_BLUE;
    w->font = SK_FONT_UI;
    im->cursor = SK_CURSOR_ARROW;
    if (ui->last)
        ui->last->next = w;
    else
        ui->first = w;
    ui->last = w;
    inval_widget(w);
    return w;
}

void sk_widget_destroy(sk_widget *w)
{
    if (!alive(w))
        return;
    sk_ui *ui = w->ui;
    if (w->visible)
        inval_widget(w);
    IMPL(w)->dead = 1;
    /* 解除 sk_ui 对它的所有引用 */
    if (ui->hover == w)
        ui->hover = NULL;
    if (ui->pressed == w)
        ui->pressed = NULL;
    if (ui->grab == w)
        ui->grab = NULL;
    if (ui->focus == w)
        ui->focus = NULL;
    if (ui->caret_owner == w)
        caret_stop(ui);
    for (int i = 0; i < ui->nclip; i++)
        if (ui->clip_req[i] == w)
            ui->clip_req[i] = NULL;
    for (wtimer **pp = &ui->timers; *pp;) {
        wtimer *t = *pp;
        if (t->w == w) {
            *pp = t->next;
            sk_app_remove_timer(ui->app, t->id);
            free(t);
        } else {
            pp = &t->next;
        }
    }
    update_cursor(ui);
    ui->has_dead = 1;
    if (ui->depth == 0) {
        ui_reap(ui);
        refresh_hover(ui);
    }
}

void sk_widget_set_rect(sk_widget *w, sk_rect r)
{
    if (!alive(w) || rect_eq(w->r, r))
        return;
    sk_widget_invalidate(w);
    w->r = r;
    sk_widget_invalidate(w);
    if (w->cls->resized)
        w->cls->resized(w);
}

void sk_widget_set_text(sk_widget *w, const char *text)
{
    if (!alive(w))
        return;
    if (IMPL(w)->text_hook) {
        IMPL(w)->text_hook(w, text ? text : "");
        return;
    }
    if (!text)
        text = "";
    if (w->text && strcmp(w->text, text) == 0)
        return;
    char *dup = strdup(text);
    if (!dup)
        return;
    free(w->text);
    w->text = dup;
    sk_widget_invalidate(w);
}

/* 隐藏或禁用时，撤销它的悬停/捕获/焦点/抓取 */
static void drop_interaction(sk_widget *w)
{
    sk_ui *ui = w->ui;
    if (ui->pressed == w)
        release_capture(ui);
    if (ui->hover == w)
        set_hover(ui, NULL);
    if (ui->focus == w)
        sk_ui_focus(ui, NULL);
    if (ui->grab == w)
        sk_ui_grab(ui, NULL);
    w->state &= ~(SK_STATE_HOVER | SK_STATE_PRESSED);
    update_cursor(ui);
}

void sk_widget_set_visible(sk_widget *w, int visible)
{
    if (!alive(w))
        return;
    visible = visible ? 1 : 0;
    if (w->visible == visible)
        return;
    inval_widget(w);
    w->visible = visible;
    if (!visible)
        drop_interaction(w);
    else if (w->ui->depth == 0)
        refresh_hover(w->ui);
}

void sk_widget_set_enabled(sk_widget *w, int en)
{
    if (!alive(w))
        return;
    int is = enabled(w);
    if ((en != 0) == is)
        return;
    if (en)
        w->state &= ~SK_STATE_DISABLED;
    else
        w->state |= SK_STATE_DISABLED;
    sk_widget_invalidate(w);
    if (!en)
        drop_interaction(w);
    else if (w->ui->depth == 0)
        refresh_hover(w->ui);
}

void sk_widget_set_checked(sk_widget *w, int checked)
{
    if (!alive(w))
        return;
    unsigned old = w->state;
    if (checked)
        w->state |= SK_STATE_CHECKED;
    else
        w->state &= ~SK_STATE_CHECKED;
    if (old != w->state)
        sk_widget_invalidate(w);
}

int sk_widget_checked(const sk_widget *w) { return w && (w->state & SK_STATE_CHECKED) ? 1 : 0; }

void sk_widget_set_accent(sk_widget *w, sk_color accent)
{
    if (!alive(w) || w->accent == accent)
        return;
    w->accent = accent;
    sk_widget_invalidate(w);
}

void sk_widget_set_font(sk_widget *w, int font_id)
{
    if (!alive(w) || w->font == font_id)
        return;
    w->font = font_id;
    if (w->cls->resized)
        w->cls->resized(w);            /* 字体变了，布局相关的缓存也要更新 */
    sk_widget_invalidate(w);
}

void sk_widget_set_callbacks(sk_widget *w, sk_widget_fn on_click, sk_widget_fn on_change, void *user)
{
    if (!alive(w))
        return;
    w->on_click = on_click;
    w->on_change = on_change;
    w->user = user;
}

/* ------------------------------------------------------------ 内部接口：标志/光标/查询 */

void sk_widget_set_flags(sk_widget *w, unsigned set, unsigned clear)
{
    if (!alive(w))
        return;
    IMPL(w)->flags = (IMPL(w)->flags & ~clear) | set;
}

unsigned sk_widget_flags(const sk_widget *w) { return w ? CIMPL(w)->flags : 0; }

void sk_widget_set_cursor(sk_widget *w, int cursor)
{
    if (!alive(w))
        return;
    IMPL(w)->cursor = cursor;
    sk_ui *ui = w->ui;
    if (ui->hover == w || ui->pressed == w)
        update_cursor(ui);
}

int sk_widget_cursor(const sk_widget *w) { return w ? CIMPL(w)->cursor : SK_CURSOR_ARROW; }

int sk_widget_alive(const sk_widget *w) { return alive(w); }

int sk_widget_has_focus(const sk_widget *w) { return alive(w) && w->ui->focus == w; }

int sk_widget_is_click(const sk_widget *w, const sk_event *e)
{
    return alive(w) && e && e->type == SK_EVENT_MOUSE && e->mouse.kind == SK_MOUSE_UP &&
           e->mouse.button == SK_BUTTON_LEFT && (w->state & SK_STATE_PRESSED) &&
           w->ui->pressed == w && sk_rect_contains(w->r, e->mouse.x, e->mouse.y);
}

const sk_font *sk_widget_get_font(const sk_widget *w)
{
    return sk_font_get(w ? w->font : SK_FONT_UI);
}

sk_color sk_widget_ink(const sk_widget *w, sk_color normal)
{
    return (w && (w->state & SK_STATE_DISABLED)) ? SK_INK_FAINT : normal;
}

int sk_ui_window_focused(sk_ui *ui) { return ui && ui->window_focused; }
int sk_ui_focus_visible(sk_ui *ui) { return ui && ui->focus_visible; }
sk_widget *sk_ui_hovered(sk_ui *ui) { return ui ? ui->hover : NULL; }
sk_widget *sk_ui_pressed(sk_ui *ui) { return ui ? ui->pressed : NULL; }

void sk_ui_grab(sk_ui *ui, sk_widget *w)
{
    if (!ui)
        return;
    if (w && (!alive(w) || w->ui != ui))
        return;
    ui->grab = w;
    if (w) {
        /* 抓取开始：其它控件的捕获和悬停立即结束 */
        release_capture(ui);
        set_hover(ui, hover_target(ui, ui->mouse_x, ui->mouse_y));
    } else {
        refresh_hover(ui);
    }
    update_cursor(ui);
}

sk_widget *sk_ui_grabbed(sk_ui *ui) { return ui ? ui->grab : NULL; }

void sk_widget_raise(sk_widget *w)
{
    if (!alive(w))
        return;
    sk_ui *ui = w->ui;
    if (ui->last == w)
        return;
    sk_widget *prev = NULL;
    for (sk_widget *p = ui->first; p; prev = p, p = p->next) {
        if (p != w)
            continue;
        if (prev)
            prev->next = w->next;
        else
            ui->first = w->next;
        break;
    }
    w->next = NULL;
    ui->last->next = w;
    ui->last = w;
    sk_widget_invalidate(w);
}

void sk_widget_set_text_hook(sk_widget *w, sk_widget_text_hook hook)
{
    if (alive(w))
        IMPL(w)->text_hook = hook;
}

/* ------------------------------------------------------------ 输入光标闪烁 */

static void caret_timer_fn(sk_app *app, void *user)
{
    (void)app;
    sk_ui *ui = user;
    if (!alive(ui->caret_owner)) {
        ui->caret_timer = 0;
        return;
    }
    ui->caret_on = !ui->caret_on;
    sk_ui_invalidate(ui, ui->caret_rect);
    if (++ui->caret_blinks >= CARET_BLINK_LIMIT && ui->caret_on) {
        /* 闪够了：停在“可见”，不再唤醒进程 */
        sk_app_remove_timer(ui->app, ui->caret_timer);
        ui->caret_timer = 0;
    }
}

static void caret_restart_timer(sk_ui *ui)
{
    if (ui->caret_timer)
        sk_app_remove_timer(ui->app, ui->caret_timer);
    ui->caret_timer = ui->win ? sk_app_add_timer(ui->app, CARET_BLINK_MS, 1, caret_timer_fn, ui) : 0;
    ui->caret_blinks = 0;
}

static void caret_stop(sk_ui *ui)
{
    if (ui->caret_timer)
        sk_app_remove_timer(ui->app, ui->caret_timer);
    ui->caret_timer = 0;
    if (ui->caret_owner)
        sk_ui_invalidate(ui, ui->caret_rect);
    ui->caret_owner = NULL;
    ui->caret_on = 0;
}

static int caret_allowed(const sk_widget *w)
{
    return alive(w) && w->ui->focus == w && (w->state & SK_STATE_FOCUSED) && w->ui->win;
}

void sk_ui_caret_set(sk_widget *w, sk_rect caret)
{
    if (!caret_allowed(w))
        return;
    sk_ui *ui = w->ui;
    if (ui->caret_owner == w && rect_eq(ui->caret_rect, caret)) {
        sk_ui_caret_reset(w);
        return;
    }
    if (ui->caret_owner)
        sk_ui_invalidate(ui, ui->caret_rect);       /* 擦掉旧位置 */
    ui->caret_owner = w;
    ui->caret_rect = caret;
    ui->caret_on = 1;
    sk_ui_invalidate(ui, caret);
    caret_restart_timer(ui);
}

void sk_ui_caret_reset(sk_widget *w)
{
    if (!caret_allowed(w) || w->ui->caret_owner != w)
        return;
    sk_ui *ui = w->ui;
    if (!ui->caret_on) {
        ui->caret_on = 1;
        sk_ui_invalidate(ui, ui->caret_rect);
    }
    caret_restart_timer(ui);
}

void sk_ui_caret_hide(sk_widget *w)
{
    if (!alive(w) || w->ui->caret_owner != w)
        return;
    caret_stop(w->ui);
}

int sk_ui_caret_visible(const sk_widget *w)
{
    return caret_allowed(w) && w->ui->caret_owner == w && w->ui->caret_on;
}

/* ------------------------------------------------------------ 控件定时器 */

static void wtimer_fire(sk_app *app, void *user)
{
    (void)app;
    wtimer *t = user;
    sk_ui *ui = t->ui;
    sk_widget *w = t->w;
    sk_widget_timer_fn fn = t->fn;
    if (!t->repeat) {
        /* 一次性定时器：client.c 已经注销了它，这里释放记录（回调之后不能再碰 t） */
        for (wtimer **pp = &ui->timers; *pp; pp = &(*pp)->next) {
            if (*pp == t) {
                *pp = t->next;
                break;
            }
        }
        free(t);
    }
    if (!alive(w))
        return;
    ui->depth++;
    fn(w);
    ui->depth--;
    if (ui->depth == 0) {
        ui_reap(ui);
        if (ui->free_pending)
            ui_free_now(ui);
    }
}

int sk_widget_add_timer(sk_widget *w, int interval_ms, int repeat, sk_widget_timer_fn fn)
{
    if (!alive(w) || !fn || !w->ui->win)
        return 0;
    sk_ui *ui = w->ui;
    wtimer *t = calloc(1, sizeof *t);
    if (!t)
        return 0;
    t->ui = ui;
    t->w = w;
    t->fn = fn;
    t->repeat = repeat ? 1 : 0;
    t->id = sk_app_add_timer(ui->app, interval_ms, repeat, wtimer_fire, t);
    if (!t->id) {
        free(t);
        return 0;
    }
    t->next = ui->timers;
    ui->timers = t;
    return t->id;
}

void sk_widget_remove_timer(sk_widget *w, int id)
{
    if (!w || id <= 0)
        return;
    sk_ui *ui = w->ui;
    for (wtimer **pp = &ui->timers; *pp; pp = &(*pp)->next) {
        wtimer *t = *pp;
        if (t->id == id && t->w == w) {
            *pp = t->next;
            sk_app_remove_timer(ui->app, t->id);
            free(t);
            return;
        }
    }
}

/* ------------------------------------------------------------ 文本编辑小工具 */

int sk_ui_utf8_prev(const char *s, int pos)
{
    if (!s || pos <= 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

int sk_ui_utf8_next(const char *s, int len, int pos)
{
    if (!s || pos >= len)
        return len;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

static uint32_t cp_at(const char *s, int pos)
{
    const char *p = s + pos;
    return sk_utf8_next(&p);
}

int sk_ui_is_word_char(uint32_t cp)
{
    if (cp < 0x80)
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
    /* 全角标点和中文标点不算词的一部分 */
    if ((cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFF0F) ||
        (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
        (cp >= 0xFF5B && cp <= 0xFF65) || (cp >= 0x2000 && cp <= 0x206F))
        return 0;
    return 1;
}

int sk_ui_word_left(const char *s, int pos)
{
    if (!s)
        return 0;
    while (pos > 0) {
        int p = sk_ui_utf8_prev(s, pos);
        if (sk_ui_is_word_char(cp_at(s, p)))
            break;
        pos = p;
    }
    while (pos > 0) {
        int p = sk_ui_utf8_prev(s, pos);
        if (!sk_ui_is_word_char(cp_at(s, p)))
            break;
        pos = p;
    }
    return pos;
}

int sk_ui_word_right(const char *s, int len, int pos)
{
    if (!s)
        return 0;
    while (pos < len && !sk_ui_is_word_char(cp_at(s, pos)))
        pos = sk_ui_utf8_next(s, len, pos);
    while (pos < len && sk_ui_is_word_char(cp_at(s, pos)))
        pos = sk_ui_utf8_next(s, len, pos);
    return pos;
}

int sk_ui_text_hit(const sk_font *f, const char *s, int len, int x)
{
    if (!s || !f || x <= 0)
        return 0;
    int pos = 0, cx = 0;
    while (pos < len) {
        int next = sk_ui_utf8_next(s, len, pos);
        int adv = sk_glyph_advance(f, cp_at(s, pos));
        if (x < cx + adv / 2)          /* 点在这个字的左半边 → 光标放在它前面 */
            return pos;
        cx += adv;
        pos = next;
    }
    return len;
}
