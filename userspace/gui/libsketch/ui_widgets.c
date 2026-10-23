/*
 * ui_widgets.c — 标准控件：标签、段落、按钮、图标按钮、色块、复选框、单行输入框、
 *                标签页、占用率条、滑块、自定义区域（sketch/ui.h）
 *
 * 每个控件就是一个 sk_widget_class（一组函数指针：画、处理事件、销毁、改变大小）
 * 加上一块 priv 私有数据。这和 C++/Java 里的“类 + 虚函数”是同一回事，只是用 C 手写：
 * sk_ui 核心只认识 sk_widget_class，不知道“按钮”是什么，所以添加新控件不需要改核心。
 *
 * 外观全部交给 theme.h / rough.h：控件只决定“画什么、画在哪儿”，
 * 线条怎么抖、颜色怎么排线由主题统一决定，整个系统风格才会一致。
 * 所有手绘抖动都用 w->seed：同一个控件每次重画都一模一样（局部刷新不会“跳”）。
 */
#include "ui_internal.h"
#include "sketch/rough.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static int is_press(const sk_event *e) { return e->type == SK_EVENT_KEY && e->key.state != 0; }

static int is_activate_key(const sk_event *e)
{
    return e->key.keycode == KEY_ENTER || e->key.keycode == KEY_KPENTER || e->key.keycode == KEY_SPACE ||
           (e->key.keycode == 0 && (e->key.codepoint == '\r' || e->key.codepoint == ' '));
}

/* 文字在高度为 h 的区域里垂直居中时的基线 */
static int center_baseline(const sk_font *f, int y, int h)
{
    int asc = sk_font_ascent(f), desc = sk_font_descent(f);
    return y + (h - (asc + desc)) / 2 + asc;
}

static char *dup_or_empty(const char *s)
{
    char *d = strdup(s ? s : "");
    return d;
}

/* 按下回车/空格时让按钮“闪”一下（按下态保持 120ms），给键盘操作也一个看得见的反馈 */
static void flash_off(sk_widget *w)
{
    if (sk_ui_pressed(w->ui) != w) {
        w->state &= ~SK_STATE_PRESSED;
        sk_widget_invalidate(w);
    }
}

static void flash(sk_widget *w)
{
    w->state |= SK_STATE_PRESSED;
    sk_widget_invalidate(w);
    sk_widget_add_timer(w, 120, 0, flash_off);
}

/* ================================================================ 标签 */

typedef struct {
    sk_color color;
    int align;
} label_priv;

static void label_draw(sk_widget *w, sk_canvas *c)
{
    label_priv *p = w->priv;
    if (!w->text || !*w->text)
        return;
    sk_draw_text_in_rect(c, sk_widget_get_font(w), w->r, w->text, sk_widget_ink(w, p->color), p->align);
}

static void free_priv(sk_widget *w)
{
    free(w->priv);
    w->priv = NULL;
}

/* event=NULL：标签对鼠标是“透明”的，点到它等于点到它下面的东西 */
static const sk_widget_class label_class = {
    .name = "label", .draw = label_draw, .destroy = free_priv,
};

sk_widget *sk_label_new(sk_ui *ui, sk_rect r, const char *text, int font_id, int align)
{
    label_priv *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    sk_widget *w = sk_widget_new(ui, &label_class, r);
    if (!w) {
        free(p);
        return NULL;
    }
    p->color = SK_INK;
    p->align = align;
    w->priv = p;
    w->font = font_id;
    w->text = dup_or_empty(text);
    return w;
}

void sk_label_set_color(sk_widget *w, sk_color color)
{
    if (!sk_widget_alive(w) || w->cls != &label_class)
        return;
    label_priv *p = w->priv;
    if (p->color == color)
        return;
    p->color = color;
    sk_widget_invalidate(w);
}

/* ================================================================ 段落（自动换行） */

static void paragraph_draw(sk_widget *w, sk_canvas *c)
{
    if (!w->text || !*w->text)
        return;
    sk_draw_text_wrapped(c, sk_widget_get_font(w), w->r, w->text, sk_widget_ink(w, SK_INK), 4);
}

static const sk_widget_class paragraph_class = { .name = "paragraph", .draw = paragraph_draw };

sk_widget *sk_paragraph_new(sk_ui *ui, sk_rect r, const char *text, int font_id)
{
    sk_widget *w = sk_widget_new(ui, &paragraph_class, r);
    if (!w)
        return NULL;
    w->font = font_id;
    w->text = dup_or_empty(text);
    return w;
}

/* ================================================================ 按钮 */

static void button_draw(sk_widget *w, sk_canvas *c)
{
    sk_draw_button(c, w->r, w->text ? w->text : "", (int)w->state, w->accent, w->seed);
}

static int button_event(sk_widget *w, const sk_event *e)
{
    if (e->type == SK_EVENT_MOUSE) {
        if (sk_widget_is_click(w, e) && w->on_click)
            w->on_click(w, w->user);
        return e->mouse.kind != SK_MOUSE_WHEEL;
    }
    if (is_press(e) && is_activate_key(e) && !(e->key.modifiers & (SK_MOD_CTRL | SK_MOD_ALT))) {
        flash(w);
        if (w->on_click)
            w->on_click(w, w->user);
        return 1;
    }
    return 0;
}

static const sk_widget_class button_class = {
    .name = "button", .draw = button_draw, .event = button_event, .focusable = 1,
};

/* 可点击控件的共同设置：手形光标，点击不夺取键盘焦点 */
static void make_clickable(sk_widget *w)
{
    sk_widget_set_cursor(w, SK_CURSOR_HAND);
    sk_widget_set_flags(w, SK_WF_NO_CLICK_FOCUS, 0);
}

sk_widget *sk_button_new(sk_ui *ui, sk_rect r, const char *label, sk_widget_fn on_click, void *user)
{
    sk_widget *w = sk_widget_new(ui, &button_class, r);
    if (!w)
        return NULL;
    w->text = dup_or_empty(label);
    w->font = SK_FONT_UI;
    w->on_click = on_click;
    w->user = user;
    make_clickable(w);
    return w;
}

/* ================================================================ 图标按钮（工具栏） */

typedef struct {
    int icon;
} icon_button_priv;

static void icon_button_draw(sk_widget *w, sk_canvas *c)
{
    icon_button_priv *p = w->priv;
    sk_rect r = w->r;
    /* 工具栏风格：平时只有图标，鼠标移上来/按下/选中时才出现手绘的按钮框 */
    if (w->state & (SK_STATE_HOVER | SK_STATE_PRESSED | SK_STATE_CHECKED))
        sk_draw_button_frame(c, r, (int)w->state, w->accent, w->seed);

    int size = imin(r.h - 10, 28);
    if (size < 8)
        size = imin(r.h, r.w);
    int push = (w->state & SK_STATE_PRESSED) ? 1 : 0;      /* 按下时整体往右下沉 1 像素 */
    const sk_font *f = sk_widget_get_font(w);
    int has_label = w->text && *w->text;
    int ix, iy = r.y + (r.h - size) / 2 + push;
    if (has_label) {
        int tw = sk_text_width(f, w->text);
        int total = size + 6 + tw;
        ix = r.x + imax(6, (r.w - total) / 2) + push;
        sk_rect tr = sk_rect_make(ix + size + 6, r.y + push, r.x + r.w - (ix + size + 6) - 4, r.h);
        sk_draw_text_in_rect(c, f, tr, w->text, sk_widget_ink(w, SK_INK), SK_ALIGN_LEFT);
    } else {
        ix = r.x + (r.w - size) / 2 + push;
    }
    if (p->icon >= 0 && p->icon < SK_ICON_COUNT) {
        sk_draw_icon(c, p->icon, ix, iy, size, w->seed);
        if (w->state & SK_STATE_DISABLED)  /* 禁用：在图标上盖一层半透明的纸，让它变淡 */
            sk_fill_rect(c, sk_rect_make(ix - 2, iy - 2, size + 4, size + 4), SK_WITH_ALPHA(SK_PAPER, 0xA8));
    }
}

static const sk_widget_class icon_button_class = {
    .name = "icon_button", .draw = icon_button_draw, .event = button_event,
    .destroy = free_priv, .focusable = 1,
};

sk_widget *sk_icon_button_new(sk_ui *ui, sk_rect r, int icon, const char *label,
                              sk_widget_fn on_click, void *user)
{
    icon_button_priv *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    sk_widget *w = sk_widget_new(ui, &icon_button_class, r);
    if (!w) {
        free(p);
        return NULL;
    }
    p->icon = icon;
    w->priv = p;
    w->text = label ? strdup(label) : NULL;
    w->on_click = on_click;
    w->user = user;
    make_clickable(w);
    return w;
}

/* ================================================================ 色块（调色盘） */

static void swatch_draw(sk_widget *w, sk_canvas *c)
{
    sk_rect r = w->r;
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    float d = (float)imin(r.w, r.h) - 8.0f;
    if (d < 6)
        d = 6;
    sk_color color = w->accent;
    if (w->state & SK_STATE_DISABLED)
        color = sk_mix(color, SK_PAPER, 150);
    if (w->state & SK_STATE_HOVER)
        d += 2;                                    /* 悬停时稍微“鼓起来” */

    /* 一团用蜡笔涂实的颜色：实心填充，边缘跟着手绘轮廓走，描边是稍深的同色 */
    sk_rough_style s;
    sk_rough_default(&s);
    s.seed = w->seed;
    s.roughness = 1.3f;
    s.fill_style = SK_FILL_SOLID;
    s.fill = color;
    s.stroke = sk_darken(color, 70);
    s.stroke_width = 1.3f;
    s.double_stroke = 0;
    sk_rough_ellipse(c, cx, cy, d, d * 0.94f, &s);

    if (w->state & (SK_STATE_CHECKED | SK_STATE_PRESSED)) {
        /* 选中：外面用铅笔圈一圈（像在调色盘上把这个颜色圈出来） */
        sk_rough_style ring;
        sk_style_pencil(&ring, w->seed ^ 0xC1C1u);
        ring.stroke_width = 2.0f;
        sk_rough_ellipse(c, cx, cy, d + 7, d + 6, &ring);
    }
}

static int swatch_event(sk_widget *w, const sk_event *e)
{
    return button_event(w, e);
}

static const sk_widget_class swatch_class = {
    .name = "swatch", .draw = swatch_draw, .event = swatch_event, .focusable = 1,
};

sk_widget *sk_swatch_new(sk_ui *ui, sk_rect r, sk_color color, sk_widget_fn on_click, void *user)
{
    sk_widget *w = sk_widget_new(ui, &swatch_class, r);
    if (!w)
        return NULL;
    w->accent = color;                             /* 色块的颜色就存在 accent 里 */
    w->on_click = on_click;
    w->user = user;
    make_clickable(w);
    return w;
}

sk_color sk_swatch_color(const sk_widget *w)
{
    return (w && w->cls == &swatch_class) ? w->accent : 0;
}

/* ================================================================ 复选框 */

static void checkbox_draw(sk_widget *w, sk_canvas *c)
{
    sk_draw_checkbox(c, w->r, w->text ? w->text : "", (int)w->state, w->seed);
}

static void checkbox_toggle(sk_widget *w)
{
    w->state ^= SK_STATE_CHECKED;
    sk_widget_invalidate(w);
    if (w->on_change)
        w->on_change(w, w->user);
}

static int checkbox_event(sk_widget *w, const sk_event *e)
{
    if (e->type == SK_EVENT_MOUSE) {
        if (sk_widget_is_click(w, e))
            checkbox_toggle(w);
        return e->mouse.kind != SK_MOUSE_WHEEL;
    }
    if (is_press(e) && (e->key.keycode == KEY_SPACE || (e->key.keycode == 0 && e->key.codepoint == ' '))) {
        checkbox_toggle(w);
        return 1;
    }
    return 0;
}

static const sk_widget_class checkbox_class = {
    .name = "checkbox", .draw = checkbox_draw, .event = checkbox_event, .focusable = 1,
};

sk_widget *sk_checkbox_new(sk_ui *ui, sk_rect r, const char *label, int checked,
                           sk_widget_fn on_change, void *user)
{
    sk_widget *w = sk_widget_new(ui, &checkbox_class, r);
    if (!w)
        return NULL;
    w->text = dup_or_empty(label);
    if (checked)
        w->state |= SK_STATE_CHECKED;
    w->on_change = on_change;
    w->user = user;
    make_clickable(w);
    return w;
}

/* ================================================================ 单行输入框
 *
 * 文字以 UTF-8 存在 w->text 里（应用随时可以直接读）。光标位置 caret 和选区起点 anchor
 * 都是“字节偏移”，并且总是落在码点边界上——一个汉字占 3 个字节，光标不能停在它中间。
 * 选区 = [min(anchor, caret), max(anchor, caret))，anchor == caret 表示没有选区。
 * 文字比框长时整体向左滚动 scroll 像素，保证光标总在可见范围内。 */

#define TB_PAD      8          /* 文字到框左右边的距离 */
#define TB_MAX      4096       /* 最多这么多字节（和剪贴板上限一致） */

typedef struct {
    char *placeholder;
    int   len;                 /* strlen(w->text) */
    int   cap;                 /* w->text 缓冲区大小 */
    int   caret, anchor;
    int   scroll;
} textbox_priv;

static const sk_widget_class textbox_class;

static textbox_priv *tb_priv(const sk_widget *w)
{
    return (w && w->cls == &textbox_class) ? w->priv : NULL;
}

static int tb_inner_w(const sk_widget *w) { return imax(1, w->r.w - 2 * TB_PAD); }

static int tb_x_of(const sk_widget *w, int pos)
{
    return sk_text_width_n(sk_widget_get_font(w), w->text, pos);
}

/* 光标的屏幕矩形（窗口坐标） */
static sk_rect tb_caret_rect(const sk_widget *w)
{
    textbox_priv *p = w->priv;
    const sk_font *f = sk_widget_get_font(w);
    int base = center_baseline(f, w->r.y, w->r.h);
    int top = base - sk_font_ascent(f) - 1;
    int bot = base + sk_font_descent(f) + 1;
    int x = w->r.x + TB_PAD + tb_x_of(w, p->caret) - p->scroll;
    return sk_rect_make(x - 1, top, 3, bot - top);
}

/* 调整水平滚动，让光标可见；文字缩短时不要在右边留出大片空白 */
static void tb_scroll_to_caret(sk_widget *w)
{
    textbox_priv *p = w->priv;
    int inner = tb_inner_w(w);
    int cx = tb_x_of(w, p->caret);
    if (cx - p->scroll > inner - 2)
        p->scroll = cx - inner + 2;
    if (cx - p->scroll < 0)
        p->scroll = cx;
    int total = tb_x_of(w, p->len);
    if (total - p->scroll < inner - 2)
        p->scroll = imax(0, total - inner + 2);
    if (p->scroll < 0)
        p->scroll = 0;
}

/* 光标或文字变了：重新滚动、重画、告诉 sk_ui 新的光标位置（光标会立刻显示并重新开始闪） */
static void tb_changed(sk_widget *w)
{
    tb_scroll_to_caret(w);
    sk_widget_invalidate(w);
    if (w->state & SK_STATE_FOCUSED)
        sk_ui_caret_set(w, tb_caret_rect(w));
}

static int tb_sel(const textbox_priv *p, int *a, int *b)
{
    *a = imin(p->anchor, p->caret);
    *b = imax(p->anchor, p->caret);
    return *a != *b;
}

static int tb_reserve(sk_widget *w, int need)
{
    textbox_priv *p = w->priv;
    if (need + 1 <= p->cap)
        return 0;
    int ncap = p->cap ? p->cap : 32;
    while (ncap < need + 1)
        ncap *= 2;
    char *t = realloc(w->text, (size_t)ncap);
    if (!t)
        return -1;
    w->text = t;
    p->cap = ncap;
    return 0;
}

/* 删除 [a, b) */
static void tb_delete(sk_widget *w, int a, int b)
{
    textbox_priv *p = w->priv;
    if (a >= b)
        return;
    memmove(w->text + a, w->text + b, (size_t)(p->len - b + 1));
    p->len -= b - a;
    p->caret = p->anchor = a;
}

static int tb_delete_selection(sk_widget *w)
{
    textbox_priv *p = w->priv;
    int a, b;
    if (!tb_sel(p, &a, &b))
        return 0;
    tb_delete(w, a, b);
    return 1;
}

/* 在光标处插入（先替换掉选区）。单行输入框里换行和制表符变成空格，其它控制字符丢掉 */
static int tb_insert(sk_widget *w, const char *s, int n)
{
    textbox_priv *p = w->priv;
    char *clean = malloc((size_t)n + 1);
    if (!clean)
        return 0;
    int m = 0;
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\r' && i + 1 < n && s[i + 1] == '\n')
            continue;
        if (ch == '\n' || ch == '\r' || ch == '\t')
            ch = ' ';
        else if (ch < 0x20 || ch == 0x7F)
            continue;
        clean[m++] = (char)ch;
    }
    int changed = tb_delete_selection(w);
    if (p->len + m > TB_MAX) {                     /* 超长：截断，且不切开多字节字符 */
        m = TB_MAX - p->len;
        while (m > 0 && ((unsigned char)clean[m] & 0xC0) == 0x80)
            m--;
    }
    if (m > 0 && tb_reserve(w, p->len + m) == 0) {
        memmove(w->text + p->caret + m, w->text + p->caret, (size_t)(p->len - p->caret + 1));
        memcpy(w->text + p->caret, clean, (size_t)m);
        p->len += m;
        p->caret += m;
        p->anchor = p->caret;
        changed = 1;
    }
    free(clean);
    return changed;
}

static void tb_copy(sk_widget *w)
{
    textbox_priv *p = w->priv;
    int a, b;
    if (tb_sel(p, &a, &b))
        sk_widget_set_clipboard(w, w->text + a, b - a);
}

static void tb_notify_change(sk_widget *w)
{
    tb_changed(w);
    if (w->on_change)
        w->on_change(w, w->user);
}

/* 鼠标 x 坐标 → 字节位置 */
static int tb_hit(const sk_widget *w, int x)
{
    textbox_priv *p = w->priv;
    int local = x - (w->r.x + TB_PAD) + p->scroll;
    return sk_ui_text_hit(sk_widget_get_font(w), w->text, p->len, local);
}

static void textbox_draw(sk_widget *w, sk_canvas *c)
{
    textbox_priv *p = w->priv;
    const sk_font *f = sk_widget_get_font(w);
    sk_draw_textbox_frame(c, w->r, (int)w->state, w->seed);

    /* 文字只画在框内部（滚动时文字不能压到手绘边框上，左右各留几个像素） */
    sk_rect inner = sk_rect_make(w->r.x + TB_PAD - 3, w->r.y + 2, w->r.w - 2 * (TB_PAD - 3), w->r.h - 4);
    sk_rect old = sk_push_clip(c, inner);
    int base = center_baseline(f, w->r.y, w->r.h);
    int x0 = w->r.x + TB_PAD - p->scroll;
    int focused = (w->state & SK_STATE_FOCUSED) != 0;

    if (p->len == 0) {
        if (p->placeholder && *p->placeholder)
            sk_draw_text(c, f, w->r.x + TB_PAD, base, p->placeholder, SK_INK_FAINT);
    } else {
        int a, b;
        if (tb_sel(p, &a, &b)) {
            /* 选区：荧光笔涂一道；窗口/控件不在焦点时颜色淡一些 */
            int xa = x0 + tb_x_of(w, a), xb = x0 + tb_x_of(w, b);
            int top = base - sk_font_ascent(f) - 2, h = sk_font_ascent(f) + sk_font_descent(f) + 4;
            sk_color hl = focused ? SK_HIGHLIGHT : SK_WITH_ALPHA(SK_HIGHLIGHT, 0x48);
            sk_marker_highlight(c, sk_rect_make(xa, top, xb - xa, h), hl, w->seed);
        }
        sk_draw_text(c, f, x0, base, w->text, sk_widget_ink(w, SK_INK));
    }
    if (sk_ui_caret_visible(w)) {
        sk_rect cr = tb_caret_rect(w);
        float cx = cr.x + 1.5f;
        sk_line_aa(c, cx, (float)cr.y + 1, cx - 0.6f, (float)(cr.y + cr.h - 1), 1.7f, SK_INK);
    }
    sk_pop_clip(c, old);
}

static int textbox_key(sk_widget *w, const sk_event *e)
{
    textbox_priv *p = w->priv;
    uint32_t mods = e->key.modifiers;
    int ctrl = (mods & SK_MOD_CTRL) != 0, shift = (mods & SK_MOD_SHIFT) != 0;
    int a, b, has_sel = tb_sel(p, &a, &b);
    uint32_t cp = e->key.codepoint;

    if (e->key.state == 0) {
        /* 松开：普通字符的松开也算我们的（免得应用收到一串莫名其妙的松开事件） */
        return !ctrl && !(mods & SK_MOD_ALT) && cp >= 0x20 && cp != 0x7F;
    }

    switch (e->key.keycode) {
    case KEY_LEFT:
        if (has_sel && !shift)
            p->caret = a;
        else
            p->caret = ctrl ? sk_ui_word_left(w->text, p->caret) : sk_ui_utf8_prev(w->text, p->caret);
        if (!shift)
            p->anchor = p->caret;
        tb_changed(w);
        return 1;
    case KEY_RIGHT:
        if (has_sel && !shift)
            p->caret = b;
        else
            p->caret = ctrl ? sk_ui_word_right(w->text, p->len, p->caret)
                            : sk_ui_utf8_next(w->text, p->len, p->caret);
        if (!shift)
            p->anchor = p->caret;
        tb_changed(w);
        return 1;
    case KEY_HOME:
        p->caret = 0;
        if (!shift)
            p->anchor = 0;
        tb_changed(w);
        return 1;
    case KEY_END:
        p->caret = p->len;
        if (!shift)
            p->anchor = p->len;
        tb_changed(w);
        return 1;
    case KEY_BACKSPACE:
        if (!tb_delete_selection(w)) {
            if (p->caret == 0) {
                sk_ui_caret_reset(w);
                return 1;
            }
            int from = ctrl ? sk_ui_word_left(w->text, p->caret) : sk_ui_utf8_prev(w->text, p->caret);
            tb_delete(w, from, p->caret);
        }
        tb_notify_change(w);
        return 1;
    case KEY_DELETE:
        if (!tb_delete_selection(w)) {
            if (p->caret >= p->len) {
                sk_ui_caret_reset(w);
                return 1;
            }
            int to = ctrl ? sk_ui_word_right(w->text, p->len, p->caret)
                          : sk_ui_utf8_next(w->text, p->len, p->caret);
            tb_delete(w, p->caret, to);
        }
        tb_notify_change(w);
        return 1;
    case KEY_ENTER:
    case KEY_KPENTER:
        if (w->on_click)
            w->on_click(w, w->user);
        return 1;
    case KEY_TAB:
    case KEY_ESC:
    case KEY_UP:
    case KEY_DOWN:
    case KEY_PAGEUP:
    case KEY_PAGEDOWN:
        return 0;                      /* 交给 sk_ui（切换焦点）或应用（历史记录、关闭对话框……） */
    default:
        break;
    }

    if (ctrl && !(mods & SK_MOD_ALT)) {
        switch (e->key.keycode) {
        case KEY_A:
            p->anchor = 0;
            p->caret = p->len;
            tb_changed(w);
            return 1;
        case KEY_C:
            tb_copy(w);
            return 1;
        case KEY_X:
            if (has_sel) {
                tb_copy(w);
                tb_delete_selection(w);
                tb_notify_change(w);
            }
            return 1;
        case KEY_V:
            /* 剪贴板在服务器那里：发请求，内容稍后以 SK_EVENT_CLIPBOARD 送回给我们 */
            sk_widget_request_clipboard(w);
            return 1;
        default:
            return 0;                  /* Ctrl+S 之类留给应用 */
        }
    }

    if (e->key.keycode == 0 && cp == '\r') {
        if (w->on_click)
            w->on_click(w, w->user);
        return 1;
    }
    if (cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp < 0xA0) && !(mods & SK_MOD_ALT)) {
        char buf[4];
        int n = sk_utf8_encode(cp, buf);
        if (tb_insert(w, buf, n))
            tb_notify_change(w);
        else
            sk_ui_caret_reset(w);
        return 1;
    }
    return 0;
}

static int textbox_event(sk_widget *w, const sk_event *e)
{
    textbox_priv *p = w->priv;
    switch (e->type) {
    case SK_EVENT_MOUSE:
        if (e->mouse.kind == SK_MOUSE_DOWN && e->mouse.button == SK_BUTTON_LEFT) {
            int pos = tb_hit(w, e->mouse.x);
            if (e->mouse.clicks >= 2) {
                /* 双击选中一个词 */
                p->anchor = sk_ui_word_left(w->text, sk_ui_utf8_next(w->text, p->len, pos));
                p->caret = sk_ui_word_right(w->text, p->len, p->anchor);
                if (p->anchor == p->caret)
                    p->anchor = sk_ui_utf8_prev(w->text, pos), p->caret = pos;
            } else {
                p->caret = pos;
                if (!(e->mouse.modifiers & SK_MOD_SHIFT))
                    p->anchor = pos;
            }
            tb_changed(w);
            return 1;
        }
        if (e->mouse.kind == SK_MOUSE_MOVE && sk_ui_pressed(w->ui) == w && (e->mouse.buttons & SK_BUTTON_LEFT)) {
            /* 拖动选择：鼠标拖到框外时 tb_hit 会落到看不见的文字上，tb_changed 随之滚动 */
            int pos = tb_hit(w, e->mouse.x);
            if (pos != p->caret) {
                p->caret = pos;
                tb_changed(w);
            }
            return 1;
        }
        return e->mouse.kind != SK_MOUSE_WHEEL;
    case SK_EVENT_KEY:
        return textbox_key(w, e);
    case SK_EVENT_FOCUS:
        if (e->focused) {
            /* 用 Tab 键进入时全选（方便直接重新输入），鼠标点进来时不全选 */
            if (sk_ui_focus_visible(w->ui)) {
                p->anchor = 0;
                p->caret = p->len;
            }
            tb_changed(w);
        }
        return 1;
    case SK_EVENT_CLIPBOARD:
        if (tb_insert(w, e->clip.text, e->clip.text_len))
            tb_notify_change(w);
        return 1;
    default:
        return 0;
    }
}

static void textbox_resized(sk_widget *w)
{
    tb_changed(w);
}

static void textbox_destroy(sk_widget *w)
{
    textbox_priv *p = w->priv;
    if (p) {
        free(p->placeholder);
        free(p);
    }
    w->priv = NULL;
}

static void textbox_text_hook(sk_widget *w, const char *text)
{
    sk_textbox_set_text(w, text);
}

static const sk_widget_class textbox_class = {
    .name = "textbox", .draw = textbox_draw, .event = textbox_event,
    .destroy = textbox_destroy, .resized = textbox_resized, .focusable = 1,
};

sk_widget *sk_textbox_new(sk_ui *ui, sk_rect r, const char *initial, sk_widget_fn on_change, void *user)
{
    textbox_priv *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    sk_widget *w = sk_widget_new(ui, &textbox_class, r);
    if (!w) {
        free(p);
        return NULL;
    }
    w->priv = p;
    w->text = calloc(1, 32);
    p->cap = w->text ? 32 : 0;
    w->on_change = on_change;
    w->user = user;
    sk_widget_set_cursor(w, SK_CURSOR_TEXT);
    sk_widget_set_flags(w, SK_WF_NO_FOCUS_RING, 0);   /* 光标和输入框外框已经表示了焦点 */
    sk_widget_set_text_hook(w, textbox_text_hook);
    if (!w->text) {
        sk_widget_destroy(w);
        return NULL;
    }
    sk_textbox_set_text(w, initial);
    return w;
}

const char *sk_textbox_text(const sk_widget *w)
{
    return tb_priv(w) ? w->text : "";
}

void sk_textbox_set_text(sk_widget *w, const char *text)
{
    textbox_priv *p = tb_priv(w);
    if (!p || !sk_widget_alive(w))
        return;
    p->len = 0;
    w->text[0] = 0;
    p->caret = p->anchor = 0;
    if (text && *text)
        tb_insert(w, text, (int)strlen(text));
    p->caret = p->anchor = p->len;
    p->scroll = 0;
    tb_changed(w);
}

void sk_textbox_set_placeholder(sk_widget *w, const char *hint)
{
    textbox_priv *p = tb_priv(w);
    if (!p || !sk_widget_alive(w))
        return;
    free(p->placeholder);
    p->placeholder = hint ? strdup(hint) : NULL;
    if (p->len == 0)
        sk_widget_invalidate(w);
}

/* ================================================================ 标签页条 */

typedef struct {
    char **labels;
    int    n;
    int    active;
    int    hover;
} tabs_priv;

static const sk_widget_class tabs_class;

static void tabs_draw(sk_widget *w, sk_canvas *c)
{
    tabs_priv *p = w->priv;
    int hover = (w->state & SK_STATE_HOVER) ? p->hover : -1;
    sk_draw_tabs(c, w->r, (const char *const *)p->labels, p->n, p->active, hover, w->seed);
}

static int tabs_hit(const sk_widget *w, int x, int y)
{
    tabs_priv *p = w->priv;
    for (int i = 0; i < p->n; i++)
        if (sk_rect_contains(sk_tabs_item_rect(w->r, (const char *const *)p->labels, p->n, i), x, y))
            return i;
    return -1;
}

static void tabs_activate(sk_widget *w, int i)
{
    tabs_priv *p = w->priv;
    if (i < 0 || i >= p->n || i == p->active)
        return;
    p->active = i;
    sk_widget_invalidate(w);
    if (w->on_change)
        w->on_change(w, w->user);
}

static int tabs_event(sk_widget *w, const sk_event *e)
{
    tabs_priv *p = w->priv;
    if (e->type == SK_EVENT_MOUSE) {
        switch (e->mouse.kind) {
        case SK_MOUSE_ENTER:
        case SK_MOUSE_MOVE: {
            int h = tabs_hit(w, e->mouse.x, e->mouse.y);
            if (h != p->hover) {                   /* 只在悬停的标签变化时重画 */
                p->hover = h;
                sk_widget_invalidate(w);
            }
            return 1;
        }
        case SK_MOUSE_LEAVE:
            p->hover = -1;
            return 1;
        case SK_MOUSE_DOWN:                        /* 标签页按下就切换（不等松开），和大多数系统一样 */
            if (e->mouse.button == SK_BUTTON_LEFT)
                tabs_activate(w, tabs_hit(w, e->mouse.x, e->mouse.y));
            return 1;
        case SK_MOUSE_WHEEL:
            return 0;
        default:
            return 1;
        }
    }
    if (is_press(e)) {
        switch (e->key.keycode) {
        case KEY_LEFT:  tabs_activate(w, p->active - 1); return 1;
        case KEY_RIGHT: tabs_activate(w, p->active + 1); return 1;
        case KEY_HOME:  tabs_activate(w, 0); return 1;
        case KEY_END:   tabs_activate(w, p->n - 1); return 1;
        default: break;
        }
    }
    return 0;
}

static void tabs_destroy(sk_widget *w)
{
    tabs_priv *p = w->priv;
    if (!p)
        return;
    for (int i = 0; i < p->n; i++)
        free(p->labels[i]);
    free(p->labels);
    free(p);
    w->priv = NULL;
}

static const sk_widget_class tabs_class = {
    .name = "tabs", .draw = tabs_draw, .event = tabs_event, .destroy = tabs_destroy, .focusable = 1,
};

sk_widget *sk_tabs_new(sk_ui *ui, sk_rect r, const char *const *labels, int n,
                       sk_widget_fn on_change, void *user)
{
    if (n < 0)
        n = 0;
    tabs_priv *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->labels = calloc((size_t)(n ? n : 1), sizeof *p->labels);
    if (!p->labels) {
        free(p);
        return NULL;
    }
    for (int i = 0; i < n; i++)
        p->labels[i] = dup_or_empty(labels ? labels[i] : "");
    p->n = n;
    p->hover = -1;
    sk_widget *w = sk_widget_new(ui, &tabs_class, r);
    if (!w) {
        for (int i = 0; i < n; i++)
            free(p->labels[i]);
        free(p->labels);
        free(p);
        return NULL;
    }
    w->priv = p;
    w->on_change = on_change;
    w->user = user;
    make_clickable(w);
    return w;
}

int sk_tabs_active(const sk_widget *w)
{
    return (w && w->cls == &tabs_class) ? ((tabs_priv *)w->priv)->active : -1;
}

void sk_tabs_set_active(sk_widget *w, int index)
{
    if (!sk_widget_alive(w) || w->cls != &tabs_class)
        return;
    tabs_priv *p = w->priv;
    if (index < 0 || index >= p->n || index == p->active)
        return;
    p->active = index;
    sk_widget_invalidate(w);
}

/* ================================================================ 占用率条 */

typedef struct {
    int   permille;
    char *caption;
} meter_priv;

static const sk_widget_class meter_class;

static void meter_draw(sk_widget *w, sk_canvas *c)
{
    meter_priv *p = w->priv;
    sk_color fill = w->accent;
    if (w->state & SK_STATE_DISABLED)
        fill = sk_mix(fill, SK_PAPER, 150);
    sk_draw_meter(c, w->r, p->permille, fill, w->seed);
    if (p->caption && *p->caption) {
        const sk_font *f = sk_widget_get_font(w);
        /* 文字压在排线上不好认：先在文字后面垫一小块半透明的纸（像贴了张便签） */
        int tw = sk_text_width(f, p->caption);
        int th = sk_font_ascent(f) + sk_font_descent(f);
        sk_rect tag = sk_rect_make(w->r.x + (w->r.w - tw) / 2 - 4, w->r.y + (w->r.h - th) / 2 - 1, tw + 8, th + 2);
        sk_fill_rect(c, tag, SK_WITH_ALPHA(SK_PAPER, 0xC0));
        sk_draw_text_in_rect(c, f, w->r, p->caption, sk_widget_ink(w, SK_INK), SK_ALIGN_CENTER);
    }
}

static void meter_destroy(sk_widget *w)
{
    meter_priv *p = w->priv;
    if (p) {
        free(p->caption);
        free(p);
    }
    w->priv = NULL;
}

static const sk_widget_class meter_class = { .name = "meter", .draw = meter_draw, .destroy = meter_destroy };

sk_widget *sk_meter_new(sk_ui *ui, sk_rect r, sk_color fill)
{
    meter_priv *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    sk_widget *w = sk_widget_new(ui, &meter_class, r);
    if (!w) {
        free(p);
        return NULL;
    }
    w->priv = p;
    w->accent = fill ? fill : SK_BLUE;
    return w;
}

void sk_meter_set(sk_widget *w, int permille, const char *caption)
{
    if (!sk_widget_alive(w) || w->cls != &meter_class)
        return;
    meter_priv *p = w->priv;
    permille = permille < 0 ? 0 : permille > 1000 ? 1000 : permille;
    int same_caption = (!caption && !p->caption) ||
                       (caption && p->caption && strcmp(caption, p->caption) == 0);
    if (p->permille == permille && same_caption)
        return;                                   /* 系统监视器每秒调用：没变就不重画 */
    p->permille = permille;
    if (!same_caption) {
        free(p->caption);
        p->caption = caption ? strdup(caption) : NULL;
    }
    sk_widget_invalidate(w);
}

/* ================================================================ 滑块 */

typedef struct {
    int min, max, value;
} slider_priv;

static const sk_widget_class slider_class;

static int slider_knob_r(const sk_widget *w) { return imax(4, imin(w->r.h / 2 - 3, 9)); }

/* 轨道两端的 x 坐标 */
static void slider_track(const sk_widget *w, int *x0, int *x1)
{
    int kr = slider_knob_r(w);
    *x0 = w->r.x + kr + 3;
    *x1 = w->r.x + w->r.w - kr - 3;
    if (*x1 <= *x0)
        *x1 = *x0 + 1;
}

static int slider_knob_x(const sk_widget *w)
{
    slider_priv *p = w->priv;
    int x0, x1;
    slider_track(w, &x0, &x1);
    if (p->max <= p->min)
        return x0;
    return x0 + (int)((long long)(p->value - p->min) * (x1 - x0) / (p->max - p->min));
}

static void slider_draw(sk_widget *w, sk_canvas *c)
{
    slider_priv *p = w->priv;
    int x0, x1;
    slider_track(w, &x0, &x1);
    float ty = w->r.y + w->r.h / 2.0f;
    int kx = slider_knob_x(w);
    int disabled = (w->state & SK_STATE_DISABLED) != 0;
    (void)p;

    /* 已经走过的部分：荧光笔涂一道 */
    if (kx > x0) {
        sk_color hl = SK_WITH_ALPHA(disabled ? SK_INK_FAINT : w->accent, 0x80);
        sk_marker_highlight(c, sk_rect_make(x0, (int)ty - 4, kx - x0, 8), hl, w->seed);
    }
    /* 轨道：一条铅笔线 */
    sk_rough_style s;
    sk_style_pencil(&s, w->seed);
    s.stroke = disabled ? SK_INK_FAINT : SK_INK_SOFT;
    s.stroke_width = 1.4f;
    sk_rough_line(c, (float)x0, ty, (float)x1, ty, &s);

    /* 把手：一个铅笔画的圆，悬停/拖动时用主题色排线填充 */
    int kr = slider_knob_r(w);
    sk_rough_style k;
    if (!disabled && (w->state & (SK_STATE_HOVER | SK_STATE_PRESSED | SK_STATE_FOCUSED)))
        sk_style_hatched(&k, w->accent, w->seed ^ 0x51DEu);
    else {
        sk_style_pencil(&k, w->seed ^ 0x51DEu);
        k.fill_style = SK_FILL_SOLID;
        k.fill = SK_PAPER;
    }
    if (disabled)
        k.stroke = SK_INK_FAINT;
    /* 排线填充下面先垫一层纸，否则会透出轨道线 */
    sk_fill_circle_aa(c, (float)kx, ty, (float)kr, SK_PAPER);
    sk_rough_circle(c, (float)kx, ty, (float)(2 * kr), &k);
}

static void slider_set(sk_widget *w, int v, int notify)
{
    slider_priv *p = w->priv;
    if (v < p->min)
        v = p->min;
    if (v > p->max)
        v = p->max;
    if (v == p->value)
        return;
    p->value = v;
    sk_widget_invalidate(w);
    if (notify && w->on_change)
        w->on_change(w, w->user);
}

static int slider_value_at(const sk_widget *w, int x)
{
    slider_priv *p = w->priv;
    int x0, x1;
    slider_track(w, &x0, &x1);
    if (x <= x0)
        return p->min;
    if (x >= x1)
        return p->max;
    long long range = (long long)p->max - p->min;
    /* 四舍五入到最近的整数值 */
    return p->min + (int)(((long long)(x - x0) * range * 2 + (x1 - x0)) / (2LL * (x1 - x0)));
}

static int slider_event(sk_widget *w, const sk_event *e)
{
    slider_priv *p = w->priv;
    int range = p->max - p->min;
    int step = imax(1, range / 100), page = imax(1, range / 10);
    if (e->type == SK_EVENT_MOUSE) {
        switch (e->mouse.kind) {
        case SK_MOUSE_DOWN:
            if (e->mouse.button == SK_BUTTON_LEFT)
                slider_set(w, slider_value_at(w, e->mouse.x), 1);
            return 1;
        case SK_MOUSE_MOVE:
            if (sk_ui_pressed(w->ui) == w && (e->mouse.buttons & SK_BUTTON_LEFT))
                slider_set(w, slider_value_at(w, e->mouse.x), 1);
            return 1;
        case SK_MOUSE_WHEEL:
            slider_set(w, p->value + (e->mouse.wheel > 0 ? step : -step), 1);
            return 1;
        default:
            return 1;
        }
    }
    if (is_press(e)) {
        switch (e->key.keycode) {
        case KEY_LEFT: case KEY_DOWN:  slider_set(w, p->value - step, 1); return 1;
        case KEY_RIGHT: case KEY_UP:   slider_set(w, p->value + step, 1); return 1;
        case KEY_PAGEDOWN:             slider_set(w, p->value - page, 1); return 1;
        case KEY_PAGEUP:               slider_set(w, p->value + page, 1); return 1;
        case KEY_HOME:                 slider_set(w, p->min, 1); return 1;
        case KEY_END:                  slider_set(w, p->max, 1); return 1;
        default: break;
        }
    }
    return 0;
}

static const sk_widget_class slider_class = {
    .name = "slider", .draw = slider_draw, .event = slider_event, .destroy = free_priv, .focusable = 1,
};

sk_widget *sk_slider_new(sk_ui *ui, sk_rect r, int min, int max, int value,
                         sk_widget_fn on_change, void *user)
{
    slider_priv *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    sk_widget *w = sk_widget_new(ui, &slider_class, r);
    if (!w) {
        free(p);
        return NULL;
    }
    if (max < min) {
        int t = min;
        min = max;
        max = t;
    }
    p->min = min;
    p->max = max;
    p->value = value < min ? min : value > max ? max : value;
    w->priv = p;
    w->on_change = on_change;
    w->user = user;
    sk_widget_set_cursor(w, SK_CURSOR_HAND);
    return w;
}

int sk_slider_value(const sk_widget *w)
{
    return (w && w->cls == &slider_class) ? ((slider_priv *)w->priv)->value : 0;
}

void sk_slider_set_value(sk_widget *w, int value)
{
    if (sk_widget_alive(w) && w->cls == &slider_class)
        slider_set(w, value, 0);
}

/* ================================================================ 自定义区域 */

typedef struct {
    sk_custom_draw_fn  draw;
    sk_custom_event_fn event;
    void *user;
} custom_priv;

static void custom_draw(sk_widget *w, sk_canvas *c)
{
    custom_priv *p = w->priv;
    if (p->draw)
        p->draw(w, c, p->user);
}

static int custom_event(sk_widget *w, const sk_event *e)
{
    custom_priv *p = w->priv;
    return p->event ? p->event(w, e, p->user) : 0;
}

/* 四个类：是否可获得焦点 × 是否处理事件（没有 event 回调的自定义区域对鼠标透明，例如时钟表盘） */
static const sk_widget_class custom_classes[2][2] = {
    { { .name = "custom", .draw = custom_draw, .destroy = free_priv },
      { .name = "custom", .draw = custom_draw, .event = custom_event, .destroy = free_priv } },
    { { .name = "custom", .draw = custom_draw, .destroy = free_priv, .focusable = 1 },
      { .name = "custom", .draw = custom_draw, .event = custom_event, .destroy = free_priv, .focusable = 1 } },
};

sk_widget *sk_custom_new(sk_ui *ui, sk_rect r, sk_custom_draw_fn draw, sk_custom_event_fn event,
                         int focusable, void *user)
{
    custom_priv *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    /* 可获得焦点的自定义控件（终端、画布）一定要能收到按键，所以总是用带 event 的类 */
    const sk_widget_class *cls = &custom_classes[focusable ? 1 : 0][(event || focusable) ? 1 : 0];
    sk_widget *w = sk_widget_new(ui, cls, r);
    if (!w) {
        free(p);
        return NULL;
    }
    p->draw = draw;
    p->event = event;
    p->user = user;
    w->priv = p;
    w->user = user;
    /* 大块自定义区域：鼠标进出不自动整块重画（需要的话在 event 里自己 invalidate） */
    sk_widget_set_flags(w, SK_WF_NO_AUTO_REDRAW, 0);
    return w;
}
