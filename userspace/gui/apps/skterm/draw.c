/*
 * draw.c — 把字符网格画到窗口上
 *
 * 终端是整个系统里“画得最多”的程序：ls 一下就是几百个字，cat 一个大文件就是成千上万行滚过。
 * 网页版跑在 v86 模拟器里，CPU 比真机慢几十倍，所以这里处处在省：
 *
 *   1. 只画变了的格子。vt.c 记录每一行的脏列区间，打一个字通常只重画两三个格子
 *      （外加光标），提交给 sketchd 的 damage 也只有这么大，服务器只重新合成这一小块。
 *   2. 合并：pty 一次可能读到很多数据（甚至几百 KB），我们在读的时候只更新网格、记脏区，
 *      等这一轮事件循环结束（空闲回调）再统一画一次——中间状态根本不画。
 *   3. 滚屏不重画文字：整屏上滚 n 行时（或者用滚轮往回翻历史时），把窗口缓冲区里的像素整体
 *      搬 n 行（每个像素行一次 memmove），只有新露出来的行需要真正画字。
 *   4. 没事的时候什么也不做：没有光标闪烁定时器，进程睡在 poll() 里。
 *
 * 外观（手绘风格）：底色是带细颗粒的纸（sk_paper_fill；颗粒跟着文字一起滚动，所以搬像素和
 * 重新画的结果逐像素相同），左边一条淡红色的“笔记本边线”，光标是铅笔涂出来的小方块，
 * 选中的文字用荧光笔涂黄，右边是手绘滚动条，响铃时文字区四周闪一下橙色的框。
 * 所有颜色都来自 theme.h（程序自己用 256 色/真彩色指定的颜色除外）。
 */
#include "skterm.h"

#include <skutil.h>

#include <stdlib.h>
#include <string.h>

enum { CUR_BLOCK = 1, CUR_HOLLOW, CUR_BAR, CUR_UNDER };

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ======================================================================
 * 版面
 * ====================================================================== */

int skt_win_w(const skterm *s, int cols) { return SKT_PAD_L + cols * s->cw + SKT_PAD_R; }
int skt_win_h(const skterm *s, int rows) { return SKT_PAD_T + rows * s->lh + SKT_PAD_B; }

void skt_layout(skterm *s, int w, int h, int *cols, int *rows)
{
    int c = (w - SKT_PAD_L - SKT_PAD_R) / s->cw;
    int r = (h - SKT_PAD_T - SKT_PAD_B) / s->lh;
    *cols = imax(c, SKT_MIN_COLS);
    *rows = imax(r, SKT_MIN_ROWS);
    s->ox = SKT_PAD_L;
    s->oy = SKT_PAD_T;
    s->sbar = sk_rect_make(w - SKT_SBAR_W - 4, SKT_PAD_T, SKT_SBAR_W, h - SKT_PAD_T - SKT_PAD_B);
}

int skt_view_alloc(skterm *s, int rows)
{
    int *lo = malloc((size_t)rows * sizeof *lo);
    int *hi = malloc((size_t)rows * sizeof *hi);
    if (!lo || !hi) {
        free(lo);
        free(hi);
        return -1;
    }
    free(s->vlo);
    free(s->vhi);
    s->vlo = lo;
    s->vhi = hi;
    s->vrows = rows;
    for (int r = 0; r < rows; r++) {
        lo[r] = 0;
        hi[r] = 0;
    }
    return 0;
}

static void mark(skterm *s, int r, int c0, int c1)
{
    if (r < 0 || r >= s->vrows)
        return;
    c0 = imax(c0, 0);
    c1 = imin(c1, s->t->cols);
    if (c0 >= c1)
        return;
    if (s->vlo[r] >= s->vhi[r]) {
        s->vlo[r] = c0;
        s->vhi[r] = c1;
    } else {
        s->vlo[r] = imin(s->vlo[r], c0);
        s->vhi[r] = imax(s->vhi[r], c1);
    }
}

void skt_mark_rows(skterm *s, int r0, int r1)
{
    for (int r = imax(r0, 0); r <= r1 && r < s->vrows; r++)
        mark(s, r, 0, s->t->cols);
}

void skt_mark_all(skterm *s)
{
    s->rows_all = 1;
}

long long skt_view_line(const skterm *s, int r)
{
    return term_screen_line(s->t) - s->view + r;
}

/* ======================================================================
 * 颜色
 * ====================================================================== */

/* 256 色：0..15 是主题的 16 色（蜡笔色，在纸上看得清），16..231 是 6×6×6 的颜色立方体，
 * 232..255 是 24 级灰（这两段是 xterm 的标准值，程序用它们时就是想要那种颜色） */
static sk_color pal256(int i)
{
    static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
    if (i < 16)
        return sk_ansi_palette[i];
    if (i < 232) {
        i -= 16;
        return SK_RGB(lv[i / 36], lv[(i / 6) % 6], lv[i % 6]);
    }
    int g = 8 + (i - 232) * 10;
    return SK_RGB(g, g, g);
}

sk_color skt_color(uint32_t c, sk_color def)
{
    switch (TC_KIND(c)) {
    case TC_INDEXED: return pal256((int)(c & 0xFF));
    case TC_RGB:     return 0xFF000000u | (c & 0x00FFFFFFu);
    default:         return def;
    }
}

/* 一个格子最终的前景/背景色。*plain=1 表示背景就是纸（不用另外涂色） */
static void cell_colors(const tcell *c, sk_color *fg, sk_color *bg, int *plain)
{
    sk_color f = skt_color(c->fg, SK_INK);
    sk_color b = skt_color(c->bg, SK_PAPER);
    int p = c->bg == TC_DEFAULT;
    if (c->attr & TA_REVERSE) {
        sk_color x = f;
        f = b;
        b = x;
        p = 0;
    }
    /* 纸色主题里 ANSI 的“白色”(7/15) 是浅灰，好在纸上能看见；但它出现在彩色背景上时
     * （例如 ls 的 37;41），程序的本意是“白字”，这时用纸色才看得清 */
    if (!p && TC_KIND(c->fg) == TC_INDEXED && ((c->fg & 0xFF) == 7 || (c->fg & 0xFF) == 15) &&
        !(c->attr & TA_REVERSE))
        f = SK_PAPER;
    if (c->attr & TA_DIM)
        f = sk_mix(f, b, 110);
    if (c->attr & TA_HIDDEN)
        f = b;
    *fg = f;
    *bg = b;
    *plain = p;
}

static const tcell *line_cell(const tline *l, int x)
{
    static const tcell empty;
    if (!l || x < 0 || x >= l->len)
        return &empty;
    return &l->c[x];
}

/* ======================================================================
 * 光标
 * ====================================================================== */

/* 光标现在应该是什么样子（不可见返回 0） */
static int want_cursor(const skterm *s, int *x, int *y, int *w)
{
    const term *t = s->t;
    if (!t->cursor_visible || s->finished)
        return 0;
    int r = t->cy + s->view;
    if (r < 0 || r >= t->rows)
        return 0;
    *x = t->cx;
    *y = r;
    *w = (t->scr[t->cy]->c[t->cx].attr & TA_WIDE) && t->cx + 1 < t->cols ? 2 : 1;
    if (!s->focused)
        return CUR_HOLLOW;
    switch (t->cursor_shape) {
    case 3: case 4: return CUR_UNDER;
    case 5: case 6: return CUR_BAR;
    default:        return CUR_BLOCK;
    }
}

static void pencil(sk_rough_style *st, uint32_t seed, sk_color ink, float width, float rough)
{
    sk_rough_default(st);
    st->seed = seed;
    st->stroke = ink;
    st->stroke_width = width;
    st->roughness = rough;
    st->bowing = 0.4f;
    st->double_stroke = 0;
}

/* 画光标的“底”（实心方块，在文字下面）或“框”（空心/竖线/下划线，在文字上面） */
static void draw_cursor_shape(skterm *s, sk_canvas *cv, sk_rect r, int style, sk_color ink)
{
    sk_rect old = sk_push_clip(cv, r);
    sk_rough_style st;
    float x = (float)r.x, y = (float)r.y, w = (float)r.w, h = (float)r.h;
    switch (style) {
    case CUR_BLOCK:
        /* 铅笔涂实的小方块：边缘略有起伏，四角稍圆 */
        pencil(&st, s->seed ^ 0xC0C0u, ink, 0.0f, 0.45f);
        st.stroke_width = 0;
        st.fill_style = SK_FILL_SOLID;
        st.fill = SK_WITH_ALPHA(ink, 0xE8);
        sk_rough_round_rect(cv, x + 0.5f, y + 1.0f, w - 1.0f, h - 2.0f, 2.0f, &st);
        break;
    case CUR_HOLLOW:
        pencil(&st, s->seed ^ 0x0C0Cu, SK_INK_SOFT, 1.3f, 0.5f);
        sk_rough_rect(cv, x + 1.0f, y + 1.5f, w - 2.0f, h - 3.0f, &st);
        break;
    case CUR_BAR:
        pencil(&st, s->seed ^ 0xBA2u, ink, 2.0f, 0.4f);
        sk_rough_line(cv, x + 1.5f, y + 2.0f, x + 1.5f, y + h - 2.0f, &st);
        break;
    case CUR_UNDER:
        pencil(&st, s->seed ^ 0x0D0u, ink, 2.0f, 0.4f);
        sk_rough_line(cv, x + 1.0f, y + h - 2.5f, x + w - 1.0f, y + h - 2.5f, &st);
        break;
    }
    sk_pop_clip(cv, old);
}

/* ======================================================================
 * 画一行的一段
 * ====================================================================== */

/* 重画显示行 r 的 [c0, c1) 格，并提交 damage */
static void draw_range(skterm *s, sk_canvas *cv, int r, int c0, int c1)
{
    term *t = s->t;
    int cols = t->cols, cw = s->cw, lh = s->lh;
    long long L = skt_view_line(s, r);
    const tline *ln = term_line(t, L);
    int y = s->oy + r * lh;
    int baseline = y + s->asc;
    sk_rect rr = sk_rect_make(s->ox + c0 * cw, y, (c1 - c0) * cw, lh);
    sk_rect old = sk_push_clip(cv, rr);

    /* 1. 纸。纸的颗粒“印”在文字所在的那张纸上，跟着内容一起滚动，而不是印在窗口的固定位置：
     *    第 L 行用纹理的第 L×行高 像素行（纹理 256 像素一个周期）。做法是建一个子画布，
     *    让它的原点比这一行高出 off 个像素。这样滚屏时把像素整体搬走，和在新位置重新画
     *    这一行，结果一模一样，局部重画也不会出现“补丁”。 */
    int off = (int)((((L % 256) + 256) % 256) * lh % 256);
    sk_canvas row;
    sk_canvas_sub(&row, cv, sk_rect_make(s->ox, y - off, cols * cw, off + lh));
    sk_paper_fill(&row, sk_rect_make(c0 * cw, off, (c1 - c0) * cw, lh), SK_PAPER, 3, s->seed);
    uint32_t row_seed = sk_hash_u32(s->seed, (uint32_t)L);

    /* 2. 有颜色的背景（同色的连续格子一起涂） */
    for (int x = c0; x < c1;) {
        sk_color fg, bg;
        int plain;
        cell_colors(line_cell(ln, x), &fg, &bg, &plain);
        int e = x + 1;
        if (!plain) {
            while (e < c1) {
                sk_color f2, b2;
                int p2;
                cell_colors(line_cell(ln, e), &f2, &b2, &p2);
                if (p2 || b2 != bg)
                    break;
                e++;
            }
            sk_paper_fill(&row, sk_rect_make(x * cw, off, (e - x) * cw, lh), bg, 5, s->seed);
        }
        x = e;
    }

    /* 3. 选区：荧光笔。笔触的形状只由整段选区的矩形和行号决定，局部重画时不会变形 */
    int sa, sb;
    if (skt_sel_cols(s, L, &sa, &sb)) {
        sk_color hl = s->focused ? SK_HIGHLIGHT : SK_WITH_ALPHA(SK_HIGHLIGHT, 0x50);
        sk_marker_highlight(cv, sk_rect_make(s->ox + sa * cw - 1, y, (sb - sa) * cw + 2, lh), hl, row_seed);
    }

    /* 4. 光标的底（实心方块光标在字的下面，字再用反色画在上面） */
    int cur_here = s->cur_on && s->cur_y == r && s->cur_x < c1 + 1 && s->cur_x + s->cur_w > c0 - 1;
    sk_rect cur_r = sk_rect_make(s->ox + s->cur_x * cw, y, s->cur_w * cw, lh);
    sk_color cur_ink = SK_INK;
    if (cur_here) {
        /* 光标用这一格显示出来的前景色；方块光标里的字用背景色——相当于把这一格反显，
         * 所以在反显的格子上光标也看得见 */
        sk_color b;
        int p;
        cell_colors(line_cell(ln, s->cur_x), &cur_ink, &b, &p);
        if (s->cur_style == CUR_BLOCK)
            draw_cursor_shape(s, cv, cur_r, CUR_BLOCK, cur_ink);
    }

    /* 5. 字。左右各多画一个格子的字（宽字符多一格）：手写体的笔画会伸出自己的格子一两个像素，
     *    邻居重画时要把伸过来的部分也画上 */
    for (int x = imax(c0 - 2, 0); x < imin(c1 + 1, cols); x++) {
        const tcell *c = line_cell(ln, x);
        if (c->attr & TA_CONT)
            continue;
        int w = (c->attr & TA_WIDE) ? 2 : 1;
        sk_color fg, bg;
        int plain;
        cell_colors(c, &fg, &bg, &plain);
        if (cur_here && s->cur_style == CUR_BLOCK && x >= s->cur_x && x < s->cur_x + s->cur_w)
            fg = bg;
        int px = s->ox + x * cw;
        if (c->cp && c->cp != ' ' && !(c->attr & TA_HIDDEN)) {
            sk_draw_glyph(cv, s->font, px, baseline, c->cp, fg);
            if (c->attr & TA_BOLD)       /* 粗体：错开 1 像素再描一遍 */
                sk_draw_glyph(cv, s->font, px + 1, baseline, c->cp, fg);
        }
        if (c->attr & TA_UNDERLINE)
            sk_hline(cv, px, px + w * cw - 1, baseline + 2, fg);
        if (c->attr & TA_STRIKE)
            sk_hline(cv, px, px + w * cw - 1, baseline - s->asc / 3, fg);
    }

    /* 6. 其它形状的光标画在字上面 */
    if (cur_here && s->cur_style != CUR_BLOCK)
        draw_cursor_shape(s, cv, cur_r, s->cur_style, s->cur_style == CUR_HOLLOW ? SK_INK_SOFT : cur_ink);

    sk_pop_clip(cv, old);
    sk_window_damage(s->win, rr);
}

/* ======================================================================
 * 边距、红线、滚动条
 * ====================================================================== */

static void scrollbar_state(const skterm *s, int *total, int *off, int *state)
{
    const term *t = s->t;
    int hist = t->alt_on ? 0 : t->sb_count;
    *total = hist + t->rows;
    *off = hist - s->view;
    *state = s->sbar_drag ? SK_STATE_PRESSED : s->sbar_hover ? SK_STATE_HOVER : 0;
}

static void draw_scrollbar(skterm *s, sk_canvas *cv)
{
    int total, off, state;
    scrollbar_state(s, &total, &off, &state);
    sk_rect area = sk_rect_inset(s->sbar, -3, -3);
    sk_rect old = sk_push_clip(cv, area);
    sk_paper_fill(cv, area, SK_PAPER, 3, s->seed);
    /* 没有历史可翻时不画滚动条（一根灰色的空轨道只会让画面显得乱） */
    if (total > s->t->rows)
        sk_draw_scrollbar(cv, s->sbar, total, s->t->rows, off, state, s->seed ^ 0x5B5Bu);
    sk_pop_clip(cv, old);
    sk_window_damage(s->win, area);
    s->sb_total = total;
    s->sb_off = off;
    s->sb_state = state;
    s->sb_valid = 1;
}

/* 文字区以外的所有地方：纸、红色边线、滚动条 */
static void draw_chrome(skterm *s, sk_canvas *cv)
{
    int W = cv->w, H = cv->h;
    int tw = s->t->cols * s->cw, th = s->t->rows * s->lh;
    sk_rect strips[4] = {
        sk_rect_make(0, 0, W, s->oy),                                   /* 上 */
        sk_rect_make(0, s->oy + th, W, H - s->oy - th),                 /* 下 */
        sk_rect_make(0, s->oy, s->ox, th),                              /* 左 */
        sk_rect_make(s->ox + tw, s->oy, W - s->ox - tw, th),            /* 右 */
    };
    for (int i = 0; i < 4; i++) {
        if (sk_rect_empty(strips[i]))
            continue;
        sk_paper_fill(cv, strips[i], SK_PAPER, 3, s->seed);
        sk_window_damage(s->win, strips[i]);
    }
    /* 笔记本的红色边线（和记事本的编辑区一样），只画在左边距里 */
    sk_rough_style st;
    pencil(&st, s->seed ^ 0xA5A5u, SK_WITH_ALPHA(SK_RED, 0x80), 1.1f, 0.3f);
    sk_rect old = sk_push_clip(cv, sk_rect_make(0, 0, s->ox - 2, H));
    sk_rough_line(cv, (float)SKT_MARGIN_X, 2.0f, (float)SKT_MARGIN_X, (float)H - 2.0f, &st);
    sk_pop_clip(cv, old);
    draw_scrollbar(s, cv);
}

/* ======================================================================
 * 响铃：在文字区四周闪一下橙色的手绘框（只画在边距里，150 毫秒后擦掉）
 * ====================================================================== */

static void bell_off(sk_app *app, void *user)
{
    (void)app;
    skterm *s = user;
    s->bell_timer = 0;
    s->chrome = 1;
}

void skt_bell(skterm *s)
{
    if (s->bell_timer || !s->win)
        return;
    sk_canvas *cv = sk_window_canvas(s->win);
    int tw = s->t->cols * s->cw, th = s->t->rows * s->lh;
    sk_rough_style st;
    pencil(&st, s->seed ^ 0xBE11u, SK_ORANGE, 2.2f, 0.8f);
    st.double_stroke = 1;
    sk_rect text = sk_rect_make(s->ox, s->oy, tw, th);
    /* 只在边距里画：分别以四条边距为裁剪区画同一个框 */
    sk_rect strips[4] = {
        sk_rect_make(0, 0, cv->w, s->oy),
        sk_rect_make(0, s->oy + th, cv->w, cv->h - s->oy - th),
        sk_rect_make(0, 0, s->ox, cv->h),
        sk_rect_make(s->ox + tw, 0, s->sbar.x - 1 - (s->ox + tw), cv->h),
    };
    for (int i = 0; i < 4; i++) {
        if (sk_rect_empty(strips[i]))
            continue;
        sk_rect old = sk_push_clip(cv, strips[i]);
        sk_rough_rect(cv, (float)text.x - 2.5f, (float)text.y - 2.5f, (float)text.w + 5.0f,
                      (float)text.h + 5.0f, &st);
        sk_pop_clip(cv, old);
        sk_window_damage(s->win, strips[i]);
    }
    s->bell_timer = sk_app_add_timer(s->app, 150, 0, bell_off, s);
}

/* ======================================================================
 * 每轮事件循环的重画
 * ====================================================================== */

/* 文字区的像素整体移动 n 行（n>0 上移：输出滚屏；n<0 下移：往回翻历史）。
 * 只搬文字区那几列，边距里的红线不动。新露出来的行标脏，之后正常画字。 */
static void shift_pixels(skterm *s, sk_canvas *cv, int n)
{
    int tw = s->t->cols * s->cw, th = s->t->rows * s->lh;
    int rows = s->vrows, k = n > 0 ? n : -n;
    int dy = k * s->lh;
    size_t bytes = (size_t)tw * sizeof(uint32_t);
    if (n > 0) {
        for (int y = s->oy; y < s->oy + th - dy; y++)
            memmove(cv->pixels + (size_t)y * cv->stride + s->ox,
                    cv->pixels + (size_t)(y + dy) * cv->stride + s->ox, bytes);
        sk_window_damage(s->win, sk_rect_make(s->ox, s->oy, tw, th - dy));
        /* 我们自己记的脏区也跟着移动 */
        memmove(s->vlo, s->vlo + k, (size_t)(rows - k) * sizeof *s->vlo);
        memmove(s->vhi, s->vhi + k, (size_t)(rows - k) * sizeof *s->vhi);
        for (int r = rows - k; r < rows; r++) {
            s->vlo[r] = 0;
            s->vhi[r] = s->t->cols;
        }
    } else {
        for (int y = s->oy + th - 1; y >= s->oy + dy; y--)
            memmove(cv->pixels + (size_t)y * cv->stride + s->ox,
                    cv->pixels + (size_t)(y - dy) * cv->stride + s->ox, bytes);
        sk_window_damage(s->win, sk_rect_make(s->ox, s->oy + dy, tw, th - dy));
        memmove(s->vlo + k, s->vlo, (size_t)(rows - k) * sizeof *s->vlo);
        memmove(s->vhi + k, s->vhi, (size_t)(rows - k) * sizeof *s->vhi);
        for (int r = 0; r < k; r++) {
            s->vlo[r] = 0;
            s->vhi[r] = s->t->cols;
        }
    }
    /* 画着的光标也被搬走了 */
    s->cur_y -= n;
    if (s->cur_y < 0 || s->cur_y >= rows)
        s->cur_on = 0;
}

void skt_render(skterm *s)
{
    term *t = s->t;
    if (!s->win || !t || s->vrows != t->rows)
        return;
    sk_canvas *cv = sk_window_canvas(s->win);
    int rows = t->rows, cols = t->cols;

    /* 1. 正在翻看历史时来了新输出：view 加上新进历史区的行数，画面停在原来的内容上 */
    long long pushed = t->sb_pushed - s->seen_pushed;
    s->seen_pushed = t->sb_pushed;
    int term_changed = t->full || t->shift;
    for (int r = 0; r < rows && !term_changed; r++)
        term_changed = t->dlo[r] < t->dhi[r];

    /* 0. 用户翻看历史（滚轮、Shift+PageUp、滚动条）：内容整体下移/上移，同样搬像素 */
    if (s->vshift) {
        int n = s->vshift;
        s->vshift = 0;
        if (s->rows_all || term_changed || n >= rows || -n >= rows)
            s->rows_all = 1;
        else
            shift_pixels(s, cv, -n);
    }
    if (s->view > 0) {
        int want = s->view + (int)pushed;
        int maxv = t->alt_on ? 0 : t->sb_count;
        s->view = imin(want, maxv);
        /* 只看得到历史区（view >= rows）且没有被截断时，画面完全没变 */
        if (term_changed && !(s->view >= rows && s->view == want))
            s->rows_all = 1;
    } else if (t->full) {
        s->rows_all = 1;
        skt_clear_selection(s);
    } else {
        /* 2. 整屏上滚：搬像素 */
        if (t->shift > 0 && !s->rows_all)
            shift_pixels(s, cv, t->shift);
        long long top = term_screen_line(t);
        for (int r = 0; r < rows; r++) {
            if (t->dlo[r] >= t->dhi[r])
                continue;
            mark(s, r, t->dlo[r], t->dhi[r]);
            /* 选中的文字被程序改掉了：选区作废（和 xterm 一样） */
            int a, b;
            if (s->sel_on && !s->sel_drag && skt_sel_cols(s, top + r, &a, &b) && a < t->dhi[r] &&
                b > t->dlo[r])
                skt_clear_selection(s);
        }
    }
    if (s->view > (t->alt_on ? 0 : t->sb_count)) {
        s->view = t->alt_on ? 0 : t->sb_count;
        s->rows_all = 1;
    }
    term_clear_dirty(t);

    if (s->rows_all) {
        for (int r = 0; r < rows; r++) {
            s->vlo[r] = 0;
            s->vhi[r] = cols;
        }
        s->rows_all = 0;
        s->cur_on = 0;
    }

    /* 3. 光标：和屏幕上画着的不一样，就把旧位置和新位置都标脏 */
    int cx = 0, cy = 0, cwid = 1;
    int style = want_cursor(s, &cx, &cy, &cwid);
    if (style != (s->cur_on ? s->cur_style : 0) ||
        (style && (cx != s->cur_x || cy != s->cur_y || cwid != s->cur_w))) {
        if (s->cur_on)
            mark(s, s->cur_y, s->cur_x, s->cur_x + s->cur_w);
        if (style)
            mark(s, cy, cx, cx + cwid);
    }
    s->cur_on = style != 0;
    s->cur_style = style;
    s->cur_x = cx;
    s->cur_y = cy;
    s->cur_w = cwid;

    /* 4. 画脏行（左右各扩一格：字的笔画会伸出格子） */
    for (int r = 0; r < rows; r++) {
        if (s->vlo[r] >= s->vhi[r])
            continue;
        draw_range(s, cv, r, imax(s->vlo[r] - 1, 0), imin(s->vhi[r] + 1, cols));
        s->vlo[r] = cols;
        s->vhi[r] = 0;
    }

    /* 5. 边距和滚动条 */
    if (s->chrome) {
        s->chrome = 0;
        draw_chrome(s, cv);
    } else {
        int total, off, state;
        scrollbar_state(s, &total, &off, &state);
        if (!s->sb_valid || total != s->sb_total || off != s->sb_off || state != s->sb_state)
            draw_scrollbar(s, cv);
    }
}

void skt_render_all(skterm *s)
{
    if (!s->win)
        return;
    sk_canvas *cv = sk_window_canvas(s->win);
    sk_reset_clip(cv);
    s->rows_all = 1;
    s->chrome = 1;
    s->sb_valid = 0;
    skt_render(s);
}
