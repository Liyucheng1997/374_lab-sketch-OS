/*
 * test_rough_sheet.c — 手绘图元样张（宿主机离线渲染）
 *
 * 画出：各种角度/粗糙度的直线、矩形、圆角矩形、椭圆、圆弧、曲线、每一种填充方式、
 * 自由笔触、下划线、荧光笔、纸张和方格纸样本。输出 rough_sheet.ppm，用
 *   python3 tools/ppm2png.py rough_sheet.ppm
 * 转成 PNG 查看。用法：test_rough_sheet [输出目录]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sketch/gfx.h"
#include "sketch/rough.h"
#include "sketch/theme.h"

static char g_out[512] = ".";

static void save(const sk_canvas *c, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_out, name);
    if (sk_image_save_ppm(c, path) != 0) {
        fprintf(stderr, "写入 %s 失败\n", path);
        exit(1);
    }
    printf("%s\n", path);
}

/* 方格纸：桌面背景的样子 */
static void grid_paper(sk_canvas *c, sk_rect r, int cell)
{
    sk_paper_fill(c, r, SK_DESK_PAPER, 6, 7);
    sk_rect old = sk_push_clip(c, r);
    for (int x = r.x + cell / 2; x < r.x + r.w; x += cell)
        sk_vline(c, x, r.y, r.y + r.h - 1, SK_DESK_GRID);
    for (int y = r.y + cell / 2; y < r.y + r.h; y += cell)
        sk_hline(c, r.x, r.x + r.w - 1, y, SK_DESK_GRID);
    sk_pop_clip(c, old);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(g_out, sizeof g_out, "%s", argv[1]);
    const int W = 1200, H = 1000;
    sk_canvas *c = sk_canvas_new(W, H);
    if (!c)
        return 1;
    sk_paper_fill(c, sk_rect_make(0, 0, W, H), SK_PAPER, 6, 1);

    sk_rough_style s;

    /* 第 1 行：不同粗糙度的直线 + 不同角度的直线 */
    for (int i = 0; i < 5; i++) {
        sk_rough_default(&s);
        s.roughness = 0.5f * (float)i;
        s.seed = 10 + i;
        float y = 30.0f + 22.0f * i;
        sk_rough_line(c, 20, y, 260, y, &s);
    }
    for (int i = 0; i < 12; i++) {
        sk_rough_default(&s);
        s.seed = 100 + i;
        float a = (float)i * 3.14159f / 12.0f;
        float cx = 380, cy = 80;
        sk_rough_line(c, cx, cy, cx + cosf(a) * 70, cy - sinf(a) * 70, &s);
    }
    /* 抗锯齿精确线（对比） */
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 3.14159f / 8.0f;
        sk_line_aa(c, 560, 80, 560 + cosf(a) * 60, 80 + sinf(a) * 60, 0.5f + 0.6f * i, SK_INK);
    }
    sk_fill_circle_aa(c, 680, 60, 18, SK_BLUE);
    sk_circle_aa(c, 680, 60, 26, 1.5f, SK_INK);
    sk_fill_circle_aa(c, 730, 60, 3.3f, SK_RED);
    sk_fill_circle_aa(c, 750, 60, 1.2f, SK_RED);
    sk_fill_circle_aa(c, 765, 60, 0.4f, SK_RED);
    sk_circle_aa(c, 680, 120, 12, 0.6f, SK_INK);
    sk_circle_aa(c, 730, 120, 12, 4.0f, SK_GREEN);

    /* 不同线宽/颜色的手绘矩形 */
    sk_rough_default(&s);
    s.seed = 5;
    sk_rough_rect(c, 800, 30, 160, 90, &s);
    s.seed = 6;
    s.stroke_width = 3;
    s.stroke = SK_RED;
    sk_rough_rect(c, 990, 30, 180, 90, &s);

    /* 第 2 行：每一种填充方式（矩形） */
    const char *fills[] = { "solid", "hachure", "cross", "zigzag", "dots", "marker" };
    sk_color fc[] = { SK_YELLOW, SK_BLUE, SK_GREEN, SK_ORANGE, SK_PURPLE, SK_PINK };
    for (int i = 0; i < 6; i++) {
        sk_rough_default(&s);
        s.seed = 200 + i;
        s.fill_style = SK_FILL_SOLID + i;
        s.fill = fc[i];
        sk_rough_rect(c, 20.0f + 195.0f * i, 170, 170, 110, &s);
        (void)fills;
    }
    /* 第 3 行：椭圆 + 填充 */
    for (int i = 0; i < 6; i++) {
        sk_rough_default(&s);
        s.seed = 300 + i;
        s.fill_style = SK_FILL_SOLID + i;
        s.fill = fc[(i + 2) % 6];
        sk_rough_ellipse(c, 105.0f + 195.0f * i, 370, 160, 110, &s);
    }
    /* 第 4 行：圆角矩形、多边形、圆弧、曲线 */
    sk_rough_default(&s);
    s.seed = 400;
    s.fill_style = SK_FILL_HACHURE;
    s.fill = SK_TEAL;
    sk_rough_round_rect(c, 20, 460, 170, 100, 18, &s);
    sk_pointf star[10];
    for (int i = 0; i < 10; i++) {
        float a = -3.14159f / 2 + i * 3.14159f / 5;
        float rr = (i & 1) ? 26 : 60;
        star[i].x = 300 + cosf(a) * rr;
        star[i].y = 515 + sinf(a) * rr;
    }
    s.seed = 401;
    s.fill = SK_YELLOW;
    sk_rough_polygon(c, star, 10, &s);
    s.seed = 402;
    s.fill = SK_RED;
    s.fill_style = SK_FILL_HACHURE;
    sk_rough_arc(c, 460, 515, 120, 120, 0.3f, 4.5f, 1, &s);
    s.fill_style = SK_FILL_NONE;
    s.seed = 403;
    sk_rough_arc(c, 600, 515, 110, 90, 3.6f, 6.0f, 0, &s);
    sk_pointf wave[8];
    for (int i = 0; i < 8; i++) {
        wave[i].x = 680 + i * 28;
        wave[i].y = 515 + ((i & 1) ? -30 : 30);
    }
    s.seed = 404;
    s.stroke = SK_BLUE;
    s.stroke_width = 2;
    sk_rough_curve(c, wave, 8, &s);
    s.seed = 405;
    s.stroke = SK_INK;
    s.stroke_width = 1.6f;
    for (int i = 0; i < 8; i++)
        wave[i].y += 10;
    sk_rough_polyline(c, wave, 8, &s);
    /* 小尺寸 */
    for (int i = 0; i < 5; i++) {
        sk_rough_default(&s);
        s.seed = 500 + i;
        s.roughness = 0.6f;
        float sz = 10.0f + i * 8.0f;
        sk_rough_rect(c, 940.0f + i * 50.0f - sz * 0.5f, 490 - sz * 0.5f, sz, sz, &s);
        sk_rough_circle(c, 940.0f + i * 50.0f, 545, sz, &s);
    }

    /* 第 5 行：自由笔触、下划线、荧光笔 */
    sk_pointf pts[40];
    for (int i = 0; i < 40; i++) {
        pts[i].x = 30 + i * 7;
        pts[i].y = 640 + sinf(i * 0.35f) * 25;
    }
    sk_brush_stroke(c, pts, 40, 1.0f, 6.0f, SK_INK, 0, 1);
    for (int i = 0; i < 40; i++)
        pts[i].y += 50;
    sk_brush_stroke(c, pts, 40, 5.0f, 1.0f, SK_BLUE, 1, 2);
    for (int i = 0; i < 40; i++)
        pts[i].y += 45;
    sk_brush_stroke(c, pts, 40, 14.0f, 14.0f, SK_WITH_ALPHA(SK_RED, 0x80), 0, 3);
    sk_rough_underline(c, 360, 600, 650, SK_INK, 2.0f, 1);
    sk_rough_underline(c, 360, 480, 690, SK_RED, 1.6f, 2);
    sk_marker_highlight(c, sk_rect_make(360, 710, 240, 26), SK_HIGHLIGHT, 3);
    sk_marker_highlight(c, sk_rect_make(360, 750, 120, 22), SK_WITH_ALPHA(SK_GREEN, 0x70), 4);
    /* 大小不同的点 */
    for (int i = 0; i < 10; i++)
        sk_fill_circle_aa(c, 640 + i * 18, 650, 0.5f + i * 0.6f, SK_INK);

    /* 纸张/方格纸样本 */
    sk_paper_fill(c, sk_rect_make(640, 690, 250, 130), SK_PAPER, 20, 2);
    sk_paper_fill(c, sk_rect_make(900, 690, 280, 130), SK_PAPER_DARK, 8, 3);
    grid_paper(c, sk_rect_make(20, 840, 560, 150), 24);
    sk_paper_fill(c, sk_rect_make(600, 840, 280, 150), SK_PAPER, 6, 4);
    sk_paper_fill(c, sk_rect_make(890, 840, 290, 150), SK_PAPER, 12, 4);
    /* 线宽对比 */
    for (int i = 0; i < 6; i++) {
        sk_rough_default(&s);
        s.seed = 600 + i;
        s.stroke_width = 0.8f + 0.6f * i;
        sk_rough_line(c, 620, 860 + i * 20, 860, 860 + i * 20, &s);
    }

    save(c, "rough_sheet.ppm");
    sk_canvas_free(c);
    return 0;
}
