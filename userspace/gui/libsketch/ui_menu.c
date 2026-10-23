/*
 * ui_menu.c — 弹出菜单（sk_menu_popup，sketch/ui.h）
 *
 * 菜单不是一个独立的窗口，而是盖在本窗口所有控件之上的一个临时控件：
 *   - 它被加在控件列表的最后（= 最上层，最后画）；
 *   - 用 sk_ui_grab() “抓取”输入：菜单打开期间，窗口里所有鼠标、键盘事件都只交给它，
 *     点到菜单外面就关闭（不回调），就像真正的模态菜单；
 *   - 关闭 = 销毁这个控件，sk_ui 会把它占过的区域标脏，下面的控件自动重画回来。
 * 外观是一张“横格笔记本纸”：淡蓝色横线、左边一条红色页边线、铅笔描的纸边、右下方排线阴影，
 * 鼠标所在的那一项用荧光笔涂亮。
 */
#include "ui_internal.h"
#include "sketch/rough.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>

#define MENU_SHADOW   6        /* 阴影向右下偏移 */
#define MENU_PAD_Y    6        /* 纸张上下留白 */
#define MENU_MARGIN   22       /* 红色页边线到纸左边的距离；文字写在页边线右边 */
#define MENU_PAD_R    18
#define MENU_SEP_H    11
#define MENU_MIN_W    120
#define MENU_MOVE_ARM 3        /* 鼠标移动超过这么多像素后，松开按钮才算“选中”（见 armed） */

typedef struct {
    char      **items;         /* NULL = 分隔线 */
    int         n;
    int         hover;         /* 鼠标/键盘所在的项，-1 无 */
    int         row_h;
    sk_rect     box;           /* 纸张本身（不含阴影） */
    sk_menu_fn  on_pick;
    void       *user;
    /* 打开菜单的那次鼠标按下，紧跟着会有一个松开事件（例如右键按下弹出菜单、马上松开）。
     * 那个松开不能算“选中”，所以只有在菜单里按下过、或者鼠标移动过之后，松开才生效。 */
    int         armed;
    int         open_x, open_y;
} menu_priv;

static const sk_widget_class menu_class;

static int menu_max(int a, int b) { return a > b ? a : b; }

static int is_sep(const menu_priv *p, int i) { return p->items[i] == NULL; }

/* 第 i 项的矩形（窗口坐标） */
static sk_rect item_rect(const menu_priv *p, int i)
{
    int y = p->box.y + MENU_PAD_Y;
    for (int k = 0; k < i; k++)
        y += is_sep(p, k) ? MENU_SEP_H : p->row_h;
    return sk_rect_make(p->box.x, y, p->box.w, is_sep(p, i) ? MENU_SEP_H : p->row_h);
}

static int item_at(const menu_priv *p, int x, int y)
{
    if (!sk_rect_contains(p->box, x, y))
        return -1;
    for (int i = 0; i < p->n; i++)
        if (sk_rect_contains(item_rect(p, i), x, y))
            return is_sep(p, i) ? -1 : i;
    return -1;
}

static void menu_draw(sk_widget *w, sk_canvas *c)
{
    menu_priv *p = w->priv;
    sk_rect b = p->box;
    const sk_font *f = sk_widget_get_font(w);

    /* 1. 阴影：右下方一块铅笔排线 */
    sk_rough_style sh;
    sk_rough_default(&sh);
    sh.seed = w->seed ^ 0x5AD0u;
    sh.stroke_width = 0;
    sh.fill_style = SK_FILL_HACHURE;
    sh.fill = SK_SHADOW;
    sh.hachure_gap = 4.5f;
    sh.fill_weight = 1.0f;
    sh.roughness = 0.8f;
    sk_rough_rect(c, (float)(b.x + MENU_SHADOW), (float)(b.y + MENU_SHADOW), (float)b.w, (float)b.h, &sh);

    /* 2. 纸：先实心铺上纸色盖住阴影，再加纸张颗粒 */
    sk_rough_style paper;
    sk_rough_default(&paper);
    paper.seed = w->seed;
    paper.stroke_width = 0;
    paper.fill_style = SK_FILL_SOLID;
    paper.fill = SK_PAPER;
    paper.roughness = 0.6f;
    sk_rough_rect(c, (float)b.x, (float)b.y, (float)b.w, (float)b.h, &paper);
    sk_paper_fill(c, sk_rect_inset(b, 2, 2), SK_PAPER, 4, w->seed);

    /* 3. 横格线（每一项下面一条淡蓝线）和红色页边线 */
    for (int i = 0; i < p->n; i++) {
        if (is_sep(p, i))
            continue;
        sk_rect r = item_rect(p, i);
        sk_hline(c, b.x + 3, b.x + b.w - 4, r.y + r.h - 1, SK_DESK_GRID);
    }
    sk_vline(c, b.x + MENU_MARGIN - 6, b.y + 2, b.y + b.h - 3, SK_WITH_ALPHA(SK_RED, 0x70));

    /* 4. 悬停项：荧光笔涂一道 */
    if (p->hover >= 0) {
        sk_rect r = item_rect(p, p->hover);
        sk_rect hl = sk_rect_make(r.x + MENU_MARGIN - 8, r.y + 3, r.w - MENU_MARGIN + 2, r.h - 6);
        sk_marker_highlight(c, hl, SK_HIGHLIGHT, w->seed + (uint32_t)p->hover * 7919u);
    }

    /* 5. 文字和分隔线 */
    for (int i = 0; i < p->n; i++) {
        sk_rect r = item_rect(p, i);
        if (is_sep(p, i)) {
            sk_draw_separator(c, b.x + MENU_MARGIN - 2, b.x + b.w - 10, r.y + r.h / 2, w->seed + (uint32_t)i);
            continue;
        }
        sk_rect tr = sk_rect_make(b.x + MENU_MARGIN, r.y, b.w - MENU_MARGIN - MENU_PAD_R / 2, r.h);
        sk_draw_text_in_rect(c, f, tr, p->items[i], SK_INK, SK_ALIGN_LEFT);
    }

    /* 6. 纸边：铅笔描两遍 */
    sk_rough_style edge;
    sk_style_pencil(&edge, w->seed ^ 0xED6Eu);
    edge.roughness = 0.9f;
    sk_rough_rect(c, (float)b.x, (float)b.y, (float)b.w, (float)b.h, &edge);
}

static void set_hover(sk_widget *w, int i)
{
    menu_priv *p = w->priv;
    if (i == p->hover)
        return;
    /* 只重画变化的两行，而不是整个菜单 */
    if (p->hover >= 0)
        sk_widget_invalidate_rect(w, sk_rect_inset(item_rect(p, p->hover), -2, -2));
    p->hover = i;
    if (i >= 0)
        sk_widget_invalidate_rect(w, sk_rect_inset(item_rect(p, i), -2, -2));
}

static void menu_close(sk_widget *w)
{
    sk_widget_destroy(w);              /* 同时释放抓取、擦掉菜单 */
}

static void menu_pick(sk_widget *w, int i)
{
    menu_priv *p = w->priv;
    sk_menu_fn fn = p->on_pick;
    void *user = p->user;
    sk_ui *ui = w->ui;
    /* 先关菜单再回调：回调里可以马上弹出另一个菜单或对话框 */
    menu_close(w);
    if (fn)
        fn(ui, i, user);
}

/* 键盘上下移动，跳过分隔线 */
static void step(sk_widget *w, int dir)
{
    menu_priv *p = w->priv;
    int i = p->hover;
    for (int k = 0; k < p->n; k++) {
        if (i < 0)
            i = dir > 0 ? 0 : p->n - 1;
        else
            i = (i + dir + p->n) % p->n;
        if (!is_sep(p, i)) {
            set_hover(w, i);
            return;
        }
    }
}

static int menu_event(sk_widget *w, const sk_event *e)
{
    menu_priv *p = w->priv;
    switch (e->type) {
    case SK_EVENT_MOUSE: {
        int x = e->mouse.x, y = e->mouse.y;
        switch (e->mouse.kind) {
        case SK_MOUSE_MOVE:
        case SK_MOUSE_ENTER:
            if (abs(x - p->open_x) > MENU_MOVE_ARM || abs(y - p->open_y) > MENU_MOVE_ARM)
                p->armed = 1;
            set_hover(w, item_at(p, x, y));
            break;
        case SK_MOUSE_LEAVE:
            set_hover(w, -1);
            break;
        case SK_MOUSE_DOWN:
            if (!sk_rect_contains(p->box, x, y)) {
                menu_close(w);         /* 点到外面：关闭，不回调 */
                return 1;
            }
            p->armed = 1;
            set_hover(w, item_at(p, x, y));
            break;
        case SK_MOUSE_UP: {
            int i = item_at(p, x, y);
            if (p->armed && i >= 0)
                menu_pick(w, i);
            break;
        }
        default:
            break;
        }
        return 1;
    }
    case SK_EVENT_KEY:
        if (e->key.state == 0)
            return 1;
        switch (e->key.keycode) {
        case KEY_UP:    step(w, -1); break;
        case KEY_DOWN:  step(w, 1); break;
        case KEY_TAB:   step(w, (e->key.modifiers & SK_MOD_SHIFT) ? -1 : 1); break;
        case KEY_HOME:  p->hover = -1; step(w, 1); break;
        case KEY_END:   p->hover = -1; step(w, -1); break;
        case KEY_ESC:   menu_close(w); break;
        case KEY_ENTER:
        case KEY_KPENTER:
        case KEY_SPACE:
            if (p->hover >= 0)
                menu_pick(w, p->hover);
            break;
        default:
            break;
        }
        return 1;
    case SK_EVENT_FOCUS:               /* 窗口失去焦点 */
    case SK_EVENT_RESIZE:              /* 窗口大小变了，位置已经不对 */
        menu_close(w);
        return 1;
    default:
        return 0;
    }
}

static void menu_destroy(sk_widget *w)
{
    menu_priv *p = w->priv;
    if (!p)
        return;
    for (int i = 0; i < p->n; i++)
        free(p->items[i]);
    free(p->items);
    free(p);
    w->priv = NULL;
}

static const sk_widget_class menu_class = {
    .name = "menu", .draw = menu_draw, .event = menu_event, .destroy = menu_destroy,
};

void sk_menu_popup(sk_ui *ui, int x, int y, const char *const *items, int n, sk_menu_fn on_pick, void *user)
{
    if (!ui || !sk_ui_window(ui) || n <= 0 || !items)
        return;
    /* 同时只有一个菜单 */
    sk_widget *old = sk_ui_grabbed(ui);
    if (old && old->cls == &menu_class)
        menu_close(old);

    menu_priv *p = calloc(1, sizeof *p);
    if (!p)
        return;
    p->items = calloc((size_t)n, sizeof *p->items);
    if (!p->items) {
        free(p);
        return;
    }
    p->n = n;
    p->hover = -1;
    p->on_pick = on_pick;
    p->user = user;
    p->open_x = x;
    p->open_y = y;

    /* 量尺寸：最宽的一项 + 页边 + 右留白 */
    const sk_font *f = sk_font_get(SK_FONT_UI);
    p->row_h = menu_max(sk_font_line_height(f) + 10, 28);
    int text_w = 0, h = 2 * MENU_PAD_Y;
    for (int i = 0; i < n; i++) {
        const char *s = items[i];
        if (!s || strcmp(s, "-") == 0) {
            p->items[i] = NULL;
            h += MENU_SEP_H;
            continue;
        }
        p->items[i] = strdup(s);
        int tw = sk_text_width(f, s);
        if (tw > text_w)
            text_w = tw;
        h += p->row_h;
    }
    int bw = text_w + MENU_MARGIN + MENU_PAD_R;
    if (bw < MENU_MIN_W)
        bw = MENU_MIN_W;

    /* 摆放：默认在点击处右下方；放不下就翻到左边/上边；最后夹在窗口里面 */
    sk_window *win = sk_ui_window(ui);
    int ww = sk_window_width(win), wh = sk_window_height(win);
    int tot_w = bw + MENU_SHADOW, tot_h = h + MENU_SHADOW;
    if (bw > ww - MENU_SHADOW - 2)
        bw = menu_max(ww - MENU_SHADOW - 2, 40), tot_w = bw + MENU_SHADOW;
    int bx = x, by = y;
    if (bx + tot_w > ww)
        bx = (x - bw >= 0) ? x - bw : ww - tot_w;
    if (by + tot_h > wh)
        by = (y - h >= 0) ? y - h : wh - tot_h;
    if (bx < 1)
        bx = 1;
    if (by < 1)
        by = 1;
    p->box = sk_rect_make(bx, by, bw, h);

    sk_widget *w = sk_widget_new(ui, &menu_class, sk_rect_make(bx, by, tot_w, tot_h));
    if (!w) {
        for (int i = 0; i < n; i++)
            free(p->items[i]);
        free(p->items);
        free(p);
        return;
    }
    w->priv = p;
    /* 菜单自己只重画变化的那两行；鼠标进出菜单不需要整块重画 */
    sk_widget_set_flags(w, SK_WF_NO_AUTO_REDRAW, 0);
    sk_ui_grab(ui, w);
}
