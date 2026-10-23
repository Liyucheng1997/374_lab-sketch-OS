/*
 * ui_list.c — 列表/表格控件 sk_list_*（文件管理器、系统监视器、文件对话框都用它）
 *
 * 外观：像一张记账纸——可选的表头（浅色纸条 + 列标题 + 排序小三角），
 * 每行一条极淡的横线；选中行用黄色荧光笔涂一道，鼠标悬停的行用很淡的蓝色涂一道。
 *
 * 数据模型很简单：行数组，每行 = 图标 + ncols 个字符串（列表自己保存副本）+ 应用的 data。
 * 列宽 widths[i] 的约定：
 *     > 0  固定宽度（像素），左对齐
 *     < 0  固定宽度 -widths[i]，右对齐（数字列：大小、PID、CPU%……）
 *     = 0  占满剩余宽度（左对齐）；没有这样的列时，最后一列占满剩余宽度
 *     widths == NULL 时各列平分。
 * 排序：列表只负责“点了哪一列”的界面——点击表头调用 on_sort(w, col, user)，
 *     由应用自己排序后重新填充。表头的小三角：第一次点某列显示 ▲（升序），
 *     再点同一列切换成 ▼（降序）。应用按同样的规则记录升降序即可保持一致。
 * 空列表时如果用 sk_widget_set_text 设置过文字，就把它淡淡地写在中间（例如“此文件夹是空的”）。
 *
 * 性能（网页版 CPU 很慢，系统监视器每秒刷新一次）：
 *   - sk_list_set_cell 内容没变就什么也不做，变了只重画那一个单元格；
 *   - 应用每次 sk_list_clear() 再全部 sk_list_add() 重建列表时，列表会把新行和旧行逐行比较，
 *     只重画真正变化的行（“重建优化”，见 li_flush）；
 *   - 悬停、选中变化只重画相关的两行。
 */
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>

#include "sketch/ui.h"
#include "sketch/rough.h"
#include "ui_internal.h"

#define LI_FRAME      3     /* 外框到内容的距离 */
#define LI_SB_W       12    /* 滚动条宽度 */
#define LI_PAD        8     /* 单元格左右留白（两边合计） */
#define LI_WHEEL_ROWS 3

typedef struct {
    int      icon;
    intptr_t data;
    char   **cells;         /* ncols 个，malloc 的副本 */
} li_row;

enum { LI_DRAG_NONE = 0, LI_DRAG_BODY, LI_DRAG_VBAR, LI_DRAG_HEADER };

typedef struct {
    int      ncols;
    char   **titles;        /* NULL = 没有表头 */
    int     *want;          /* 应用设定的列宽（约定见文件开头） */
    int     *cx, *cw;       /* 计算出的列起点（窗口坐标）和宽度 */
    li_row  *rows;
    int      n, cap;
    /* 重建优化：clear 之后旧行先留着，和新加的行逐行比较 */
    li_row  *old;
    int      old_n, old_sel, rebuilding, flush_timer;
    int      sel;           /* 选中行，-1 = 无 */
    int      hover;         /* 鼠标下的行，-1 = 无 */
    int      hover_col;     /* 鼠标下的表头列 */
    int      press_col;     /* 按下的表头列 */
    int      scroll;        /* 第一可见行 */
    int      row_h, row_h_user, header_h;
    int      sort_col, sort_desc;
    void   (*on_sort)(sk_widget *w, int col, void *user);
    int      has_icons, icon_size;
    sk_rect  inner, header, body, vbar;
    int      vis_rows;
    int      drag, drag_grab;
    const sk_font *font;
} li_priv;

#define LI(w) ((li_priv *)(w)->priv)

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ================================================================ 行数据 */

static void li_row_free(li_row *r, int ncols)
{
    if (r->cells) {
        for (int i = 0; i < ncols; i++)
            free(r->cells[i]);
        free(r->cells);
    }
    r->cells = NULL;
}

static void li_rows_free(li_row *rows, int n, int ncols)
{
    for (int i = 0; i < n; i++)
        li_row_free(&rows[i], ncols);
    free(rows);
}

static const char *li_cell_text(const li_row *r, int col)
{
    return (r->cells && r->cells[col]) ? r->cells[col] : "";
}

/* 两行显示出来是否完全一样（图标 + 每个单元格的文字） */
static int li_row_same(const li_row *a, const li_row *b, int ncols)
{
    if (a->icon != b->icon)
        return 0;
    for (int i = 0; i < ncols; i++)
        if (strcmp(li_cell_text(a, i), li_cell_text(b, i)) != 0)
            return 0;
    return 1;
}

/* ================================================================ 布局 */

static void li_layout(sk_widget *w)
{
    li_priv *p = LI(w);
    p->font = sk_widget_get_font(w);
    int lh = sk_font_line_height(p->font);
    p->row_h = p->row_h_user > 0 ? imax(p->row_h_user, lh + 2) : imax(26, lh + 8);
    p->icon_size = p->row_h >= 32 ? 24 : 16;
    int has_header = p->titles != NULL && p->ncols > 0;
    p->header_h = has_header ? imax(26, lh + 10) : 0;

    p->inner = sk_rect_inset(w->r, LI_FRAME, LI_FRAME);
    p->header = sk_rect_make(p->inner.x, p->inner.y, p->inner.w, p->header_h);
    p->body = sk_rect_make(p->inner.x, p->inner.y + p->header_h, p->inner.w - LI_SB_W - 3,
                           p->inner.h - p->header_h);
    p->vbar = sk_rect_make(p->inner.x + p->inner.w - LI_SB_W - 1, p->body.y + 3, LI_SB_W, p->body.h - 6);
    p->vis_rows = imax(1, p->body.h / p->row_h);

    /* 列宽 */
    int nc = imax(1, p->ncols);
    int x0 = p->body.x + 4, total = p->body.w - 8;
    int fixed = 0, fills = 0;
    for (int i = 0; i < nc; i++) {
        int v = p->want ? p->want[i] : 0;
        if (v == 0)
            fills++;
        else
            fixed += v > 0 ? v : -v;
    }
    int rem = total - fixed;
    int x = x0;
    for (int i = 0; i < nc; i++) {
        int v = p->want ? p->want[i] : 0, cw;
        if (!p->want)
            cw = total / nc;
        else if (v == 0)
            cw = imax(40, rem / imax(1, fills));
        else
            cw = v > 0 ? v : -v;
        /* 没有“占满”列时，最后一列吃掉剩余宽度，让表头看起来是完整的一条 */
        if (i == nc - 1 && (fills == 0 || !p->want) && x + cw < x0 + total)
            cw = x0 + total - x;
        p->cx[i] = x;
        p->cw[i] = cw;
        x += cw;
    }
    int max_scroll = imax(0, p->n - p->vis_rows);
    if (p->scroll > max_scroll)
        p->scroll = max_scroll;
    if (p->scroll < 0)
        p->scroll = 0;
}

static int li_max_scroll(const li_priv *p) { return imax(0, p->n - p->vis_rows); }

static int li_row_y(const li_priv *p, int row) { return p->body.y + (row - p->scroll) * p->row_h; }

static int li_row_visible(const li_priv *p, int row)
{
    return row >= p->scroll && row <= p->scroll + p->vis_rows;
}

static void li_inval_row(sk_widget *w, int row)
{
    li_priv *p = LI(w);
    if (row < 0 || !li_row_visible(p, row))
        return;
    sk_rect r = sk_rect_intersect(sk_rect_make(p->body.x, li_row_y(p, row), p->body.w, p->row_h), p->body);
    if (!sk_rect_empty(r))
        sk_widget_invalidate_rect(w, r);
}

/* 从第 row 行一直到列表底部 */
static void li_inval_from(sk_widget *w, int row)
{
    li_priv *p = LI(w);
    if (row < p->scroll)
        row = p->scroll;
    int y = li_row_y(p, row);
    int bottom = p->body.y + p->body.h;
    if (y < bottom)
        sk_widget_invalidate_rect(w, sk_rect_make(p->body.x, y, p->body.w, bottom - y));
}

static void li_inval_bar(sk_widget *w)
{
    sk_widget_invalidate_rect(w, sk_rect_inset(LI(w)->vbar, -3, -3));
}

/* 空列表的提示文字画在第 0、1 行之间（跨两行），见 li_draw */
static sk_rect li_placeholder_rect(const li_priv *p)
{
    return sk_rect_make(p->body.x, p->body.y + p->row_h / 2, p->body.w, p->row_h);
}

/* 列表从空变成非空：提示文字要擦掉。只让第 0 行失效会留下提示文字的下半截 */
static void li_inval_placeholder(sk_widget *w)
{
    li_priv *p = LI(w);
    if (w->text && *w->text)
        sk_widget_invalidate_rect(w, sk_rect_intersect(li_placeholder_rect(p), p->body));
}

/* 滚动条滑块（与 theme.c 的 sk_draw_scrollbar 同一套公式，见 ui_textarea.c 的 ta_thumb） */
static void li_thumb(const li_priv *p, int *pos, int *len)
{
    float track = (float)p->vbar.h - 4.0f;
    int total = imax(p->n, p->vis_rows);
    if (track < 1.0f)
        track = 1.0f;
    if (total <= p->vis_rows) {
        *pos = 2;
        *len = (int)track;
        return;
    }
    float tl = track * (float)p->vis_rows / (float)total;
    float minl = track < 22.0f ? track : 22.0f;
    if (tl < minl)
        tl = minl;
    int maxoff = total - p->vis_rows;
    int off = imin(imax(p->scroll, 0), maxoff);
    *pos = 2 + (int)((track - tl) * (float)off / (float)maxoff);
    *len = (int)tl;
}

static void li_set_scroll(sk_widget *w, int first)
{
    li_priv *p = LI(w);
    first = imax(0, imin(first, li_max_scroll(p)));
    if (first == p->scroll)
        return;
    p->scroll = first;
    /* 表头和外框不随滚动变化：只重画行区域和滚动条 */
    sk_widget_invalidate_rect(w, p->body);
    li_inval_bar(w);
}

static void li_ensure_visible(sk_widget *w, int row)
{
    li_priv *p = LI(w);
    if (row < 0)
        return;
    if (row < p->scroll)
        li_set_scroll(w, row);
    else if (row >= p->scroll + p->vis_rows)
        li_set_scroll(w, row - p->vis_rows + 1);
}

/* 选中某行（-1 = 取消）。notify=1 时选中真的变了才调用 on_change */
static void li_select(sk_widget *w, int row, int notify)
{
    li_priv *p = LI(w);
    if (row >= p->n)
        row = p->n - 1;
    if (row < -1)
        row = -1;
    li_ensure_visible(w, row);
    if (row == p->sel)
        return;
    if (!p->rebuilding) {
        li_inval_row(w, p->sel);
        li_inval_row(w, row);
    }
    p->sel = row;
    if (notify && w->on_change)
        w->on_change(w, w->user);
}

/* ================================================================ 重建优化 */
/*
 * 系统监视器每秒做一次：sk_list_clear(); for (...) sk_list_add(...);
 * 如果每次都整块重画，在网页模拟器里每秒要画好几十行文字和图标。
 * 所以 clear 并不立刻失效，而是把旧行留着；add 第 i 行时和旧的第 i 行比较，
 * 不一样才让这一行失效。应用什么时候“加完”我们不知道，于是在 clear 时登记一个
 * 0 毫秒的一次性定时器——它在下一轮事件循环才触发，那时这一轮的 add 一定都做完了：
 * 再把“旧行比新行多出来的那几行”和选中行的变化补上失效，然后丢掉旧行。
 */
static void li_flush(sk_widget *w)
{
    li_priv *p = LI(w);
    if (!p)
        return;
    p->flush_timer = 0;
    if (!p->rebuilding)
        return;
    if (p->old_n > p->n)
        li_inval_from(w, p->n);
    if (p->old_n == 0 && p->n > 0)
        li_inval_placeholder(w);
    if (p->old_n != p->n)
        li_inval_bar(w);
    if (p->old_sel != p->sel) {
        li_inval_row(w, p->old_sel);
        li_inval_row(w, p->sel);
    }
    li_rows_free(p->old, p->old_n, imax(1, p->ncols));
    p->old = NULL;
    p->old_n = 0;
    p->rebuilding = 0;
    if (p->hover >= p->n)
        p->hover = -1;
    /* 新内容里已经没有图标了：第一列收回图标位置 */
    int any_icon = 0;
    for (int i = 0; i < p->n && !any_icon; i++)
        any_icon = p->rows[i].icon >= 0;
    if (any_icon != p->has_icons) {
        p->has_icons = any_icon;
        sk_widget_invalidate(w);
    }
    int ms = li_max_scroll(p);
    if (p->scroll > ms) {
        p->scroll = ms;
        sk_widget_invalidate(w);
    }
}

/* 结束重建（立刻对比完），在需要“现在就一致”的操作之前调用 */
static void li_finish_rebuild(sk_widget *w)
{
    li_priv *p = LI(w);
    if (p->rebuilding) {
        if (p->flush_timer)
            sk_widget_remove_timer(w, p->flush_timer);
        li_flush(w);
    }
}

/* ================================================================ 绘制 */

static void li_draw_frame(sk_widget *w, sk_canvas *c)
{
    sk_rough_style s;
    sk_style_pencil(&s, w->seed);
    s.stroke = (w->state & SK_STATE_DISABLED) ? SK_INK_FAINT : SK_INK_SOFT;
    s.stroke_width = 1.3f;
    s.roughness = 0.5f;
    sk_rough_rect(c, w->r.x + 1.5f, w->r.y + 1.5f, w->r.w - 3.0f, w->r.h - 3.0f, &s);
}

/* 表头的排序小三角 */
static void li_draw_sort_mark(sk_canvas *c, float x, float cy, int desc, uint32_t seed)
{
    sk_pointf pts[3];
    if (desc) {
        pts[0] = (sk_pointf){ x, cy - 3 };
        pts[1] = (sk_pointf){ x + 9, cy - 3 };
        pts[2] = (sk_pointf){ x + 4.5f, cy + 4 };
    } else {
        pts[0] = (sk_pointf){ x, cy + 3 };
        pts[1] = (sk_pointf){ x + 9, cy + 3 };
        pts[2] = (sk_pointf){ x + 4.5f, cy - 4 };
    }
    sk_rough_style s;
    sk_style_hatched(&s, SK_BLUE, seed);
    s.stroke = SK_INK_SOFT;
    s.stroke_width = 1.1f;
    s.roughness = 0.4f;
    s.fill_style = SK_FILL_SOLID;
    s.fill = SK_WITH_ALPHA(SK_BLUE, 0xB0);
    sk_rough_polygon(c, pts, 3, &s);
}

static void li_draw_header(sk_widget *w, sk_canvas *c)
{
    li_priv *p = LI(w);
    sk_rect hr = p->header;
    sk_rect area = sk_rect_intersect(c->clip, hr);
    if (sk_rect_empty(area))
        return;
    sk_paper_fill(c, area, SK_PAPER_DARK, 3, w->seed ^ 0x4EADu);
    int asc = sk_font_ascent(p->font), desc = sk_font_descent(p->font);
    int base = hr.y + (hr.h - (asc + desc)) / 2 + asc;
    for (int i = 0; i < p->ncols; i++) {
        const char *t = p->titles[i] ? p->titles[i] : "";
        sk_rect cell = sk_rect_make(p->cx[i] + 2, hr.y, p->cw[i] - LI_PAD, hr.h);
        int sorted = i == p->sort_col;
        if (sorted)
            cell.w -= 14;                /* 给小三角留位置 */
        if (i == p->press_col && p->drag == LI_DRAG_HEADER)
            sk_marker_highlight(c, sk_rect_make(p->cx[i] + 1, hr.y + 4, p->cw[i] - 4, hr.h - 8),
                                SK_WITH_ALPHA(SK_BLUE, 0x40), sk_hash_u32(w->seed, 0x700u + i));
        sk_color ink = (i == p->hover_col || sorted) ? SK_INK : SK_INK_SOFT;
        int align = (p->want && p->want[i] < 0) ? SK_ALIGN_RIGHT : SK_ALIGN_LEFT;
        sk_draw_text_in_rect(c, p->font, cell, t, sk_widget_ink(w, ink), align);
        if (sorted) {
            int tw = sk_text_width(p->font, t);
            float mx = align == SK_ALIGN_RIGHT ? (float)(cell.x + cell.w + 4)
                                               : (float)(cell.x + imin(tw, cell.w) + 6);
            li_draw_sort_mark(c, mx, (float)(base - asc / 2 + 1), p->sort_desc, sk_hash_u32(w->seed, 0x50u));
        }
        /* 列之间的分隔短线 */
        if (i > 0) {
            sk_rough_style s;
            sk_style_pencil(&s, sk_hash_u32(w->seed, 0x1100u + i));
            s.stroke = SK_INK_FAINT;
            s.stroke_width = 1.0f;
            s.double_stroke = 0;
            s.roughness = 0.4f;
            float lx = (float)p->cx[i] - 2.0f;
            sk_rough_line(c, lx, (float)hr.y + 6, lx, (float)(hr.y + hr.h) - 6, &s);
        }
    }
    sk_draw_separator(c, hr.x + 2, hr.x + hr.w - 2, hr.y + hr.h - 2, w->seed ^ 0x5E9u);
}

static void li_draw_row(sk_widget *w, sk_canvas *c, int i)
{
    li_priv *p = LI(w);
    li_row *r = &p->rows[i];
    int y = li_row_y(p, i);
    sk_rect band = sk_rect_make(p->body.x, y, p->body.w, p->row_h);
    if (i == p->sel) {
        sk_color hl = (w->state & SK_STATE_FOCUSED) ? SK_HIGHLIGHT : SK_WITH_ALPHA(SK_HIGHLIGHT, 0x60);
        sk_marker_highlight(c, sk_rect_make(band.x + 3, y + 3, band.w - 6, p->row_h - 6), hl,
                            sk_hash_u32(w->seed, (uint32_t)i));
    } else if (i == p->hover) {
        sk_marker_highlight(c, sk_rect_make(band.x + 3, y + 4, band.w - 6, p->row_h - 8),
                            SK_WITH_ALPHA(SK_BLUE, 0x1C), sk_hash_u32(w->seed ^ 0x40u, (uint32_t)i));
    }
    /* 记账纸的淡横线 */
    sk_hline(c, band.x + 6, band.x + band.w - 6, y + p->row_h - 1, SK_WITH_ALPHA(SK_DESK_GRID, 0xB0));

    sk_color ink = sk_widget_ink(w, SK_INK);
    int nc = imax(1, p->ncols);
    for (int col = 0; col < nc; col++) {
        int x = p->cx[col] + 2, cw = p->cw[col] - LI_PAD;
        if (col == 0 && p->has_icons) {
            if (r->icon >= 0)
                sk_draw_icon(c, r->icon, x, y + (p->row_h - p->icon_size) / 2, p->icon_size,
                             sk_hash_u32(0x1C0Du, (uint32_t)r->icon));
            x += p->icon_size + 6;
            cw -= p->icon_size + 6;
        }
        if (cw <= 4)
            continue;
        int align = (p->want && p->want[col] < 0) ? SK_ALIGN_RIGHT : SK_ALIGN_LEFT;
        sk_draw_text_in_rect(c, p->font, sk_rect_make(x, y, cw, p->row_h), li_cell_text(r, col),
                             col == 0 ? ink : sk_widget_ink(w, SK_INK_SOFT), align);
    }
}

static void li_draw(sk_widget *w, sk_canvas *c)
{
    li_priv *p = LI(w);
    if (!p)
        return;
    if (sk_widget_get_font(w) != p->font)
        li_layout(w);
    sk_rect clip0 = c->clip;
    sk_rect area = sk_rect_intersect(clip0, p->inner);
    if (!sk_rect_empty(area)) {
        sk_rect old = sk_push_clip(c, p->inner);
        sk_paper_fill(c, area, SK_PAPER, 3, w->seed);
        if (p->header_h)
            li_draw_header(w, c);
        sk_rect body_area = sk_rect_intersect(area, p->body);
        if (!sk_rect_empty(body_area)) {
            sk_rect ob = sk_push_clip(c, p->body);
            int first = p->scroll + (body_area.y - p->body.y) / p->row_h;
            int last = p->scroll + (body_area.y + body_area.h - 1 - p->body.y) / p->row_h;
            for (int i = first; i <= last && i < p->n; i++)
                li_draw_row(w, c, i);
            /* 空列表的提示文字（sk_widget_set_text 设置） */
            if (p->n == 0 && w->text && *w->text)
                sk_draw_text_in_rect(c, p->font, li_placeholder_rect(p), w->text, SK_INK_FAINT, SK_ALIGN_CENTER);
            sk_pop_clip(c, ob);
        }
        sk_pop_clip(c, old);
    }
    if (sk_rect_intersect(clip0, sk_rect_inset(p->vbar, -3, -3)).w > 0)
        sk_draw_scrollbar(c, p->vbar, imax(p->n, p->vis_rows), p->vis_rows, p->scroll,
                          p->drag == LI_DRAG_VBAR ? SK_STATE_PRESSED : 0, w->seed ^ 0x77u);
    li_draw_frame(w, c);
}

/* ================================================================ 事件 */

static int li_row_at(const li_priv *p, int y)
{
    if (y < p->body.y || y >= p->body.y + p->body.h)
        return -1;
    int row = p->scroll + (y - p->body.y) / p->row_h;
    return row < p->n ? row : -1;
}

static int li_col_at(const li_priv *p, int x)
{
    for (int i = 0; i < p->ncols; i++)
        if (x >= p->cx[i] - 4 && x < p->cx[i] + p->cw[i] - 4)
            return i;
    return -1;
}

static void li_set_hover(sk_widget *w, int row, int col)
{
    li_priv *p = LI(w);
    if (row != p->hover) {
        int old = p->hover;
        p->hover = row;
        li_inval_row(w, old);
        li_inval_row(w, row);
    }
    if (col != p->hover_col) {
        p->hover_col = col;
        if (p->header_h)
            sk_widget_invalidate_rect(w, p->header);
    }
}

static void li_activate(sk_widget *w)
{
    if (LI(w)->sel >= 0 && w->on_click)
        w->on_click(w, w->user);
}

static int li_mouse(sk_widget *w, const sk_event *e)
{
    li_priv *p = LI(w);
    int x = e->mouse.x, y = e->mouse.y;
    int in_bar = x >= p->vbar.x - 2 && y >= p->body.y && y < p->body.y + p->body.h;
    int in_header = p->header_h && y >= p->header.y && y < p->header.y + p->header.h;
    switch (e->mouse.kind) {
    case SK_MOUSE_DOWN:
        if (e->mouse.button != SK_BUTTON_LEFT)
            return 0;              /* 右键交给应用（右键菜单） */
        if (in_header) {
            p->drag = LI_DRAG_HEADER;
            p->press_col = li_col_at(p, x);
            sk_widget_invalidate_rect(w, p->header);
            return 1;
        }
        if (in_bar) {
            int pos, len;
            li_thumb(p, &pos, &len);
            int ty = y - p->vbar.y;
            if (ty >= pos && ty < pos + len) {
                p->drag = LI_DRAG_VBAR;
                p->drag_grab = ty - pos;
                li_inval_bar(w);
            } else {
                li_set_scroll(w, p->scroll + (ty < pos ? -(p->vis_rows - 1) : p->vis_rows - 1));
            }
            return 1;
        }
        {
            li_finish_rebuild(w);
            int row = li_row_at(p, y);
            p->drag = LI_DRAG_BODY;
            if (row >= 0) {
                li_select(w, row, 1);
                if (!sk_widget_alive(w))
                    return 1;
                if (e->mouse.clicks >= 2 && LI(w)->sel == row)
                    li_activate(w);
            }
        }
        return 1;
    case SK_MOUSE_MOVE:
        if (p->drag == LI_DRAG_VBAR) {
            float track = (float)p->vbar.h - 4.0f;
            int pos, len;
            li_thumb(p, &pos, &len);
            float range = track - (float)len;
            int maxoff = li_max_scroll(p);
            int np = y - p->vbar.y - p->drag_grab - 2;
            li_set_scroll(w, range > 0 ? (int)((float)np * (float)maxoff / range + 0.5f) : 0);
            return 1;
        }
        if (p->drag == LI_DRAG_BODY && (e->mouse.buttons & SK_BUTTON_LEFT)) {
            /* 按住拖动：选中跟着鼠标走，拖出上下边时滚动 */
            int row;
            if (y < p->body.y)
                row = p->scroll - 1;
            else if (y >= p->body.y + p->body.h)
                row = p->scroll + p->vis_rows;
            else
                row = p->scroll + (y - p->body.y) / p->row_h;
            row = imax(0, imin(row, p->n - 1));
            if (p->n > 0 && row != p->sel)
                li_select(w, row, 1);
            return 1;
        }
        if (p->drag == LI_DRAG_HEADER)
            return 1;
        li_set_hover(w, in_bar || in_header ? -1 : li_row_at(p, y), in_header ? li_col_at(p, x) : -1);
        return 1;
    case SK_MOUSE_UP:
        if (p->drag == LI_DRAG_HEADER) {
            int col = p->press_col;
            p->drag = LI_DRAG_NONE;
            p->press_col = -1;
            sk_widget_invalidate_rect(w, p->header);
            if (sk_widget_is_click(w, e) && col >= 0 && col == li_col_at(p, x) && in_header) {
                if (p->sort_col == col)
                    p->sort_desc = !p->sort_desc;
                else {
                    p->sort_col = col;
                    p->sort_desc = 0;
                }
                if (p->on_sort)
                    p->on_sort(w, col, w->user);
            }
            return 1;
        }
        if (p->drag == LI_DRAG_VBAR)
            li_inval_bar(w);
        p->drag = LI_DRAG_NONE;
        return 1;
    case SK_MOUSE_WHEEL:
        li_set_scroll(w, p->scroll - e->mouse.wheel * LI_WHEEL_ROWS);
        /* 滚动后鼠标下面换了一行 */
        li_set_hover(w, li_row_at(p, y), -1);
        return 1;
    case SK_MOUSE_LEAVE:
        li_set_hover(w, -1, -1);
        return 1;
    case SK_MOUSE_ENTER:
        return 1;
    }
    return 0;
}

/* 键入字母快速定位：从当前行之后找第一个“首列以这个字开头”的行 */
static int li_type_ahead(sk_widget *w, uint32_t cp)
{
    li_priv *p = LI(w);
    if (p->n == 0)
        return 0;
    if (cp >= 'A' && cp <= 'Z')
        cp += 'a' - 'A';
    for (int k = 1; k <= p->n; k++) {
        int i = (p->sel + k + p->n) % p->n;
        const char *s = li_cell_text(&p->rows[i], 0);
        uint32_t c0 = sk_utf8_next(&s);
        if (c0 >= 'A' && c0 <= 'Z')
            c0 += 'a' - 'A';
        if (c0 == cp) {
            li_select(w, i, 1);
            return 1;
        }
    }
    return 1;
}

static int li_key(sk_widget *w, const sk_event *e)
{
    li_priv *p = LI(w);
    uint32_t mods = e->key.modifiers;
    if (e->key.state == 0 || (mods & (SK_MOD_CTRL | SK_MOD_ALT | SK_MOD_SUPER)))
        return 0;
    li_finish_rebuild(w);
    int page = imax(1, p->vis_rows - 1);
    int sel = p->sel;
    switch (e->key.keycode) {
    case KEY_UP:       li_select(w, sel < 0 ? 0 : imax(0, sel - 1), 1); return 1;
    case KEY_DOWN:     li_select(w, sel < 0 ? 0 : imin(p->n - 1, sel + 1), 1); return 1;
    case KEY_PAGEUP:   li_select(w, imax(0, (sel < 0 ? 0 : sel) - page), 1); return 1;
    case KEY_PAGEDOWN: li_select(w, imin(p->n - 1, (sel < 0 ? 0 : sel) + page), 1); return 1;
    case KEY_HOME:     if (p->n) li_select(w, 0, 1); return 1;
    case KEY_END:      if (p->n) li_select(w, p->n - 1, 1); return 1;
    case KEY_ENTER:
    case KEY_KPENTER:
        if (sel < 0)
            return 0;           /* 没有选中行：回车交给应用（例如对话框的“确定”） */
        li_activate(w);
        return 1;
    case KEY_TAB:
    case KEY_ESC:
        return 0;
    }
    uint32_t cp = e->key.codepoint;
    if (e->key.keycode == 0 && cp == '\r') {
        if (sel < 0)
            return 0;
        li_activate(w);
        return 1;
    }
    if (cp > 0x20 && cp != 0x7F && !(cp >= 0x80 && cp < 0xA0))
        return li_type_ahead(w, cp);
    return 0;
}

static int li_event(sk_widget *w, const sk_event *e)
{
    li_priv *p = LI(w);
    if (!p)
        return 0;
    switch (e->type) {
    case SK_EVENT_MOUSE:
        return li_mouse(w, e);
    case SK_EVENT_KEY:
        return li_key(w, e);
    case SK_EVENT_FOCUS:
        /* 选中行的荧光笔深浅随焦点变化 */
        li_inval_row(w, p->sel);
        return 1;
    default:
        return 0;
    }
}

static void li_destroy(sk_widget *w)
{
    li_priv *p = LI(w);
    if (!p)
        return;
    int nc = imax(1, p->ncols);
    li_rows_free(p->rows, p->n, nc);
    li_rows_free(p->old, p->old_n, nc);
    if (p->titles) {
        for (int i = 0; i < p->ncols; i++)
            free(p->titles[i]);
        free(p->titles);
    }
    free(p->want);
    free(p->cx);
    free(p->cw);
    free(p);
    w->priv = NULL;
}

static void li_resized(sk_widget *w)
{
    if (!LI(w))
        return;
    li_layout(w);
    sk_widget_invalidate(w);
}

static const sk_widget_class li_class = {
    .name = "list",
    .draw = li_draw,
    .event = li_event,
    .destroy = li_destroy,
    .resized = li_resized,
    .focusable = 1,
};

static li_priv *li_get(const sk_widget *w)
{
    return (w && w->cls == &li_class) ? (li_priv *)w->priv : NULL;
}

/* 给 cx/cw 分配 n 列的空间 */
static int li_alloc_cols(li_priv *p, int n)
{
    int *cx = calloc(n, sizeof(int)), *cw = calloc(n, sizeof(int));
    if (!cx || !cw) {
        free(cx);
        free(cw);
        return -1;
    }
    free(p->cx);
    free(p->cw);
    p->cx = cx;
    p->cw = cw;
    return 0;
}

/* ================================================================ 公共接口 */

sk_widget *sk_list_new(sk_ui *ui, sk_rect r, sk_widget_fn on_change, sk_widget_fn on_click, void *user)
{
    sk_widget *w = sk_widget_new(ui, &li_class, r);
    if (!w)
        return NULL;
    li_priv *p = calloc(1, sizeof(*p));
    if (!p || li_alloc_cols(p, 1) < 0) {
        free(p);
        sk_widget_destroy(w);
        return NULL;
    }
    p->sel = p->hover = p->hover_col = p->press_col = p->sort_col = -1;
    p->old_sel = -1;
    w->priv = p;
    w->on_change = on_change;
    w->on_click = on_click;
    w->user = user;
    sk_widget_set_flags(w, SK_WF_NO_AUTO_REDRAW, 0);
    li_layout(w);
    sk_widget_invalidate(w);
    return w;
}

void sk_list_set_columns(sk_widget *w, const char *const *titles, const int *widths, int ncols)
{
    li_priv *p = li_get(w);
    if (!p || ncols < 1)
        return;
    li_finish_rebuild(w);
    int old_nc = imax(1, p->ncols);
    char **nt = NULL;
    int *nw = NULL;
    if (titles) {
        nt = calloc(ncols, sizeof(char *));
        if (!nt)
            return;
        for (int i = 0; i < ncols; i++)
            nt[i] = strdup(titles[i] ? titles[i] : "");
    }
    if (widths) {
        nw = malloc(ncols * sizeof(int));
        if (!nw) {
            if (nt) {
                for (int i = 0; i < ncols; i++)
                    free(nt[i]);
                free(nt);
            }
            return;
        }
        memcpy(nw, widths, ncols * sizeof(int));
    }
    if (li_alloc_cols(p, ncols) < 0) {
        free(nw);
        if (nt) {
            for (int i = 0; i < ncols; i++)
                free(nt[i]);
            free(nt);
        }
        return;
    }
    /* 已有的行：单元格数组改成新的列数 */
    for (int i = 0; i < p->n; i++) {
        char **cells = calloc(ncols, sizeof(char *));
        if (!cells)
            continue;
        for (int c = 0; c < ncols; c++)
            cells[c] = c < old_nc && p->rows[i].cells ? p->rows[i].cells[c] : NULL;
        for (int c = ncols; c < old_nc; c++)
            free(p->rows[i].cells ? p->rows[i].cells[c] : NULL);
        free(p->rows[i].cells);
        p->rows[i].cells = cells;
    }
    if (p->titles) {
        for (int i = 0; i < p->ncols; i++)
            free(p->titles[i]);
        free(p->titles);
    }
    free(p->want);
    p->titles = nt;
    p->want = nw;
    p->ncols = ncols;
    if (p->sort_col >= ncols)
        p->sort_col = -1;
    li_layout(w);
    sk_widget_invalidate(w);
}

void sk_list_set_sort_handler(sk_widget *w, void (*on_sort)(sk_widget *w, int col, void *user))
{
    li_priv *p = li_get(w);
    if (p)
        p->on_sort = on_sort;
}

void sk_list_set_row_height(sk_widget *w, int h)
{
    li_priv *p = li_get(w);
    if (!p || h == p->row_h_user)
        return;
    p->row_h_user = h;
    li_layout(w);
    sk_widget_invalidate(w);
}

void sk_list_clear(sk_widget *w)
{
    li_priv *p = li_get(w);
    if (!p)
        return;
    if (p->rebuilding) {
        /* 重建还没对比完又 clear 了一次：旧行（屏幕上画着的）继续作为对比基准 */
        li_rows_free(p->rows, p->n, imax(1, p->ncols));
    } else {
        p->old = p->rows;
        p->old_n = p->n;
        p->old_sel = p->sel;
        p->rebuilding = 1;
        if (!p->flush_timer)
            p->flush_timer = sk_widget_add_timer(w, 0, 0, li_flush);
        if (!p->flush_timer) {
            /* 定时器不可用（窗口已关闭等）：直接全部重画 */
            li_rows_free(p->old, p->old_n, imax(1, p->ncols));
            p->old = NULL;
            p->old_n = 0;
            p->rebuilding = 0;
            sk_widget_invalidate(w);
        }
    }
    p->rows = NULL;
    p->n = p->cap = 0;
    p->sel = -1;
    /* hover 和 has_icons 保持不变：鼠标还停在同一行的位置上；图标列也大概率还在。
     * 重建结束时（li_flush）再按新内容校正，避免每次重建都整块重画。 */
}

int sk_list_add(sk_widget *w, int icon, const char *const *cells, intptr_t data)
{
    li_priv *p = li_get(w);
    if (!p)
        return -1;
    int nc = imax(1, p->ncols);
    if (p->n + 1 > p->cap) {
        int cap = p->cap ? p->cap * 2 : 32;
        li_row *rows = realloc(p->rows, cap * sizeof(li_row));
        if (!rows)
            return -1;
        p->rows = rows;
        p->cap = cap;
    }
    li_row *r = &p->rows[p->n];
    r->icon = icon;
    r->data = data;
    r->cells = calloc(nc, sizeof(char *));
    if (!r->cells)
        return -1;
    for (int i = 0; i < nc; i++)
        r->cells[i] = strdup(cells && cells[i] ? cells[i] : "");
    int row = p->n++;
    if (icon >= 0 && !p->has_icons) {
        /* 第一次出现图标：第一列让出图标位置，整个列表重画 */
        p->has_icons = 1;
        sk_widget_invalidate(w);
    }
    if (p->rebuilding) {
        /* 重建中：和屏幕上画着的旧行比较，一样就不用重画 */
        if (row >= p->old_n || !li_row_same(r, &p->old[row], nc))
            li_inval_row(w, row);
    } else {
        if (row == 0)
            li_inval_placeholder(w);
        li_inval_row(w, row);
        li_inval_bar(w);
    }
    return row;
}

int sk_list_count(const sk_widget *w)
{
    li_priv *p = li_get(w);
    return p ? p->n : 0;
}

int sk_list_selected(const sk_widget *w)
{
    li_priv *p = li_get(w);
    return p ? p->sel : -1;
}

void sk_list_select(sk_widget *w, int row)
{
    li_priv *p = li_get(w);
    if (p)
        li_select(w, row, 0);
}

intptr_t sk_list_data(const sk_widget *w, int row)
{
    li_priv *p = li_get(w);
    return (p && row >= 0 && row < p->n) ? p->rows[row].data : 0;
}

const char *sk_list_cell(const sk_widget *w, int row, int col)
{
    li_priv *p = li_get(w);
    if (!p || row < 0 || row >= p->n || col < 0 || col >= imax(1, p->ncols))
        return NULL;
    return li_cell_text(&p->rows[row], col);
}

void sk_list_set_cell(sk_widget *w, int row, int col, const char *text)
{
    li_priv *p = li_get(w);
    if (!p || row < 0 || row >= p->n || col < 0 || col >= imax(1, p->ncols))
        return;
    li_row *r = &p->rows[row];
    if (!text)
        text = "";
    if (strcmp(li_cell_text(r, col), text) == 0)
        return;                 /* 没变：什么也不做（系统监视器每秒刷新，大部分单元格不变） */
    char *dup = strdup(text);
    if (!dup)
        return;
    free(r->cells[col]);
    r->cells[col] = dup;
    if (p->rebuilding)
        return;                 /* 重建过程中：这一行在 add 时已经比较过/失效过了 */
    if (li_row_visible(p, row)) {
        sk_rect cell = sk_rect_make(p->cx[col] - 2, li_row_y(p, row), p->cw[col] + 2, p->row_h);
        sk_widget_invalidate_rect(w, sk_rect_intersect(cell, p->body));
    }
}

int sk_list_scroll(const sk_widget *w)
{
    li_priv *p = li_get(w);
    return p ? p->scroll : 0;
}

void sk_list_set_scroll(sk_widget *w, int first_row)
{
    li_priv *p = li_get(w);
    if (p)
        li_set_scroll(w, first_row);
}
