/*
 * test_rough_bench.c — 图形核心的性能基准
 *
 * 网页版跑在 v86 模拟器里，CPU 大约比宿主机慢 20 倍（浮点运算更慢）。这里测出宿主机上
 * 每个操作的耗时，乘以 20 就大致是网页版里的耗时，用来判断“一帧画得完吗”。
 * 也可以用 sk-cc 编成 i686 程序在 WSL 里直接跑（32 位 + x87 浮点，更接近模拟器里的情况）。
 * 用法：test_rough_bench [最短测量时间(秒)，默认 0.25]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sketch/gfx.h"
#include "sketch/rough.h"
#include "sketch/theme.h"
#include "sketch/font.h"

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static sk_canvas *g_c;
static double g_min = 0.25;
static int g_i;

typedef void (*bench_fn)(void);

static void run(const char *name, bench_fn fn)
{
    fn();                                   /* 预热：笔刷缓存、纸纹母版 */
    int n = 0;
    double t0 = now_s(), t;
    do {
        fn();
        n++;
        t = now_s() - t0;
    } while (t < g_min);
    double us = t / n * 1e6;
    /* 按“显示宽度”补空格：中文字符占两格（printf 的 %-34s 按字节数算，会对不齐） */
    int cols = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if ((*p & 0xC0) != 0x80)
            cols += *p >= 0xE0 ? 2 : 1;
    printf("  %s%*s %10.1f 微秒   （×20 ≈ 网页版 %7.2f 毫秒）\n", name, cols < 36 ? 36 - cols : 0, "", us,
           us * 20 / 1000);
}

static void b_rect_button(void)
{
    sk_rough_style s;
    sk_style_pencil(&s, (uint32_t)g_i++);
    sk_rough_rect(g_c, 100, 100, 120, 36, &s);
}
static void b_rect_big(void)
{
    sk_rough_style s;
    sk_style_pencil(&s, (uint32_t)g_i++);
    sk_rough_rect(g_c, 100, 100, 600, 400, &s);
}
static void b_rect_hachure(void)
{
    sk_rough_style s;
    sk_style_hatched(&s, SK_BLUE, (uint32_t)g_i++);
    sk_rough_rect(g_c, 100, 100, 120, 36, &s);
}
static void b_rect_hachure_big(void)
{
    sk_rough_style s;
    sk_style_hatched(&s, SK_BLUE, (uint32_t)g_i++);
    sk_rough_rect(g_c, 100, 100, 600, 400, &s);
}
static void b_ellipse(void)
{
    sk_rough_style s;
    sk_style_pencil(&s, (uint32_t)g_i++);
    sk_rough_ellipse(g_c, 300, 300, 200, 150, &s);
}
static void b_line_aa(void)
{
    sk_line_aa(g_c, 10, 20, 900, 600, 2.0f, SK_INK);
}
static void b_brush(void)
{
    static sk_pointf p[64];
    for (int i = 0; i < 64; i++) {
        p[i].x = 50 + i * 12.0f;
        p[i].y = 300 + (float)((i * 37) % 50);
    }
    sk_brush_stroke(g_c, p, 64, 2.0f, 6.0f, SK_INK, 1, (uint32_t)g_i++);
}
static void b_paper(void)
{
    sk_paper_fill(g_c, sk_rect_make(0, 0, 1024, 768), SK_DESK_PAPER, 6, 1);
}
static void b_fill_rect(void)
{
    sk_fill_rect(g_c, sk_rect_make(0, 0, 1024, 768), SK_PAPER);
}
static void b_polygon(void)
{
    static const sk_pointf p[5] = { { 100, 100 }, { 700, 150 }, { 650, 600 }, { 300, 700 }, { 50, 400 } };
    sk_fill_polygon(g_c, p, 5, SK_WITH_ALPHA(SK_BLUE, 0x80));
}
static void b_frame(void)
{
    sk_draw_window_frame(g_c, sk_rect_make(150, 120, 640, 480), "终端", 1, -1, SK_BLUE, 7);
}
static void b_frame_unfocused(void)
{
    sk_draw_window_frame(g_c, sk_rect_make(150, 120, 640, 480), "终端", 0, -1, SK_BLUE, 7);
}
static void b_frame_titlebar_only(void)
{
    /* 鼠标悬停在关闭按钮上：只需要重画按钮那一小块 */
    sk_rect cr = sk_rect_make(150, 120, 640, 480);
    sk_rect old = sk_push_clip(g_c, sk_frame_button_rect(cr, 0));
    sk_draw_window_frame(g_c, cr, "终端", 1, 0, SK_BLUE, 7);
    sk_pop_clip(g_c, old);
}
static void b_button(void)
{
    sk_draw_button(g_c, sk_rect_make(100, 100, 120, 36), "确定", SK_STATE_HOVER, SK_BLUE, 3);
}
static void b_icon48(void)
{
    sk_draw_icon(g_c, g_i++ % SK_ICON_COUNT, 200, 200, 48, 1);
}
static void b_icon24(void)
{
    sk_draw_icon(g_c, g_i++ % SK_ICON_COUNT, 200, 200, 24, 1);
}
static void b_blit(void)
{
    static sk_canvas *src;
    if (!src)
        src = sk_canvas_new(640, 480);
    sk_blit(g_c, 100, 100, src, sk_rect_make(0, 0, 640, 480));
}

int main(int argc, char **argv)
{
    if (argc > 1)
        g_min = atof(argv[1]);
    if (g_min <= 0)
        g_min = 0.25;
    g_c = sk_canvas_new(1024, 768);
    if (!g_c)
        return 1;
    sk_clear(g_c, SK_PAPER);
    printf("SketchOS 图形核心基准（%s）\n", sizeof(void *) == 4 ? "i686 32 位" : "宿主机 64 位");
    run("手绘矩形 120x36（描两遍）", b_rect_button);
    run("手绘矩形 600x400（描两遍）", b_rect_big);
    run("手绘矩形 120x36 + 排线", b_rect_hachure);
    run("手绘矩形 600x400 + 排线", b_rect_hachure_big);
    run("手绘椭圆 200x150", b_ellipse);
    run("精确抗锯齿直线 1070px 宽 2", b_line_aa);
    run("自由笔触 64 点（铅笔颗粒）", b_brush);
    run("纸张纹理 1024x768", b_paper);
    run("纯色填充 1024x768", b_fill_rect);
    run("多边形填充（约 26 万像素）", b_polygon);
    run("窗口边框 640x480（有焦点）", b_frame);
    run("窗口边框 640x480（无焦点）", b_frame_unfocused);
    run("窗口边框：只重画关闭按钮一块", b_frame_titlebar_only);
    run("按钮（悬停排线 + 文字）", b_button);
    run("图标 48px（轮流全部 35 个）", b_icon48);
    run("图标 24px（轮流全部 35 个）", b_icon24);
    run("blit 640x480", b_blit);
    sk_canvas_free(g_c);
    return 0;
}
