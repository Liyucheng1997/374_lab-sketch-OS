/*
 * theme.c — SketchOS 的视觉语言：调色板、预设样式、标准控件外观、窗口装饰
 *
 * “外观”和“行为”分开：ui.c 决定按钮什么时候算按下、焦点在谁身上；这里只回答
 * “一个处于某种状态的按钮长什么样”。sketchd（窗口边框）和每个应用（控件）都调用
 * 同一套函数，所以整个系统的风格是统一的——这和真实桌面系统里“主题引擎”的分工一样。
 *
 * 几条共同的规矩：
 *   - 线条用 rough.h 的手绘图元，随机种子来自控件自己的 seed：同一个控件每次重绘一模一样；
 *     状态变化（悬停、按下）只改变填充，不改变轮廓（轮廓和填充是两条独立的随机数流）。
 *   - 控件可以在自己矩形外 SK_WIDGET_BLEED(4) 像素内作画，焦点框画在外面 3 像素处；
 *     所以轮廓画在矩形里面一点点，抖动和“出头”不会越界。
 *   - 彩色只用排线或荧光笔，大面积不平涂；文字永远清晰（深色墨水，必要时加一圈纸色描边）。
 */
#include <math.h>
#include <string.h>

#include "sketch/theme.h"
#include "gfx_internal.h"

/* ======================================================================
 * 调色板
 * 终端 16 色要画在暖白色的纸上：传统终端的“白色/亮白色”在白底上看不见，
 * 黄色也太浅。这里全部调成在纸上有足够对比度的蜡笔色。
 * ====================================================================== */
const sk_color sk_ansi_palette[16] = {
    SK_RGB(0x2B, 0x2B, 0x2B),   /* 0 黑：铅笔墨色 */
    SK_RGB(0xC4, 0x3E, 0x37),   /* 1 红 */
    SK_RGB(0x3B, 0x8A, 0x42),   /* 2 绿 */
    SK_RGB(0xA8, 0x7A, 0x00),   /* 3 黄（深芥末黄，白底上也看得清） */
    SK_RGB(0x2F, 0x5F, 0xB8),   /* 4 蓝 */
    SK_RGB(0x96, 0x45, 0xA8),   /* 5 品红 */
    SK_RGB(0x1E, 0x84, 0x80),   /* 6 青 */
    SK_RGB(0x8A, 0x8A, 0x8A),   /* 7 “白”= 浅灰 */
    SK_RGB(0x66, 0x66, 0x66),   /* 8 亮黑 = 深灰 */
    SK_RGB(0xE0, 0x60, 0x5A),   /* 9 亮红 */
    SK_RGB(0x4E, 0xA6, 0x55),   /* 10 亮绿 */
    SK_RGB(0xC9, 0x9A, 0x10),   /* 11 亮黄 */
    SK_RGB(0x4A, 0x7B, 0xD0),   /* 12 亮蓝 */
    SK_RGB(0xB8, 0x62, 0xC8),   /* 13 亮品红 */
    SK_RGB(0x2F, 0xA8, 0xA0),   /* 14 亮青 */
    SK_RGB(0xB4, 0xB4, 0xB4),   /* 15 亮白 = 淡灰 */
};

/* ======================================================================
 * 预设样式
 * ====================================================================== */

void sk_style_pencil(sk_rough_style *s, uint32_t seed)
{
    sk_rough_default(s);
    s->seed = seed;
}

void sk_style_hatched(sk_rough_style *s, sk_color fill, uint32_t seed)
{
    sk_rough_default(s);
    s->seed = seed;
    s->fill_style = SK_FILL_HACHURE;
    s->fill = fill;
}

/* 控件用的铅笔：比默认的规整一点（小控件上 2 像素的抖动就很显眼了） */
static void ui_pencil(sk_rough_style *s, uint32_t seed, sk_color ink, float width, float rough)
{
    sk_rough_default(s);
    s->seed = seed;
    s->stroke = ink;
    s->stroke_width = width;
    s->roughness = rough;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static float fminf2(float a, float b) { return a < b ? a : b; }

/* 文字加一圈纸色描边：画在排线上面时，笔画边缘不会和排线混在一起 */
static void text_halo(sk_canvas *c, const sk_font *f, sk_rect r, const char *t, sk_color ink,
                      sk_color halo, int align)
{
    static const signed char off[8][2] = {
        { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 },
    };
    for (int k = 0; k < 8; k++)
        sk_draw_text_in_rect(c, f, sk_rect_offset(r, off[k][0], off[k][1]), t, halo, align);
    sk_draw_text_in_rect(c, f, r, t, ink, align);
}

/* ======================================================================
 * 按钮
 * 平时：一个描了两遍的手绘圆角框；悬停：框里用主题色排线“涂”上颜色；
 * 按下：排线更密、整体往右下沉 1 像素；选中（开关按钮）：荧光笔涂满；
 * 禁用：淡灰色、只描一遍；有焦点：轮廓加粗（虚线焦点框由 ui.c 另外画）。
 * ====================================================================== */

static int draw_button_shape(sk_canvas *c, sk_rect r, int state, sk_color accent, uint32_t seed)
{
    int pressed = (state & SK_STATE_PRESSED) != 0;
    int disabled = (state & SK_STATE_DISABLED) != 0;
    int push = (pressed && !disabled) ? 1 : 0;
    float x = (float)r.x + 1.5f + (float)push, y = (float)r.y + 1.5f + (float)push;
    float w = (float)r.w - 3.0f, h = (float)r.h - 3.0f;
    if (w < 4.0f || h < 4.0f)
        return push;
    float rad = fminf2(h * 0.28f, 9.0f);
    sk_rough_style s;
    ui_pencil(&s, seed, SK_INK, 1.5f, 0.7f);
    if (disabled) {
        s.stroke = SK_INK_FAINT;
        s.double_stroke = 0;
    } else if (pressed) {
        s.fill_style = SK_FILL_HACHURE;
        s.fill = accent;
        s.hachure_gap = 3.2f;
        s.fill_weight = 1.5f;
    } else if (state & SK_STATE_CHECKED) {
        s.fill_style = SK_FILL_MARKER;
        s.fill = SK_WITH_ALPHA(accent, 0x70);
        s.hachure_gap = fminf2(h * 0.32f, 7.0f);
    } else if (state & SK_STATE_HOVER) {
        s.fill_style = SK_FILL_HACHURE;
        s.fill = SK_WITH_ALPHA(accent, 0xB0);
        s.hachure_gap = 4.6f;
        s.fill_weight = 1.3f;
    }
    if ((state & SK_STATE_FOCUSED) && !disabled)
        s.stroke_width = 1.9f;
    /* 填充先画在略小的框里，让排线留在轮廓里面 */
    if (s.fill_style != SK_FILL_NONE) {
        sk_rough_style f = s;
        f.stroke_width = 0;
        sk_rough_round_rect(c, x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f, rad, &f);
    }
    s.fill_style = SK_FILL_NONE;
    sk_rough_round_rect(c, x, y, w, h, rad, &s);
    return push;
}

void sk_draw_button_frame(sk_canvas *c, sk_rect r, int state, sk_color accent, uint32_t seed)
{
    if (!c || sk_rect_empty(r))
        return;
    draw_button_shape(c, r, state, accent, seed);
}

void sk_draw_button(sk_canvas *c, sk_rect r, const char *label, int state, sk_color accent, uint32_t seed)
{
    if (!c || sk_rect_empty(r))
        return;
    int push = draw_button_shape(c, r, state, accent, seed);
    if (!label || !*label)
        return;
    const sk_font *f = sk_font_get(r.h >= 34 ? SK_FONT_TITLE : SK_FONT_UI);
    sk_rect tr = sk_rect_make(r.x + 6 + push, r.y + push, r.w - 12, r.h);
    if (state & SK_STATE_DISABLED) {
        sk_draw_text_in_rect(c, f, tr, label, SK_INK_FAINT, SK_ALIGN_CENTER);
    } else if (state & (SK_STATE_HOVER | SK_STATE_PRESSED | SK_STATE_CHECKED)) {
        text_halo(c, f, tr, label, SK_INK, SK_WITH_ALPHA(SK_PAPER, 0xD0), SK_ALIGN_CENTER);
    } else {
        sk_draw_text_in_rect(c, f, tr, label, SK_INK, SK_ALIGN_CENTER);
    }
}

/* ======================================================================
 * 复选框：小方框 + 手写的对勾（勾的尾巴故意画出框外，像随手打的勾）
 * ====================================================================== */

static void draw_check_mark(sk_canvas *c, float x, float y, float s, sk_color color, uint32_t seed)
{
    sk_rng g;
    sk_rng_seed(&g, seed);
    sk_pointf p[4] = {
        { x + s * (0.16f + sk_rng_range(&g, -0.03f, 0.03f)), y + s * (0.50f + sk_rng_range(&g, -0.03f, 0.03f)) },
        { x + s * 0.30f, y + s * 0.66f },
        { x + s * (0.43f + sk_rng_range(&g, -0.02f, 0.02f)), y + s * 0.86f },
        { x + s * (1.02f + sk_rng_range(&g, -0.04f, 0.04f)), y + s * (-0.10f + sk_rng_range(&g, -0.04f, 0.04f)) },
    };
    float w = s < 14.0f ? 2.0f : s * 0.16f;
    sk_brush_stroke(c, p, 4, w * 1.1f, w * 0.55f, color, 0, seed);
}

void sk_draw_checkbox(sk_canvas *c, sk_rect r, const char *label, int state, uint32_t seed)
{
    if (!c || sk_rect_empty(r))
        return;
    int disabled = (state & SK_STATE_DISABLED) != 0;
    int bs = imin(r.h - 6, 18);
    if (bs < 8)
        bs = imax(4, imin(r.h, r.w) - 2);
    float bx = (float)r.x + 3.0f, by = (float)r.y + (float)(r.h - bs) * 0.5f;
    sk_rough_style s;
    ui_pencil(&s, seed, disabled ? SK_INK_FAINT : SK_INK, 1.4f, 0.55f);
    if (disabled)
        s.double_stroke = 0;
    if ((state & (SK_STATE_HOVER | SK_STATE_PRESSED)) && !disabled) {
        sk_rough_style f = s;
        f.stroke_width = 0;
        f.fill_style = SK_FILL_HACHURE;
        f.fill = SK_WITH_ALPHA(SK_BLUE, (state & SK_STATE_PRESSED) ? 0xE0 : 0x90);
        f.hachure_gap = (state & SK_STATE_PRESSED) ? 2.6f : 3.4f;
        f.fill_weight = 1.1f;
        sk_rough_rect(c, bx + 1.0f, by + 1.0f, (float)bs - 2.0f, (float)bs - 2.0f, &f);
    }
    if ((state & SK_STATE_FOCUSED) && !disabled)
        s.stroke_width = 1.8f;
    sk_rough_rect(c, bx, by, (float)bs, (float)bs, &s);
    if (state & SK_STATE_CHECKED)
        draw_check_mark(c, bx, by, (float)bs, disabled ? SK_INK_FAINT : SK_INK, sk_hash_u32(seed, 0xC4ECu));
    if (label && *label) {
        const sk_font *f = sk_font_get(SK_FONT_UI);
        int tx = r.x + 3 + bs + 9;
        sk_draw_text_in_rect(c, f, sk_rect_make(tx, r.y, r.x + r.w - tx, r.h), label,
                             disabled ? SK_INK_FAINT : SK_INK, SK_ALIGN_LEFT);
    }
}

/* ======================================================================
 * 输入框：像纸质表格上的手写格子——淡淡的框，底边是一条更实的“横线”
 * ====================================================================== */

void sk_draw_textbox_frame(sk_canvas *c, sk_rect r, int state, uint32_t seed)
{
    if (!c || r.w < 6 || r.h < 6)
        return;
    int disabled = (state & SK_STATE_DISABLED) != 0;
    int focused = (state & SK_STATE_FOCUSED) && !disabled;
    float x = (float)r.x + 1.5f, y = (float)r.y + 1.5f, w = (float)r.w - 3.0f, h = (float)r.h - 3.0f;
    sk_color box = disabled ? SK_INK_FAINT : focused ? SK_FOCUS :
                   (state & SK_STATE_HOVER) ? SK_INK_SOFT : SK_INK_FAINT;
    sk_rough_style s;
    ui_pencil(&s, seed, box, focused ? 1.5f : 1.2f, 0.45f);
    s.double_stroke = focused;
    sk_rough_rect(c, x, y, w, h, &s);
    /* 底边的“书写线” */
    sk_rough_style u;
    ui_pencil(&u, sk_hash_u32(seed, 0xB07Du), disabled ? SK_INK_FAINT : focused ? SK_FOCUS : SK_INK_SOFT,
              focused ? 1.8f : 1.5f, 0.4f);
    u.double_stroke = 0;
    sk_rough_line(c, x + 2.0f, y + h - 0.5f, x + w - 2.0f, y + h - 0.5f, &u);
}

/* ======================================================================
 * 占用率条：手绘框，里面按比例用彩色蜡笔排线涂满
 * ====================================================================== */

void sk_draw_meter(sk_canvas *c, sk_rect r, int permille, sk_color fill, uint32_t seed)
{
    if (!c || r.w < 6 || r.h < 4)
        return;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    float x = (float)r.x + 1.5f, y = (float)r.y + 1.5f, w = (float)r.w - 3.0f, h = (float)r.h - 3.0f;
    float fw = (w - 2.0f) * (float)permille / 1000.0f;
    if (fw >= 1.0f) {
        sk_pointf q[4] = { { x + 1, y + 1 }, { x + 1 + fw, y + 1 }, { x + 1 + fw, y + h - 1 }, { x + 1, y + h - 1 } };
        /* 先一层很淡的底色，再排线：远看是“涂满了”，近看是一笔一笔的蜡笔 */
        sk_fill_polygon(c, q, 4, SK_WITH_ALPHA(fill, 0x38));
        sk_rough_style f;
        ui_pencil(&f, sk_hash_u32(seed, 0x3E7Eu), fill, 0, 0.6f);
        f.stroke_width = 0;
        f.fill_style = SK_FILL_HACHURE;
        f.fill = fill;
        f.hachure_gap = h < 14.0f ? 2.8f : 3.6f;
        f.fill_weight = 1.5f;
        sk_rough_fill_polygon(c, q, 4, &f);
    }
    sk_rough_style s;
    ui_pencil(&s, seed, SK_INK, 1.4f, 0.55f);
    sk_rough_rect(c, x, y, w, h, &s);
}

/* ======================================================================
 * 标签页：像笔记本侧面贴的彩色索引签
 * ====================================================================== */

static const sk_color tab_colors[8] = {
    SK_BLUE, SK_GREEN, SK_ORANGE, SK_PURPLE, SK_TEAL, SK_PINK, SK_YELLOW, SK_RED,
};

#define TAB_PAD   16     /* 文字左右留白 */
#define TAB_MINW  56
#define TAB_GAP   3
#define TAB_X0    6

/* 计算第 i 个标签的矩形；超出宽度时等比压缩（文字会被截断成“…”） */
static sk_rect tab_rect(sk_rect r, const char *const *labels, int n, int i)
{
    if (n <= 0 || i < 0 || i >= n || sk_rect_empty(r))
        return sk_rect_make(0, 0, 0, 0);
    const sk_font *f = sk_font_get(SK_FONT_UI);
    int total = TAB_X0, xi = 0, wi = 0;
    for (int k = 0; k < n; k++) {
        int tw = (labels && labels[k]) ? sk_text_width(f, labels[k]) : 0;
        int w = imax(tw + 2 * TAB_PAD, TAB_MINW);
        if (k == i) {
            xi = total;
            wi = w;
        }
        total += w + TAB_GAP;
    }
    int avail = r.w - TAB_X0 - 4;
    int need = total - TAB_X0;
    if (need > avail && avail > 0) {
        /* 等比缩放：用 64 位防止大数相乘溢出 */
        xi = TAB_X0 + (int)((int64_t)(xi - TAB_X0) * avail / need);
        wi = (int)((int64_t)(wi + TAB_GAP) * avail / need) - TAB_GAP;
        if (wi < 8) wi = 8;
    }
    return sk_rect_make(r.x + xi, r.y, wi, r.h);
}

sk_rect sk_tabs_item_rect(sk_rect r, const char *const *labels, int n, int i)
{
    return tab_rect(r, labels, n, i);
}

/* 一个标签的轮廓点：底边两端，两条略向内斜的侧边，顶上两个小圆角 */
static int tab_outline(sk_pointf *q, float x, float yt, float w, float yb)
{
    float sl = fminf2(5.0f, w * 0.1f);
    float cr = fminf2(5.0f, (yb - yt) * 0.3f);
    q[0] = (sk_pointf){ x, yb };
    q[1] = (sk_pointf){ x + sl * 0.7f, yt + cr };
    q[2] = (sk_pointf){ x + sl + cr * 0.6f, yt };
    q[3] = (sk_pointf){ x + w - sl - cr * 0.6f, yt };
    q[4] = (sk_pointf){ x + w - sl * 0.7f, yt + cr };
    q[5] = (sk_pointf){ x + w, yb };
    return 6;
}

void sk_draw_tabs(sk_canvas *c, sk_rect r, const char *const *labels, int n, int active, int hover, uint32_t seed)
{
    if (!c || sk_rect_empty(r) || n <= 0)
        return;
    const sk_font *f = sk_font_get(SK_FONT_UI);
    float yb = (float)(r.y + r.h) - 2.0f;
    sk_rect ar = sk_rect_make(0, 0, 0, 0);
    /* 先画未选中的标签，最后画选中的那一个（它要盖住底线、显得在最前面） */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            int is_active = (i == active);
            if (is_active != pass)
                continue;
            sk_rect tr = tab_rect(r, labels, n, i);
            if (sk_rect_empty(tr))
                continue;
            uint32_t ts = sk_hash_u32(seed, (uint32_t)i + 1u);
            float x = (float)tr.x, w = (float)tr.w;
            float yt = (float)r.y + (is_active ? 1.5f : 5.0f);
            sk_pointf q[6];
            tab_outline(q, x, yt, w, yb + (is_active ? 1.0f : 0.0f));
            sk_color col = tab_colors[i & 7];
            if (is_active) {
                ar = tr;
                sk_fill_polygon(c, q, 6, SK_PAPER);
                /* 索引签顶上的一道彩色荧光笔 */
                int mh = imax(4, imin(7, r.h / 5));
                sk_marker_highlight(c, sk_rect_make(tr.x + 6, r.y + 3, tr.w - 12, mh), SK_WITH_ALPHA(col, 0x90), ts);
            } else {
                sk_rough_style fs;
                ui_pencil(&fs, ts, col, 0, 0.6f);
                fs.stroke_width = 0;
                fs.fill_style = SK_FILL_HACHURE;
                fs.fill = SK_WITH_ALPHA(col, i == hover ? 0xD0 : 0x78);
                fs.hachure_gap = i == hover ? 3.4f : 4.6f;
                fs.fill_weight = 1.2f;
                sk_pointf in[6];
                tab_outline(in, x + 1.5f, yt + 1.5f, w - 3.0f, yb);
                sk_rough_fill_polygon(c, in, 6, &fs);
            }
            sk_rough_style s;
            ui_pencil(&s, ts ^ 0x7AB5u, is_active ? SK_INK : SK_INK_SOFT, is_active ? 1.5f : 1.3f, 0.5f);
            s.double_stroke = is_active;
            sk_rough_polyline(c, q, 6, &s);
            const char *lab = (labels && labels[i]) ? labels[i] : "";
            sk_rect lr = sk_rect_make(tr.x + 4, (int)yt + 2, tr.w - 8, (int)(yb - yt) - 1);
            if (is_active)
                sk_draw_text_in_rect(c, f, lr, lab, SK_INK, SK_ALIGN_CENTER);
            else
                text_halo(c, f, lr, lab, i == hover ? SK_INK : SK_INK_SOFT, SK_WITH_ALPHA(SK_PAPER, 0xC0),
                          SK_ALIGN_CENTER);
        }
    }
    /* 底线：选中标签下面断开 */
    sk_rough_style b;
    ui_pencil(&b, sk_hash_u32(seed, 0xBA5Eu), SK_INK, 1.4f, 0.45f);
    b.double_stroke = 0;
    float x0 = (float)r.x + 1.0f, x1 = (float)(r.x + r.w) - 1.0f;
    if (!sk_rect_empty(ar)) {
        if ((float)ar.x - x0 > 2.0f)
            sk_rough_line(c, x0, yb, (float)ar.x + 0.5f, yb, &b);
        b.seed ^= 0x55u;
        if (x1 - (float)(ar.x + ar.w) > 2.0f)
            sk_rough_line(c, (float)(ar.x + ar.w) - 0.5f, yb, x1, yb, &b);
    } else {
        sk_rough_line(c, x0, yb, x1, yb, &b);
    }
}

/* ======================================================================
 * 滚动条：一条淡淡的轨道线 + 一个排线填充的胶囊形滑块
 * ====================================================================== */

void sk_draw_scrollbar(sk_canvas *c, sk_rect r, int total, int visible, int offset, int state, uint32_t seed)
{
    if (!c || r.w < 4 || r.h < 4)
        return;
    int vertical = r.h >= r.w;
    float len = vertical ? (float)r.h - 4.0f : (float)r.w - 4.0f;
    float thick = vertical ? (float)r.w : (float)r.h;
    float cx = (float)r.x + (float)r.w * 0.5f, cy = (float)r.y + (float)r.h * 0.5f;
    /* 轨道 */
    sk_rough_style t;
    ui_pencil(&t, sk_hash_u32(seed, 0x7A4Cu), SK_INK_FAINT, 1.2f, 0.4f);
    t.double_stroke = 0;
    if (vertical)
        sk_rough_line(c, cx, (float)r.y + 3.0f, cx, (float)(r.y + r.h) - 3.0f, &t);
    else
        sk_rough_line(c, (float)r.x + 3.0f, cy, (float)(r.x + r.w) - 3.0f, cy, &t);
    /* 滑块位置 */
    int scrollable = total > visible && visible > 0;
    float tl = len, tp = 0.0f;
    if (scrollable) {
        tl = len * (float)visible / (float)total;
        float minl = fminf2(22.0f, len);
        if (tl < minl) tl = minl;
        int maxoff = total - visible;
        int off = offset < 0 ? 0 : offset > maxoff ? maxoff : offset;
        tp = (len - tl) * (float)off / (float)maxoff;
    }
    float tw = thick - 5.0f;
    if (tw < 3.0f) tw = 3.0f;
    float bx, by, bw, bh;
    if (vertical) {
        bx = cx - tw * 0.5f; by = (float)r.y + 2.0f + tp; bw = tw; bh = tl;
    } else {
        bx = (float)r.x + 2.0f + tp; by = cy - tw * 0.5f; bw = tl; bh = tw;
    }
    int disabled = (state & SK_STATE_DISABLED) || !scrollable;
    sk_color fillc = disabled ? SK_WITH_ALPHA(SK_INK_FAINT, 0x60) :
                     (state & SK_STATE_PRESSED) ? SK_BLUE :
                     (state & SK_STATE_HOVER) ? SK_WITH_ALPHA(SK_BLUE, 0xC0) : SK_WITH_ALPHA(SK_INK_SOFT, 0x90);
    sk_rough_style f;
    ui_pencil(&f, seed, SK_INK, 0, 0.5f);
    f.stroke_width = 0;
    f.fill_style = SK_FILL_HACHURE;
    f.fill = fillc;
    f.hachure_gap = (state & SK_STATE_PRESSED) ? 2.6f : 3.4f;
    f.fill_weight = 1.2f;
    float rr = fminf2(bw, bh) * 0.5f;
    sk_rough_round_rect(c, bx + 1.0f, by + 1.0f, bw - 2.0f, bh - 2.0f, rr - 1.0f, &f);
    sk_rough_style s;
    ui_pencil(&s, seed, disabled ? SK_INK_FAINT : SK_INK_SOFT, 1.3f, 0.45f);
    s.double_stroke = !disabled;
    sk_rough_round_rect(c, bx, by, bw, bh, rr, &s);
}

/* ======================================================================
 * 焦点框：沿矩形内侧一圈手绘虚线。所有虚线放在“同一笔”里光栅化，只合成一次。
 * ====================================================================== */

void sk_draw_focus_ring(sk_canvas *c, sk_rect r, uint32_t seed)
{
    if (!c || r.w < 4 || r.h < 4)
        return;
    float x0 = (float)r.x + 1.2f, y0 = (float)r.y + 1.2f;
    float x1 = (float)(r.x + r.w) - 1.2f, y1 = (float)(r.y + r.h) - 1.2f;
    sk__stroke st;
    if (!sk__stroke_begin(&st, c, x0 - 2.0f, y0 - 2.0f, x1 + 2.0f, y1 + 2.0f))
        return;
    sk_rng g;
    sk_rng_seed(&g, seed);
    const float dash = 5.0f, gap = 3.5f, rad = 0.7f, jit = 0.35f;
    /* 四条边首尾相接，虚线的节奏沿整圈连续 */
    sk_pointf v[5] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
    const float period = dash + gap;
    float phase0 = sk_rng_range(&g, 0.0f, period);   /* 第一段虚线从周长上的哪里开始 */
    float g0 = 0.0f;                                  /* 这条边起点在整圈周长上的位置 */
    for (int e = 0; e < 4; e++) {
        float dx = v[e + 1].x - v[e].x, dy = v[e + 1].y - v[e].y;
        float L = sqrtf(dx * dx + dy * dy);
        if (L < 0.5f)
            continue;
        float ux = dx / L, uy = dy / L;
        int k = (int)floorf((g0 - phase0) / period) - 1;
        for (;; k++) {
            float a = (float)k * period + phase0 - g0, b = a + dash;
            if (a >= L)
                break;
            if (a < 0.0f) a = 0.0f;
            if (b > L) b = L;
            if (b - a > 0.8f) {
                float o0 = sk_rng_range(&g, -jit, jit), o1 = sk_rng_range(&g, -jit, jit);
                sk__stroke_move(&st, v[e].x + ux * a - uy * o0, v[e].y + uy * a + ux * o0, rad);
                sk__stroke_line(&st, v[e].x + ux * b - uy * o1, v[e].y + uy * b + ux * o1, rad);
            }
        }
        g0 += L;
    }
    sk__stroke_end(&st, SK_FOCUS, 0, 0);
}

void sk_draw_separator(sk_canvas *c, int x1, int x2, int y, uint32_t seed)
{
    if (!c)
        return;
    sk_rough_style s;
    ui_pencil(&s, seed, SK_INK_FAINT, 1.2f, 0.5f);
    s.double_stroke = 0;
    sk_rough_line(c, (float)x1, (float)y + 0.5f, (float)x2, (float)y + 0.5f, &s);
}

/* ======================================================================
 * 窗口装饰
 *
 * 几何（content 是内容区，屏幕坐标）：
 *        ┌──────────────── 标题栏 SK_TITLE_H ───────────── [–][□][×] ┐   ← 外框 box
 *        │ ┌──────────────────── content ──────────────────────────┐ │
 *        │ │                                                       │ │   ← 左右下边框 SK_BORDER
 *        │ └───────────────────────────────────────────────────────┘ │
 *        └───────────────────────────────────────────────────────────┘▒
 *          ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒ 排线阴影（偏移 SK_SHADOW_OFF）▒▒▒▒▒▒▒▒
 * 外接矩形 = box ∪ 阴影，再四周留 FRAME_MARGIN 给手绘线的抖动和“出头”。
 * ====================================================================== */

#define FRAME_MARGIN  4
#define FRAME_BTN     22     /* 标题栏按钮的绘制尺寸 */
#define FRAME_PITCH   28     /* 相邻按钮中心的距离 */
#define FRAME_HIT     27     /* 命中区域（比画出来的大一点，好点；小于 PITCH，互不重叠） */
#define FRAME_RIGHT   7      /* 关闭按钮离右边框的距离 */

static sk_rect frame_box(sk_rect content)
{
    return sk_rect_make(content.x - SK_BORDER, content.y - SK_TITLE_H,
                        content.w + 2 * SK_BORDER, content.h + SK_TITLE_H + SK_BORDER);
}

sk_rect sk_frame_outer_rect(sk_rect content)
{
    sk_rect b = frame_box(content);
    return sk_rect_make(b.x - FRAME_MARGIN, b.y - FRAME_MARGIN,
                        b.w + SK_SHADOW_OFF + 2 * FRAME_MARGIN, b.h + SK_SHADOW_OFF + 2 * FRAME_MARGIN);
}

/* 按钮中心（which: 0 关闭 1 最大化 2 最小化，从右往左排）。
 * 窗口太窄放不下时，从最小化开始依次省略（返回 0）。 */
static int frame_button_center(sk_rect content, int which, float *cx, float *cy)
{
    sk_rect b = frame_box(content);
    float right = (float)(b.x + b.w) - FRAME_RIGHT - FRAME_BTN * 0.5f;
    *cx = right - (float)which * FRAME_PITCH;
    *cy = (float)b.y + SK_TITLE_H * 0.5f + 0.5f;
    return *cx - FRAME_HIT * 0.5f >= (float)b.x + 4.0f;
}

sk_rect sk_frame_button_rect(sk_rect content, int which)
{
    float cx, cy;
    if (which < 0 || which > 2 || !frame_button_center(content, which, &cx, &cy))
        return sk_rect_make(0, 0, 0, 0);
    return sk_rect_make((int)floorf(cx - FRAME_HIT * 0.5f), (int)floorf(cy - FRAME_HIT * 0.5f), FRAME_HIT, FRAME_HIT);
}

static void frame_button(sk_canvas *c, int which, float cx, float cy, int focused, int hover, uint32_t seed)
{
    const float R = FRAME_BTN * 0.5f;
    sk_color ink = focused ? SK_INK : SK_INK_SOFT;
    sk_rough_style s;
    ui_pencil(&s, seed, ink, 1.5f, 0.5f);
    if (which == 0) {
        /* 关闭：红色手绘圆圈 + ×；悬停时圆圈里涂满红色排线 */
        sk_rough_style o = s;
        o.stroke = focused || hover ? SK_RED : sk_mix(SK_RED, SK_PAPER, 110);
        o.stroke_width = 1.6f;
        if (hover) {
            o.fill_style = SK_FILL_HACHURE;
            o.fill = SK_RED;
            o.hachure_gap = 2.6f;
            o.fill_weight = 1.4f;
        }
        sk_rough_circle(c, cx, cy, R * 2.0f - 1.0f, &o);
        float k = R * 0.42f;
        sk_rough_style x = s;
        x.stroke_width = hover ? 2.0f : 1.7f;
        x.roughness = 0.4f;
        sk_rough_line(c, cx - k, cy - k, cx + k, cy + k, &x);
        x.seed ^= 0x99u;
        sk_rough_line(c, cx + k, cy - k, cx - k, cy + k, &x);
        return;
    }
    if (hover) {
        sk_rough_style h = s;
        h.stroke = SK_INK_SOFT;
        h.stroke_width = 1.2f;
        h.double_stroke = 0;
        h.fill_style = SK_FILL_HACHURE;
        h.fill = SK_WITH_ALPHA(SK_BLUE, 0xB0);
        h.hachure_gap = 3.0f;
        h.fill_weight = 1.2f;
        sk_rough_circle(c, cx, cy, R * 2.0f - 1.0f, &h);
    }
    if (which == 1) {
        float k = R * 0.48f;
        sk_rough_rect(c, cx - k, cy - k, 2.0f * k, 2.0f * k, &s);
    } else {
        float k = R * 0.5f;
        s.stroke_width = 1.9f;
        sk_rough_line(c, cx - k, cy + R * 0.3f, cx + k, cy + R * 0.3f, &s);
    }
}

/* 阴影 + 外框：会被调用好几次，每次裁剪到“外接矩形减去内容区”的一块（见下） */
static void frame_outline_and_shadow(sk_canvas *c, sk_rect b, int focused, uint32_t seed)
{
    float bx = (float)b.x, by = (float)b.y, bw = (float)b.w, bh = (float)b.h;
    const float S = SK_SHADOW_OFF;
    /* 排线阴影：只画露在窗口右边和下边的那个 L 形，所以也可以画在透明图层上 */
    sk_pointf L[6] = {
        { bx + bw + 1.0f, by + S }, { bx + bw + S, by + S }, { bx + bw + S, by + bh + S },
        { bx + S, by + bh + S }, { bx + S, by + bh + 1.0f }, { bx + bw + 1.0f, by + bh + 1.0f },
    };
    sk_rough_style sh;
    ui_pencil(&sh, sk_hash_u32(seed, 0x5AD0u), SK_SHADOW, 0, 0.5f);
    sh.stroke_width = 0;
    sh.fill_style = SK_FILL_HACHURE;
    sh.fill = focused ? SK_SHADOW : SK_WITH_ALPHA(SK_SHADOW, 0xA0);
    sh.hachure_gap = 3.6f;
    sh.fill_weight = 1.1f;
    sh.hachure_angle = -50.0f;
    sk_rough_fill_polygon(c, L, 6, &sh);
    /* 外框：描两遍的铅笔线，角上出头 */
    sk_rough_style o;
    ui_pencil(&o, seed, focused ? SK_INK : SK_INK_SOFT, 1.8f, 0.75f);
    sk_rough_rect(c, bx + 0.5f, by + 0.5f, bw - 1.0f, bh - 1.0f, &o);
}

void sk_draw_window_frame(sk_canvas *c, sk_rect content, const char *title, int focused,
                          int hover_button, sk_color accent, uint32_t seed)
{
    if (!c || content.w < 0 || content.h < 0)
        return;
    sk_rect outer = sk_frame_outer_rect(content);
    sk_rect old = sk_push_clip(c, outer);
    if (sk_rect_empty(c->clip)) {
        sk_pop_clip(c, old);
        return;
    }
    sk_rect b = frame_box(content);
    float bx = (float)b.x, by = (float)b.y, bw = (float)b.w;
    int cx1 = content.x + content.w, cy1 = content.y + content.h;
    int ox1 = outer.x + outer.w, oy1 = outer.y + outer.h;
    /* “外接矩形减去内容区”拆成互不重叠的 4 块。手绘线条会抖动，靠裁剪保证
     * 装饰绝对不会画进内容区（那是应用自己的像素）。 */
    sk_rect zone[4] = {
        sk_rect_make(outer.x, outer.y, outer.w, content.y - outer.y),     /* 上：标题栏 */
        sk_rect_make(outer.x, content.y, content.x - outer.x, content.h), /* 左 */
        sk_rect_make(cx1, content.y, ox1 - cx1, content.h),               /* 右 */
        sk_rect_make(outer.x, cy1, outer.w, oy1 - cy1),                   /* 下 */
    };

    /* 1. 边框和标题栏铺上纸 */
    sk_rect bands[4] = {
        sk_rect_make(b.x, b.y, b.w, SK_TITLE_H),
        sk_rect_make(b.x, content.y, SK_BORDER, content.h + SK_BORDER),
        sk_rect_make(cx1, content.y, SK_BORDER, content.h + SK_BORDER),
        sk_rect_make(content.x, cy1, content.w, SK_BORDER),
    };
    /* 纹理以内容区左上角为原点：窗口移动、边框被拆成几块画布分别画，纸纹都连续 */
    for (int i = 0; i < 4; i++)
        sk__paper_fill_at(c, bands[i], i == 0 ? SK_PAPER_DARK : SK_PAPER, 4, seed, content.x, content.y);

    /* 2. 标题栏里的东西：只在上面那块里画 */
    sk_rect mid = sk_push_clip(c, zone[0]);
    if (!sk_rect_empty(c->clip)) {
        /* 标题栏上色：有焦点时用应用主题色排线，没焦点时只是淡淡的灰色 */
        sk_pointf q[4] = {
            { bx + 2.0f, by + 2.0f }, { bx + bw - 2.0f, by + 2.0f },
            { bx + bw - 2.0f, by + SK_TITLE_H - 2.0f }, { bx + 2.0f, by + SK_TITLE_H - 2.0f },
        };
        sk_rough_style tf;
        ui_pencil(&tf, sk_hash_u32(seed, 0x717Eu), accent, 0, 0.6f);
        tf.stroke_width = 0;
        tf.fill_style = SK_FILL_HACHURE;
        tf.fill = focused ? SK_WITH_ALPHA(accent, 0xC8) : SK_WITH_ALPHA(SK_SHADOW, 0x70);
        tf.hachure_gap = focused ? 4.2f : 5.5f;
        tf.fill_weight = focused ? 1.6f : 1.2f;
        sk_rough_fill_polygon(c, q, 4, &tf);

        /* 标题文字：有焦点时写在一张贴上去的“纸条”上，排线不会干扰阅读 */
        int nbtn = 0;
        float bcx, bcy;
        for (int i = 0; i < 3; i++)
            if (frame_button_center(content, i, &bcx, &bcy))
                nbtn = i + 1;
        int text_right = b.x + b.w - FRAME_RIGHT - nbtn * FRAME_PITCH - 8;
        if (title && *title) {
            const sk_font *f = sk_font_get(SK_FONT_TITLE);
            int maxw = text_right - (b.x + 22) - 6;
            int tw = sk_text_width(f, title);
            if (tw > maxw) tw = maxw;
            if (tw > 4) {
                int lh = SK_TITLE_H - 9;
                float lx = bx + 12.0f, ly = by + 4.5f;
                if (focused) {
                    sk_rough_style lab;
                    ui_pencil(&lab, sk_hash_u32(seed, 0x1ABEu), SK_INK, 1.2f, 0.5f);
                    lab.double_stroke = 0;
                    lab.fill_style = SK_FILL_SOLID;
                    lab.fill = SK_PAPER;
                    sk_rough_round_rect(c, lx, ly, (float)tw + 20.0f, (float)lh, 5.0f, &lab);
                }
                sk_draw_text_in_rect(c, f, sk_rect_make((int)lx + 10, (int)ly, tw + 2, lh), title,
                                     focused ? SK_INK : SK_INK_SOFT, SK_ALIGN_LEFT);
            }
        }

        /* 标题栏和内容区之间的分隔线 */
        sk_rough_style sep;
        ui_pencil(&sep, sk_hash_u32(seed, 0x5E9Au), focused ? SK_INK : SK_INK_SOFT, 1.3f, 0.35f);
        sep.double_stroke = 0;
        sk_rough_line(c, bx + 1.0f, (float)content.y - 1.5f, bx + bw - 1.0f, (float)content.y - 1.5f, &sep);

        /* 标题栏按钮（窗口太窄时省略放不下的） */
        for (int i = 0; i < 3; i++) {
            if (frame_button_center(content, i, &bcx, &bcy))
                frame_button(c, i, bcx, bcy, focused, hover_button == i, sk_hash_u32(seed, 0xB7700u + (uint32_t)i));
        }
    }
    sk_pop_clip(c, mid);

    /* 3. 阴影和外框：四块分别裁剪着画（每块里看不见的线条在光栅化前就被跳过，代价很小） */
    for (int i = 0; i < 4; i++) {
        mid = sk_push_clip(c, zone[i]);
        if (!sk_rect_empty(c->clip))
            frame_outline_and_shadow(c, b, focused, seed);
        sk_pop_clip(c, mid);
    }
    sk_pop_clip(c, old);
}
