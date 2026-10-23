/*
 * test_gfx.c — gfx.c 的单元测试（宿主机运行，不需要显示器）
 *
 *   1. 矩形运算（含溢出边界）、裁剪栈、子画布；
 *   2. 颜色混合的数值；
 *   3. 抗锯齿图元的覆盖率（面积）是否正确：圆、圆环、直线、多边形（含偶奇规则）；
 *   4. blit / keyed / alpha / scaled / mask 的结果，含同一块内存里重叠拷贝（滚屏）；
 *   5. PPM 读写（含各种坏文件）；
 *   6. “哨兵画布”模糊测试：画布四周包一圈哨兵像素，随机调用所有绘制函数（含屏幕外、
 *      巨大、负数、NaN 坐标和随机裁剪区），检查哨兵和裁剪区外的像素一个都没被改。
 * 用法：test_gfx [临时目录]；全部通过返回 0。
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sketch/gfx.h"

static int g_fail, g_checks;
static char g_tmp[512] = ".";

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "失败 %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static int near(int a, int b, int tol) { return abs(a - b) <= tol; }
static int col_near(sk_color a, sk_color b, int tol)
{
    return near(SK_A(a), SK_A(b), tol) && near(SK_R(a), SK_R(b), tol) && near(SK_G(a), SK_G(b), tol) &&
           near(SK_B(a), SK_B(b), tol);
}
static int rect_eq(sk_rect a, sk_rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

/* ---------------------------------------------------------------- 矩形 */
static void test_rects(void)
{
    sk_rect a = sk_rect_make(0, 0, 10, 10), b = sk_rect_make(5, 5, 10, 10);
    CHECK(rect_eq(sk_rect_intersect(a, b), sk_rect_make(5, 5, 5, 5)), "intersect");
    CHECK(sk_rect_empty(sk_rect_intersect(a, sk_rect_make(10, 0, 5, 5))), "相邻矩形无交集");
    CHECK(sk_rect_empty(sk_rect_intersect(a, sk_rect_make(-5, -5, 0, 100))), "空矩形无交集");
    CHECK(rect_eq(sk_rect_union(a, b), sk_rect_make(0, 0, 15, 15)), "union");
    CHECK(rect_eq(sk_rect_union(sk_rect_make(100, 100, 0, 5), b), b), "union 忽略空矩形");
    CHECK(rect_eq(sk_rect_union(b, sk_rect_make(1, 1, -3, 5)), b), "union 忽略负尺寸");
    CHECK(sk_rect_empty(sk_rect_union(sk_rect_make(1, 1, 0, 0), sk_rect_make(2, 2, 0, 3))), "两个空的并为空");
    CHECK(sk_rect_contains(a, 0, 0) && sk_rect_contains(a, 9, 9) && !sk_rect_contains(a, 10, 5) &&
          !sk_rect_contains(a, -1, 5), "contains 是半开区间");
    CHECK(rect_eq(sk_rect_inset(a, 2, 3), sk_rect_make(2, 3, 6, 4)), "inset");
    CHECK(rect_eq(sk_rect_inset(a, -2, -2), sk_rect_make(-2, -2, 14, 14)), "inset 负数外扩");
    CHECK(sk_rect_empty(sk_rect_inset(a, 6, 0)), "缩没了就是空");
    CHECK(rect_eq(sk_rect_offset(a, 3, -4), sk_rect_make(3, -4, 10, 10)), "offset");
    /* 溢出边界：x + w 超过 INT_MAX 不能绕回负数 */
    sk_rect big = sk_rect_make(INT_MAX - 10, 0, INT_MAX, 10);
    sk_rect r = sk_rect_intersect(big, sk_rect_make(INT_MAX - 20, 0, 15, 10));
    CHECK(rect_eq(r, sk_rect_make(INT_MAX - 10, 0, 5, 10)), "intersect 溢出 (%d %d %d %d)", r.x, r.y, r.w, r.h);
    CHECK(!sk_rect_contains(sk_rect_make(INT_MAX - 1, 0, 5, 5), INT_MIN, 0), "contains 溢出");
    sk_rect u = sk_rect_union(sk_rect_make(INT_MIN, 0, 10, 10), sk_rect_make(INT_MAX - 10, 0, 10, 10));
    CHECK(u.w > 0, "union 溢出时也不能得到负宽度");
}

/* ---------------------------------------------------------------- 画布、裁剪、子画布 */
static void test_canvas(void)
{
    CHECK(sk_canvas_new(0, 10) == NULL && sk_canvas_new(10, -1) == NULL && sk_canvas_new(100000, 2) == NULL,
          "非法尺寸返回 NULL");
    sk_canvas_free(NULL);
    sk_canvas *c = sk_canvas_new(40, 30);
    CHECK(c && c->stride == 40 && rect_eq(c->clip, sk_rect_make(0, 0, 40, 30)), "new 初始 clip");
    CHECK(c->pixels[0] == 0 && c->pixels[40 * 30 - 1] == 0, "new 初始为 0");
    sk_set_clip(c, sk_rect_make(-10, -10, 25, 100));
    CHECK(rect_eq(c->clip, sk_rect_make(0, 0, 15, 30)), "set_clip 与画布求交");
    sk_rect old = sk_push_clip(c, sk_rect_make(5, 5, 100, 10));
    CHECK(rect_eq(c->clip, sk_rect_make(5, 5, 10, 10)), "push_clip 求交");
    sk_pop_clip(c, old);
    CHECK(rect_eq(c->clip, sk_rect_make(0, 0, 15, 30)), "pop_clip 恢复");
    sk_reset_clip(c);
    CHECK(rect_eq(c->clip, sk_rect_make(0, 0, 40, 30)), "reset_clip");

    /* 子画布：原点移动，clip = r ∩ parent.clip */
    sk_clear(c, SK_RGB(0, 0, 0));
    sk_set_clip(c, sk_rect_make(0, 0, 30, 30));
    sk_canvas sub;
    sk_canvas_sub(&sub, c, sk_rect_make(20, 10, 15, 8));
    CHECK(sub.w == 15 && sub.h == 8 && rect_eq(sub.clip, sk_rect_make(0, 0, 10, 8)), "sub clip (%d %d %d %d)",
          sub.clip.x, sub.clip.y, sub.clip.w, sub.clip.h);
    sk_fill_rect(&sub, sk_rect_make(-100, -100, 1000, 1000), SK_RGB(255, 255, 255));
    int changed = 0, wrong = 0;
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++) {
            int in = x >= 20 && x < 30 && y >= 10 && y < 18;
            uint32_t v = c->pixels[y * 40 + x];
            if (v == SK_RGB(255, 255, 255)) changed++;
            if ((v == SK_RGB(255, 255, 255)) != in) wrong++;
        }
    CHECK(changed == 80 && wrong == 0, "子画布只画到 r∩clip (%d, %d)", changed, wrong);
    sk_canvas_sub(&sub, c, sk_rect_make(35, 25, 20, 20));
    CHECK(sub.w == 5 && sub.h == 5, "右下超出父画布的部分从尺寸里去掉");
    CHECK(sk_rect_empty(sub.clip), "父 clip 外的子画布 clip 为空");
    sk_reset_clip(c);
    sk_canvas_sub(&sub, c, sk_rect_make(-5, -5, 10, 10));
    CHECK(rect_eq(sub.clip, sk_rect_make(5, 5, 5, 5)), "左上超出时靠 clip 排除");
    sk_fill_rect(&sub, sk_rect_make(0, 0, 10, 10), SK_RGB(9, 9, 9));
    CHECK(c->pixels[0] == SK_RGB(9, 9, 9) && c->pixels[4 * 40 + 4] == SK_RGB(9, 9, 9) &&
          c->pixels[5 * 40 + 5] != SK_RGB(9, 9, 9), "负原点子画布画到正确位置");
    sk_canvas_sub(&sub, c, sk_rect_make(100, 100, 10, 10));
    CHECK(sub.pixels == NULL && sk_rect_empty(sub.clip), "完全在外的子画布是空画布");
    sk_fill_rect(&sub, sk_rect_make(0, 0, 10, 10), SK_RGB(1, 1, 1));   /* 不能崩 */
    sk_canvas_free(c);
}

/* ---------------------------------------------------------------- 颜色 */
static void test_colors(void)
{
    sk_color w = SK_RGB(255, 255, 255);
    CHECK(sk_blend(w, SK_RGB(10, 20, 30)) == SK_RGB(10, 20, 30), "不透明直接覆盖");
    CHECK(sk_blend(w, SK_RGBA(0, 0, 0, 0)) == w, "全透明不变");
    sk_color h = sk_blend(w, SK_RGBA(255, 0, 0, 128));
    CHECK(col_near(h, SK_RGB(255, 127, 127), 1), "半透明红叠在白上 %08x", h);
    CHECK(SK_A(sk_blend(SK_RGB(0, 0, 0), SK_RGBA(10, 10, 10, 77))) == 255, "目标不透明 → 结果不透明");
    /* 目标的 alpha 不参与计算（gfx.h 约定）：sketchd 的边框层先清成“alpha=0 的纸色”，
     * 抗锯齿边缘就和纸色混合，合成后没有黑边；结果总是不透明 */
    sk_color t = sk_blend(SK_WITH_ALPHA(w, 0), SK_RGBA(0, 0, 0, 128));
    CHECK(col_near(t, SK_RGB(127, 127, 127), 1), "画在“透明的纸色”上 %08x", t);
    sk_color s2 = sk_blend(SK_RGBA(0, 0, 255, 128), SK_RGBA(255, 0, 0, 128));
    CHECK(col_near(s2, SK_RGB(128, 0, 127), 1), "目标 alpha 被忽略 %08x", s2);
    CHECK(sk_mix(SK_RGB(0, 0, 0), w, 0) == SK_RGB(0, 0, 0) && sk_mix(SK_RGB(0, 0, 0), w, 255) == w, "mix 端点");
    CHECK(col_near(sk_mix(SK_RGB(0, 0, 0), w, 128), SK_RGB(128, 128, 128), 1), "mix 中点");
    CHECK(col_near(sk_mix(SK_RGBA(0, 0, 0, 0), SK_RGBA(0, 0, 0, 255), 128), SK_RGBA(0, 0, 0, 128), 1),
          "mix 也插值 alpha");
    CHECK(sk_lighten(SK_RGB(0, 0, 0), 255) == w && sk_darken(w, 255) == SK_RGB(0, 0, 0), "lighten/darken 端点");
    CHECK(SK_A(sk_lighten(SK_RGBA(10, 10, 10, 77), 100)) == 77, "lighten 保留 alpha");
    CHECK(col_near(sk_darken(SK_RGB(200, 100, 0), 128), SK_RGB(100, 50, 0), 1), "darken 一半");
}

/* ---------------------------------------------------------------- 覆盖率 */
static double coverage_sum(const sk_canvas *c, sk_color bg)
{
    /* 黑色画在白底上：每个像素的“墨量” = (255 - R) / 255 */
    double s = 0;
    (void)bg;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++)
            s += (255 - SK_R(c->pixels[y * c->stride + x])) / 255.0;
    return s;
}

static void test_coverage(void)
{
    sk_canvas *c = sk_canvas_new(120, 120);
    const sk_color W = SK_RGB(255, 255, 255), B = SK_RGB(0, 0, 0);

    sk_clear(c, W);
    sk_fill_circle_aa(c, 60.3f, 59.7f, 20.0f, B);
    double a = coverage_sum(c, W), e = M_PI * 400;
    CHECK(fabs(a - e) / e < 0.01, "实心圆面积 %.1f（理论 %.1f）", a, e);
    CHECK(c->pixels[60 * 120 + 60] == B, "圆心不透明");

    sk_clear(c, W);
    sk_circle_aa(c, 60, 60, 30, 3.0f, B);
    a = coverage_sum(c, W);
    e = 2 * M_PI * 30 * 3;
    CHECK(fabs(a - e) / e < 0.02, "圆环面积 %.1f（理论 %.1f）", a, e);
    CHECK(SK_R(c->pixels[60 * 120 + 60]) == 255, "圆环中间是空的");

    sk_clear(c, W);
    sk_line_aa(c, 10, 20.5f, 90, 20.5f, 3.0f, B);
    a = coverage_sum(c, W);
    e = 80 * 3 + M_PI * 1.5 * 1.5;
    CHECK(fabs(a - e) / e < 0.03, "粗直线面积 %.1f（理论 %.1f）", a, e);
    CHECK(c->pixels[20 * 120 + 50] == B && c->pixels[19 * 120 + 50] == B && c->pixels[21 * 120 + 50] == B,
          "3px 宽水平线覆盖 3 行");
    CHECK(SK_R(c->pixels[23 * 120 + 50]) == 255 && SK_R(c->pixels[17 * 120 + 50]) == 255, "线外不受影响");

    sk_clear(c, W);
    sk_line_aa(c, 10, 10, 100, 100, 2.0f, B);
    a = coverage_sum(c, W);
    e = sqrt(2.0) * 90 * 2 + M_PI;
    CHECK(fabs(a - e) / e < 0.03, "斜线面积 %.1f（理论 %.1f）", a, e);

    /* 整数坐标的正方形：内部完全覆盖，外面一点不碰 */
    sk_clear(c, W);
    sk_pointf sq[4] = { { 10, 10 }, { 30, 10 }, { 30, 25 }, { 10, 25 } };
    sk_fill_polygon(c, sq, 4, B);
    int inside = 0, other = 0;
    for (int y = 0; y < 120; y++)
        for (int x = 0; x < 120; x++) {
            uint32_t v = c->pixels[y * 120 + x];
            int in = x >= 10 && x < 30 && y >= 10 && y < 25;
            if (in && v == B) inside++;
            if (!in && v != W) other++;
        }
    CHECK(inside == 300 && other == 0, "整数正方形 (%d, %d)", inside, other);

    sk_clear(c, W);
    sk_pointf tri[3] = { { 5.3f, 7.1f }, { 95.2f, 12.8f }, { 30.7f, 101.4f } };
    sk_fill_polygon(c, tri, 3, B);
    a = coverage_sum(c, W);
    e = fabs((tri[1].x - tri[0].x) * (tri[2].y - tri[0].y) - (tri[2].x - tri[0].x) * (tri[1].y - tri[0].y)) / 2;
    CHECK(fabs(a - e) / e < 0.01, "三角形面积 %.1f（理论 %.1f）", a, e);

    /* 偶奇规则：五角星（隔一个顶点连线）中间的五边形是“外部” */
    sk_clear(c, W);
    sk_pointf star[5];
    for (int i = 0; i < 5; i++) {
        double ang = -M_PI / 2 + i * 4 * M_PI / 5;
        star[i].x = (float)(60 + 50 * cos(ang));
        star[i].y = (float)(60 + 50 * sin(ang));
    }
    sk_fill_polygon(c, star, 5, B);
    CHECK(SK_R(c->pixels[60 * 120 + 60]) == 255, "五角星中心不填（偶奇规则）");
    CHECK(c->pixels[20 * 120 + 60] == B, "五角星的尖要填");

    /* 半透明颜色：多边形内部每个像素只混合一次 */
    sk_clear(c, W);
    sk_fill_polygon(c, sq, 4, SK_RGBA(0, 0, 0, 128));
    CHECK(col_near(c->pixels[15 * 120 + 15], SK_RGB(127, 127, 127), 1), "半透明多边形 %08x", c->pixels[15 * 120 + 15]);

    sk_clear(c, W);
    sk_rect_outline(c, sk_rect_make(10, 10, 20, 20), SK_RGBA(0, 0, 0, 128));
    CHECK(c->pixels[10 * 120 + 10] == c->pixels[10 * 120 + 20] && c->pixels[10 * 120 + 10] == c->pixels[20 * 120 + 10],
          "半透明边框的角只混合一次");
    sk_clear(c, W);
    sk_hline(c, 30, 20, 5, B);
    CHECK(c->pixels[5 * 120 + 20] == B && c->pixels[5 * 120 + 30] == B && c->pixels[5 * 120 + 31] == W &&
          c->pixels[5 * 120 + 19] == W, "hline 包含两端、端点可以反着给");
    sk_vline(c, 7, 9, 3, B);
    CHECK(c->pixels[3 * 120 + 7] == B && c->pixels[9 * 120 + 7] == B && c->pixels[10 * 120 + 7] == W, "vline");
    sk_fill_rect(c, sk_rect_make(50, 50, 4, 4), SK_RGBA(0, 0, 0, 64));
    CHECK(col_near(c->pixels[51 * 120 + 51], SK_RGB(191, 191, 191), 1), "fill_rect 半透明");
    sk_put_pixel(c, 70, 70, B);
    sk_blend_pixel_cov(c, 71, 70, B, 128);
    CHECK(c->pixels[70 * 120 + 70] == B && col_near(c->pixels[70 * 120 + 71], SK_RGB(127, 127, 127), 1),
          "put_pixel / blend_pixel_cov");
    sk_canvas_free(c);
}

/* ---------------------------------------------------------------- blit */
static void test_blits(void)
{
    sk_canvas *s = sk_canvas_new(8, 8), *d = sk_canvas_new(16, 16);
    for (int i = 0; i < 64; i++)
        s->pixels[i] = SK_RGB(i, 255 - i, i * 2);
    sk_clear(d, SK_RGB(1, 1, 1));
    sk_blit(d, 10, 12, s, sk_rect_make(-2, -2, 20, 20));   /* 源矩形超出源画布、目标也超出 */
    int ok = 1;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            uint32_t want = (x >= 12 && y >= 14) ? s->pixels[(y - 14) * 8 + (x - 12)] : SK_RGB(1, 1, 1);
            if (d->pixels[y * 16 + x] != want) ok = 0;
        }
    CHECK(ok, "blit 双向裁剪后的位置");

    /* 同一块内存的重叠拷贝（滚屏）：结果必须等于“先复制一份再拷贝” */
    for (int dir = -1; dir <= 1; dir += 2) {
        sk_canvas *c = sk_canvas_new(20, 20), *ref = sk_canvas_new(20, 20);
        for (int i = 0; i < 400; i++)
            c->pixels[i] = ref->pixels[i] = SK_RGB(i & 255, i >> 8, 7);
        sk_canvas *copy = sk_canvas_new(20, 20);
        memcpy(copy->pixels, c->pixels, 400 * 4);
        sk_blit(c, 1 * dir, 3 * dir, c, sk_rect_make(0, 0, 20, 20));
        sk_blit(ref, 1 * dir, 3 * dir, copy, sk_rect_make(0, 0, 20, 20));
        CHECK(memcmp(c->pixels, ref->pixels, 400 * 4) == 0, "重叠 blit 方向 %d", dir);
        memcpy(c->pixels, copy->pixels, 400 * 4);
        memcpy(ref->pixels, copy->pixels, 400 * 4);
        sk_blit_keyed(c, 2 * dir, 1 * dir, c, sk_rect_make(0, 0, 20, 20));
        sk_blit_keyed(ref, 2 * dir, 1 * dir, copy, sk_rect_make(0, 0, 20, 20));
        CHECK(memcmp(c->pixels, ref->pixels, 400 * 4) == 0, "重叠 keyed blit 方向 %d", dir);
        sk_canvas_free(c);
        sk_canvas_free(ref);
        sk_canvas_free(copy);
    }

    /* keyed：alpha==0 的像素跳过 */
    sk_canvas *k = sk_canvas_new(4, 1);
    k->pixels[0] = 0x00FF0000u;
    k->pixels[1] = 0xFF00FF00u;
    k->pixels[2] = 0x800000FFu;
    k->pixels[3] = 0;
    sk_clear(d, SK_RGB(9, 9, 9));
    sk_blit_keyed(d, 0, 0, k, sk_rect_make(0, 0, 4, 1));
    CHECK(d->pixels[0] == SK_RGB(9, 9, 9) && d->pixels[1] == 0xFF00FF00u && d->pixels[2] == 0x800000FFu &&
          d->pixels[3] == SK_RGB(9, 9, 9), "keyed blit");
    sk_clear(d, SK_RGB(255, 255, 255));
    sk_blit_alpha(d, 0, 0, k, sk_rect_make(0, 0, 4, 1));
    CHECK(d->pixels[0] == SK_RGB(255, 255, 255) && d->pixels[1] == 0xFF00FF00u &&
          col_near(d->pixels[2], SK_RGB(127, 127, 255), 1), "alpha blit %08x", d->pixels[2]);

    /* 缩放：2x2 放大到 4x4，每个源像素变成 2x2 块 */
    sk_canvas *s2 = sk_canvas_new(2, 2);
    s2->pixels[0] = SK_RGB(1, 0, 0); s2->pixels[1] = SK_RGB(2, 0, 0);
    s2->pixels[2] = SK_RGB(3, 0, 0); s2->pixels[3] = SK_RGB(4, 0, 0);
    sk_clear(d, 0);
    sk_blit_scaled(d, sk_rect_make(4, 4, 4, 4), s2, sk_rect_make(0, 0, 2, 2));
    CHECK(d->pixels[4 * 16 + 4] == SK_RGB(1, 0, 0) && d->pixels[5 * 16 + 5] == SK_RGB(1, 0, 0) &&
          d->pixels[4 * 16 + 7] == SK_RGB(2, 0, 0) && d->pixels[7 * 16 + 4] == SK_RGB(3, 0, 0) &&
          d->pixels[7 * 16 + 7] == SK_RGB(4, 0, 0) && d->pixels[3 * 16 + 4] == 0 && d->pixels[4 * 16 + 8] == 0,
          "scaled 放大");
    sk_clear(d, 0);
    sk_blit_scaled(d, sk_rect_make(0, 0, 4, 4), s, sk_rect_make(0, 0, 8, 8));
    CHECK(d->pixels[0] == s->pixels[1 * 8 + 1] && d->pixels[3 * 16 + 3] == s->pixels[7 * 8 + 7], "scaled 缩小取中心");

    /* 蒙版 */
    uint8_t m[6] = { 0, 255, 128, 0, 0, 0 };
    sk_clear(d, SK_RGB(255, 255, 255));
    sk_draw_mask(d, 5, 5, m, 3, 1, 6, SK_RGB(0, 0, 0));
    CHECK(d->pixels[5 * 16 + 5] == SK_RGB(255, 255, 255) && d->pixels[5 * 16 + 6] == SK_RGB(0, 0, 0) &&
          col_near(d->pixels[5 * 16 + 7], SK_RGB(127, 127, 127), 1), "draw_mask");
    sk_draw_mask(d, 5, 5, m, 3, 1, 2, SK_RGB(0, 0, 0));   /* stride < w：参数非法，什么都不做 */
    sk_canvas_free(s);
    sk_canvas_free(s2);
    sk_canvas_free(d);
    sk_canvas_free(k);
}

/* ---------------------------------------------------------------- PPM */
static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(data, 1, n, f);
    fclose(f);
}

static void test_ppm(void)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/test_gfx_tmp.ppm", g_tmp);
    sk_canvas *c = sk_canvas_new(13, 7);
    for (int i = 0; i < 13 * 7; i++)
        c->pixels[i] = SK_RGB(i * 3, 255 - i, i * 7);
    CHECK(sk_image_save_ppm(c, p) == 0, "保存 PPM");
    sk_canvas *l = sk_image_load_ppm(p);
    CHECK(l && l->w == 13 && l->h == 7 && memcmp(l->pixels, c->pixels, 13 * 7 * 4) == 0, "PPM 往返一致");
    sk_canvas_free(l);
    /* 带注释的头、maxval=15 */
    const char hdr[] = "P6\n# 注释\n2 1\n# 又一个\n15\n";
    unsigned char buf[64];
    memcpy(buf, hdr, sizeof hdr - 1);
    unsigned char px[6] = { 15, 0, 7, 0, 15, 15 };
    memcpy(buf + sizeof hdr - 1, px, 6);
    write_file(p, buf, sizeof hdr - 1 + 6);
    l = sk_image_load_ppm(p);
    CHECK(l && l->w == 2 && l->pixels[0] == SK_RGB(255, 0, 119) && l->pixels[1] == SK_RGB(0, 255, 255),
          "PPM 注释和 maxval 缩放");
    sk_canvas_free(l);
    /* 16 位 */
    const char h16[] = "P6 1 1 65535\n";
    unsigned char b16[32];
    memcpy(b16, h16, sizeof h16 - 1);
    unsigned char p16[6] = { 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00 };
    memcpy(b16 + sizeof h16 - 1, p16, 6);
    write_file(p, b16, sizeof h16 - 1 + 6);
    l = sk_image_load_ppm(p);
    CHECK(l && col_near(l->pixels[0], SK_RGB(255, 128, 0), 1), "16 位 PPM");
    sk_canvas_free(l);
    /* 坏文件 */
    const char *bad[] = { "P5\n1 1\n255\n\x01", "P6\n1 1\n255\n\x01\x02", "P6\n0 1\n255\n", "P6\nx 1\n255\n",
                          "P6\n1 1\n255", "", "P6\n99999999 1\n255\n" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        write_file(p, bad[i], strlen(bad[i]));
        l = sk_image_load_ppm(p);
        CHECK(l == NULL, "坏 PPM #%u 应返回 NULL", i);
        sk_canvas_free(l);
    }
    CHECK(sk_image_load_ppm("/不存在/的文件.ppm") == NULL && sk_image_load_ppm(NULL) == NULL, "不存在的文件");
    CHECK(sk_image_save_ppm(c, "/不存在的目录/x.ppm") != 0, "写不进去要返回错误");
    remove(p);
    sk_canvas_free(c);
}

/* ---------------------------------------------------------------- 哨兵画布模糊测试 */
#define CANARY 0xDEADBEEFu
typedef struct {
    uint32_t *buf;
    int bw, bh, G;
    sk_canvas c;
} guarded;

static void guard_init(guarded *g, int w, int h, int G)
{
    g->G = G;
    g->bw = w + 2 * G;
    g->bh = h + 2 * G;
    g->buf = malloc(sizeof(uint32_t) * (size_t)g->bw * g->bh);
    for (int i = 0; i < g->bw * g->bh; i++)
        g->buf[i] = CANARY;
    sk_canvas_init(&g->c, g->buf + G * g->bw + G, w, h, g->bw);
    sk_clear(&g->c, SK_RGB(250, 250, 250));
}

static int guard_ok(const guarded *g)
{
    for (int y = 0; y < g->bh; y++)
        for (int x = 0; x < g->bw; x++) {
            int in = x >= g->G && x < g->G + g->c.w && y >= g->G && y < g->G + g->c.h;
            if (!in && g->buf[y * g->bw + x] != CANARY)
                return 0;
        }
    return 1;
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static float rf(float lo, float hi)
{
    static const float special[] = { NAN, INFINITY, -INFINITY, 1e9f, -1e9f, 3e38f, -0.0f, 1e-30f };
    if (rnd() % 23 == 0)
        return special[rnd() % 8];
    return lo + (hi - lo) * (float)(rnd() % 100000) / 100000.0f;
}
static int rint_c(int lo, int hi)
{
    static const int special[] = { INT_MAX, INT_MIN, INT_MAX - 1, INT_MIN + 1, 0, -1 };
    if (rnd() % 29 == 0)
        return special[rnd() % 6];
    return ri(lo, hi);
}

static void fuzz_one(sk_canvas *c, sk_canvas *src, int W, int H)
{
    sk_color col = rnd() % 3 == 0 ? SK_RGBA(ri(0, 255), ri(0, 255), ri(0, 255), ri(0, 255)) : SK_RGB(ri(0, 255), 0, 0);
    switch (rnd() % 16) {
    case 0: sk_fill_rect(c, sk_rect_make(rint_c(-2 * W, 2 * W), rint_c(-2 * H, 2 * H), rint_c(-W, 3 * W), rint_c(-H, 3 * H)), col); break;
    case 1: sk_hline(c, rint_c(-2 * W, 2 * W), rint_c(-2 * W, 2 * W), rint_c(-H, 2 * H), col); break;
    case 2: sk_vline(c, rint_c(-W, 2 * W), rint_c(-2 * H, 2 * H), rint_c(-2 * H, 2 * H), col); break;
    case 3: sk_rect_outline(c, sk_rect_make(rint_c(-W, 2 * W), rint_c(-H, 2 * H), rint_c(-W, 3 * W), rint_c(-H, 3 * H)), col); break;
    case 4: sk_put_pixel(c, rint_c(-5, W + 5), rint_c(-5, H + 5), col);
            sk_blend_pixel_cov(c, rint_c(-5, W + 5), rint_c(-5, H + 5), col, ri(-10, 300)); break;
    case 5: sk_line_aa(c, rf(-W, 2 * W), rf(-H, 2 * H), rf(-W, 2 * W), rf(-H, 2 * H), rf(-1, 30), col); break;
    case 6: sk_fill_circle_aa(c, rf(-W, 2 * W), rf(-H, 2 * H), rf(-2, W), col); break;
    case 7: sk_circle_aa(c, rf(-W, 2 * W), rf(-H, 2 * H), rf(-2, W), rf(-1, 20), col); break;
    case 8: {
        sk_pointf p[12];
        int n = ri(0, 12);
        for (int i = 0; i < n; i++) {
            p[i].x = rf(-W, 2 * W);
            p[i].y = rf(-H, 2 * H);
        }
        sk_fill_polygon(c, p, n, col);
        break;
    }
    case 9: sk_blit(c, rint_c(-2 * W, 2 * W), rint_c(-2 * H, 2 * H), src,
                    sk_rect_make(rint_c(-20, 40), rint_c(-20, 40), rint_c(-10, 80), rint_c(-10, 80))); break;
    case 10: sk_blit_keyed(c, rint_c(-W, W), rint_c(-H, H), src, sk_rect_make(ri(-20, 40), ri(-20, 40), ri(-10, 80), ri(-10, 80))); break;
    case 11: sk_blit_alpha(c, rint_c(-W, W), rint_c(-H, H), src, sk_rect_make(ri(-20, 40), ri(-20, 40), ri(-10, 80), ri(-10, 80))); break;
    case 12: sk_blit_scaled(c, sk_rect_make(rint_c(-W, W), rint_c(-H, H), rint_c(-5, 3 * W), rint_c(-5, 3 * H)), src,
                            sk_rect_make(ri(-20, 40), ri(-20, 40), ri(-10, 80), ri(-10, 80))); break;
    case 13: {
        static uint8_t mask[40 * 40];
        for (int i = 0; i < 40 * 40; i++)
            mask[i] = (uint8_t)rnd();
        int mw = ri(0, 40), mh = ri(0, 40);
        sk_draw_mask(c, rint_c(-W, 2 * W), rint_c(-H, 2 * H), mask, mw, mh, 40, col);
        break;
    }
    case 14: sk_clear(c, col); break;
    default: {
        /* 自己拷给自己（重叠） */
        sk_blit(c, ri(-10, 10), ri(-10, 10), c, sk_rect_make(ri(-5, W), ri(-5, H), ri(0, W), ri(0, H)));
        break;
    }
    }
}

static void test_fuzz(void)
{
    const int W = 61, H = 47;
    guarded g;
    guard_init(&g, W, H, 24);
    sk_canvas *src = sk_canvas_new(37, 29);
    for (int i = 0; i < 37 * 29; i++)
        src->pixels[i] = rnd();
    uint32_t *snap = malloc(sizeof(uint32_t) * W * H);
    int bad_guard = 0, bad_clip = 0;
    for (int it = 0; it < 30000; it++) {
        /* 随机裁剪区（有时是空的、有时比画布大） */
        if (rnd() % 4 == 0)
            sk_reset_clip(&g.c);
        else
            sk_set_clip(&g.c, sk_rect_make(ri(-20, W), ri(-20, H), ri(-5, W + 40), ri(-5, H + 40)));
        sk_rect clip = g.c.clip;
        if (clip.x < 0 || clip.y < 0 || clip.x + clip.w > W || clip.y + clip.h > H)
            bad_clip++;
        for (int y = 0; y < H; y++)
            memcpy(snap + y * W, g.c.pixels + y * g.c.stride, W * 4);
        fuzz_one(&g.c, src, W, H);
        /* 裁剪区外的像素必须原封不动（自拷贝的源区域可以在裁剪外，但写只能在裁剪内） */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (!sk_rect_contains(clip, x, y) && g.c.pixels[y * g.c.stride + x] != snap[y * W + x])
                    bad_clip++;
        if (it % 500 == 0 && !guard_ok(&g))
            bad_guard++;
    }
    CHECK(guard_ok(&g) && bad_guard == 0, "模糊测试：哨兵被改写");
    CHECK(bad_clip == 0, "模糊测试：裁剪区外有 %d 个像素被改写", bad_clip);
    free(snap);
    free(g.buf);
    sk_canvas_free(src);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(g_tmp, sizeof g_tmp, "%s", argv[1]);
    test_rects();
    test_canvas();
    test_colors();
    test_coverage();
    test_blits();
    test_ppm();
    test_fuzz();
    if (g_fail) {
        fprintf(stderr, "test_gfx: %d / %d 项检查失败\n", g_fail, g_checks);
        return 1;
    }
    printf("test_gfx: 全部 %d 项检查通过\n", g_checks);
    return 0;
}
