/*
 * test_rough_unit.c — rough.c / theme.c / icons.c 的单元测试
 *
 *   1. 随机数与哈希：确定性、seed=0 可用、分布大致均匀、FNV-1a 的已知值；
 *   2. 确定性：同一个 seed 画两遍逐位相同，换 seed 就不同；
 *   3. 轮廓和填充是两条独立的随机数流：加上排线后轮廓一像素都不动；
 *   4. 局部重绘 == 整体重绘（逐位相同）：把画布切成小块，每块单独设裁剪区重画，
 *      拼起来必须和一次画完完全一样——这是“只重绘脏矩形”能成立的前提；
 *   5. 笔触的面积/粗细准确，半透明笔触自身重叠处只混合一次；
 *   6. 健壮性：随机（含 NaN、无穷大、巨大、负数）参数调用所有手绘/控件/图标函数，
 *      不崩溃、不越界（哨兵画布）、不会因为巨大的图形而卡死（限时）。
 * 用法：test_rough_unit；全部通过返回 0。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sketch/gfx.h"
#include "sketch/rough.h"
#include "sketch/theme.h"
#include "sketch/font.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "失败 %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---------------------------------------------------------------- 随机数 */
static void test_rng(void)
{
    sk_rng a, b;
    sk_rng_seed(&a, 42);
    sk_rng_seed(&b, 42);
    int same = 1;
    for (int i = 0; i < 1000; i++)
        if (sk_rng_u32(&a) != sk_rng_u32(&b)) same = 0;
    CHECK(same, "同 seed 同序列");
    sk_rng_seed(&a, 0);
    uint32_t first = sk_rng_u32(&a), changed = 0;
    for (int i = 0; i < 100; i++)
        if (sk_rng_u32(&a) != first) changed++;
    CHECK(changed > 95, "seed=0 不能卡住");
    sk_rng_seed(&a, 1);
    sk_rng_seed(&b, 2);
    int diff = 0;
    for (int i = 0; i < 64; i++)
        if (sk_rng_u32(&a) != sk_rng_u32(&b)) diff++;
    CHECK(diff > 60, "相邻 seed 的序列不相关");
    double sum = 0;
    float mn = 1, mx = 0;
    sk_rng_seed(&a, 7);
    for (int i = 0; i < 100000; i++) {
        float f = sk_rng_float(&a);
        sum += f;
        if (f < mn) mn = f;
        if (f > mx) mx = f;
    }
    CHECK(mn >= 0.0f && mx < 1.0f && fabs(sum / 100000 - 0.5) < 0.01, "float ∈ [0,1) 且均值 0.5（%.4f）", sum / 100000);
    float r = sk_rng_range(&a, -3.0f, -1.0f);
    CHECK(r >= -3.0f && r < -1.0f, "range");
    CHECK(sk_hash_str("") == 2166136261u && sk_hash_str("a") == 0xE40C292Cu && sk_hash_str(NULL) == 2166136261u,
          "FNV-1a 已知值");
    CHECK(sk_hash_str("terminal") != sk_hash_str("terminaL"), "hash_str 区分大小写");
    CHECK(sk_hash_u32(1, 2) != sk_hash_u32(2, 1) && sk_hash_u32(0, 0) != sk_hash_u32(0, 1), "hash_u32 混合");
}

/* ---------------------------------------------------------------- 绘制场景 */
typedef void (*scene_fn)(sk_canvas *c, uint32_t seed);

static void bg(sk_canvas *c) { sk_clear(c, SK_PAPER); }

static void sc_rect_hachure(sk_canvas *c, uint32_t seed)
{
    sk_rough_style s;
    sk_style_hatched(&s, SK_BLUE, seed);
    sk_rough_rect(c, 13.3f, 11.7f, 150.2f, 90.4f, &s);
}
static void sc_ellipse_cross(sk_canvas *c, uint32_t seed)
{
    sk_rough_style s;
    sk_style_hatched(&s, SK_WITH_ALPHA(SK_GREEN, 0xA0), seed);
    s.fill_style = SK_FILL_CROSS_HATCH;
    sk_rough_ellipse(c, 90, 60, 150, 100, &s);
}
static void sc_rrect_marker(sk_canvas *c, uint32_t seed)
{
    sk_rough_style s;
    sk_style_hatched(&s, SK_PINK, seed);
    s.fill_style = SK_FILL_MARKER;
    sk_rough_round_rect(c, 10, 15, 160, 80, 14, &s);
}
static void sc_poly_zigzag(sk_canvas *c, uint32_t seed)
{
    sk_pointf p[6] = { { 10, 20 }, { 90, 5 }, { 170, 30 }, { 150, 110 }, { 80, 70 }, { 20, 100 } };
    sk_rough_style s;
    sk_style_hatched(&s, SK_ORANGE, seed);
    s.fill_style = SK_FILL_ZIGZAG;
    sk_rough_polygon(c, p, 6, &s);
}
static void sc_dots_solid(sk_canvas *c, uint32_t seed)
{
    sk_rough_style s;
    sk_style_hatched(&s, SK_PURPLE, seed);
    s.fill_style = SK_FILL_DOTS;
    sk_rough_circle(c, 50, 60, 80, &s);
    s.fill_style = SK_FILL_SOLID;
    s.seed ^= 0x77;
    sk_rough_rect(c, 100, 20, 70, 80, &s);
}
static void sc_lines_arcs(sk_canvas *c, uint32_t seed)
{
    sk_rough_style s;
    sk_style_pencil(&s, seed);
    sk_rough_line(c, -40, 5, 220, 110, &s);
    s.fill_style = SK_FILL_HACHURE;
    s.fill = SK_RED;
    sk_rough_arc(c, 90, 60, 100, 90, 0.5f, 4.0f, 1, &s);
    sk_pointf p[5] = { { 5, 100 }, { 40, 80 }, { 90, 110 }, { 130, 70 }, { 175, 95 } };
    s.fill_style = SK_FILL_NONE;
    sk_rough_curve(c, p, 5, &s);
    sk_rough_polyline(c, p, 5, &s);
}
static void sc_brush(sk_canvas *c, uint32_t seed)
{
    sk_pointf p[30];
    for (int i = 0; i < 30; i++) {
        p[i].x = 10 + i * 5.5f;
        p[i].y = 60 + 40 * sinf(i * 0.4f);
    }
    sk_brush_stroke(c, p, 30, 1.0f, 9.0f, SK_INK, 1, seed);
    sk_rough_underline(c, 20, 160, 110, SK_RED, 2.0f, seed);
    sk_marker_highlight(c, sk_rect_make(30, 10, 120, 24), SK_HIGHLIGHT, seed);
}
static void sc_paper(sk_canvas *c, uint32_t seed)
{
    sk_paper_fill(c, sk_rect_make(0, 0, c->w, c->h), SK_PAPER, 14, seed);
}
static void sc_widgets(sk_canvas *c, uint32_t seed)
{
    const char *tabs[3] = { "一", "二二", "三三三" };
    sk_draw_button(c, sk_rect_make(5, 5, 80, 34), "", SK_STATE_HOVER, SK_BLUE, seed);
    sk_draw_checkbox(c, sk_rect_make(95, 8, 80, 24), "", SK_STATE_CHECKED | SK_STATE_HOVER, seed);
    sk_draw_meter(c, sk_rect_make(5, 50, 170, 18), 640, SK_GREEN, seed);
    sk_draw_scrollbar(c, sk_rect_make(160, 75, 14, 45), 100, 30, 40, SK_STATE_HOVER, seed);
    sk_draw_focus_ring(c, sk_rect_make(3, 75, 150, 40), seed);
    sk_draw_tabs(c, sk_rect_make(5, 80, 150, 30), tabs, 0, 0, -1, seed);   /* n=0：什么都不画 */
    sk_draw_icon(c, SK_ICON_PAINT, 60, 72, 48, seed);
}
static void sc_frame(sk_canvas *c, uint32_t seed)
{
    sk_draw_window_frame(c, sk_rect_make(12, 40, 140, 60), NULL, 1, 0, SK_TEAL, seed);
}

static const struct { const char *name; scene_fn fn; } scenes[] = {
    { "矩形+排线", sc_rect_hachure }, { "椭圆+交叉排线", sc_ellipse_cross }, { "圆角矩形+马克笔", sc_rrect_marker },
    { "多边形+zigzag", sc_poly_zigzag }, { "点+实心", sc_dots_solid }, { "直线/圆弧/曲线", sc_lines_arcs },
    { "笔触/下划线/高亮", sc_brush }, { "纸张", sc_paper }, { "控件", sc_widgets }, { "窗口边框", sc_frame },
};
#define NSCENES (int)(sizeof scenes / sizeof scenes[0])
#define SW 180
#define SH 125

static void render(sk_canvas *c, int i, uint32_t seed)
{
    sk_reset_clip(c);
    bg(c);
    scenes[i].fn(c, seed);
}

static void test_determinism(void)
{
    sk_canvas *a = sk_canvas_new(SW, SH), *b = sk_canvas_new(SW, SH);
    for (int i = 0; i < NSCENES; i++) {
        render(a, i, 1234);
        render(b, i, 1234);
        CHECK(memcmp(a->pixels, b->pixels, SW * SH * 4) == 0, "%s：同 seed 两次结果不同", scenes[i].name);
        render(b, i, 99);
        CHECK(memcmp(a->pixels, b->pixels, SW * SH * 4) != 0, "%s：换 seed 结果应该不同", scenes[i].name);
    }
    sk_canvas_free(a);
    sk_canvas_free(b);
}

/* 局部重绘：按 tw×th 的小块分别设裁剪区重画，结果必须和整体一次画完逐位相同 */
static void test_partial_redraw(void)
{
    sk_canvas *full = sk_canvas_new(SW, SH), *tiled = sk_canvas_new(SW, SH);
    static const int tiles[3][2] = { { 37, 29 }, { 7, 11 }, { SW, 3 } };
    for (int i = 0; i < NSCENES; i++) {
        render(full, i, 777);
        for (int t = 0; t < 3; t++) {
            int tw = tiles[t][0], th = tiles[t][1];
            sk_reset_clip(tiled);
            sk_clear(tiled, 0);
            for (int y = 0; y < SH; y += th)
                for (int x = 0; x < SW; x += tw) {
                    sk_set_clip(tiled, sk_rect_make(x, y, tw, th));
                    bg(tiled);
                    scenes[i].fn(tiled, 777);
                }
            int diff = 0, maxd = 0;
            for (int k = 0; k < SW * SH; k++) {
                uint32_t p = full->pixels[k], q = tiled->pixels[k];
                if (p != q) {
                    diff++;
                    int d = abs((int)SK_R(p) - (int)SK_R(q)) + abs((int)SK_G(p) - (int)SK_G(q)) +
                            abs((int)SK_B(p) - (int)SK_B(q));
                    if (d > maxd) maxd = d;
                }
            }
            CHECK(diff == 0, "%s：按 %dx%d 分块重绘有 %d 个像素不同（最大差 %d）", scenes[i].name, tw, th, diff, maxd);
        }
    }
    sk_canvas_free(full);
    sk_canvas_free(tiled);
}

/* 给图形加上填充以后，轮廓（完全被墨覆盖的像素）必须一点不动 */
static void test_fill_independent(void)
{
    sk_canvas *a = sk_canvas_new(SW, SH), *b = sk_canvas_new(SW, SH);
    for (int shape = 0; shape < 3; shape++) {
        for (int fs = SK_FILL_SOLID; fs <= SK_FILL_MARKER; fs++) {
            sk_rough_style s;
            sk_style_pencil(&s, 555 + shape);
            bg(a);
            bg(b);
            for (int pass = 0; pass < 2; pass++) {
                sk_canvas *c = pass ? b : a;
                s.fill_style = pass ? fs : SK_FILL_NONE;
                s.fill = SK_YELLOW;
                if (shape == 0) sk_rough_rect(c, 20, 20, 140, 80, &s);
                else if (shape == 1) sk_rough_ellipse(c, 90, 62, 140, 90, &s);
                else sk_rough_round_rect(c, 20, 20, 140, 80, 12, &s);
            }
            int ink = 0, moved = 0;
            for (int k = 0; k < SW * SH; k++)
                if (a->pixels[k] == SK_INK) {
                    ink++;
                    if (b->pixels[k] != SK_INK) moved++;
                }
            CHECK(ink > 200 && moved == 0, "形状 %d 填充 %d：轮廓变了（%d/%d）", shape, fs, moved, ink);
        }
    }
    sk_canvas_free(a);
    sk_canvas_free(b);
}

/* ---------------------------------------------------------------- 笔触精度 */
static void test_stroke_accuracy(void)
{
    sk_canvas *c = sk_canvas_new(200, 100);
    sk_clear(c, SK_RGB(255, 255, 255));
    sk_pointf p[2] = { { 30.3f, 40.6f }, { 170.1f, 61.2f } };
    sk_brush_stroke(c, p, 2, 4.0f, 4.0f, SK_RGB(0, 0, 0), 0, 1);
    double a = 0;
    for (int k = 0; k < 200 * 100; k++)
        a += (255 - SK_R(c->pixels[k])) / 255.0;
    double len = hypot(p[1].x - p[0].x, p[1].y - p[0].y), e = len * 4 + M_PI * 4;
    CHECK(fabs(a - e) / e < 0.03, "笔触面积 %.1f（理论 %.1f）", a, e);

    /* 一笔自己绕圈重叠：半透明颜色在重叠处也只混合一次（整笔一次合成） */
    sk_clear(c, SK_RGB(255, 255, 255));
    sk_pointf loop[40];
    for (int i = 0; i < 40; i++) {
        float t = i * 0.2f;
        loop[i].x = 100 + 30 * cosf(t) + i * 0.8f;
        loop[i].y = 50 + 30 * sinf(t);
    }
    sk_brush_stroke(c, loop, 40, 8.0f, 8.0f, SK_RGBA(0, 0, 255, 128), 0, 2);
    uint32_t darkest = 0xFFFFFFFFu;
    int full = 0;
    for (int k = 0; k < 200 * 100; k++) {
        uint32_t v = c->pixels[k];
        if (SK_R(v) < SK_R(darkest)) darkest = v;
        if (SK_B(v) == 255 && SK_R(v) < 140) full++;
    }
    CHECK(full > 500 && SK_R(darkest) >= 126, "半透明笔触重叠处被混合了两次（最深 R=%d）", SK_R(darkest));

    /* roughness=0 的“手绘线”就是一条直线：离线 3 像素外不受影响 */
    sk_clear(c, SK_RGB(255, 255, 255));
    sk_rough_style s;
    sk_style_pencil(&s, 3);
    s.roughness = 0;
    sk_rough_line(c, 20, 50, 180, 50, &s);
    int stray = 0, onl = 0;
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 200; x++) {
            int v = SK_R(c->pixels[y * 200 + x]);
            if (v < 250 && (y < 47 || y > 52)) stray++;
            if (y == 50 && x > 25 && x < 175 && v < 128) onl++;
        }
    CHECK(stray == 0 && onl > 140, "roughness=0 的直线（越界 %d，线上 %d）", stray, onl);

    /* 纸张：颜色只由 (seed,x,y) 决定，和填充的矩形无关；strength=0 是纯色 */
    sk_canvas *p1 = sk_canvas_new(64, 64), *p2 = sk_canvas_new(64, 64);
    sk_paper_fill(p1, sk_rect_make(0, 0, 64, 64), SK_PAPER, 10, 5);
    sk_clear(p2, 0);
    sk_paper_fill(p2, sk_rect_make(-100, 7, 120, 3), SK_PAPER, 10, 5);
    sk_paper_fill(p2, sk_rect_make(30, 30, 1000, 1000), SK_PAPER, 10, 5);
    int mism = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint32_t v = p2->pixels[y * 64 + x];
            if (v && v != p1->pixels[y * 64 + x]) mism++;
        }
    CHECK(mism == 0, "纸张颗粒和填充范围有关（%d）", mism);
    sk_paper_fill(p2, sk_rect_make(0, 0, 64, 64), SK_PAPER, 0, 5);
    CHECK(p2->pixels[100] == SK_PAPER && p2->pixels[4000] == SK_PAPER, "strength=0 是纯色");
    sk_paper_fill(p2, sk_rect_make(0, 0, 64, 64), SK_PAPER, 10, 6);
    CHECK(memcmp(p1->pixels, p2->pixels, 64 * 64 * 4) != 0, "不同 seed 的纸纹不同");
    int maxdark = 0;
    for (int k = 0; k < 64 * 64; k++) {
        int d = (int)SK_G(SK_PAPER) - (int)SK_G(p1->pixels[k]);
        if (d > maxdark) maxdark = d;
        if (SK_A(p1->pixels[k]) != 255) maxdark = 999;
    }
    CHECK(maxdark <= 10 && maxdark >= 3, "颗粒强度 10 时最暗约 10 级（实际 %d），且不透明", maxdark);
    sk_canvas_free(p1);
    sk_canvas_free(p2);
    sk_canvas_free(c);
}

/* ---------------------------------------------------------------- 健壮性模糊测试 */
#define CANARY 0x0BADF00Du
static uint32_t rs = 99;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}
static float rf(float lo, float hi)
{
    static const float sp[] = { NAN, INFINITY, -INFINITY, 1e9f, -1e9f, 3e38f, 0.0f, -0.0f, 1e-20f, 5e6f };
    if (rnd() % 17 == 0)
        return sp[rnd() % 10];
    return lo + (hi - lo) * (float)(rnd() % 100000) / 100000.0f;
}
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

static void rnd_style(sk_rough_style *s)
{
    sk_rough_default(s);
    s->seed = rnd();
    if (rnd() % 3 == 0) s->roughness = rf(-1, 5);
    if (rnd() % 3 == 0) s->bowing = rf(-5, 5);
    if (rnd() % 3 == 0) s->stroke_width = rf(-2, 30);
    s->stroke = rnd() % 2 ? SK_INK : SK_RGBA(ri(0, 255), 0, 0, ri(0, 255));
    s->double_stroke = (int)(rnd() % 2);
    s->fill_style = ri(-1, 8);
    s->fill = rnd() % 2 ? SK_BLUE : SK_RGBA(0, ri(0, 255), 0, ri(0, 255));
    if (rnd() % 2) s->hachure_angle = rf(-720, 720);
    if (rnd() % 2) s->hachure_gap = rf(-3, 60);
    if (rnd() % 2) s->fill_weight = rf(-1, 20);
}

static void fuzz_call(sk_canvas *c, int W, int H)
{
    sk_rough_style s;
    rnd_style(&s);
    sk_pointf p[16];
    int n = ri(0, 16);
    for (int i = 0; i < n; i++) {
        p[i].x = rf(-W, 2 * W);
        p[i].y = rf(-H, 2 * H);
    }
    const char *lab = rnd() % 4 ? "标签 label" : (rnd() % 2 ? "" : NULL);
    const char *tabs[4] = { "甲", NULL, "丙丙丙丙丙丙丙丙丙丙", "" };
    sk_rect r = sk_rect_make(ri(-W, 2 * W), ri(-H, 2 * H), ri(-10, 2 * W), ri(-10, 2 * H));
    switch (rnd() % 24) {
    case 0: sk_rough_line(c, rf(-W, 2 * W), rf(-H, 2 * H), rf(-W, 2 * W), rf(-H, 2 * H), &s); break;
    case 1: sk_rough_rect(c, rf(-W, W), rf(-H, H), rf(-W, 3 * W), rf(-H, 3 * H), &s); break;
    case 2: sk_rough_round_rect(c, rf(-W, W), rf(-H, H), rf(-W, 3 * W), rf(-H, 3 * H), rf(-5, 80), &s); break;
    case 3: sk_rough_ellipse(c, rf(-W, 2 * W), rf(-H, 2 * H), rf(-W, 3 * W), rf(-H, 3 * H), &s); break;
    case 4: sk_rough_circle(c, rf(-W, 2 * W), rf(-H, 2 * H), rf(-W, 3 * W), &s); break;
    case 5: sk_rough_polygon(c, p, n, &s); break;
    case 6: sk_rough_polyline(c, p, n, &s); break;
    case 7: sk_rough_curve(c, p, n, &s); break;
    case 8: sk_rough_arc(c, rf(-W, 2 * W), rf(-H, 2 * H), rf(-W, 3 * W), rf(-H, 3 * H), rf(-20, 20), rf(-20, 20),
                         (int)(rnd() % 2), &s); break;
    case 9: sk_rough_fill_polygon(c, p, n, &s); break;
    case 10: sk_brush_stroke(c, p, n, rf(-1, 40), rf(-1, 40), s.stroke, (int)(rnd() % 2), rnd()); break;
    case 11: sk_rough_underline(c, rf(-W, 2 * W), rf(-W, 2 * W), rf(-H, 2 * H), s.stroke, rf(-1, 10), rnd()); break;
    case 12: sk_marker_highlight(c, r, SK_HIGHLIGHT, rnd()); break;
    case 13: sk_paper_fill(c, r, SK_PAPER, ri(-5, 40), rnd()); break;
    case 14: sk_draw_button(c, r, lab, ri(0, 31), SK_BLUE, rnd()); break;
    case 15: sk_draw_checkbox(c, r, lab, ri(0, 31), rnd()); break;
    case 16: sk_draw_textbox_frame(c, r, ri(0, 31), rnd()); break;
    case 17: sk_draw_meter(c, r, ri(-2000, 3000), SK_GREEN, rnd()); break;
    case 18: sk_draw_tabs(c, r, rnd() % 5 ? tabs : NULL, ri(-1, 4), ri(-2, 5), ri(-2, 5), rnd()); break;
    case 19: sk_draw_scrollbar(c, r, ri(-100, 10000), ri(-100, 10000), ri(-1000, 20000), ri(0, 31), rnd()); break;
    case 20: sk_draw_focus_ring(c, r, rnd()); sk_draw_separator(c, ri(-W, 2 * W), ri(-W, 2 * W), ri(-H, 2 * H), rnd()); break;
    case 21: sk_draw_window_frame(c, r, lab, (int)(rnd() % 2), ri(-2, 4), SK_PURPLE, rnd()); break;
    case 22: sk_draw_icon(c, ri(-3, SK_ICON_COUNT + 3), ri(-W, 2 * W), ri(-H, 2 * H), ri(-5, 200), rnd()); break;
    default: {
        sk_rect tr = sk_tabs_item_rect(r, tabs, ri(-1, 4), ri(-2, 5));
        (void)tr;
        sk_rect o = sk_frame_outer_rect(r), b = sk_frame_button_rect(r, ri(-2, 4));
        (void)o;
        (void)b;
        break;
    }
    }
}

static void test_fuzz(void)
{
    const int W = 90, H = 70, G = 16;
    int bw = W + 2 * G, bh = H + 2 * G;
    uint32_t *buf = malloc(sizeof(uint32_t) * bw * bh);
    for (int i = 0; i < bw * bh; i++)
        buf[i] = CANARY;
    sk_canvas c;
    sk_canvas_init(&c, buf + G * bw + G, W, H, bw);
    uint32_t *snap = malloc(sizeof(uint32_t) * W * H);
    double t0 = now_s(), worst = 0;
    int bad_clip = 0, bad_guard = 0;
    for (int it = 0; it < 6000; it++) {
        if (rnd() % 3 == 0)
            sk_reset_clip(&c);
        else
            sk_set_clip(&c, sk_rect_make(ri(-10, W), ri(-10, H), ri(-5, W + 20), ri(-5, H + 20)));
        sk_rect clip = c.clip;
        for (int y = 0; y < H; y++)
            memcpy(snap + y * W, c.pixels + y * bw, W * 4);
        double a = now_s();
        fuzz_call(&c, W, H);
        double dt = now_s() - a;
        if (dt > worst) worst = dt;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (!sk_rect_contains(clip, x, y) && c.pixels[y * bw + x] != snap[y * W + x])
                    bad_clip++;
    }
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            int in = x >= G && x < G + W && y >= G && y < G + H;
            if (!in && buf[y * bw + x] != CANARY)
                bad_guard++;
        }
    double total = now_s() - t0;
    CHECK(bad_guard == 0, "模糊测试：哨兵被改写 %d 个像素", bad_guard);
    CHECK(bad_clip == 0, "模糊测试：裁剪区外被改写 %d 个像素", bad_clip);
    /* 巨大图形（1e9 像素的矩形、几百万像素的椭圆）只画看得见的部分：单次调用不应超过 0.5 秒 */
    CHECK(worst < 0.5, "模糊测试：最慢的一次调用 %.3f 秒", worst);
    printf("  模糊测试 6000 次调用：%.2f 秒，最慢一次 %.1f 毫秒\n", total, worst * 1000);
    free(snap);
    free(buf);
}

int main(void)
{
    test_rng();
    test_determinism();
    test_partial_redraw();
    test_fill_independent();
    test_stroke_accuracy();
    test_fuzz();
    if (g_fail) {
        fprintf(stderr, "test_rough_unit: %d / %d 项检查失败\n", g_fail, g_checks);
        return 1;
    }
    printf("test_rough_unit: 全部 %d 项检查通过\n", g_checks);
    return 0;
}
