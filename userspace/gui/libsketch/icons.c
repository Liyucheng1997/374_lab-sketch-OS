/*
 * icons.c — 手绘图标：全部由代码画出，系统里没有一张图标图片
 *
 * 为什么用代码画而不是放 PNG？
 *   1. 任何尺寸都清晰：16、24、32、48 像素各自重新“画”一遍，而不是把一张图缩放
 *      （缩小会糊、放大会出锯齿）。尺寸小时自动减少细节、调细线条、降低抖动。
 *   2. 风格统一：图标和控件、窗口用的是同一套手绘图元（rough.h），同样的铅笔、同样的排线。
 *   3. 体积小：几十个图标只是几 KB 的代码；网页版的镜像越小，加载越快。
 *
 * 每个图标都在一个 48×48 的“设计网格”上设计，绘制时按 size/48 缩放。
 * 小尺寸（< 24）时排线改成淡色实心（几个像素宽的排线只会变成噪点），描线只描一遍。
 */
#include <math.h>
#include <string.h>

#include "sketch/theme.h"
#include "gfx_internal.h"

#define SK_PI 3.14159265358979f

typedef struct {
    sk_canvas *c;
    float ox, oy, k;     /* 网格坐标 u → 像素 ox + u*k */
    int size;
    float lw;            /* 描线宽度（像素） */
    float rough;
    int dbl;             /* 描两遍 */
    int hatch;           /* 用排线填充（否则淡色实心） */
    int small;           /* 小尺寸：省略细节 */
    uint32_t seed;
    uint32_t n;          /* 已分配的子种子个数 */
} ictx;

static uint32_t nseed(ictx *ic) { return sk_hash_u32(ic->seed, ++ic->n); }
static float X(const ictx *ic, float u) { return ic->ox + u * ic->k; }
static float Y(const ictx *ic, float v) { return ic->oy + v * ic->k; }
static float K(const ictx *ic, float d) { return d * ic->k; }

/* 设定样式：ink 描线色（alpha=0 表示不描线），fill 填充色（alpha=0 表示不填） */
static void ist(ictx *ic, sk_rough_style *s, sk_color ink, sk_color fill)
{
    sk_rough_default(s);
    s->seed = nseed(ic);
    s->stroke = ink;
    s->stroke_width = SK_A(ink) ? ic->lw : 0.0f;
    s->roughness = ic->rough;
    s->double_stroke = ic->dbl;
    if (SK_A(fill)) {
        if (ic->hatch && fill != SK_PAPER && fill != SK_PAPER_DARK) {
            s->fill_style = SK_FILL_HACHURE;
            s->fill = fill;
            s->hachure_gap = 1.6f + (float)ic->size * 0.045f;
            s->fill_weight = 0.75f + (float)ic->size * 0.016f;
        } else {
            s->fill_style = SK_FILL_SOLID;
            /* 小图标：淡一点的实心色，像蜡笔轻轻涂了一层 */
            s->fill = (fill == SK_PAPER || SK_A(fill) != 255) ? fill : sk_mix(fill, SK_PAPER, 70);
        }
    }
}

static void i_rect(ictx *ic, float x, float y, float w, float h, sk_color ink, sk_color fill)
{
    sk_rough_style s;
    ist(ic, &s, ink, fill);
    sk_rough_rect(ic->c, X(ic, x), Y(ic, y), K(ic, w), K(ic, h), &s);
}

static void i_rrect(ictx *ic, float x, float y, float w, float h, float r, sk_color ink, sk_color fill)
{
    sk_rough_style s;
    ist(ic, &s, ink, fill);
    sk_rough_round_rect(ic->c, X(ic, x), Y(ic, y), K(ic, w), K(ic, h), K(ic, r), &s);
}

static void i_circle(ictx *ic, float cx, float cy, float d, sk_color ink, sk_color fill)
{
    sk_rough_style s;
    ist(ic, &s, ink, fill);
    sk_rough_ellipse(ic->c, X(ic, cx), Y(ic, cy), K(ic, d), K(ic, d), &s);
}

static void i_ellipse(ictx *ic, float cx, float cy, float w, float h, sk_color ink, sk_color fill)
{
    sk_rough_style s;
    ist(ic, &s, ink, fill);
    sk_rough_ellipse(ic->c, X(ic, cx), Y(ic, cy), K(ic, w), K(ic, h), &s);
}

static void i_line(ictx *ic, float x1, float y1, float x2, float y2, sk_color ink)
{
    sk_rough_style s;
    ist(ic, &s, ink, 0);
    sk_rough_line(ic->c, X(ic, x1), Y(ic, y1), X(ic, x2), Y(ic, y2), &s);
}

/* 粗一点的线（箭头杆、指针等） */
static void i_line_w(ictx *ic, float x1, float y1, float x2, float y2, sk_color ink, float wmul)
{
    sk_rough_style s;
    ist(ic, &s, ink, 0);
    s.stroke_width = ic->lw * wmul;
    sk_rough_line(ic->c, X(ic, x1), Y(ic, y1), X(ic, x2), Y(ic, y2), &s);
}

#define IMAXP 40
static void to_px(const ictx *ic, const float *g, int n, sk_pointf *out)
{
    for (int i = 0; i < n; i++) {
        out[i].x = X(ic, g[2 * i]);
        out[i].y = Y(ic, g[2 * i + 1]);
    }
}

static void i_poly(ictx *ic, const float *g, int n, sk_color ink, sk_color fill)
{
    sk_pointf p[IMAXP];
    if (n > IMAXP) n = IMAXP;
    to_px(ic, g, n, p);
    sk_rough_style s;
    ist(ic, &s, ink, fill);
    sk_rough_polygon(ic->c, p, n, &s);
}

static void i_polyline(ictx *ic, const float *g, int n, sk_color ink, float wmul)
{
    sk_pointf p[IMAXP];
    if (n > IMAXP) n = IMAXP;
    to_px(ic, g, n, p);
    sk_rough_style s;
    ist(ic, &s, ink, 0);
    s.stroke_width = ic->lw * wmul;
    sk_rough_polyline(ic->c, p, n, &s);
}

/* 自由笔触（宽度以网格单位给出，但不会细于 1 像素） */
static void i_brush(ictx *ic, const float *g, int n, float w0, float w1, sk_color color)
{
    sk_pointf p[IMAXP];
    if (n > IMAXP) n = IMAXP;
    to_px(ic, g, n, p);
    float a = K(ic, w0), b = K(ic, w1);
    if (a < 1.1f) a = 1.1f;
    if (b < 1.0f) b = 1.0f;
    sk_brush_stroke(ic->c, p, n, a, b, color, 0, nseed(ic));
}

static void i_arc(ictx *ic, float cx, float cy, float d, float a0, float a1, sk_color ink, float wmul)
{
    sk_rough_style s;
    ist(ic, &s, ink, 0);
    s.stroke_width = ic->lw * wmul;
    sk_rough_arc(ic->c, X(ic, cx), Y(ic, cy), K(ic, d), K(ic, d), a0, a1, 0, &s);
}

/* 精确的实心圆点（颜料点、钟表中心） */
static void i_dot(ictx *ic, float cx, float cy, float d, sk_color color)
{
    float r = K(ic, d) * 0.5f;
    if (r < 0.8f) r = 0.8f;
    sk_fill_circle_aa(ic->c, X(ic, cx), Y(ic, cy), r, color);
}

/* 沿轴线 (ax,ay)→(bx,by)、半宽 hw 的斜放矩形的四个角（网格坐标） */
static void slab(float *o, float ax, float ay, float bx, float by, float hw)
{
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    float nx = len > 0 ? -dy / len * hw : 0, ny = len > 0 ? dx / len * hw : 0;
    o[0] = ax + nx; o[1] = ay + ny;
    o[2] = bx + nx; o[3] = by + ny;
    o[4] = bx - nx; o[5] = by - ny;
    o[6] = ax - nx; o[7] = ay - ny;
}

/* 箭头头部：从 (tx,ty) 指向的方向 (dx,dy) */
static void arrow_head(ictx *ic, float tx, float ty, float dx, float dy, float len, sk_color ink, float wmul)
{
    float l = sqrtf(dx * dx + dy * dy);
    if (l <= 0)
        return;
    dx /= l;
    dy /= l;
    float px = -dy, py = dx;
    float g[6] = { tx - dx * len + px * len * 0.75f, ty - dy * len + py * len * 0.75f, tx, ty,
                   tx - dx * len - px * len * 0.75f, ty - dy * len - py * len * 0.75f };
    i_polyline(ic, g, 3, ink, wmul);
}

/* ======================================================================
 * 各个图标（48 网格）
 * ====================================================================== */

static void ic_folder_shape(ictx *ic, float x, float y, float w, float h, sk_color back, sk_color front)
{
    float tab = w * 0.36f;
    float g[12] = { x, y + h * 0.12f, x + tab, y + h * 0.12f, x + tab + w * 0.08f, y + h * 0.26f,
                    x + w, y + h * 0.26f, x + w, y + h, x, y + h };
    i_poly(ic, g, 6, SK_INK, back);
    i_rect(ic, x, y + h * 0.40f, w, h * 0.60f, SK_INK, front);
}

static void ic_terminal(ictx *ic)
{
    i_rrect(ic, 4, 7, 40, 34, 4, SK_INK, SK_PAPER);
    i_rect(ic, 5.5f, 8.5f, 37, 6, 0, SK_BLUE);
    i_line(ic, 4.5f, 14.5f, 43.5f, 14.5f, SK_INK);
    if (!ic->small) {
        i_dot(ic, 9, 11.5f, 2.6f, SK_RED);
        i_dot(ic, 14, 11.5f, 2.6f, SK_YELLOW);
    }
    float p[6] = { 11, 21, 19, 27, 11, 33 };
    i_polyline(ic, p, 3, SK_GREEN, 1.35f);
    i_line_w(ic, 22, 34, 33, 34, SK_INK, 1.3f);
}

static void ic_folder(ictx *ic)
{
    ic_folder_shape(ic, 4, 8, 40, 34, SK_ORANGE, SK_YELLOW);
}

static void ic_page(ictx *ic, sk_color fold_ink)
{
    float g[10] = { 10, 4, 30, 4, 38, 12, 38, 44, 10, 44 };
    i_poly(ic, g, 5, SK_INK, SK_PAPER);
    float f[6] = { 30, 4, 30, 12, 38, 12 };
    i_polyline(ic, f, 3, fold_ink, 1.0f);
}

static void ic_file(ictx *ic)
{
    ic_page(ic, SK_INK);
    if (!ic->small)
        i_rect(ic, 15, 32, 18, 7, 0, SK_BLUE);
}

static void ic_text(ictx *ic)
{
    ic_page(ic, SK_INK);
    static const float len[5] = { 13, 18, 16, 18, 10 };
    int n = ic->small ? 3 : 5;
    for (int i = 0; i < n; i++) {
        float y = ic->small ? 19.0f + i * 8.0f : 18.0f + i * 5.5f;
        i_line(ic, 15, y, 15 + len[i], y, SK_BLUE);
    }
}

static void ic_image(ictx *ic)
{
    i_rect(ic, 5, 9, 38, 31, SK_INK, SK_PAPER);
    float m[10] = { 7.5f, 37.5f, 18, 22, 25, 30, 30, 25, 40.5f, 37.5f };
    i_poly(ic, m, 5, SK_INK, SK_GREEN);
    i_circle(ic, 33, 17.5f, 8, SK_INK, SK_ORANGE);
}

static void ic_program(ictx *ic)
{
    i_rrect(ic, 5, 8, 38, 32, 3, SK_INK, SK_PAPER);
    i_rect(ic, 6.5f, 9.5f, 35, 5, 0, SK_PURPLE);
    i_line(ic, 5.5f, 14.5f, 42.5f, 14.5f, SK_INK);
    float l[6] = { 18, 20, 12, 27, 18, 34 };
    float r[6] = { 30, 20, 36, 27, 30, 34 };
    i_polyline(ic, l, 3, SK_PURPLE, 1.3f);
    i_polyline(ic, r, 3, SK_PURPLE, 1.3f);
    if (!ic->small)
        i_line(ic, 26.5f, 19, 21.5f, 35, SK_INK);
}

static void ic_notepad(ictx *ic)
{
    i_rect(ic, 9, 7, 30, 37, SK_INK, SK_PAPER);
    i_rect(ic, 9, 7, 30, 6, SK_INK, SK_GREEN);
    if (!ic->small) {
        for (int i = 0; i < 4; i++)
            i_circle(ic, 15 + i * 6.0f, 7, 4, SK_INK, 0);
        for (int i = 0; i < 4; i++)
            i_line(ic, 14, 20 + i * 6.0f, i == 3 ? 26 : 34, 20 + i * 6.0f, SK_BLUE);
        i_line(ic, 13, 15, 13, 42, SK_WITH_ALPHA(SK_RED, 0xB0));
    } else {
        for (int i = 0; i < 3; i++)
            i_line(ic, 14, 21 + i * 7.0f, 34, 21 + i * 7.0f, SK_BLUE);
    }
}

static void ic_paint(ictx *ic)
{
    /* 调色板：一个椭圆，右下角挖一个拇指孔 */
    i_ellipse(ic, 23, 27, 40, 32, SK_INK, SK_PAPER_DARK);
    i_circle(ic, 31, 35, 7, SK_INK, SK_PAPER);
    i_dot(ic, 13, 23, 7, SK_RED);
    i_dot(ic, 21, 17, 7, SK_YELLOW);
    i_dot(ic, 31, 19, 7, SK_BLUE);
    i_dot(ic, 13, 33, 7, SK_GREEN);
    if (!ic->small) {
        float h[8];
        slab(h, 45, 3, 35, 15, 1.6f);
        i_poly(ic, h, 4, SK_INK, SK_ORANGE);
        i_brush(ic, (const float[]){ 35, 15, 32.5f, 18.5f }, 2, 3.4f, 1.4f, SK_PURPLE);
    }
}

static void ic_monitor(ictx *ic)
{
    i_rect(ic, 5, 7, 38, 34, SK_INK, SK_PAPER);
    if (!ic->small) {
        for (int i = 0; i < 4; i++) {
            float h = (float)(6 + ((i * 7) % 11));
            i_rect(ic, 10 + i * 8.0f, 37 - h, 5, h, 0, SK_PURPLE);
        }
    }
    float g[12] = { 9, 31, 15, 24, 21, 28, 28, 15, 34, 20, 40, 12 };
    i_polyline(ic, g, 6, SK_RED, 1.3f);
}

static void ic_clock(ictx *ic)
{
    i_circle(ic, 24, 24, 40, SK_INK, SK_TEAL);
    i_circle(ic, 24, 24, 30, 0, SK_PAPER);
    if (!ic->small) {
        for (int i = 0; i < 4; i++) {
            float a = i * SK_PI / 2;
            i_line(ic, 24 + cosf(a) * 12, 24 + sinf(a) * 12, 24 + cosf(a) * 14.5f, 24 + sinf(a) * 14.5f, SK_INK);
        }
    }
    i_line_w(ic, 24, 24, 24, 13, SK_INK, 1.3f);
    i_line_w(ic, 24, 24, 32, 28, SK_INK, 1.2f);
    i_dot(ic, 24, 24, 3.5f, SK_RED);
}

static void ic_about(ictx *ic)
{
    i_circle(ic, 24, 24, 40, SK_INK, SK_YELLOW);
    i_circle(ic, 24, 24, 26, 0, SK_PAPER);
    i_dot(ic, 24, 14.5f, 5, SK_INK);
    i_brush(ic, (const float[]){ 21, 21, 24.5f, 20.5f, 24, 34, 28, 34 }, 4, 3.6f, 3.0f, SK_INK);
}

/* 齿轮：每个齿是一个梯形（齿根宽、齿顶窄），一共 4×teeth 个顶点 */
static void ic_gear(ictx *ic, float cx, float cy, float ro, float ri, int teeth, sk_color fill)
{
    float g[IMAXP * 2];
    int n = teeth * 4 > IMAXP ? IMAXP / 4 : teeth;
    float step = 2.0f * SK_PI / (float)n;
    for (int i = 0; i < n; i++) {
        float a = (float)i * step - SK_PI / 2;
        float ang[4] = { a - step * 0.30f, a - step * 0.17f, a + step * 0.17f, a + step * 0.30f };
        float rad[4] = { ri, ro, ro, ri };
        for (int k = 0; k < 4; k++) {
            g[(i * 4 + k) * 2] = cx + cosf(ang[k]) * rad[k];
            g[(i * 4 + k) * 2 + 1] = cy + sinf(ang[k]) * rad[k];
        }
    }
    i_poly(ic, g, n * 4, SK_INK, fill);
}

static void ic_settings(ictx *ic)
{
    ic_gear(ic, 24, 24, 20.5f, 15, ic->small ? 6 : 8, SK_INK_FAINT);
    i_circle(ic, 24, 24, 13, SK_INK, SK_PAPER);
}

static void ic_power(ictx *ic)
{
    i_arc(ic, 24, 26, 32, -SK_PI / 2 + 0.75f, 3 * SK_PI / 2 - 0.75f, SK_RED, 1.5f);
    i_line_w(ic, 24, 6, 24, 23, SK_RED, 1.5f);
}

static void ic_reboot(ictx *ic)
{
    float a0 = -SK_PI / 2 + 0.5f, a1 = 3 * SK_PI / 2 - 0.3f;
    i_arc(ic, 24, 25, 32, a0, a1, SK_ORANGE, 1.5f);
    float ex = 24 + cosf(a1) * 16, ey = 25 + sinf(a1) * 16;
    arrow_head(ic, ex, ey, -sinf(a1), cosf(a1), 7, SK_ORANGE, 1.5f);
}

static void ic_home(ictx *ic)
{
    i_rect(ic, 10, 22, 28, 21, SK_INK, SK_PAPER);
    float roof[6] = { 4, 24, 24, 6, 44, 24 };
    i_poly(ic, roof, 3, SK_INK, SK_RED);
    i_rect(ic, 20, 30, 8, 13, SK_INK, SK_ORANGE);
}

static void ic_up(ictx *ic)
{
    i_line_w(ic, 24, 41, 24, 9, SK_BLUE, 1.6f);
    arrow_head(ic, 24, 8, 0, -1, 12, SK_BLUE, 1.6f);
}

static void ic_back(ictx *ic)
{
    i_line_w(ic, 41, 24, 9, 24, SK_BLUE, 1.6f);
    arrow_head(ic, 8, 24, -1, 0, 12, SK_BLUE, 1.6f);
}

static void ic_new_folder(ictx *ic)
{
    ic_folder_shape(ic, 3, 7, 36, 31, SK_ORANGE, SK_YELLOW);
    i_circle(ic, 36, 36, 17, SK_INK, SK_PAPER);
    i_line_w(ic, 36, 30, 36, 42, SK_GREEN, 1.5f);
    i_line_w(ic, 30, 36, 42, 36, SK_GREEN, 1.5f);
}

static void ic_trash(ictx *ic)
{
    float can[8] = { 11, 14, 37, 14, 34, 43, 14, 43 };
    i_poly(ic, can, 4, SK_INK, SK_INK_FAINT);
    i_line_w(ic, 7, 13, 41, 13, SK_INK, 1.2f);
    float h[8] = { 19, 12.5f, 19.5f, 8, 28.5f, 8, 29, 12.5f };
    i_polyline(ic, h, 4, SK_INK, 1.0f);
    if (!ic->small) {
        i_line(ic, 19, 19, 20, 38, SK_INK_SOFT);
        i_line(ic, 24, 19, 24, 38, SK_INK_SOFT);
        i_line(ic, 29, 19, 28, 38, SK_INK_SOFT);
    }
}

static void ic_save(ictx *ic)
{
    i_rrect(ic, 6, 6, 36, 36, 3, SK_INK, SK_BLUE);
    i_rect(ic, 13, 6, 21, 12, SK_INK, SK_PAPER);
    if (!ic->small)
        i_rect(ic, 27, 8.5f, 4, 7, 0, SK_INK_SOFT);
    i_rect(ic, 11, 25, 26, 17, SK_INK, SK_PAPER);
    if (!ic->small) {
        i_line(ic, 15, 31, 33, 31, SK_INK_FAINT);
        i_line(ic, 15, 36, 30, 36, SK_INK_FAINT);
    }
}

static void ic_open(ictx *ic)
{
    float back[12] = { 4, 10, 16, 10, 20, 14, 40, 14, 40, 40, 4, 40 };
    i_poly(ic, back, 6, SK_INK, SK_ORANGE);
    if (!ic->small)
        i_rect(ic, 9, 17, 26, 10, SK_INK, SK_PAPER);
    float front[8] = { 10, 22, 46, 22, 40, 40, 4, 40 };
    i_poly(ic, front, 4, SK_INK, SK_YELLOW);
}

static void ic_pencil(ictx *ic)
{
    /* 轴线从左下到右上：笔尖在左下 */
    float body[8];
    slab(body, 14, 34, 37, 11, 5);
    i_poly(ic, body, 4, SK_INK, SK_YELLOW);
    float er[8];
    slab(er, 37, 11, 42, 6, 5);
    i_poly(ic, er, 4, SK_INK, SK_PINK);
    /* 削好的木头尖 */
    float tip[6] = { body[0], body[1], 6.5f, 41.5f, body[6], body[7] };
    i_poly(ic, tip, 3, SK_INK, SK_PAPER_DARK);
    float lead[6] = { 6.5f, 41.5f, 6.5f + 3.2f, 41.5f - 0.2f, 6.5f + 0.2f, 41.5f - 3.2f };
    i_poly(ic, lead, 3, 0, SK_INK);
}

static void ic_eraser(ictx *ic)
{
    float a[8];
    slab(a, 12, 36, 34, 14, 8);
    i_poly(ic, a, 4, SK_INK, SK_PINK);
    float band[8];
    slab(band, 18, 30, 22, 26, 8);
    i_poly(ic, band, 4, SK_INK, SK_BLUE);
    if (!ic->small) {
        i_line(ic, 4, 44, 26, 44, SK_INK_FAINT);
        i_dot(ic, 31, 43, 1.8f, SK_INK_FAINT);
        i_dot(ic, 36, 42, 1.4f, SK_INK_FAINT);
    }
}

static void ic_brush(ictx *ic)
{
    float handle[8];
    slab(handle, 26, 22, 43, 5, 3);
    i_poly(ic, handle, 4, SK_INK, SK_ORANGE);
    float fer[8];
    slab(fer, 21, 27, 26, 22, 4);
    i_poly(ic, fer, 4, SK_INK, SK_INK_FAINT);
    /* 笔头：蘸了蓝色颜料 */
    i_brush(ic, (const float[]){ 21, 27, 17, 32, 12, 35, 8, 37 }, 4, 8, 1.5f, SK_BLUE);
    if (!ic->small)
        i_brush(ic, (const float[]){ 6, 44, 14, 41, 22, 43, 30, 41 }, 4, 2.2f, 1.2f, SK_WITH_ALPHA(SK_BLUE, 0xC0));
}

static void ic_line_tool(ictx *ic)
{
    i_line_w(ic, 9, 39, 39, 9, SK_INK, 1.4f);
    i_dot(ic, 9, 39, 6, SK_BLUE);
    i_dot(ic, 39, 9, 6, SK_BLUE);
}

static void ic_rect_tool(ictx *ic)
{
    i_rect(ic, 7, 12, 34, 24, SK_INK, ic->small ? 0 : SK_WITH_ALPHA(SK_BLUE, 0x60));
}

static void ic_circle_tool(ictx *ic)
{
    i_circle(ic, 24, 24, 34, SK_INK, ic->small ? 0 : SK_WITH_ALPHA(SK_RED, 0x60));
}

static void ic_fill(ictx *ic)
{
    /* 油漆桶：提手、桶身、桶口里的蓝色颜料，颜料从桶沿流下来滴了一滴 */
    i_arc(ic, 22, 19, 24, SK_PI, 2.0f * SK_PI, SK_INK, 1.0f);
    float body[8] = { 8, 19, 36, 19, 32, 43, 12, 43 };
    i_poly(ic, body, 4, SK_INK, SK_INK_FAINT);
    i_ellipse(ic, 22, 19, 28, 8, SK_INK, SK_PAPER);
    i_ellipse(ic, 22, 19.5f, 21, 4.5f, 0, SK_BLUE);
    i_brush(ic, (const float[]){ 33, 20, 38.5f, 21, 40.5f, 27, 40.5f, 31 }, 4, 4.2f, 3.0f, SK_BLUE);
    i_dot(ic, 40.8f, 38, ic->small ? 6.5f : 5.5f, SK_BLUE);
}

static void ic_kernel(ictx *ic)
{
    /* 操作系统的“洋葱图”：最里面是内核，外面一圈一圈是系统服务和应用 */
    i_circle(ic, 24, 24, 42, SK_INK, SK_WITH_ALPHA(SK_BLUE, 0x50));
    i_circle(ic, 24, 24, 28, SK_INK, SK_WITH_ALPHA(SK_GREEN, 0x70));
    i_circle(ic, 24, 24, 14, SK_INK, SK_ORANGE);
    i_dot(ic, 24, 24, 3, SK_INK);
}

static void ic_chip(ictx *ic)
{
    int pins = ic->small ? 2 : 3;
    for (int i = 0; i < pins; i++) {
        float t = pins == 2 ? 19.0f + i * 10.0f : 16.0f + i * 8.0f;
        i_line(ic, t, 5, t, 12, SK_INK_SOFT);
        i_line(ic, t, 36, t, 43, SK_INK_SOFT);
        i_line(ic, 5, t, 12, t, SK_INK_SOFT);
        i_line(ic, 36, t, 43, t, SK_INK_SOFT);
    }
    i_rrect(ic, 11, 11, 26, 26, 3, SK_INK, SK_TEAL);
    i_rect(ic, 18, 18, 12, 12, SK_INK, SK_PAPER);
}

static void ic_memory(ictx *ic)
{
    i_rect(ic, 3, 14, 42, 19, SK_INK, SK_GREEN);
    int chips = ic->small ? 2 : 3;
    for (int i = 0; i < chips; i++) {
        float w = chips == 2 ? 13.0f : 9.0f;
        float x = chips == 2 ? 8.0f + i * 18.0f : 7.0f + i * 12.5f;
        i_rect(ic, x, 18, w, 9, 0, SK_INK);
    }
    if (!ic->small) {
        for (int i = 0; i < 7; i++)
            i_line(ic, 7 + i * 5.5f, 33, 7 + i * 5.5f, 37, SK_ORANGE);
        i_line(ic, 3, 37.5f, 45, 37.5f, SK_INK_FAINT);
    }
}

static void ic_process(ictx *ic)
{
    i_rrect(ic, 14, 5, 29, 21, 3, SK_INK_SOFT, SK_PAPER);
    i_rrect(ic, 9, 12, 29, 21, 3, SK_INK_SOFT, SK_PAPER);
    i_rrect(ic, 4, 19, 29, 22, 3, SK_INK, SK_PAPER);
    float tri[6] = { 14, 24, 14, 36, 24, 30 };
    i_poly(ic, tri, 3, SK_INK, SK_GREEN);
}

static void ic_logo(ictx *ic)
{
    /* SketchOS：一张纸，上面一笔写出的蓝色 S，旁边一支小铅笔 */
    i_rrect(ic, 4, 4, 36, 40, 4, SK_INK, SK_PAPER);
    if (!ic->small)
        i_rect(ic, 6, 6, 32, 5, 0, SK_WITH_ALPHA(SK_YELLOW, 0xB0));
    i_brush(ic, (const float[]){ 30, 14, 22, 12, 14, 16, 16, 23, 24, 25.5f, 31, 30, 27, 37, 18, 38, 12, 35 }, 9,
            5.0f, 3.0f, SK_BLUE);
    if (!ic->small) {
        float body[8];
        slab(body, 36, 40, 44, 28, 2.6f);
        i_poly(ic, body, 4, SK_INK, SK_ORANGE);
        float tip[6] = { body[0], body[1], 34, 44, body[6], body[7] };
        i_poly(ic, tip, 3, SK_INK, SK_PAPER_DARK);
    }
}

static void ic_book(ictx *ic)
{
    i_rect(ic, 11, 37, 28, 6, SK_INK, SK_PAPER);
    float cover[8] = { 9, 5, 39, 5, 39, 38, 9, 38 };
    i_poly(ic, cover, 4, SK_INK, SK_RED);
    i_line(ic, 14, 5.5f, 14, 37.5f, SK_INK);
    if (!ic->small)
        i_rect(ic, 19, 12, 15, 8, SK_INK, SK_PAPER);
}

static void ic_refresh(ictx *ic)
{
    float a0 = -SK_PI * 0.95f, a1 = -SK_PI * 0.12f;
    i_arc(ic, 24, 24, 32, a0, a1, SK_GREEN, 1.5f);
    arrow_head(ic, 24 + cosf(a1) * 16, 24 + sinf(a1) * 16, -sinf(a1), cosf(a1), 6.5f, SK_GREEN, 1.5f);
    float b0 = SK_PI * 0.05f, b1 = SK_PI * 0.88f;
    i_arc(ic, 24, 24, 32, b0, b1, SK_BLUE, 1.5f);
    arrow_head(ic, 24 + cosf(b1) * 16, 24 + sinf(b1) * 16, -sinf(b1), cosf(b1), 6.5f, SK_BLUE, 1.5f);
}

/* 放大镜（查找）：镜片淡蓝排线，粗手柄，左上角一道反光 */
static void ic_search(ictx *ic)
{
    i_line_w(ic, 29, 29, 41, 41, SK_INK, 2.6f);
    i_circle(ic, 20, 20, 28, SK_INK, SK_BLUE);
    i_circle(ic, 20, 20, 20, 0, SK_PAPER);
    if (!ic->small)
        i_arc(ic, 20, 20, 14, SK_PI * 1.05f, SK_PI * 1.45f, SK_BLUE, 1.2f);
}

/* 音乐：粉色唱片圈里两个连在一起的八分音符（斜椭圆的符头、竖直的符干、一道粗横梁） */
static void ic_music(ictx *ic)
{
    i_circle(ic, 24, 24, 42, SK_INK, SK_PINK);
    i_circle(ic, 24, 24, 31, 0, SK_PAPER);
    i_line_w(ic, 20.5f, 31, 20.5f, 14, SK_INK, 1.3f);
    i_line_w(ic, 32.5f, 28, 32.5f, 11, SK_INK, 1.3f);
    float beam[8] = { 20.5f, 13, 32.5f, 10, 32.5f, 14.5f, 20.5f, 17.5f };
    i_poly(ic, beam, 4, SK_INK, SK_INK);
    i_ellipse(ic, 17.5f, 32, 8, 6, SK_INK, SK_INK);
    i_ellipse(ic, 29.5f, 29, 8, 6, SK_INK, SK_INK);
}

typedef void (*icon_fn)(ictx *);
static const icon_fn icon_table[SK_ICON_COUNT] = {
    [SK_ICON_TERMINAL] = ic_terminal,     [SK_ICON_FOLDER] = ic_folder,
    [SK_ICON_FILE] = ic_file,             [SK_ICON_TEXT] = ic_text,
    [SK_ICON_IMAGE] = ic_image,           [SK_ICON_PROGRAM] = ic_program,
    [SK_ICON_NOTEPAD] = ic_notepad,       [SK_ICON_PAINT] = ic_paint,
    [SK_ICON_MONITOR] = ic_monitor,       [SK_ICON_CLOCK] = ic_clock,
    [SK_ICON_ABOUT] = ic_about,           [SK_ICON_SETTINGS] = ic_settings,
    [SK_ICON_POWER] = ic_power,           [SK_ICON_REBOOT] = ic_reboot,
    [SK_ICON_HOME] = ic_home,             [SK_ICON_UP] = ic_up,
    [SK_ICON_BACK] = ic_back,             [SK_ICON_NEW_FOLDER] = ic_new_folder,
    [SK_ICON_TRASH] = ic_trash,           [SK_ICON_SAVE] = ic_save,
    [SK_ICON_OPEN] = ic_open,             [SK_ICON_PENCIL] = ic_pencil,
    [SK_ICON_ERASER] = ic_eraser,         [SK_ICON_BRUSH] = ic_brush,
    [SK_ICON_LINE] = ic_line_tool,        [SK_ICON_RECT] = ic_rect_tool,
    [SK_ICON_CIRCLE] = ic_circle_tool,    [SK_ICON_FILL] = ic_fill,
    [SK_ICON_KERNEL] = ic_kernel,         [SK_ICON_CHIP] = ic_chip,
    [SK_ICON_MEMORY] = ic_memory,         [SK_ICON_PROCESS] = ic_process,
    [SK_ICON_SKETCH_LOGO] = ic_logo,      [SK_ICON_BOOK] = ic_book,
    [SK_ICON_REFRESH] = ic_refresh,       [SK_ICON_SEARCH] = ic_search,
    [SK_ICON_MUSIC] = ic_music,
};

void sk_draw_icon(sk_canvas *c, int icon, int x, int y, int size, uint32_t seed)
{
    if (!c || size < 6 || size > 1024)
        return;
    if (icon < 0 || icon >= SK_ICON_COUNT || !icon_table[icon])
        icon = SK_ICON_PROGRAM;
    ictx ic;
    ic.c = c;
    ic.size = size;
    ic.k = (float)size / 48.0f;
    ic.ox = (float)x;
    ic.oy = (float)y;
    /* 线宽、抖动随尺寸变化：16px 时细而规整，48px 时粗一点、更“手绘” */
    ic.lw = 0.85f + (float)size * 0.029f;
    if (ic.lw > 3.2f) ic.lw = 3.2f;
    ic.rough = (float)size / 58.0f;
    if (ic.rough < 0.22f) ic.rough = 0.22f;
    if (ic.rough > 0.9f) ic.rough = 0.9f;
    ic.dbl = size >= 30;
    ic.hatch = size >= 24;
    ic.small = size < 22;
    ic.seed = sk_hash_u32(seed, (uint32_t)icon * 0x9E37u + 0x1C0Eu);
    ic.n = 0;
    /* 图标只画在自己的方块里（外扩 1 像素给抖动），局部重绘时不会弄脏邻居 */
    sk_rect old = sk_push_clip(c, sk_rect_make(x - 1, y - 1, size + 2, size + 2));
    if (!sk_rect_empty(c->clip))
        icon_table[icon](&ic);
    sk_pop_clip(c, old);
}

int sk_icon_from_name(const char *name)
{
    static const struct { const char *name; int icon; } map[] = {
        { "terminal", SK_ICON_TERMINAL }, { "folder", SK_ICON_FOLDER }, { "file", SK_ICON_FILE },
        { "text", SK_ICON_TEXT }, { "image", SK_ICON_IMAGE }, { "program", SK_ICON_PROGRAM },
        { "notepad", SK_ICON_NOTEPAD }, { "paint", SK_ICON_PAINT }, { "monitor", SK_ICON_MONITOR },
        { "clock", SK_ICON_CLOCK }, { "about", SK_ICON_ABOUT }, { "settings", SK_ICON_SETTINGS },
        { "power", SK_ICON_POWER }, { "reboot", SK_ICON_REBOOT }, { "home", SK_ICON_HOME },
        { "up", SK_ICON_UP }, { "back", SK_ICON_BACK }, { "new-folder", SK_ICON_NEW_FOLDER },
        { "trash", SK_ICON_TRASH }, { "save", SK_ICON_SAVE }, { "open", SK_ICON_OPEN },
        { "pencil", SK_ICON_PENCIL }, { "eraser", SK_ICON_ERASER }, { "brush", SK_ICON_BRUSH },
        { "line", SK_ICON_LINE }, { "rect", SK_ICON_RECT }, { "circle", SK_ICON_CIRCLE },
        { "fill", SK_ICON_FILL }, { "kernel", SK_ICON_KERNEL }, { "chip", SK_ICON_CHIP },
        { "memory", SK_ICON_MEMORY }, { "process", SK_ICON_PROCESS }, { "logo", SK_ICON_SKETCH_LOGO },
        { "book", SK_ICON_BOOK }, { "refresh", SK_ICON_REFRESH }, { "search", SK_ICON_SEARCH },
        { "music", SK_ICON_MUSIC },
        /* 常见的别名 */
        { "new_folder", SK_ICON_NEW_FOLDER }, { "sketch", SK_ICON_SKETCH_LOGO },
        { "sketchos", SK_ICON_SKETCH_LOGO }, { "document", SK_ICON_TEXT }, { "picture", SK_ICON_IMAGE },
        { "cpu", SK_ICON_CHIP }, { "ram", SK_ICON_MEMORY }, { "shutdown", SK_ICON_POWER },
        { "restart", SK_ICON_REBOOT }, { "delete", SK_ICON_TRASH }, { "info", SK_ICON_ABOUT },
        { "rectangle", SK_ICON_RECT }, { "ellipse", SK_ICON_CIRCLE }, { "bucket", SK_ICON_FILL },
        { "files", SK_ICON_FOLDER }, { "help", SK_ICON_BOOK }, { "doc", SK_ICON_BOOK },
        { "find", SK_ICON_SEARCH }, { "audio", SK_ICON_MUSIC }, { "sound", SK_ICON_MUSIC },
    };
    if (!name)
        return SK_ICON_PROGRAM;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(name, map[i].name) == 0)
            return map[i].icon;
    return SK_ICON_PROGRAM;
}
