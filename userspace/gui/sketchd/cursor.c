/*
 * cursor.c — 手绘鼠标光标
 *
 * 光标是“软件光标”：每次刷新屏幕时由合成器临时叠加上去（见 compositor.c），
 * 不写进后备缓冲区。真实显卡还有“硬件光标”平面（一个小图层，由显卡自己叠加，
 * 移动光标只改两个寄存器），bochs-drm 没有提供，所以我们自己画。
 *
 * 光标需要真正的 alpha 通道（半透明的抗锯齿边缘、淡淡的阴影），而 libsketch 的绘图
 * 函数是“画到不透明画布上”的。做法：把每一层形状先用白色画在黑底的“蒙版画布”上，
 * 红色通道的值就是覆盖率 (0..255)；再用这个覆盖率把颜色按 alpha 叠到光标图像上。
 * 每个光标由三层组成：阴影（灰色、偏移 2px）→ 白色填充 → 铅笔墨线。
 * 所有光标只在启动时画一次，之后直接使用。
 */
#include "sketchd.h"

#include <stdlib.h>
#include <string.h>

#define CUR_BOX 32

typedef struct {
    sk_canvas *img;     /* 裁掉透明边之后的 ARGB 图像 */
    int hot_x, hot_y;   /* 热点（“指针尖”）在 img 里的位置 */
} cursor_img;

static cursor_img g_cur[SK_CURSOR_COUNT];
static sk_canvas *g_mask;          /* 蒙版画布（复用） */
static float      g_acc[CUR_BOX * CUR_BOX][4];   /* 正在合成的光标：r,g,b,a（0..1，非预乘） */
static int        g_shape = SK_CURSOR_ARROW;
static int        g_x, g_y;
static int        g_visible = 1;

/* ---------- 蒙版与图层 ---------- */

static void mask_begin(void)
{
    sk_reset_clip(g_mask);
    sk_clear(g_mask, SK_RGB(0, 0, 0));
}

/* 用当前蒙版把颜色 col（乘以 opacity 0..1）“盖”到正在合成的光标上（source-over） */
static void layer_apply(sk_color col, float opacity, int dx, int dy)
{
    float cr = SK_R(col) / 255.0f, cg = SK_G(col) / 255.0f, cb = SK_B(col) / 255.0f;
    for (int y = 0; y < CUR_BOX; y++)
        for (int x = 0; x < CUR_BOX; x++) {
            int sx = x - dx, sy = y - dy;
            if (sx < 0 || sy < 0 || sx >= CUR_BOX || sy >= CUR_BOX)
                continue;
            float sa = SK_R(g_mask->pixels[sy * g_mask->stride + sx]) / 255.0f * opacity;
            if (sa <= 0.0f)
                continue;
            float *d = g_acc[y * CUR_BOX + x];
            float oa = sa + d[3] * (1.0f - sa);
            d[0] = (cr * sa + d[0] * d[3] * (1.0f - sa)) / oa;
            d[1] = (cg * sa + d[1] * d[3] * (1.0f - sa)) / oa;
            d[2] = (cb * sa + d[2] * d[3] * (1.0f - sa)) / oa;
            d[3] = oa;
        }
}

/* 合成完毕：裁掉四周全透明的部分，存成 ARGB 画布 */
static void finish(int id, int hot_x, int hot_y)
{
    int x0 = CUR_BOX, y0 = CUR_BOX, x1 = -1, y1 = -1;
    for (int y = 0; y < CUR_BOX; y++)
        for (int x = 0; x < CUR_BOX; x++)
            if (g_acc[y * CUR_BOX + x][3] > 0.004f) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) {
        x0 = y0 = 0;
        x1 = y1 = 0;
    }
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    sk_canvas *img = sk_canvas_new(w, h);
    if (!img)
        return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float *p = g_acc[(y + y0) * CUR_BOX + (x + x0)];
            uint32_t a = (uint32_t)(p[3] * 255.0f + 0.5f);
            uint32_t r = (uint32_t)(p[0] * 255.0f + 0.5f);
            uint32_t g = (uint32_t)(p[1] * 255.0f + 0.5f);
            uint32_t b = (uint32_t)(p[2] * 255.0f + 0.5f);
            img->pixels[y * img->stride + x] = a ? SK_RGBA(r, g, b, a) : 0;
        }
    g_cur[id].img = img;
    g_cur[id].hot_x = hot_x - x0;
    g_cur[id].hot_y = hot_y - y0;
}

static void acc_begin(void) { memset(g_acc, 0, sizeof g_acc); }

/* 铅笔描边样式：轻微抖动、描两遍 */
static void pencil_style(sk_rough_style *s, uint32_t seed, float width)
{
    sk_rough_default(s);
    s->roughness = 0.55f;
    s->bowing = 0.6f;
    s->stroke_width = width;
    s->stroke = SK_RGB(255, 255, 255);
    s->double_stroke = 1;
    s->fill_style = SK_FILL_NONE;
    s->seed = seed;
}

/* 一个“实心形状”光标：多边形白色填充 + 手绘墨线轮廓 + 阴影 */
static void shape_cursor(int id, const sk_pointf *pts, int n, int hot_x, int hot_y, sk_color fill)
{
    sk_rough_style st;
    acc_begin();
    /* 阴影：同一个填充形状，偏移 (1,2)，很淡 */
    mask_begin();
    sk_fill_polygon(g_mask, pts, n, SK_RGB(255, 255, 255));
    layer_apply(SK_INK, 0.22f, 1, 2);
    layer_apply(fill, 1.0f, 0, 0);
    mask_begin();
    pencil_style(&st, 0xC0FFEE00u + (uint32_t)id, 1.5f);
    sk_rough_polygon(g_mask, pts, n, &st);
    layer_apply(SK_INK, 1.0f, 0, 0);
    finish(id, hot_x, hot_y);
}

/* 由线条组成的光标：先画粗白线当“衬底”（在任何背景上都看得清），再画细墨线 */
typedef struct { float x1, y1, x2, y2; } seg;

static void line_cursor(int id, const seg *s, int n, const sk_pointf *heads, int nheads,
                        int hot_x, int hot_y)
{
    acc_begin();
    mask_begin();
    for (int i = 0; i < n; i++)
        sk_line_aa(g_mask, s[i].x1, s[i].y1, s[i].x2, s[i].y2, 4.6f, SK_RGB(255, 255, 255));
    for (int i = 0; i + 2 < nheads; i += 3) {
        sk_pointf tri[3] = { heads[i], heads[i + 1], heads[i + 2] };
        sk_fill_polygon(g_mask, tri, 3, SK_RGB(255, 255, 255));
        for (int k = 0; k < 3; k++)
            sk_line_aa(g_mask, tri[k].x, tri[k].y, tri[(k + 1) % 3].x, tri[(k + 1) % 3].y, 3.0f,
                       SK_RGB(255, 255, 255));
    }
    layer_apply(SK_INK, 0.18f, 1, 2);
    layer_apply(SK_RGB(255, 255, 255), 1.0f, 0, 0);
    mask_begin();
    sk_rough_style st;
    pencil_style(&st, 0xBEEF0000u + (uint32_t)id, 1.4f);
    st.double_stroke = 0;
    for (int i = 0; i < n; i++) {
        st.seed++;
        sk_rough_line(g_mask, s[i].x1, s[i].y1, s[i].x2, s[i].y2, &st);
    }
    for (int i = 0; i + 2 < nheads; i += 3) {
        sk_pointf tri[3] = { heads[i], heads[i + 1], heads[i + 2] };
        sk_fill_polygon(g_mask, tri, 3, SK_RGB(255, 255, 255));
    }
    layer_apply(SK_INK, 1.0f, 0, 0);
    finish(id, hot_x, hot_y);
}

/* ---------- 各个光标 ---------- */

static void make_arrow(void)
{
    static const sk_pointf p[] = {
        { 2, 2 }, { 2, 21 }, { 6.6f, 16.8f }, { 10, 24.5f }, { 13.4f, 23 }, { 10, 15.6f }, { 16.5f, 15.4f },
    };
    shape_cursor(SK_CURSOR_ARROW, p, (int)SK_ARRAY_LEN(p), 2, 2, SK_RGB(255, 255, 255));
}

static void make_hand(void)
{
    /* 竖起食指的小手：手指 + 手掌（沿轮廓描一圈），再在指缝处补两道短线 */
    static const sk_pointf p[] = {
        { 8, 3 }, { 9.5f, 1.6f }, { 11, 3 }, { 11, 10 }, { 12.5f, 9 }, { 14, 10 }, { 14.5f, 10.6f },
        { 16, 10 }, { 17.5f, 11 }, { 18, 11.6f }, { 19.5f, 11.4f }, { 20.5f, 12.6f }, { 20.5f, 18 },
        { 18.6f, 22.5f }, { 10.4f, 22.5f }, { 7.2f, 18.6f }, { 4.2f, 14.4f }, { 4.6f, 12.6f },
        { 6.4f, 12.8f }, { 8, 14.6f },
    };
    sk_rough_style st;
    acc_begin();
    mask_begin();
    sk_fill_polygon(g_mask, p, (int)SK_ARRAY_LEN(p), SK_RGB(255, 255, 255));
    layer_apply(SK_INK, 0.22f, 1, 2);
    layer_apply(SK_RGB(255, 255, 255), 1.0f, 0, 0);
    mask_begin();
    pencil_style(&st, 0x4A9D0001u, 1.4f);
    sk_rough_polygon(g_mask, p, (int)SK_ARRAY_LEN(p), &st);
    st.double_stroke = 0;
    st.seed++;
    sk_rough_line(g_mask, 14.2f, 10.8f, 14.2f, 14.5f, &st);
    st.seed++;
    sk_rough_line(g_mask, 17.6f, 11.4f, 17.6f, 14.8f, &st);
    layer_apply(SK_INK, 1.0f, 0, 0);
    finish(SK_CURSOR_HAND, 9, 2);
}

static void make_text(void)
{
    static const seg s[] = {
        { 8.5f, 3, 8.5f, 20 }, { 5.5f, 2.5f, 7.5f, 3.2f }, { 9.5f, 3.2f, 11.5f, 2.5f },
        { 5.5f, 20.5f, 7.5f, 19.8f }, { 9.5f, 19.8f, 11.5f, 20.5f },
    };
    line_cursor(SK_CURSOR_TEXT, s, (int)SK_ARRAY_LEN(s), NULL, 0, 8, 11);
}

static void make_cross(void)
{
    static const seg s[] = { { 11, 2, 11, 9 }, { 11, 13, 11, 20 }, { 2, 11, 9, 11 }, { 13, 11, 20, 11 } };
    line_cursor(SK_CURSOR_CROSS, s, (int)SK_ARRAY_LEN(s), NULL, 0, 11, 11);
}

static void make_move(void)
{
    static const seg s[] = { { 12, 4, 12, 20 }, { 4, 12, 20, 12 } };
    static const sk_pointf h[] = {
        { 12, 1 }, { 8.5f, 5.5f }, { 15.5f, 5.5f },       /* 上 */
        { 12, 23 }, { 8.5f, 18.5f }, { 15.5f, 18.5f },    /* 下 */
        { 1, 12 }, { 5.5f, 8.5f }, { 5.5f, 15.5f },       /* 左 */
        { 23, 12 }, { 18.5f, 8.5f }, { 18.5f, 15.5f },    /* 右 */
    };
    line_cursor(SK_CURSOR_MOVE, s, (int)SK_ARRAY_LEN(s), h, (int)SK_ARRAY_LEN(h), 12, 12);
}

static void make_resize(void)
{
    static const seg s[] = { { 5, 5, 17, 17 } };
    static const sk_pointf h[] = {
        { 2, 2 }, { 9.5f, 3 }, { 3, 9.5f },               /* 左上箭头 */
        { 20, 20 }, { 12.5f, 19 }, { 19, 12.5f },         /* 右下箭头 */
    };
    line_cursor(SK_CURSOR_RESIZE, s, (int)SK_ARRAY_LEN(s), h, (int)SK_ARRAY_LEN(h), 11, 11);
}

static void make_pencil(void)
{
    /* 斜放的小铅笔：笔尖在左下角（热点），黄色笔杆、木色削尖部分、粉色橡皮 */
    sk_rough_style st;
    acc_begin();
    static const sk_pointf body[] = { { 6.5f, 17.5f }, { 18.5f, 5.5f }, { 22.5f, 9.5f }, { 10.5f, 21.5f } };
    static const sk_pointf wood[] = { { 6.5f, 17.5f }, { 10.5f, 21.5f }, { 2, 26 } };
    static const sk_pointf lead[] = { { 3.6f, 22.6f }, { 5.4f, 24.4f }, { 2, 26 } };
    static const sk_pointf rubber[] = { { 18.5f, 5.5f }, { 21, 3 }, { 25, 7 }, { 22.5f, 9.5f } };
    static const sk_pointf outline[] = {
        { 2, 26 }, { 6.5f, 17.5f }, { 21, 3 }, { 25, 7 }, { 10.5f, 21.5f },
    };
    mask_begin();
    sk_fill_polygon(g_mask, outline, (int)SK_ARRAY_LEN(outline), SK_RGB(255, 255, 255));
    layer_apply(SK_INK, 0.22f, 1, 2);
    mask_begin();
    sk_fill_polygon(g_mask, body, 4, SK_RGB(255, 255, 255));
    layer_apply(SK_YELLOW, 1.0f, 0, 0);
    mask_begin();
    sk_fill_polygon(g_mask, wood, 3, SK_RGB(255, 255, 255));
    layer_apply(SK_RGB(0xF3, 0xD9, 0xB1), 1.0f, 0, 0);
    mask_begin();
    sk_fill_polygon(g_mask, lead, 3, SK_RGB(255, 255, 255));
    layer_apply(SK_INK, 1.0f, 0, 0);
    mask_begin();
    sk_fill_polygon(g_mask, rubber, 4, SK_RGB(255, 255, 255));
    layer_apply(SK_PINK, 1.0f, 0, 0);
    mask_begin();
    pencil_style(&st, 0x9E9C11u, 1.2f);
    sk_rough_polygon(g_mask, outline, (int)SK_ARRAY_LEN(outline), &st);
    st.double_stroke = 0;
    st.seed++;
    sk_rough_line(g_mask, 6.5f, 17.5f, 10.5f, 21.5f, &st);
    st.seed++;
    sk_rough_line(g_mask, 18.5f, 5.5f, 22.5f, 9.5f, &st);
    st.seed++;
    sk_rough_line(g_mask, 8.5f, 19.5f, 20.5f, 7.5f, &st);    /* 笔杆中间的棱 */
    layer_apply(SK_INK, 0.95f, 0, 0);
    finish(SK_CURSOR_PENCIL, 2, 26);
}

static void make_wait(void)
{
    /* 沙漏：上下两块木板 + 两个玻璃三角，下半部分有一堆沙子 */
    sk_rough_style st;
    acc_begin();
    static const sk_pointf glass[] = {
        { 5, 4 }, { 17, 4 }, { 12.2f, 12 }, { 17, 20 }, { 5, 20 }, { 9.8f, 12 },
    };
    static const sk_pointf sand_top[] = { { 7.6f, 7 }, { 14.4f, 7 }, { 11, 11.2f } };
    static const sk_pointf sand_bot[] = { { 6.6f, 19 }, { 15.4f, 19 }, { 11, 14.6f } };
    mask_begin();
    sk_fill_polygon(g_mask, glass, 6, SK_RGB(255, 255, 255));
    sk_fill_rect(g_mask, sk_rect_make(3, 2, 17, 3), SK_RGB(255, 255, 255));
    sk_fill_rect(g_mask, sk_rect_make(3, 20, 17, 3), SK_RGB(255, 255, 255));
    layer_apply(SK_INK, 0.22f, 1, 2);
    layer_apply(SK_RGB(255, 255, 255), 1.0f, 0, 0);
    mask_begin();
    sk_fill_polygon(g_mask, sand_top, 3, SK_RGB(255, 255, 255));
    sk_fill_polygon(g_mask, sand_bot, 3, SK_RGB(255, 255, 255));
    layer_apply(SK_ORANGE, 1.0f, 0, 0);
    mask_begin();
    pencil_style(&st, 0x5A9D61u, 1.3f);
    sk_rough_polygon(g_mask, glass, 6, &st);
    st.seed++;
    sk_rough_rect(g_mask, 3, 2, 16, 2.5f, &st);
    st.seed++;
    sk_rough_rect(g_mask, 3, 20, 16, 2.5f, &st);
    layer_apply(SK_INK, 1.0f, 0, 0);
    finish(SK_CURSOR_WAIT, 11, 12);
}

/* ---------- 接口 ---------- */

void cursor_init(void)
{
    g_mask = sk_canvas_new(CUR_BOX, CUR_BOX);
    if (!g_mask)
        return;
    make_arrow();
    make_text();
    make_hand();
    make_cross();
    make_pencil();
    make_move();
    make_resize();
    make_wait();
    sk_canvas_free(g_mask);
    g_mask = NULL;
}

static sk_rect rect_for(int shape, int x, int y)
{
    const cursor_img *c = &g_cur[shape];
    if (!c->img)
        return sk_rect_make(0, 0, 0, 0);
    return sk_rect_make(x - c->hot_x, y - c->hot_y, c->img->w, c->img->h);
}

sk_rect cursor_rect(void)
{
    if (!g_visible)
        return sk_rect_make(0, 0, 0, 0);
    return rect_for(g_shape, g_x, g_y);
}

const sk_canvas *cursor_image(void)
{
    return g_visible ? g_cur[g_shape].img : NULL;
}

int cursor_shape(void) { return g_shape; }

void cursor_set_shape(int id)
{
    if (id < 0 || id >= SK_CURSOR_COUNT || !g_cur[id].img)
        id = SK_CURSOR_ARROW;
    if (id == g_shape)
        return;
    comp_flush_only(cursor_rect());
    g_shape = id;
    comp_flush_only(cursor_rect());
}

void cursor_set_pos(int x, int y)
{
    if (x == g_x && y == g_y)
        return;
    comp_flush_only(cursor_rect());
    g_x = x;
    g_y = y;
    comp_flush_only(cursor_rect());
}

void cursor_set_visible(int on)
{
    on = on ? 1 : 0;
    if (on == g_visible)
        return;
    comp_flush_only(rect_for(g_shape, g_x, g_y));
    g_visible = on;
}
