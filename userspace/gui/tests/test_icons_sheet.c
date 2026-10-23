/*
 * test_icons_sheet.c — 全部手绘图标在 16 / 24 / 32 / 48 像素下的样张，外加名称映射检查
 *
 * 每个图标都画在自己的方块里：检查图标不会画到方块外 1 像素以外（局部重绘安全）。
 * 输出 icons_sheet.ppm（以及放大 3 倍的 icons_small_x3.ppm，方便看 16/24 像素的细节）。
 * 用法：test_icons_sheet [输出目录]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sketch/gfx.h"
#include "sketch/rough.h"
#include "sketch/theme.h"
#include "sketch/font.h"

static char g_out[512] = ".";
static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; fprintf(stderr, "失败 %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

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

static const char *names[SK_ICON_COUNT] = {
    "terminal", "folder", "file", "text", "image", "program", "notepad", "paint", "monitor", "clock",
    "about", "settings", "power", "reboot", "home", "up", "back", "new-folder", "trash", "save",
    "open", "pencil", "eraser", "brush", "line", "rect", "circle", "fill", "kernel", "chip",
    "memory", "process", "logo", "book", "refresh", "search",
};

/* 在哨兵画布上单独画一个图标，确认没有画出方块（外扩 1 像素）之外 */
static void bounds_check(int icon, int size)
{
    const sk_color SENT = SK_RGBA(1, 2, 3, 4);
    int W = size + 40;
    sk_canvas *c = sk_canvas_new(W, W);
    if (!c) {
        g_fail++;
        return;
    }
    sk_clear(c, SENT);
    sk_draw_icon(c, icon, 20, 20, size, 5);
    int out = 0, in = 0;
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            if (c->pixels[y * c->stride + x] == SENT)
                continue;
            if (x < 19 || y < 19 || x > 20 + size || y > 20 + size)
                out++;
            else
                in++;
        }
    CHECK(out == 0, "图标 %s@%d 画出了方块 %d 个像素", names[icon], size, out);
    CHECK(in > size, "图标 %s@%d 几乎是空的（%d 像素）", names[icon], size, in);
    sk_canvas_free(c);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(g_out, sizeof g_out, "%s", argv[1]);
    static const int sizes[4] = { 16, 24, 32, 48 };
    const int cols = 7, cellw = 150, cellh = 96;
    int rows = (SK_ICON_COUNT + cols - 1) / cols;
    int W = cols * cellw + 20, H = rows * cellh + 60;
    sk_canvas *c = sk_canvas_new(W, H);
    if (!c)
        return 1;
    sk_paper_fill(c, sk_rect_make(0, 0, W, H), SK_PAPER, 3, 21);
    const sk_font *ui = sk_font_get(SK_FONT_UI);
    sk_draw_text(c, sk_font_get(SK_FONT_TITLE), 14, 32, "手绘图标：16 / 24 / 32 / 48 像素", SK_INK);
    for (int i = 0; i < SK_ICON_COUNT; i++) {
        int cx = 14 + (i % cols) * cellw, cy = 50 + (i / cols) * cellh;
        int x = cx;
        for (int s = 0; s < 4; s++) {
            int sz = sizes[s];
            sk_draw_icon(c, i, x, cy + 48 - sz, sz, 5);
            x += sz + 6;
        }
        sk_draw_text_in_rect(c, ui, sk_rect_make(cx, cy + 52, cellw - 10, 22), names[i], SK_INK_SOFT, SK_ALIGN_LEFT);
        CHECK(sk_icon_from_name(names[i]) == i, "sk_icon_from_name(\"%s\") 应为 %d", names[i], i);
        for (int s = 0; s < 4; s++)
            bounds_check(i, sizes[s]);
    }
    CHECK(sk_icon_from_name("不存在") == SK_ICON_PROGRAM, "未知名称应返回 SK_ICON_PROGRAM");
    CHECK(sk_icon_from_name(NULL) == SK_ICON_PROGRAM, "NULL 应返回 SK_ICON_PROGRAM");
    /* 非法编号画成“程序”图标而不是崩溃；画到画布外、极端尺寸也不能出事 */
    {
        sk_canvas *t = sk_canvas_new(64, 64);
        if (t) {
            sk_draw_icon(t, -5, 0, 0, 16, 1);
            sk_draw_icon(t, 9999, 20, 20, 16, 1);
            sk_draw_icon(t, SK_ICON_CLOCK, -30, -30, 48, 1);
            sk_draw_icon(t, SK_ICON_CLOCK, 50, 50, 48, 1);
            sk_draw_icon(t, SK_ICON_FOLDER, 0, 0, 3, 1);
            sk_draw_icon(t, SK_ICON_FOLDER, 0, 0, 100000, 1);
            sk_canvas_free(t);
        }
    }
    save(c, "icons_sheet.ppm");

    /* 16/24 像素放大 3 倍：小尺寸的可读性最难 */
    int sw = SK_ICON_COUNT * 30 + 10, sh = 70;
    sk_canvas *sm = sk_canvas_new(sw, sh);
    sk_canvas *big = sk_canvas_new(sw * 3, sh * 3);
    if (sm && big) {
        sk_paper_fill(sm, sk_rect_make(0, 0, sw, sh), SK_PAPER, 3, 22);
        for (int i = 0; i < SK_ICON_COUNT; i++) {
            sk_draw_icon(sm, i, 8 + i * 30, 6, 16, 5);
            sk_draw_icon(sm, i, 4 + i * 30, 32, 24, 5);
        }
        sk_blit_scaled(big, sk_rect_make(0, 0, sw * 3, sh * 3), sm, sk_rect_make(0, 0, sw, sh));
        save(big, "icons_small_x3.ppm");
    }
    sk_canvas_free(sm);
    sk_canvas_free(big);
    sk_canvas_free(c);
    if (g_fail) {
        fprintf(stderr, "test_icons_sheet: %d 项检查失败\n", g_fail);
        return 1;
    }
    printf("test_icons_sheet: 全部通过\n");
    return 0;
}
