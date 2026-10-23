/*
 * draw.c — 画板的“笔”：铅笔、刷子、直线/矩形/椭圆、油漆桶
 *
 * 这些函数只管往一块 sk_canvas 上画，不知道窗口、撤销这些事情；
 * 同一个函数既用来画进图片，也用来在窗口上画“拖动中的预览”。
 * 所有函数都遵守 canvas 的裁剪区 clip：调用者把 clip 设成“已经交给撤销记录的区域”，
 * 就能保证画笔绝不会改到撤销记录没保存过的像素。
 */
#include "skpaint.h"

#include <sketch/rough.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int iabs(int v) { return v < 0 ? -v : v; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ================================================================ 铅笔：Bresenham 直线 */

/*
 * 鼠标移动事件是“离散”的：手快的时候两次事件之间可能隔了几十个像素
 * （模拟器里更明显，库还会把堆积的移动事件合并掉）。所以画笔不能只点一个点，
 * 而要从上一个位置连一条线到当前位置——这样再快的笔画也不会断。
 *
 * Bresenham 算法只用整数加减就能决定一条线经过哪些像素：沿着变化较大的方向
 * 每次走一格，用“累计误差”判断另一个方向什么时候该走一格。
 */
void draw_pencil_line(sk_canvas *c, int x0, int y0, int x1, int y1, sk_color color)
{
    if (!c || !c->pixels || sk_rect_empty(c->clip))
        return;
    sk_rect cl = c->clip;
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= cl.x && x0 < cl.x + cl.w && y0 >= cl.y && y0 < cl.y + cl.h)
            c->pixels[(size_t)y0 * c->stride + x0] = color;
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* ================================================================ 刷子 */

void draw_brush_line(sk_canvas *c, float x0, float y0, float x1, float y1, float width, sk_color color)
{
    /* “胶囊”形状（线段 + 两头半圆）正好是一个圆形笔刷从起点拖到终点扫过的区域 */
    sk_line_aa(c, x0, y0, x1, y1, width, color);
}

/* ================================================================ 直线 / 矩形 / 椭圆 */

/* 端点规范化：返回左上角和右下角 */
static void shape_box(const paint_shape *s, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = imin(s->x0, s->x1);
    *y0 = imin(s->y0, s->y1);
    *x1 = imax(s->x0, s->x1);
    *y1 = imax(s->y0, s->y1);
}

/* 规整的椭圆：沿椭圆取一圈点，交给 sk_brush_stroke 画成“一笔”。
 * 它先在蒙版上盖章、最后统一混合一次，所以首尾相接的地方不会因为重复混合而变深。 */
static void smooth_ellipse(sk_canvas *c, float cx, float cy, float rx, float ry, float width, sk_color color)
{
    float perim = 2.0f * 3.14159265f * sqrtf((rx * rx + ry * ry) * 0.5f);
    int n = (int)(perim / 3.0f);
    if (n < 16) n = 16;
    if (n > 720) n = 720;
    /* 首尾各多一个点：样条曲线的两端也能得到正确的切线，整圈是闭合光滑的 */
    sk_pointf *pts = malloc(sizeof *pts * (size_t)(n + 3));
    if (!pts)
        return;
    for (int i = -1; i <= n + 1; i++) {
        float a = 2.0f * 3.14159265f * (float)i / (float)n;
        pts[i + 1].x = cx + rx * cosf(a);
        pts[i + 1].y = cy + ry * sinf(a);
    }
    sk_brush_stroke(c, pts, n + 3, width, width, color, 0, 0);
    free(pts);
}

void shape_draw(sk_canvas *c, const paint_shape *s, int ox, int oy)
{
    int x0, y0, x1, y1;
    shape_box(s, &x0, &y0, &x1, &y1);
    /* 像素 (x,y) 的中心在 (x+0.5, y+0.5)：线条居中压在像素上，奇数线宽才不会糊成两格 */
    float fx0 = (float)(x0 + ox) + 0.5f, fy0 = (float)(y0 + oy) + 0.5f;
    float fx1 = (float)(x1 + ox) + 0.5f, fy1 = (float)(y1 + oy) + 0.5f;
    float w = (float)s->size;

    if (s->rough) {
        /* 手绘风格：和系统界面一样的 rough.h 图元，seed 固定所以预览和定稿一致 */
        sk_rough_style st;
        sk_rough_default(&st);
        st.seed = s->seed;
        st.stroke = s->color;
        st.stroke_width = w;
        st.double_stroke = s->size <= 6;      /* 粗笔描两遍会糊成一团 */
        st.roughness = s->size > 12 ? 0.6f : 1.0f;
        switch (s->tool) {
        case TOOL_LINE:
            sk_rough_line(c, (float)(s->x0 + ox) + 0.5f, (float)(s->y0 + oy) + 0.5f,
                          (float)(s->x1 + ox) + 0.5f, (float)(s->y1 + oy) + 0.5f, &st);
            break;
        case TOOL_RECT:
            sk_rough_rect(c, fx0, fy0, fx1 - fx0, fy1 - fy0, &st);
            break;
        case TOOL_ELLIPSE:
            sk_rough_ellipse(c, (fx0 + fx1) * 0.5f, (fy0 + fy1) * 0.5f, fx1 - fx0, fy1 - fy0, &st);
            break;
        default:
            break;
        }
        return;
    }

    switch (s->tool) {
    case TOOL_LINE:
        if (s->size <= 1)
            draw_pencil_line(c, s->x0 + ox, s->y0 + oy, s->x1 + ox, s->y1 + oy, s->color);
        else
            sk_line_aa(c, (float)(s->x0 + ox) + 0.5f, (float)(s->y0 + oy) + 0.5f,
                       (float)(s->x1 + ox) + 0.5f, (float)(s->y1 + oy) + 0.5f, w, s->color);
        break;
    case TOOL_RECT: {
        /* 规整矩形 = 四条实心的横竖带子，边缘是整像素，不需要抗锯齿 */
        int h0 = s->size / 2;
        int L = x0 + ox - h0, T = y0 + oy - h0;
        int W = x1 - x0 + s->size, H = y1 - y0 + s->size;
        sk_fill_rect(c, sk_rect_make(L, T, W, s->size), s->color);
        sk_fill_rect(c, sk_rect_make(L, y1 + oy - h0, W, s->size), s->color);
        sk_fill_rect(c, sk_rect_make(L, T, s->size, H), s->color);
        sk_fill_rect(c, sk_rect_make(x1 + ox - h0, T, s->size, H), s->color);
        break;
    }
    case TOOL_ELLIPSE:
        smooth_ellipse(c, (fx0 + fx1) * 0.5f, (fy0 + fy1) * 0.5f, (fx1 - fx0) * 0.5f, (fy1 - fy0) * 0.5f,
                       w, s->color);
        break;
    default:
        break;
    }
}

sk_rect shape_bounds(const paint_shape *s)
{
    int x0, y0, x1, y1;
    shape_box(s, &x0, &y0, &x1, &y1);
    /* 线宽的一半 + 抗锯齿边缘；手绘风格还要加上抖动、角上“出头”和椭圆半径的随机变化 */
    int pad = s->size / 2 + 3;
    if (s->rough)
        pad += 10 + imax(x1 - x0, y1 - y0) / 10;
    return sk_rect_make(x0 - pad, y0 - pad, x1 - x0 + 1 + 2 * pad, y1 - y0 + 1 + 2 * pad);
}

/* ================================================================ 油漆桶 */

/* 两个颜色“相近”：每个分量相差都不超过 tol。
 * 刷子的边缘是抗锯齿的（介于笔色和底色之间），留一点容差，填色才能贴着线条边缘。 */
static int close_color(uint32_t a, uint32_t b, int tol)
{
    return iabs((int)SK_R(a) - (int)SK_R(b)) <= tol && iabs((int)SK_G(a) - (int)SK_G(b)) <= tol &&
           iabs((int)SK_B(a) - (int)SK_B(b)) <= tol;
}

typedef struct { int x, y; } seed_pt;

/*
 * 扫描线填充，用自己管理的“栈”而不是递归。
 *
 * 教科书上的写法最简单：fill(x,y) 涂一个点，再递归 fill 上下左右四个邻居。
 * 但递归每深入一层就要在线程栈上压一个栈帧（返回地址、参数、保存的寄存器，几十字节），
 * 一片 640×420 的空白区域最坏要递归 27 万层，栈帧加起来十几 MB——而 Linux 给主线程的栈
 * 默认只有 8 MB（ulimit -s），超过了就碰到栈底下的“保护页”，内核发 SIGSEGV 把进程杀掉。
 * 栈的大小在进程启动时就定了，程序没法“检查一下够不够”。
 *
 * 所以这里把“待处理的种子点”放进 malloc 来的数组里（堆上的栈，不够了就 realloc 扩大，
 * 分配失败时可以体面地报错），而且一次处理一整段横线：从种子点向左右扩展到边界，
 * 然后只在上下两行里“每一段连续的可填区域”压一个种子。这样栈里的元素数量
 * 和区域的“段数”成正比，而不是和像素数成正比，通常只有几百个。
 */
int fill_compute(const sk_canvas *img, int sx, int sy, sk_color color, int tol, uint8_t **mask_out, sk_rect *bbox)
{
    *mask_out = NULL;
    int w = img->w, h = img->h;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h)
        return 0;
    const uint32_t *px = img->pixels;
    int stride = img->stride;
    uint32_t target = px[(size_t)sy * stride + sx];
    if ((target & 0xFFFFFFu) == (color & 0xFFFFFFu))
        return 0;                                   /* 点在同色区域上：什么都不用做 */

    uint8_t *mask = calloc((size_t)w * h, 1);       /* 已经涂过的像素（也防止重复入栈） */
    int cap = 256, n = 0;
    seed_pt *stack = malloc(sizeof *stack * (size_t)cap);
    if (!mask || !stack) {
        free(mask);
        free(stack);
        return -1;
    }
    int bx0 = sx, by0 = sy, bx1 = sx, by1 = sy;
    stack[n++] = (seed_pt){ sx, sy };

#define FILLABLE(X, Y) (!mask[(size_t)(Y) * w + (X)] && close_color(px[(size_t)(Y) * stride + (X)], target, tol))

    while (n > 0) {
        seed_pt p = stack[--n];
        if (!FILLABLE(p.x, p.y))
            continue;
        /* 向左右扩展成一整段 [xl, xr] */
        int xl = p.x, xr = p.x;
        while (xl > 0 && FILLABLE(xl - 1, p.y))
            xl--;
        while (xr < w - 1 && FILLABLE(xr + 1, p.y))
            xr++;
        memset(mask + (size_t)p.y * w + xl, 1, (size_t)(xr - xl + 1));
        if (xl < bx0) bx0 = xl;
        if (xr > bx1) bx1 = xr;
        if (p.y < by0) by0 = p.y;
        if (p.y > by1) by1 = p.y;
        /* 上一行和下一行：每段连续的可填像素只压一个种子 */
        for (int dir = -1; dir <= 1; dir += 2) {
            int ny = p.y + dir;
            if (ny < 0 || ny >= h)
                continue;
            int in_run = 0;
            for (int x = xl; x <= xr; x++) {
                if (!FILLABLE(x, ny)) {
                    in_run = 0;
                    continue;
                }
                if (in_run)
                    continue;
                in_run = 1;
                if (n == cap) {
                    seed_pt *ns = realloc(stack, sizeof *ns * (size_t)cap * 2);
                    if (!ns) {
                        free(stack);
                        free(mask);
                        return -1;
                    }
                    stack = ns;
                    cap *= 2;
                }
                stack[n++] = (seed_pt){ x, ny };
            }
        }
    }
#undef FILLABLE
    free(stack);
    *mask_out = mask;
    *bbox = sk_rect_make(bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
    return 1;
}

void fill_apply(sk_canvas *img, const uint8_t *mask, sk_rect bbox, sk_color color)
{
    for (int y = bbox.y; y < bbox.y + bbox.h; y++) {
        const uint8_t *m = mask + (size_t)y * img->w;
        uint32_t *p = img->pixels + (size_t)y * img->stride;
        for (int x = bbox.x; x < bbox.x + bbox.w; x++)
            if (m[x])
                p[x] = color;
    }
}
