/*
 * splash.c — 开机动画：一支铅笔在纸上把 logo 和 “SketchOS · 手绘操作系统” 画出来
 *
 * 做法：先把最终画面（logo、标题、下划线、副标题）画进一张透明图层 g_final，
 * 动画只是按时间“揭开”它的一部分，再在揭开的边缘画一支铅笔：
 *   - logo：像排线一样一行一行来回扫（之字形），铅笔跟着来回走；
 *   - 文字和下划线：从左往右揭开，铅笔尖沿着字的中线轻轻上下晃动。
 * 每一帧只损坏“新揭开的那一条”和铅笔新旧位置，所以即使在很慢的网页模拟器里，
 * 每帧要合成的像素也很少。
 *
 * 动画按“经过的时间”计算进度而不是按帧数：机器慢、掉帧时，动画会跳着走，
 * 但总时长不变（约 1.2 秒），不会把开机拖得很久。
 */
#include "sketchd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SPLASH_SEED   0x5B1A5400u
#define LOGO_SIZE     132
#define LOGO_BANDS    7
#define T_LOGO_END    480
#define T_TEXT_START  420
#define T_TEXT_END    980
#define T_LINE_START  960
#define T_LINE_END    1180
#define T_DONE        1320
#define FRAME_MS      33

static int        g_active;
static uint64_t   g_t0;
static int        g_w, g_h;
static sk_canvas *g_final;           /* 屏幕大小的透明图层（只有中间画了东西） */
static sk_rect    g_logo, g_text, g_line;
static int        g_logo_prog;       /* 已揭开的 logo 进度（0..LOGO_BANDS*1000） */
static int        g_text_x, g_line_x;
static int        g_pen_x, g_pen_y, g_pen_on;

static const char *const TITLE = "SketchOS · 手绘操作系统";
static const char *const SUBTITLE = "正在铺开纸张，削好铅笔……";

int splash_active(void) { return g_active; }

/* ---------- 进度 → 揭开的区域、铅笔位置 ---------- */

static int band_h(void) { return (g_logo.h + LOGO_BANDS - 1) / LOGO_BANDS; }

/* logo 揭开的部分：前面若干整行 + 当前行的一段（偶数行从左往右，奇数行从右往左） */
static void logo_reveal(int prog, sk_rect *full, sk_rect *part, int *pen_x, int *pen_y)
{
    int bh = band_h();
    int band = prog / 1000, frac = prog % 1000;
    if (band >= LOGO_BANDS) {
        *full = g_logo;
        *part = sk_rect_make(0, 0, 0, 0);
        *pen_x = g_logo.x + g_logo.w;
        *pen_y = g_logo.y + g_logo.h;
        return;
    }
    *full = sk_rect_make(g_logo.x, g_logo.y, g_logo.w, band * bh);
    int by = g_logo.y + band * bh;
    int px = g_logo.w * frac / 1000;
    if (band % 2 == 0) {
        *part = sk_rect_make(g_logo.x, by, px, bh);
        *pen_x = g_logo.x + px;
    } else {
        *part = sk_rect_make(g_logo.x + g_logo.w - px, by, px, bh);
        *pen_x = g_logo.x + g_logo.w - px;
    }
    *pen_y = by + bh / 2;
}

static int clamp_prog(uint64_t t, int t0, int t1, int span)
{
    if ((int)t <= t0)
        return 0;
    if ((int)t >= t1)
        return span;
    return (int)(((int64_t)t - t0) * span / (t1 - t0));
}

/* 铅笔的外接矩形（笔尖在 (x,y)，笔杆往右上方伸出去） */
static sk_rect pen_rect(int x, int y) { return sk_rect_make(x - 8, y - 84, 96, 96); }

/* ---------- 绘制 ---------- */

static void draw_pencil(sk_canvas *c, float tx, float ty)
{
    /* 笔杆方向：右上 45°；u = 沿笔杆方向，v = 垂直方向 */
    const float ux = 0.7071f, uy = -0.7071f, vx = 0.7071f, vy = 0.7071f;
    const float half = 7.0f;
#define P(a, b) { tx + ux * (a) + vx * (b), ty + uy * (a) + vy * (b) }
    sk_pointf tip[]    = { P(0, 0), P(8, -2.4f), P(8, 2.4f) };
    sk_pointf wood[]   = { P(0, 0), P(20, -half), P(20, half) };
    sk_pointf body[]   = { P(20, -half), P(84, -half), P(84, half), P(20, half) };
    sk_pointf band[]   = { P(84, -half), P(92, -half), P(92, half), P(84, half) };
    sk_pointf rubber[] = { P(92, -half), P(101, -half), P(101, half), P(92, half) };
#undef P
    /* 投在纸上的影子 */
    sk_pointf shadow[4];
    for (int i = 0; i < 4; i++) {
        shadow[i].x = body[i].x + 5;
        shadow[i].y = body[i].y + 6;
    }
    sk_fill_polygon(c, shadow, 4, SK_RGBA(0x2B, 0x2B, 0x2B, 0x22));
    sk_rough_style s;
    sk_rough_default(&s);
    s.roughness = 0.6f;
    s.stroke_width = 1.4f;
    s.stroke = SK_INK;
    s.fill_style = SK_FILL_SOLID;
    s.seed = SPLASH_SEED + 1;
    s.fill = SK_YELLOW;
    sk_rough_polygon(c, body, 4, &s);
    s.seed++;
    s.fill = SK_RGB(0xF3, 0xD9, 0xB1);
    sk_rough_polygon(c, wood, 3, &s);
    s.seed++;
    s.fill = SK_INK_FAINT;
    sk_rough_polygon(c, band, 4, &s);
    s.seed++;
    s.fill = SK_PINK;
    sk_rough_polygon(c, rubber, 4, &s);
    sk_fill_polygon(c, tip, 3, SK_INK);
    /* 笔杆上的一道棱线 */
    sk_line_aa(c, body[0].x * 0.5f + body[3].x * 0.5f, body[0].y * 0.5f + body[3].y * 0.5f,
               body[1].x * 0.5f + body[2].x * 0.5f, body[1].y * 0.5f + body[2].y * 0.5f, 1.0f,
               SK_RGBA(0xF2, 0x9E, 0x4C, 0xC0));
}

static void reveal_blit(sk_canvas *dst, sk_rect area, sk_rect clip)
{
    sk_rect ir = sk_rect_intersect(area, clip);
    if (!sk_rect_empty(ir) && g_final)
        sk_blit_keyed(dst, ir.x, ir.y, g_final, ir);
}

void splash_paint(sk_canvas *dst, sk_rect r)
{
    sk_paper_fill(dst, r, SK_PAPER, 6, SPLASH_SEED);
    sk_rect full, part;
    int px, py;
    logo_reveal(g_logo_prog, &full, &part, &px, &py);
    reveal_blit(dst, full, r);
    reveal_blit(dst, part, r);
    reveal_blit(dst, sk_rect_make(g_text.x, g_text.y, g_text_x - g_text.x, g_text.h), r);
    reveal_blit(dst, sk_rect_make(g_line.x, g_line.y, g_line_x - g_line.x, g_line.h), r);
    if (g_pen_on && !sk_rect_empty(sk_rect_intersect(pen_rect(g_pen_x, g_pen_y), r)))
        draw_pencil(dst, (float)g_pen_x, (float)g_pen_y);
}

/* ---------- 动画控制 ---------- */

static void render_final(void)
{
    sk_canvas *c = g_final;
    sk_clear(c, SK_WITH_ALPHA(SK_PAPER, 0));
    sk_draw_icon(c, SK_ICON_SKETCH_LOGO, g_logo.x, g_logo.y, LOGO_SIZE, SPLASH_SEED + 10);
    sk_font *big = sk_font_get(SK_FONT_LARGE), *ui = sk_font_get(SK_FONT_UI);
    int tw = sk_text_width(big, TITLE);
    int base = g_text.y + sk_font_ascent(big) + 4;
    sk_draw_text(c, big, g_w / 2 - tw / 2, base, TITLE, SK_INK);
    float ux0 = (float)(g_w / 2 - tw / 2 - 8), ux1 = (float)(g_w / 2 + tw / 2 + 10);
    sk_rough_underline(c, ux0, ux1, (float)(g_line.y + 8), SK_RGBA(0xE0, 0x60, 0x5A, 0xE0), 2.6f,
                       SPLASH_SEED + 11);
    sk_rough_underline(c, ux0 + 30, ux1 - 50, (float)(g_line.y + 14), SK_RGBA(0x4A, 0x7B, 0xD0, 0xA0),
                       1.6f, SPLASH_SEED + 12);
    int sw = sk_text_width(ui, SUBTITLE);
    sk_draw_text(c, ui, g_w / 2 - sw / 2, g_line.y + 44, SUBTITLE, SK_INK_SOFT);
}

void splash_start(int w, int h)
{
    g_w = w;
    g_h = h;
    g_final = sk_canvas_new(w, h);
    if (!g_final) {
        skd_desktop_ready();
        return;
    }
    sk_font *big = sk_font_get(SK_FONT_LARGE);
    int tw = sk_text_width(big, TITLE);
    int lh = sk_font_line_height(big);
    /* 整组内容（logo + 标题 + 下划线 + 副标题，约 300px 高）在屏幕上垂直居中 */
    int cy = h / 2 + 30;
    g_logo = sk_rect_make(w / 2 - LOGO_SIZE / 2 - 6, cy - LOGO_SIZE - 26, LOGO_SIZE + 12, LOGO_SIZE + 12);
    g_text = sk_rect_make(w / 2 - tw / 2 - 4, cy, tw + 8, lh + 10);
    g_line = sk_rect_make(w / 2 - tw / 2 - 16, cy + lh + 6, tw + 40, 56);
    render_final();
    g_logo_prog = 0;
    g_text_x = g_text.x;
    g_line_x = g_line.x;
    g_pen_on = 0;
    g_active = 1;
    g_t0 = skd_now_ms();
    cursor_set_visible(0);
    comp_damage_all();
}

static void finish(void)
{
    if (!g_active)
        return;
    g_active = 0;
    if (g_final)
        sk_canvas_free(g_final);
    g_final = NULL;
    cursor_set_visible(1);
    comp_damage_all();
    skd_desktop_ready();
}

void splash_skip(void) { finish(); }

int splash_timeout_ms(void)
{
    if (!g_active)
        return -1;
    uint64_t t = skd_now_ms() - g_t0;
    uint64_t next = (t / FRAME_MS + 1) * FRAME_MS;
    return (int)(next - t);
}

void splash_tick(void)
{
    if (!g_active)
        return;
    uint64_t t = skd_now_ms() - g_t0;
    if (t >= T_DONE) {
        finish();
        return;
    }
    /* logo：损坏从旧进度到新进度跨过的那几行 */
    int lp = clamp_prog(t, 0, T_LOGO_END, LOGO_BANDS * 1000);
    if (lp != g_logo_prog) {
        int bh = band_h();
        int b0 = g_logo_prog / 1000, b1 = SK_MIN(lp / 1000, LOGO_BANDS - 1);
        comp_damage(sk_rect_make(g_logo.x, g_logo.y + b0 * bh, g_logo.w, (b1 - b0 + 1) * bh));
        g_logo_prog = lp;
    }
    int tx = g_text.x + clamp_prog(t, T_TEXT_START, T_TEXT_END, g_text.w);
    if (tx != g_text_x) {
        comp_damage(sk_rect_make(g_text_x, g_text.y, tx - g_text_x, g_text.h));
        g_text_x = tx;
    }
    int lx = g_line.x + clamp_prog(t, T_LINE_START, T_LINE_END, g_line.w);
    if (lx != g_line_x) {
        comp_damage(sk_rect_make(g_line_x, g_line.y, lx - g_line_x, g_line.h));
        g_line_x = lx;
    }
    /* 铅笔在当前正在画的地方 */
    int px, py, on = 1;
    if (t < T_TEXT_START) {
        sk_rect a, b;
        logo_reveal(g_logo_prog, &a, &b, &px, &py);
    } else if (t < T_LINE_START) {
        px = g_text_x;
        py = g_text.y + g_text.h / 2 + (int)(6.0f * sinf((float)t * 0.045f));
    } else if (t < T_LINE_END) {
        px = g_line_x;
        py = g_line.y + 10;
    } else {
        /* 画完了：铅笔往右上方抬起来，然后消失 */
        int k = (int)(t - T_LINE_END);
        px = g_line.x + g_line.w + k / 2;
        py = g_line.y + 10 - k / 2;
        on = k < 110;
    }
    if (px != g_pen_x || py != g_pen_y || on != g_pen_on) {
        if (g_pen_on)
            comp_damage(pen_rect(g_pen_x, g_pen_y));
        g_pen_x = px;
        g_pen_y = py;
        g_pen_on = on;
        if (on)
            comp_damage(pen_rect(px, py));
    }
}
