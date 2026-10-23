/*
 * test_theme_frames.c — 窗口装饰样张：有焦点 / 无焦点 / 三个按钮分别悬停，带模拟内容
 *
 * 同时检查几何约定：
 *   - 装饰只画在 sk_frame_outer_rect 之内（在一块“哨兵色”画布上画，外面必须原封不动）；
 *   - 装饰不碰内容区（内容区先填满哨兵色，画完后必须不变）；
 *   - 标题栏按钮的命中矩形在外接矩形之内、互不重叠、位于标题栏里。
 * 输出 theme_frames.ppm。用法：test_theme_frames [输出目录]
 */
#include <stdio.h>
#include <stdlib.h>

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

static void desk(sk_canvas *c, sk_rect r)
{
    sk_paper_fill(c, r, SK_DESK_PAPER, 5, 77);
    for (int x = r.x + 12; x < r.x + r.w; x += 24)
        sk_vline(c, x, r.y, r.y + r.h - 1, SK_DESK_GRID);
    for (int y = r.y + 12; y < r.y + r.h; y += 24)
        sk_hline(c, r.x, r.x + r.w - 1, y, SK_DESK_GRID);
}

/* 模拟窗口内容：纸 + 几行字 + 一个按钮 */
static void content(sk_canvas *c, sk_rect r, int kind)
{
    sk_rect old = sk_push_clip(c, r);
    sk_paper_fill(c, r, SK_PAPER, 3, 5);
    const sk_font *f = sk_font_get(SK_FONT_UI);
    const sk_font *mono = sk_font_get(SK_FONT_MONO);
    if (kind == 0) {
        const char *lines[] = { "root@sketchos:~# uname -a", "Linux sketchos 6.12.114 #1 i686 GNU/Linux",
                                "root@sketchos:~# ls", "笔记.txt  画板  文档", "root@sketchos:~# _" };
        for (int i = 0; i < 5; i++)
            sk_draw_text(c, mono, r.x + 10, r.y + 24 + i * 20, lines[i], i % 2 == 0 ? SK_INK : sk_ansi_palette[4]);
    } else {
        sk_draw_text(c, f, r.x + 14, r.y + 28, "这是一个手绘风格的窗口。", SK_INK);
        sk_draw_text(c, f, r.x + 14, r.y + 52, "窗口装饰全部由 theme.c 画出。", SK_INK_SOFT);
        sk_draw_button(c, sk_rect_make(r.x + r.w - 110, r.y + r.h - 50, 96, 34), "好的", SK_STATE_HOVER, SK_GREEN, 9);
    }
    sk_pop_clip(c, old);
}

/* nbtn: 这个尺寸下应该显示几个按钮（太窄的窗口会省略最小化/最大化） */
static void frame_checks(sk_rect content_r, int nbtn)
{
    const sk_color SENT = SK_RGBA(0x12, 0x34, 0x56, 0x78);
    sk_rect outer = sk_frame_outer_rect(content_r);
    int W = outer.x + outer.w + 40, H = outer.y + outer.h + 40;
    sk_canvas *c = sk_canvas_new(W, H);
    if (!c) {
        g_fail++;
        return;
    }
    for (int hov = -1; hov < 3; hov++) {
        for (int foc = 0; foc < 2; foc++) {
            sk_clear(c, SENT);
            sk_draw_window_frame(c, content_r, "一个很长很长很长很长很长很长很长的窗口标题", foc, hov, SK_PURPLE, 42);
            int outside = 0, inside = 0, drawn = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    uint32_t v = c->pixels[y * c->stride + x];
                    if (v != SENT) {
                        drawn++;
                        if (!sk_rect_contains(outer, x, y)) outside++;
                        if (sk_rect_contains(content_r, x, y)) inside++;
                    }
                }
            CHECK(outside == 0, "窗口装饰画到了外接矩形之外 %d 个像素", outside);
            CHECK(inside == 0, "窗口装饰碰到了内容区 %d 个像素", inside);
            /* 至少把边框那一圈画满（外框周长 × 线宽的量级） */
            int ring = 2 * (content_r.w + content_r.h + SK_TITLE_H);
            CHECK(drawn > ring, "窗口装饰几乎什么都没画（%d 像素）", drawn);
        }
    }
    sk_rect b[3];
    for (int i = 0; i < 3; i++) {
        b[i] = sk_frame_button_rect(content_r, i);
        if (i >= nbtn) {
            CHECK(sk_rect_empty(b[i]), "窄窗口里按钮 %d 应该被省略", i);
            continue;
        }
        CHECK(!sk_rect_empty(b[i]), "按钮 %d 矩形为空", i);
        CHECK(sk_rect_intersect(b[i], outer).w == b[i].w && sk_rect_intersect(b[i], outer).h == b[i].h,
              "按钮 %d 不在外接矩形内", i);
        CHECK(b[i].y + b[i].h <= content_r.y && b[i].y >= content_r.y - SK_TITLE_H,
              "按钮 %d 不在标题栏里", i);
        CHECK(b[i].x >= content_r.x - SK_BORDER, "按钮 %d 伸出了窗口左边", i);
    }
    for (int i = 1; i < nbtn; i++)
        CHECK(b[i - 1].x > b[i].x, "按钮顺序应为 关闭(最右) 最大化 最小化");
    for (int i = 0; i < nbtn; i++)
        for (int j = i + 1; j < nbtn; j++)
            CHECK(sk_rect_empty(sk_rect_intersect(b[i], b[j])), "按钮 %d 和 %d 重叠", i, j);
    CHECK(sk_rect_empty(sk_frame_button_rect(content_r, 3)), "非法按钮号应返回空矩形");
    sk_canvas_free(c);
}

/*
 * sketchd 的做法：边框拆成上/下/左/右四条小画布，每条单独调用一次 sk_draw_window_frame
 * （内容区坐标相对这条画布），合成时再拼起来。拼出来的结果必须和一次画在大画布上一样。
 * 画布先清成 alpha=0 的桌面纸色（和 sketchd 一样）。
 */
static void strip_check(int cw, int ch, int focused, int hover)
{
    const sk_color CLR = SK_WITH_ALPHA(SK_DESK_PAPER, 0);
    sk_rect o = sk_frame_outer_rect(sk_rect_make(0, 0, cw, ch));
    int top = -o.y, bottom = o.y + o.h - ch, left = -o.x, right = o.x + o.w - cw;
    sk_rect s[4] = {
        sk_rect_make(o.x, o.y, o.w, top), sk_rect_make(o.x, ch, o.w, bottom),
        sk_rect_make(o.x, 0, left, ch), sk_rect_make(cw, 0, right, ch),
    };
    /* 整体：内容区放在 (-o.x, -o.y)，外接矩形正好铺满画布 */
    sk_canvas *full = sk_canvas_new(o.w, o.h), *tiled = sk_canvas_new(o.w, o.h);
    if (!full || !tiled) {
        g_fail++;
        return;
    }
    sk_clear(full, CLR);
    sk_clear(tiled, CLR);
    sk_draw_window_frame(full, sk_rect_make(-o.x, -o.y, cw, ch), "拼接测试", focused, hover, SK_BLUE, 31);
    for (int i = 0; i < 4; i++) {
        if (s[i].w <= 0 || s[i].h <= 0)
            continue;
        sk_canvas *st = sk_canvas_new(s[i].w, s[i].h);
        sk_clear(st, CLR);
        sk_draw_window_frame(st, sk_rect_make(-s[i].x, -s[i].y, cw, ch), "拼接测试", focused, hover, SK_BLUE, 31);
        sk_blit(tiled, s[i].x - o.x, s[i].y - o.y, st, sk_rect_make(0, 0, s[i].w, s[i].h));
        sk_canvas_free(st);
    }
    int diff = 0, maxd = 0;
    for (int k = 0; k < o.w * o.h; k++) {
        uint32_t p = full->pixels[k], q = tiled->pixels[k];
        if (p != q) {
            diff++;
            int d = abs((int)SK_R(p) - (int)SK_R(q)) + abs((int)SK_G(p) - (int)SK_G(q)) + abs((int)SK_B(p) - (int)SK_B(q));
            if (d > maxd) maxd = d;
        }
    }
    /* 四条画布的原点不同，平移后的浮点坐标最后一位可能舍入得不一样，偶尔让某个笔刷章落到
     * 相邻的 1/8 像素相位上：允许个别抗锯齿边缘像素有差别，但不能出现成片的接缝 */
    CHECK(diff <= 16 && maxd <= 64, "四条拼接和整体不一致：%d 像素不同，最大差 %d", diff, maxd);
    printf("  四条拼接 %dx%d：%d 个像素有差别（最大 %d 级）\n", cw, ch, diff, maxd);
    sk_canvas_free(full);
    sk_canvas_free(tiled);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(g_out, sizeof g_out, "%s", argv[1]);
    const int W = 1100, H = 820;
    sk_canvas *c = sk_canvas_new(W, H);
    if (!c)
        return 1;
    desk(c, sk_rect_make(0, 0, W, H));

    struct { sk_rect r; const char *t; int foc, hov; sk_color acc; int kind; } wins[] = {
        { { 30, 60, 470, 150 }, "终端", 1, -1, SK_BLUE, 0 },
        { { 580, 60, 470, 150 }, "终端（没有焦点）", 0, -1, SK_BLUE, 0 },
        { { 30, 300, 300, 150 }, "关于本机", 1, 0, SK_YELLOW, 1 },
        { { 390, 300, 300, 150 }, "记事本 - 笔记.txt", 1, 1, SK_GREEN, 1 },
        { { 750, 300, 300, 150 }, "画板", 1, 2, SK_RED, 1 },
        { { 30, 540, 420, 220 }, "系统监视器", 1, -1, SK_PURPLE, 1 },
        { { 520, 540, 220, 100 }, "时钟", 0, 0, SK_TEAL, 1 },
        { { 800, 560, 120, 60 }, "很小的窗口标题会被截断", 1, -1, SK_ORANGE, 1 },
    };
    for (unsigned i = 0; i < sizeof wins / sizeof wins[0]; i++) {
        content(c, wins[i].r, wins[i].kind);
        sk_draw_window_frame(c, wins[i].r, wins[i].t, wins[i].foc, wins[i].hov, wins[i].acc, 100 + i);
    }
    /* 画出命中矩形（淡红框）检查和按钮图形是否对齐 */
    for (int k = 0; k < 3; k++)
        sk_rect_outline(c, sk_frame_button_rect(wins[0].r, k), SK_WITH_ALPHA(SK_RED, 0x60));
    save(c, "theme_frames.ppm");
    sk_canvas_free(c);

    frame_checks(sk_rect_make(60, 80, 400, 200), 3);
    frame_checks(sk_rect_make(60, 80, 70, 30), 2);
    frame_checks(sk_rect_make(60, 80, 40, 10), 1);
    frame_checks(sk_rect_make(60, 80, 0, 0), 0);
    strip_check(640, 480, 1, -1);
    strip_check(300, 200, 0, 0);
    strip_check(517, 333, 1, 2);
    if (g_fail) {
        fprintf(stderr, "test_theme_frames: %d 项检查失败\n", g_fail);
        return 1;
    }
    printf("test_theme_frames: 全部通过\n");
    return 0;
}
