/*
 * gfx.c — libsketch 的光栅图形核心
 *
 * 这里是整个图形系统的“像素工厂”：手绘线条、控件、窗口边框、文字，最后都落到这几个
 * 函数上——往一块 32 位像素内存里写数。屏幕本身也只是这样一块内存（帧缓冲 framebuffer）：
 * 显卡不停地按行扫描它，把每个 0xAARRGGBB 变成屏幕上一个点的颜色。
 *
 * 三条贯穿全文件的原则：
 *   1. 裁剪（clipping）：任何写操作之前先和 canvas->clip 求交集，只在交集里写。
 *      这不仅是为了“不画到别的窗口上”，更是内存安全：写出缓冲区就是踩坏别的数据。
 *   2. 按行处理：内存里一行像素是连续的，按行循环、用 32 位一次写一个像素，
 *      比逐像素调用“带裁剪检查的 put_pixel”快一个数量级（网页版的 CPU 是模拟出来的，
 *      慢 10~50 倍，这一点非常要紧）。
 *   3. 整数运算：混合、覆盖率都用定点整数，浮点只用在每条线、每行的准备工作上。
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sketch/gfx.h"
#include "gfx_internal.h"

/* 画布尺寸上限：再大就不是一块合理的屏幕/窗口了，也防止 w*h 溢出 */
#define SK_CANVAS_MAX 16384

/* 浮点坐标转整数前先夹到一个安全范围（超出 int 范围的 float→int 转换是未定义行为） */
#define SK_COORD_LIMIT 1.0e7f

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline int floor_i(float v)
{
    v = clampf(v, -SK_COORD_LIMIT, SK_COORD_LIMIT);
    int i = (int)v;
    return (v < (float)i) ? i - 1 : i;
}

static inline int ceil_i(float v)
{
    v = clampf(v, -SK_COORD_LIMIT, SK_COORD_LIMIT);
    int i = (int)v;
    return (v > (float)i) ? i + 1 : i;
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static inline int64_t clamp64_int(int64_t v)
{
    return v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : v;
}

/* ======================================================================
 * 矩形运算
 * 边界一律用 64 位计算，防止 x + w 这种加法在极端输入下溢出。
 * ====================================================================== */

int sk_rect_empty(sk_rect r)
{
    return r.w <= 0 || r.h <= 0;
}

sk_rect sk_rect_intersect(sk_rect a, sk_rect b)
{
    sk_rect r = { 0, 0, 0, 0 };
    if (sk_rect_empty(a) || sk_rect_empty(b))
        return r;
    int64_t x0 = a.x > b.x ? a.x : b.x;
    int64_t y0 = a.y > b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.w, bx1 = (int64_t)b.x + b.w;
    int64_t ay1 = (int64_t)a.y + a.h, by1 = (int64_t)b.y + b.h;
    int64_t x1 = ax1 < bx1 ? ax1 : bx1;
    int64_t y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0)
        return r;
    r.x = (int)x0;
    r.y = (int)y0;
    r.w = (int)clamp64_int(x1 - x0);
    r.h = (int)clamp64_int(y1 - y0);
    return r;
}

sk_rect sk_rect_union(sk_rect a, sk_rect b)
{
    if (sk_rect_empty(a))
        return sk_rect_empty(b) ? sk_rect_make(0, 0, 0, 0) : b;
    if (sk_rect_empty(b))
        return a;
    int64_t x0 = a.x < b.x ? a.x : b.x;
    int64_t y0 = a.y < b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.w, bx1 = (int64_t)b.x + b.w;
    int64_t ay1 = (int64_t)a.y + a.h, by1 = (int64_t)b.y + b.h;
    int64_t x1 = ax1 > bx1 ? ax1 : bx1;
    int64_t y1 = ay1 > by1 ? ay1 : by1;
    sk_rect r = { (int)x0, (int)y0, (int)clamp64_int(x1 - x0), (int)clamp64_int(y1 - y0) };
    return r;
}

int sk_rect_contains(sk_rect r, int x, int y)
{
    return x >= r.x && y >= r.y &&
           (int64_t)x < (int64_t)r.x + r.w && (int64_t)y < (int64_t)r.y + r.h;
}

sk_rect sk_rect_inset(sk_rect r, int dx, int dy)
{
    sk_rect o;
    o.x = (int)clamp64_int((int64_t)r.x + dx);
    o.y = (int)clamp64_int((int64_t)r.y + dy);
    o.w = (int)clamp64_int((int64_t)r.w - 2 * (int64_t)dx);
    o.h = (int)clamp64_int((int64_t)r.h - 2 * (int64_t)dy);
    return o;
}

sk_rect sk_rect_offset(sk_rect r, int dx, int dy)
{
    r.x = (int)clamp64_int((int64_t)r.x + dx);
    r.y = (int)clamp64_int((int64_t)r.y + dy);
    return r;
}

/* ======================================================================
 * 画布与裁剪
 * ====================================================================== */

void sk_canvas_init(sk_canvas *c, uint32_t *pixels, int w, int h, int stride)
{
    if (!c)
        return;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (stride < w) stride = w;
    c->pixels = pixels;
    c->w = w;
    c->h = h;
    c->stride = stride;
    c->clip = pixels ? sk_rect_make(0, 0, w, h) : sk_rect_make(0, 0, 0, 0);
}

sk_canvas *sk_canvas_new(int w, int h)
{
    if (w <= 0 || h <= 0 || w > SK_CANVAS_MAX || h > SK_CANVAS_MAX)
        return NULL;
    /* 结构体和像素一次分配：sk_canvas_free 只需要一次 free。
     * calloc 保证像素初始为 0（完全透明的黑色）。 */
    size_t head = (sizeof(sk_canvas) + 15) & ~(size_t)15;
    sk_canvas *c = calloc(1, head + (size_t)w * (size_t)h * 4);
    if (!c)
        return NULL;
    sk_canvas_init(c, (uint32_t *)((char *)c + head), w, h, w);
    return c;
}

void sk_canvas_free(sk_canvas *c)
{
    free(c);
}

/*
 * 子画布：不复制像素，只是换一个“原点”和尺寸来看父画布的同一块内存。
 * 控件、窗口内容区都可以这样拿到一个“自己的坐标系”。
 * 右/下方超出父画布的部分直接从尺寸里去掉；左/上方超出的部分没法从尺寸里去掉
 * （原点必须留在 r 的左上角），只能靠 clip 排除——所以不要对这种子画布调用
 * sk_reset_clip / sk_set_clip 去扩大裁剪区。
 */
void sk_canvas_sub(sk_canvas *sub, const sk_canvas *parent, sk_rect r)
{
    if (!sub)
        return;
    if (!parent || !parent->pixels || sk_rect_empty(r)) {
        sk_canvas_init(sub, NULL, 0, 0, 0);
        return;
    }
    int64_t w = r.w, h = r.h;
    if ((int64_t)r.x + w > parent->w) w = (int64_t)parent->w - r.x;
    if ((int64_t)r.y + h > parent->h) h = (int64_t)parent->h - r.y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    /* 和父画布完全不重叠：没有任何像素可以画，给一个空画布 */
    if (w == 0 || h == 0 || (int64_t)r.x + r.w <= 0 || (int64_t)r.y + r.h <= 0) {
        sk_canvas_init(sub, NULL, 0, 0, 0);
        return;
    }
    sk_rect clip = sk_rect_intersect(parent->clip, r);
    /* 原点指针可能在父缓冲区之前（r.x/r.y 为负），但我们只会通过 clip 内的坐标访问 */
    sub->pixels = parent->pixels + ((intptr_t)r.y * parent->stride + r.x);
    sub->w = (int)w;
    sub->h = (int)h;
    sub->stride = parent->stride;
    sub->clip = sk_rect_empty(clip) ? sk_rect_make(0, 0, 0, 0)
              : sk_rect_intersect(sk_rect_offset(clip, -r.x, -r.y), sk_rect_make(0, 0, (int)w, (int)h));
}

void sk_set_clip(sk_canvas *c, sk_rect r)
{
    if (!c)
        return;
    c->clip = c->pixels ? sk_rect_intersect(r, sk_rect_make(0, 0, c->w, c->h)) : sk_rect_make(0, 0, 0, 0);
}

void sk_reset_clip(sk_canvas *c)
{
    if (!c)
        return;
    c->clip = c->pixels ? sk_rect_make(0, 0, c->w, c->h) : sk_rect_make(0, 0, 0, 0);
}

sk_rect sk_push_clip(sk_canvas *c, sk_rect r)
{
    if (!c)
        return sk_rect_make(0, 0, 0, 0);
    sk_rect old = c->clip;
    c->clip = sk_rect_intersect(old, r);
    return old;
}

void sk_pop_clip(sk_canvas *c, sk_rect old)
{
    if (!c)
        return;
    /* 恢复时也和画布范围求交，保证 clip 永远在画布内 */
    sk_set_clip(c, old);
}

/* ======================================================================
 * 颜色
 * ====================================================================== */

sk_color sk_blend(sk_color dst, sk_color src)
{
    return sk__over(dst, src, SK_A(src));
}

sk_color sk_mix(sk_color a, sk_color b, int t)
{
    if (t <= 0)
        return a;
    if (t >= 255)
        return b;
    uint32_t t2 = (uint32_t)t + ((uint32_t)t >> 7);
    uint32_t it = 256 - t2;
    uint32_t rb = (((a & 0xFF00FFu) * it + (b & 0xFF00FFu) * t2) >> 8) & 0xFF00FFu;
    uint32_t ag = ((((a >> 8) & 0xFF00FFu) * it + ((b >> 8) & 0xFF00FFu) * t2)) & 0xFF00FF00u;
    return ag | rb;
}

sk_color sk_lighten(sk_color c, int amount)
{
    return sk_mix(c, c | 0x00FFFFFFu, amount);
}

sk_color sk_darken(sk_color c, int amount)
{
    return sk_mix(c, c & 0xFF000000u, amount);
}

/* ======================================================================
 * 基本绘制
 * ====================================================================== */

static inline uint32_t *px_at(const sk_canvas *c, int x, int y)
{
    return c->pixels + ((intptr_t)y * c->stride + x);
}

/* 32 位整行填充（手动展开 4 次，编译器在 i686 上也能生成紧凑的 store 序列） */
static inline void fill32(uint32_t *p, uint32_t v, int n)
{
    while (n >= 4) {
        p[0] = v; p[1] = v; p[2] = v; p[3] = v;
        p += 4;
        n -= 4;
    }
    while (n-- > 0)
        *p++ = v;
}

/* 一行像素和同一个颜色以同一不透明度混合 */
static void blend_row(uint32_t *p, int n, uint32_t color, uint32_t a)
{
    uint32_t a2 = a + (a >> 7);
    uint32_t ia = 256 - a2;
    uint32_t srb = (color & 0xFF00FFu) * a2;
    uint32_t sg = (color & 0x00FF00u) * a2;
    for (int i = 0; i < n; i++) {
        uint32_t d = p[i];
        uint32_t rb = (srb + (d & 0xFF00FFu) * ia) >> 8;
        uint32_t g = (sg + (d & 0x00FF00u) * ia) >> 8;
        p[i] = 0xFF000000u | (rb & 0xFF00FFu) | (g & 0x00FF00u);
    }
}

static int clip_ok(const sk_canvas *c)
{
    return c && c->pixels && !sk_rect_empty(c->clip);
}

void sk_clear(sk_canvas *c, sk_color color)
{
    if (!clip_ok(c))
        return;
    sk_rect r = c->clip;
    for (int y = r.y; y < r.y + r.h; y++)
        fill32(px_at(c, r.x, y), color, r.w);
}

void sk_fill_rect(sk_canvas *c, sk_rect rr, sk_color color)
{
    if (!clip_ok(c))
        return;
    uint32_t a = SK_A(color);
    if (a == 0)
        return;
    sk_rect r = sk_rect_intersect(rr, c->clip);
    if (sk_rect_empty(r))
        return;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *p = px_at(c, r.x, y);
        if (a == 255)
            fill32(p, color, r.w);
        else
            blend_row(p, r.w, color, a);
    }
}

void sk_put_pixel(sk_canvas *c, int x, int y, sk_color color)
{
    if (!clip_ok(c) || !sk_rect_contains(c->clip, x, y))
        return;
    uint32_t *p = px_at(c, x, y);
    *p = sk__over(*p, color, SK_A(color));
}

void sk_blend_pixel_cov(sk_canvas *c, int x, int y, sk_color color, int cov)
{
    if (cov <= 0 || !clip_ok(c) || !sk_rect_contains(c->clip, x, y))
        return;
    if (cov > 255)
        cov = 255;
    uint32_t *p = px_at(c, x, y);
    *p = sk__over(*p, color, sk__mul255(SK_A(color), (uint32_t)cov));
}

void sk_hline(sk_canvas *c, int x0, int x1, int y, sk_color color)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    /* 两端都包含：长度 = x1 - x0 + 1 */
    sk_fill_rect(c, sk_rect_make(x0, y, (int)clamp64_int((int64_t)x1 - x0 + 1), 1), color);
}

void sk_vline(sk_canvas *c, int x, int y0, int y1, sk_color color)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    sk_fill_rect(c, sk_rect_make(x, y0, 1, (int)clamp64_int((int64_t)y1 - y0 + 1)), color);
}

void sk_rect_outline(sk_canvas *c, sk_rect r, sk_color color)
{
    if (sk_rect_empty(r))
        return;
    /* 四条边互不重叠，这样半透明颜色在角上也不会被混合两次 */
    sk_fill_rect(c, sk_rect_make(r.x, r.y, r.w, 1), color);
    if (r.h > 1)
        sk_fill_rect(c, sk_rect_make(r.x, r.y + r.h - 1, r.w, 1), color);
    if (r.h > 2) {
        sk_fill_rect(c, sk_rect_make(r.x, r.y + 1, 1, r.h - 2), color);
        if (r.w > 1)
            sk_fill_rect(c, sk_rect_make(r.x + r.w - 1, r.y + 1, 1, r.h - 2), color);
    }
}

/* ======================================================================
 * 临时缓冲区
 * ====================================================================== */

static void  *g_scratch[SK__SCRATCH_COUNT];
static size_t g_scratch_cap[SK__SCRATCH_COUNT];

void *sk__scratch(int slot, size_t bytes)
{
    if (slot < 0 || slot >= SK__SCRATCH_COUNT)
        return NULL;
    if (bytes > g_scratch_cap[slot]) {
        size_t cap = g_scratch_cap[slot] ? g_scratch_cap[slot] : 4096;
        while (cap < bytes) {
            if (cap > ((size_t)-1) / 2)
                return NULL;
            cap *= 2;
        }
        void *p = realloc(g_scratch[slot], cap);
        if (!p)
            return NULL;
        g_scratch[slot] = p;
        g_scratch_cap[slot] = cap;
    }
    return g_scratch[slot];
}

/* ======================================================================
 * 笔触累积器（原理见 gfx_internal.h）
 * ====================================================================== */

/* 蒙版缓冲区：不变式是“不在笔触中间时，整块内存全是 0”。
 * 所以 begin 不需要清零，end 只清零真正写过的那些列。 */
static uint8_t *g_mask;
static size_t   g_mask_cap;
static int      g_stroke_active;

static uint8_t *mask_get(size_t bytes)
{
    if (bytes > g_mask_cap) {
        size_t cap = g_mask_cap ? g_mask_cap : 16384;
        while (cap < bytes) {
            if (cap > ((size_t)-1) / 2)
                return NULL;
            cap *= 2;
        }
        free(g_mask);
        g_mask = calloc(cap, 1);
        g_mask_cap = g_mask ? cap : 0;
    }
    return g_mask;
}

/* ---- 笔刷印章缓存 ----
 * 一个“章”是一个抗锯齿实心圆的覆盖率位图。圆心不一定落在像素中心，所以每个半径预先算好
 * ph×ph 种子像素相位（1/8 或 1/4 像素精度）。盖章时只是查表 + 取最大值，没有任何浮点运算。 */
#define BRUSH_MAX_R8 (24 * 8)   /* 缓存的最大半径（1/8 像素为单位），更大的笔刷直接算 */
#define BRUSH_SLOTS  12

typedef struct {
    int      r8;        /* 半径×8，0 表示空槽 */
    int      D, H, ph;  /* 位图边长、圆心所在格、每轴相位数 */
    unsigned lru;
    uint8_t *data;      /* ph*ph 张 D*D 的位图 */
    uint8_t *box;       /* 每个相位非零区域的包围盒：u0 v0 u1 v1（右/下开区间） */
} brush_t;

static brush_t  g_brush[BRUSH_SLOTS];
static unsigned g_brush_clock;

/* 半径 r 的圆，离圆心 d 处的像素覆盖率（0..1）。
 * 用“边缘 1 像素宽线性过渡”近似真实面积覆盖；细于 1 像素的笔改成“1 像素宽的笔乘以粗细”，
 * 否则 0.6px 的线中心会和 1px 的线一样黑。 */
static float brush_profile(float d, float r)
{
    if (r < 0.5f) {
        float v = 1.0f - d;
        return v <= 0.0f ? 0.0f : v * (2.0f * r);
    }
    float v = r + 0.5f - d;
    return v <= 0.0f ? 0.0f : v >= 1.0f ? 1.0f : v;
}

static float brush_reach(float r)
{
    return r < 0.5f ? 1.0f : r + 0.5f;
}

static const brush_t *g_brush_last;    /* 一笔里几乎所有章的半径都一样：先看上次用的那个 */

static const brush_t *brush_get(int r8)
{
    if (g_brush_last && g_brush_last->r8 == r8 && g_brush_last->data)
        return g_brush_last;
    for (int i = 0; i < BRUSH_SLOTS; i++) {
        if (g_brush[i].data && g_brush[i].r8 == r8) {
            g_brush[i].lru = ++g_brush_clock;
            g_brush_last = &g_brush[i];
            return g_brush_last;
        }
    }
    /* 没命中：优先用空槽，否则淘汰最久没用的（LRU） */
    brush_t *victim = &g_brush[0];
    for (int i = 0; i < BRUSH_SLOTS; i++) {
        if (!g_brush[i].data) {
            victim = &g_brush[i];
            break;
        }
        if (g_brush[i].lru < victim->lru)
            victim = &g_brush[i];
    }
    float r = (float)r8 / 8.0f;
    float reach = brush_reach(r);
    int H = ceil_i(reach + 0.5f);
    int D = H + ceil_i(reach + 1.5f);
    int ph = D <= 24 ? 8 : 4;
    size_t nphase = (size_t)ph * ph;
    uint8_t *mem = malloc(nphase * (size_t)D * D + nphase * 4);
    if (!mem)
        return NULL;
    free(victim->data);
    victim->data = mem;
    victim->box = mem + nphase * (size_t)D * D;
    victim->r8 = r8;
    victim->D = D;
    victim->H = H;
    victim->ph = ph;
    victim->lru = ++g_brush_clock;
    g_brush_last = victim;
    for (int qy = 0; qy < ph; qy++) {
        for (int qx = 0; qx < ph; qx++) {
            uint8_t *pd = victim->data + ((size_t)qy * ph + qx) * D * D;
            uint8_t *bx = victim->box + ((size_t)qy * ph + qx) * 4;
            float fx = ((float)qx + 0.5f) / (float)ph;
            float fy = ((float)qy + 0.5f) / (float)ph;
            int u0 = D, v0 = D, u1 = 0, v1 = 0;
            for (int v = 0; v < D; v++) {
                float dy = (float)(v - H) + 0.5f - fy;
                for (int u = 0; u < D; u++) {
                    float dx = (float)(u - H) + 0.5f - fx;
                    float cov = brush_profile(sqrtf(dx * dx + dy * dy), r);
                    int val = (int)(cov * 255.0f + 0.5f);
                    pd[v * D + u] = (uint8_t)val;
                    if (val) {
                        if (u < u0) u0 = u;
                        if (v < v0) v0 = v;
                        if (u + 1 > u1) u1 = u + 1;
                        if (v + 1 > v1) v1 = v + 1;
                    }
                }
            }
            if (u1 <= u0) { u0 = v0 = u1 = v1 = 0; }
            bx[0] = (uint8_t)u0; bx[1] = (uint8_t)v0; bx[2] = (uint8_t)u1; bx[3] = (uint8_t)v1;
        }
    }
    return victim;
}

/* 把一行 [x0,x1) 标记为脏（相对蒙版坐标） */
static inline void span_mark(sk__stroke *st, int row, int x0, int x1)
{
    if (x0 < st->lo[row]) st->lo[row] = x0;
    if (x1 - 1 > st->hi[row]) st->hi[row] = x1 - 1;
}

/* 半径太大（或内存不足）时的直接计算版本：每个像素算一次距离 */
static void stamp_direct(sk__stroke *st, float x, float y, float r)
{
    float reach = brush_reach(r);
    int x0 = imax(floor_i(x - reach), st->r.x), x1 = imin(ceil_i(x + reach) + 1, st->r.x + st->r.w);
    int y0 = imax(floor_i(y - reach), st->r.y), y1 = imin(ceil_i(y + reach) + 1, st->r.y + st->r.h);
    for (int py = y0; py < y1; py++) {
        uint8_t *row = st->m + (size_t)(py - st->r.y) * st->r.w;
        float dy = (float)py + 0.5f - y;
        int any0 = INT_MAX, any1 = -1;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - x;
            int val = (int)(brush_profile(sqrtf(dx * dx + dy * dy), r) * 255.0f + 0.5f);
            if (val) {
                uint8_t *m = row + (px - st->r.x);
                if (val > *m) *m = (uint8_t)val;
                if (px < any0) any0 = px;
                any1 = px;
            }
        }
        if (any1 >= 0)
            span_mark(st, py - st->r.y, any0 - st->r.x, any1 - st->r.x + 1);
    }
}

/*
 * 盖一个章。圆心 (X, Y) 是 16.16 定点数（高 16 位整数像素、低 16 位小数），半径以 1/8 像素计。
 * 为什么用定点数？32 位 x86 上 float→int 的转换要改写 x87 的控制字，非常慢，在 v86 模拟器
 * 里更慢；而盖章是最热的循环，一条线要盖成百上千次。用整数后，取整数部分只是右移 16 位，
 * 取子像素相位只是再取几位，没有任何浮点运算。
 * 调用者保证圆心离蒙版区域不远（线段已经裁剪过），所以 X>>16 一定在 int 范围内。
 */
static void stamp_fx(sk__stroke *st, int64_t X, int64_t Y, int r8)
{
    if (r8 < 1)
        r8 = 1;
    const brush_t *b = r8 <= BRUSH_MAX_R8 ? brush_get(r8) : NULL;
    if (!b) {
        stamp_direct(st, (float)((double)X / 65536.0), (float)((double)Y / 65536.0), (float)r8 / 8.0f);
        return;
    }
    int fx = (int)(X >> 16), fy = (int)(Y >> 16);     /* 算术右移 = 向下取整（负数也对） */
    int sh = b->ph == 8 ? 13 : 14;                     /* 小数部分的最高 3 位或 2 位就是相位 */
    int qx = (int)((X >> sh) & (b->ph - 1));
    int qy = (int)((Y >> sh) & (b->ph - 1));
    int phase = qy * b->ph + qx;
    const uint8_t *pd = b->data + (size_t)phase * b->D * b->D;
    const uint8_t *bx = b->box + (size_t)phase * 4;
    int ix = fx - b->H - st->r.x;     /* 位图 (0,0) 在蒙版里的位置 */
    int iy = fy - b->H - st->r.y;
    if (ix >= st->r.w || iy >= st->r.h || ix + b->D <= 0 || iy + b->D <= 0)
        return;
    int u0 = imax(bx[0], -ix), u1 = imin(bx[2], st->r.w - ix);
    int v0 = imax(bx[1], -iy), v1 = imin(bx[3], st->r.h - iy);
    if (u0 >= u1 || v0 >= v1)
        return;
    for (int v = v0; v < v1; v++) {
        const uint8_t *src = pd + v * b->D;
        uint8_t *dst = st->m + (size_t)(iy + v) * st->r.w + ix;
        for (int u = u0; u < u1; u++) {
            uint8_t s = src[u];
            if (s > dst[u])
                dst[u] = s;
        }
        span_mark(st, iy + v, ix + u0, ix + u1);
    }
}

/* 浮点 → 16.16 定点（四舍五入）。不调用 libm 的 floor()：那在 i686 上是一次函数调用 */
static inline int64_t to_fx(double v)
{
    double s = v * 65536.0 + 0.5;
    int64_t i = (int64_t)s;
    return (s < (double)i) ? i - 1 : i;
}

static inline int to_r8(float r)
{
    int r8 = (int)(r * 8.0f + 0.5f);
    return r8 < 1 ? 1 : r8;
}

/* 浮点坐标的盖章入口（点、起笔处）：先用浮点做粗剔除（顺便挡住 NaN 和离谱的坐标） */
static void stamp(sk__stroke *st, float x, float y, float r)
{
    float reach = brush_reach(r);
    if (!(x + reach + 1.0f >= (float)st->r.x && x - reach - 1.0f <= (float)(st->r.x + st->r.w) &&
          y + reach + 1.0f >= (float)st->r.y && y - reach - 1.0f <= (float)(st->r.y + st->r.h)))
        return;
    stamp_fx(st, to_fx(x), to_fx(y), to_r8(r));
}

int sk__stroke_begin(sk__stroke *st, sk_canvas *c, float x0, float y0, float x1, float y1)
{
    st->c = c;
    st->m = NULL;
    st->have_pt = 0;
    st->maxr = 0.0f;
    st->gx = 0;
    st->gy = 0;
    if (!clip_ok(c) || g_stroke_active)
        return 0;
    if (!(x0 <= x1 && y0 <= y1))     /* 同时挡住 NaN */
        return 0;
    sk_rect bb;
    int ix0 = floor_i(x0) - 1, iy0 = floor_i(y0) - 1;
    int ix1 = ceil_i(x1) + 1, iy1 = ceil_i(y1) + 1;
    bb = sk_rect_make(ix0, iy0, ix1 - ix0, iy1 - iy0);
    st->r = sk_rect_intersect(bb, c->clip);
    if (sk_rect_empty(st->r))
        return 0;
    uint8_t *m = mask_get((size_t)st->r.w * st->r.h);
    int *spans = sk__scratch(SK__SCRATCH_SPAN, sizeof(int) * 2 * (size_t)st->r.h);
    if (!m || !spans)
        return 0;
    st->m = m;
    st->lo = spans;
    st->hi = spans + st->r.h;
    for (int i = 0; i < st->r.h; i++) {
        st->lo[i] = INT_MAX;
        st->hi[i] = -1;
    }
    g_stroke_active = 1;
    return 1;
}

void sk__stroke_dot(sk__stroke *st, float x, float y, float r)
{
    if (!st->m || !(r > 0.0f))
        return;
    stamp(st, x, y, r);
}

void sk__stroke_jump(sk__stroke *st, float x, float y, float r)
{
    if (!st->m)
        return;
    if (!(r > 0.0f)) r = 0.05f;
    if (r > st->maxr) st->maxr = r;
    st->px = x;
    st->py = y;
    st->pr = r;
    st->have_pt = 1;
}

void sk__stroke_move(sk__stroke *st, float x, float y, float r)
{
    if (!st->m)
        return;
    if (!(r > 0.0f)) r = 0.05f;
    if (r > st->maxr) st->maxr = r;
    st->px = x;
    st->py = y;
    st->pr = r;
    st->have_pt = 1;
    stamp(st, x, y, r);
}

/* 相邻两个章的间距：间距越小边缘越平滑，但越慢。
 * 两个半径 r 的圆相距 s 时，它们“并集”的边缘比理想直线最多凹进去约 s²/(8r)，
 * 这里让这个误差不超过 0.04 像素（肉眼完全看不出，线条的平均粗细也几乎不变）。 */
static float stamp_spacing(float r)
{
    float rr = r < 0.5f ? 0.5f : r;
    float s = 2.0f * sqrtf(0.08f * rr);
    return s < 0.4f ? 0.4f : s;
}

/* Liang–Barsky 参数裁剪：求线段 p + t*d (t∈[0,1]) 落在矩形内的参数区间 */
static int clip_segment(float px, float py, float dx, float dy,
                        float xmin, float ymin, float xmax, float ymax, float *t0, float *t1)
{
    float a = 0.0f, b = 1.0f;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { px - xmin, xmax - px, py - ymin, ymax - py };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f)
                return 0;
        } else {
            float t = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (t > b) return 0;
                if (t > a) a = t;
            } else {
                if (t < a) return 0;
                if (t < b) b = t;
            }
        }
    }
    *t0 = a;
    *t1 = b;
    return 1;
}

void sk__stroke_line(sk__stroke *st, float x, float y, float r)
{
    if (!st->m)
        return;
    if (!(r > 0.0f)) r = 0.05f;
    if (!st->have_pt) {
        sk__stroke_move(st, x, y, r);
        return;
    }
    if (r > st->maxr) st->maxr = r;
    float x0 = st->px, y0 = st->py, r0 = st->pr;
    st->px = x;
    st->py = y;
    st->pr = r;
    float dx = x - x0, dy = y - y0;
    if (!(fabsf(dx) < 1.0e8f && fabsf(dy) < 1.0e8f))   /* 非法/极端坐标：不画 */
        return;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0e-4f) {
        stamp(st, x, y, r > r0 ? r : r0);
        return;
    }
    /* 只处理落在蒙版区域（外扩一个笔刷）里的那一段，屏幕外一公里长的线也不会变慢 */
    float pad = brush_reach(st->maxr) + 1.0f;
    float t0, t1;
    if (!clip_segment(x0, y0, dx, dy, (float)st->r.x - pad, (float)st->r.y - pad,
                      (float)(st->r.x + st->r.w) + pad, (float)(st->r.y + st->r.h) + pad, &t0, &t1))
        return;
    /* 章的位置固定在整条线段的等分点上（只由线段本身决定，和裁剪区无关），
     * 裁剪只是挑出其中落在区域附近的那些。所以只重绘一小块时，盖出来的覆盖率和
     * 整条重绘时逐位相同——局部刷新不会留下接缝。 */
    float rmin = r < r0 ? r : r0;
    float sp = stamp_spacing(rmin);
    float steps_f = len / sp;
    if (steps_f > 4.0e8f) steps_f = 4.0e8f;
    int steps = (int)steps_f + 1;               /* 向上取整（多一个章无妨） */
    /* k=0 是起点，已经在 move 或上一段的终点盖过章了；t0/t1 两边各多留一个章 */
    int k0 = (int)((double)t0 * steps);
    int k1 = (int)((double)t1 * steps) + 1;
    if (k0 < 1) k0 = 1;
    if (k1 > steps) k1 = steps;
    if (k0 > k1)
        return;
    /* 第 k 个章的位置 = 起点 + k × 步长，全部用定点整数算（起点和步长只由线段决定） */
    int64_t X0 = to_fx(x0), Y0 = to_fx(y0);
    int64_t DX = to_fx((double)dx / steps), DY = to_fx((double)dy / steps);
    int64_t R0 = to_fx((double)r0 * 8.0), DR = to_fx(((double)r - r0) * 8.0 / steps);
    for (int k = k0; k <= k1; k++) {
        int64_t rk = R0 + DR * k;
        stamp_fx(st, X0 + DX * k, Y0 + DY * k, (int)((rk + 32768) >> 16));
    }
}

void sk__stroke_bezier(sk__stroke *st, float x1, float y1, float x2, float y2,
                       float x3, float y3, float r)
{
    if (!st->m)
        return;
    if (!st->have_pt) {
        sk__stroke_move(st, x1, y1, r);
    }
    float x0 = st->px, y0 = st->py, r0 = st->pr;
    if (!(r > 0.0f)) r = 0.05f;
    /* 整条曲线都在控制点凸包里：凸包包围盒碰不到蒙版区域就整段跳过 */
    float bx0 = fminf(fminf(x0, x1), fminf(x2, x3)), bx1 = fmaxf(fmaxf(x0, x1), fmaxf(x2, x3));
    float by0 = fminf(fminf(y0, y1), fminf(y2, y3)), by1 = fmaxf(fmaxf(y0, y1), fmaxf(y2, y3));
    float pad = brush_reach(fmaxf(fmaxf(r, r0), st->maxr)) + 1.0f;
    if (!(bx1 + pad >= (float)st->r.x && bx0 - pad <= (float)(st->r.x + st->r.w) &&
          by1 + pad >= (float)st->r.y && by0 - pad <= (float)(st->r.y + st->r.h))) {
        if (r > st->maxr) st->maxr = r;
        st->px = x3;
        st->py = y3;
        st->pr = r;
        return;
    }
    /* 曲线细分成几段直线？按“弯曲程度”而不是长度来定（Wang 公式）：
     * 三次曲线用 n 段直线近似时，最大误差 ≤ (3/4)·M/n²，M 是控制点二阶差分的最大长度。
     * 让误差 ≤ 0.1 像素：几乎笔直的手绘线（大多数情况）只需要一两段，弯得厉害的才多分。 */
    float ax = x0 - 2.0f * x1 + x2, ay = y0 - 2.0f * y1 + y2;
    float bx = x1 - 2.0f * x2 + x3, by = y1 - 2.0f * y2 + y3;
    float M2 = fmaxf(ax * ax + ay * ay, bx * bx + by * by);
    float nf = sqrtf(7.5f * sqrtf(M2));                 /* sqrt(0.75·M / 0.1) */
    int n = nf < 1.0f ? 1 : nf > 255.0f ? 256 : (int)nf + 1;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, mt = 1.0f - t;
        float a = mt * mt * mt, b = 3.0f * mt * mt * t, cc = 3.0f * mt * t * t, d = t * t * t;
        sk__stroke_line(st, a * x0 + b * x1 + cc * x2 + d * x3,
                        a * y0 + b * y1 + cc * y2 + d * y3, r0 + (r - r0) * t);
    }
}

/* 铅笔/蜡笔颗粒：纸面凹凸不平，笔尖只蹭到凸起的地方。用 (x,y,seed) 的哈希
 * 模拟“这个点有没有蹭到”。两个均匀随机数相乘得到偏向小值的分布：大部分像素
 * 只淡一点点，少数像素明显变淡，看起来就是颗粒而不是均匀的灰。返回 0..256 的乘数。 */
static inline uint32_t grain_factor(int x, int y, uint32_t seed, int level)
{
    uint32_t h = sk__hash3((uint32_t)x, (uint32_t)y, seed);
    uint32_t p = (h & 0xFFu) * ((h >> 8) & 0xFFu);   /* 0..65025 */
    if (level >= 2)
        return 256u - (p >> 8) * 3u / 4u;           /* 最多淡到约 25% */
    return 256u - (p >> 10);                        /* 最多淡约 25%，平均很轻 */
}

void sk__stroke_end(sk__stroke *st, sk_color color, int grain, uint32_t seed)
{
    if (!st->m)
        return;
    sk_canvas *c = st->c;
    uint32_t ca = SK_A(color);
    for (int row = 0; row < st->r.h; row++) {
        int lo = st->lo[row], hi = st->hi[row];
        if (hi < lo)
            continue;
        uint8_t *m = st->m + (size_t)row * st->r.w;
        int y = st->r.y + row;
        uint32_t *p = px_at(c, st->r.x, y);
        for (int i = lo; i <= hi; i++) {
            uint32_t cov = m[i];
            if (!cov)
                continue;
            m[i] = 0;
            uint32_t a = (cov == 255) ? ca : sk__mul255(cov, ca);
            if (grain)
                a = (a * grain_factor(st->r.x + i - st->gx, y - st->gy, seed, grain)) >> 8;
            p[i] = sk__over(p[i], color, a);
        }
    }
    st->m = NULL;
    g_stroke_active = 0;
}

/* ======================================================================
 * 抗锯齿直线与圆
 * ====================================================================== */

/*
 * 精确的抗锯齿粗直线（“胶囊”形：一个矩形两头各加半个圆）。
 * 每个像素的覆盖率 = 像素中心到线段的距离换算出来的值（边缘 1 像素线性过渡）。
 * 胶囊是凸形，所以每一行和它相交的部分是一个区间：先解析地求出这个区间
 * （两个端点圆 + 中间的平行带），只在区间里逐像素计算，不浪费在包围盒的空白处。
 * 手绘线条走的是更便宜的“盖章”路径；这里给图表、钟表指针等需要精确直线的地方用。
 */
void sk_line_aa(sk_canvas *c, float x0, float y0, float x1, float y1, float width, sk_color color)
{
    if (!clip_ok(c) || SK_A(color) == 0)
        return;
    if (!(fabsf(x0) < SK_COORD_LIMIT && fabsf(y0) < SK_COORD_LIMIT &&
          fabsf(x1) < SK_COORD_LIMIT && fabsf(y1) < SK_COORD_LIMIT))
        return;
    if (!(width >= 0.5f)) width = 0.5f;
    if (width > 1024.0f) width = 1024.0f;
    float r = width * 0.5f;
    float reach = brush_reach(r);                 /* 距离 ≥ reach 的像素覆盖率为 0 */
    float thin = r < 0.5f ? 2.0f * r : 1.0f;
    sk_rect clip = c->clip;
    /* 先把线段裁到“裁剪区外扩 reach+1”之内：坐标变小，浮点更准；被裁掉的端点帽本来就看不见 */
    float pad = reach + 1.0f;
    float t0, t1, dx = x1 - x0, dy = y1 - y0;
    if (!clip_segment(x0, y0, dx, dy, (float)clip.x - pad, (float)clip.y - pad,
                      (float)(clip.x + clip.w) + pad, (float)(clip.y + clip.h) + pad, &t0, &t1))
        return;
    float ax = x0 + dx * t0, ay = y0 + dy * t0, bx = x0 + dx * t1, by = y0 + dy * t1;
    dx = bx - ax;
    dy = by - ay;
    float L2 = dx * dx + dy * dy, L = sqrtf(L2);
    uint32_t ca = SK_A(color);
    int ya = imax(floor_i(fminf(ay, by) - reach), clip.y);
    int yb = imin(ceil_i(fmaxf(ay, by) + reach) + 1, clip.y + clip.h);
    for (int y = ya; y < yb; y++) {
        float py = (float)y + 0.5f;
        float lo = 1e30f, hi = -1e30f;
        /* 两个端点圆 */
        float ex[2] = { ax, bx }, ey[2] = { ay, by };
        for (int k = 0; k < 2; k++) {
            float h2 = reach * reach - (py - ey[k]) * (py - ey[k]);
            if (h2 > 0.0f) {
                float hw = sqrtf(h2);
                if (ex[k] - hw < lo) lo = ex[k] - hw;
                if (ex[k] + hw > hi) hi = ex[k] + hw;
            }
        }
        /* 中间的带：|叉积| < reach·L 且 0 ≤ 点积 ≤ L²，两个条件对 px 都是线性的 */
        if (L > 1e-6f) {
            float blo = -1e30f, bhi = 1e30f;
            float c1 = -ax * dy - (py - ay) * dx;     /* cross(px) = dy*px + c1 */
            float c2 = -ax * dx + (py - ay) * dy;     /* dot(px)   = dx*px + c2 */
            int ok = 1;
            if (fabsf(dy) > 1e-9f) {
                float u = (-reach * L - c1) / dy, v = (reach * L - c1) / dy;
                blo = fmaxf(blo, fminf(u, v));
                bhi = fminf(bhi, fmaxf(u, v));
            } else if (!(fabsf(c1) < reach * L)) {
                ok = 0;
            }
            if (fabsf(dx) > 1e-9f) {
                float u = -c2 / dx, v = (L2 - c2) / dx;
                blo = fmaxf(blo, fminf(u, v));
                bhi = fminf(bhi, fmaxf(u, v));
            } else if (!(c2 >= 0.0f && c2 <= L2)) {
                ok = 0;
            }
            if (ok && bhi > blo) {
                if (blo < lo) lo = blo;
                if (bhi > hi) hi = bhi;
            }
        }
        if (!(hi > lo))
            continue;
        int xa = imax(ceil_i(lo - 0.5f), clip.x), xb = imin(floor_i(hi - 0.5f) + 1, clip.x + clip.w);
        uint32_t *row = px_at(c, 0, y);
        float qy = py - ay;
        for (int x = xa; x < xb; x++) {
            float qx = (float)x + 0.5f - ax;
            float t = L2 > 0.0f ? (qx * dx + qy * dy) / L2 : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float ux = qx - dx * t, uy = qy - dy * t;
            float d = sqrtf(ux * ux + uy * uy);
            float cov = (r < 0.5f ? 1.0f - d : r + 0.5f - d) * thin;
            if (cov <= 0.0f)
                continue;
            if (cov >= 1.0f) {
                row[x] = sk__over(row[x], color, ca);
            } else {
                row[x] = sk__over(row[x], color, sk__mul255((uint32_t)(cov * 255.0f + 0.5f), ca));
            }
        }
    }
}

/* 圆和圆环都按“到圆心的距离”算覆盖率；每行先解析地求出可能非零的列区间，
 * 圆内部完全覆盖的那一段直接整段填充，只有边缘一两个像素才开平方。 */
void sk_fill_circle_aa(sk_canvas *c, float cx, float cy, float r, sk_color color)
{
    if (!clip_ok(c) || SK_A(color) == 0 || !(r > 0.0f) || !(fabsf(cx) < SK_COORD_LIMIT) ||
        !(fabsf(cy) < SK_COORD_LIMIT))
        return;
    if (r > SK_COORD_LIMIT) r = SK_COORD_LIMIT;
    uint32_t ca = SK_A(color);
    float ro = r + 0.5f;
    float ri = r - 0.5f;
    float thin = r < 0.5f ? 2.0f * r : 1.0f;      /* 小于 1 像素的点按面积变淡 */
    sk_rect clip = c->clip;
    int y0 = imax(floor_i(cy - ro), clip.y), y1 = imin(ceil_i(cy + ro), clip.y + clip.h);
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float h2 = ro * ro - dy * dy;
        if (h2 <= 0.0f)
            continue;
        float xo = sqrtf(h2);
        int xa = imax(floor_i(cx - xo), clip.x), xb = imin(ceil_i(cx + xo), clip.x + clip.w);
        if (xa >= xb)
            continue;
        /* 完全覆盖的内段 [fa, fb) */
        int fa = xb, fb = xb;
        if (ri > 0.0f && ri * ri - dy * dy > 0.0f && thin >= 1.0f) {
            float xi = sqrtf(ri * ri - dy * dy);
            fa = imax(ceil_i(cx - xi - 0.5f), xa);
            fb = imin(floor_i(cx + xi - 0.5f) + 1, xb);
            if (fb <= fa) { fa = xb; fb = xb; }
        }
        uint32_t *row = px_at(c, 0, y);
        for (int x = xa; x < xb; x++) {
            if (x == fa) {
                if (ca == 255)
                    fill32(row + fa, color, fb - fa);
                else
                    blend_row(row + fa, fb - fa, color, ca);
                x = fb - 1;
                continue;
            }
            float dx = (float)x + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = (r < 0.5f ? 1.0f - d : ro - d) * thin;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f) cov = 1.0f;
            uint32_t a = sk__mul255((uint32_t)(cov * 255.0f + 0.5f), ca);
            row[x] = sk__over(row[x], color, a);
        }
    }
}

void sk_circle_aa(sk_canvas *c, float cx, float cy, float r, float width, sk_color color)
{
    if (!clip_ok(c) || SK_A(color) == 0 || !(r > 0.0f) || !(fabsf(cx) < SK_COORD_LIMIT) ||
        !(fabsf(cy) < SK_COORD_LIMIT))
        return;
    if (!(width > 0.0f)) width = 1.0f;
    if (r > SK_COORD_LIMIT) r = SK_COORD_LIMIT;
    float hw = width * 0.5f;
    float thin = width < 1.0f ? width : 1.0f;
    float hw_eff = width < 1.0f ? 0.5f : hw;      /* 细线：1px 的轮廓乘以粗细 */
    float ro = r + hw_eff + 0.5f;
    float ri = r - hw_eff - 0.5f;
    uint32_t ca = SK_A(color);
    sk_rect clip = c->clip;
    int y0 = imax(floor_i(cy - ro), clip.y), y1 = imin(ceil_i(cy + ro), clip.y + clip.h);
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float h2 = ro * ro - dy * dy;
        if (h2 <= 0.0f)
            continue;
        float xo = sqrtf(h2);
        int xa = imax(floor_i(cx - xo), clip.x), xb = imin(ceil_i(cx + xo), clip.x + clip.w);
        /* 内圆挖掉的部分 [ha, hb) 不用算 */
        int ha = xb, hb = xb;
        if (ri > 0.0f && ri * ri - dy * dy > 0.0f) {
            float xi = sqrtf(ri * ri - dy * dy);
            ha = imax(ceil_i(cx - xi - 0.5f), xa);
            hb = imin(floor_i(cx + xi - 0.5f) + 1, xb);
            if (hb <= ha) { ha = xb; hb = xb; }
        }
        uint32_t *row = px_at(c, 0, y);
        for (int x = xa; x < xb; x++) {
            if (x == ha) {
                x = hb - 1;
                continue;
            }
            float dx = (float)x + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = (hw_eff + 0.5f - fabsf(d - r)) * thin;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f) cov = 1.0f;
            uint32_t a = sk__mul255((uint32_t)(cov * 255.0f + 0.5f), ca);
            row[x] = sk__over(row[x], color, a);
        }
    }
}

/* ======================================================================
 * 多边形填充：扫描线 + 偶奇规则 + 纵向 4 倍超采样
 *
 * 扫描线算法：对每一行，求出这条水平线和多边形所有边的交点，从左到右排序，
 * 第 1-2 个交点之间在内部、第 3-4 个之间在内部……（偶奇规则）。
 * 抗锯齿：每个像素行取 4 条子扫描线，每条子扫描线贡献 0..64 的覆盖率，
 * 水平方向在区间两端按小数部分精确计算，4 条加起来就是 0..256 的覆盖率。
 * ====================================================================== */

typedef struct {
    float ytop, ybot;   /* 这条边覆盖 [ytop, ybot) */
    float x, dxdy;      /* ytop 处的 x，以及每下降 1 像素 x 的变化 */
} poly_edge;

static int edge_cmp(const void *a, const void *b)
{
    float ya = ((const poly_edge *)a)->ytop, yb = ((const poly_edge *)b)->ytop;
    return ya < yb ? -1 : ya > yb ? 1 : 0;
}

static inline void acc_span(uint16_t *acc, int w, float fa, float fb, int *lo, int *hi)
{
    if (fa < 0.0f) fa = 0.0f;
    if (fb > (float)w) fb = (float)w;
    if (!(fb > fa))
        return;
    int ia = (int)fa, ib = (int)fb;
    if (ia >= w)
        return;
    if (ia == ib) {
        acc[ia] += (uint16_t)((fb - fa) * 64.0f + 0.5f);
    } else {
        acc[ia] += (uint16_t)(((float)(ia + 1) - fa) * 64.0f + 0.5f);
        for (int k = ia + 1; k < ib; k++)
            acc[k] += 64;
        if (ib < w)
            acc[ib] += (uint16_t)((fb - (float)ib) * 64.0f + 0.5f);
    }
    if (ia < *lo) *lo = ia;
    int last = ib < w ? ib : w - 1;
    if (last > *hi) *hi = last;
}

void sk_fill_polygon(sk_canvas *c, const sk_pointf *pts, int n, sk_color color)
{
    if (!clip_ok(c) || !pts || n < 3)
        return;
    uint32_t ca = SK_A(color);
    if (!ca)
        return;
    float minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
    for (int i = 0; i < n; i++) {
        float x = pts[i].x, y = pts[i].y;
        if (!(fabsf(x) < SK_COORD_LIMIT && fabsf(y) < SK_COORD_LIMIT))
            return;                       /* NaN / 无穷大 / 离谱的坐标 */
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    sk_rect clip = c->clip;
    int y0 = imax(floor_i(miny), clip.y), y1 = imin(ceil_i(maxy), clip.y + clip.h);
    int x0 = imax(floor_i(minx), clip.x), x1 = imin(ceil_i(maxx) + 1, clip.x + clip.w);
    if (y0 >= y1 || x0 >= x1)
        return;
    int w = x1 - x0;

    /* 一块临时内存放：边表 n 项、活动边 n 个、交点 n 个、行覆盖率 w+1 个 */
    size_t sz_e = sizeof(poly_edge) * (size_t)n;
    size_t sz_a = sizeof(int) * (size_t)n;
    size_t sz_x = sizeof(float) * (size_t)n;
    size_t sz_acc = sizeof(uint16_t) * ((size_t)w + 1);
    char *mem = sk__scratch(SK__SCRATCH_POLY, sz_e + sz_a + sz_x + sz_acc + 16);
    if (!mem)
        return;
    poly_edge *edges = (poly_edge *)mem;
    int *active = (int *)(mem + sz_e);
    float *xs = (float *)(mem + sz_e + sz_a);
    uint16_t *acc = (uint16_t *)(mem + sz_e + sz_a + sz_x);
    memset(acc, 0, sz_acc);

    int ne = 0;
    for (int i = 0; i < n; i++) {
        sk_pointf a = pts[i], b = pts[(i + 1) % n];
        if (a.y == b.y)
            continue;                     /* 水平边对扫描线没有贡献 */
        if (a.y > b.y) { sk_pointf t = a; a = b; b = t; }
        if (b.y <= (float)y0 || a.y >= (float)y1)
            continue;                     /* 整条边都在要画的行范围之外 */
        edges[ne].ytop = a.y;
        edges[ne].ybot = b.y;
        edges[ne].dxdy = (b.x - a.x) / (b.y - a.y);
        edges[ne].x = a.x;
        ne++;
    }
    if (ne < 2)
        return;
    qsort(edges, (size_t)ne, sizeof(poly_edge), edge_cmp);

    int next = 0, nact = 0;
    for (int y = y0; y < y1; y++) {
        int lo = INT_MAX, hi = -1;
        for (int s = 0; s < 4; s++) {
            float sy = (float)y + ((float)s + 0.5f) * 0.25f;
            while (next < ne && edges[next].ytop <= sy)
                active[nact++] = next++;
            int nx = 0, keep = 0;
            for (int k = 0; k < nact; k++) {
                const poly_edge *e = &edges[active[k]];
                if (e->ybot <= sy)
                    continue;             /* 这条边已经结束，移出活动表 */
                active[keep++] = active[k];
                float xv = e->x + (sy - e->ytop) * e->dxdy;
                /* 插入排序：活动边通常只有两三条 */
                int j = nx++;
                while (j > 0 && xs[j - 1] > xv) {
                    xs[j] = xs[j - 1];
                    j--;
                }
                xs[j] = xv;
            }
            nact = keep;
            for (int k = 0; k + 1 < nx; k += 2)
                acc_span(acc, w, xs[k] - (float)x0, xs[k + 1] - (float)x0, &lo, &hi);
        }
        if (hi < lo)
            continue;
        uint32_t *row = px_at(c, x0, y);
        for (int k = lo; k <= hi; k++) {
            uint32_t v = acc[k];
            if (!v)
                continue;
            acc[k] = 0;
            if (v >= 255) {
                row[k] = (ca == 255) ? (color | 0xFF000000u) : sk__over(row[k], color, ca);
            } else {
                row[k] = sk__over(row[k], color, sk__mul255(v, ca));
            }
        }
    }
}

/* ======================================================================
 * 位块传送（blit）
 * ====================================================================== */

/* 求出源矩形 sr 和目标位置 (dx,dy) 经过双方裁剪后真正要拷贝的部分。
 * 源矩形只受源画布尺寸限制（源的 clip 不影响读取），目标受目标 clip 限制。 */
static int blit_prepare(const sk_canvas *dst, int *dx, int *dy, const sk_canvas *src, sk_rect *sr)
{
    if (!clip_ok(dst) || !src || !src->pixels)
        return 0;
    sk_rect s = sk_rect_intersect(*sr, sk_rect_make(0, 0, src->w, src->h));
    if (sk_rect_empty(s))
        return 0;
    int64_t ox = (int64_t)*dx + (s.x - sr->x), oy = (int64_t)*dy + (s.y - sr->y);
    if (ox < INT_MIN || ox > INT_MAX || oy < INT_MIN || oy > INT_MAX)
        return 0;
    sk_rect d = sk_rect_intersect(sk_rect_make((int)ox, (int)oy, s.w, s.h), dst->clip);
    if (sk_rect_empty(d))
        return 0;
    s.x += d.x - (int)ox;
    s.y += d.y - (int)oy;
    s.w = d.w;
    s.h = d.h;
    *dx = d.x;
    *dy = d.y;
    *sr = s;
    return 1;
}

/* 源和目标可能是同一块内存（比如终端滚屏：把画面整体往上挪）。
 * 这时要选对拷贝方向：目标在源后面就从后往前拷，否则会先把还没读的源像素覆盖掉。 */
static int rows_backward(const sk_canvas *dst, int dx, int dy, const sk_canvas *src, sk_rect sr)
{
    const uint32_t *d = px_at(dst, dx, dy);
    const uint32_t *s = px_at(src, sr.x, sr.y);
    return d > s;
}

void sk_blit(sk_canvas *dst, int dx, int dy, const sk_canvas *src, sk_rect sr)
{
    if (!blit_prepare(dst, &dx, &dy, src, &sr))
        return;
    size_t bytes = (size_t)sr.w * 4;
    if (rows_backward(dst, dx, dy, src, sr)) {
        for (int y = sr.h - 1; y >= 0; y--)
            memmove(px_at(dst, dx, dy + y), px_at(src, sr.x, sr.y + y), bytes);
    } else {
        for (int y = 0; y < sr.h; y++)
            memmove(px_at(dst, dx, dy + y), px_at(src, sr.x, sr.y + y), bytes);
    }
}

/* keyed / alpha 两种逐像素拷贝共用的循环：mode 0=抠像 1=alpha 混合 */
static void blit_px(sk_canvas *dst, int dx, int dy, const sk_canvas *src, sk_rect sr, int mode)
{
    if (!blit_prepare(dst, &dx, &dy, src, &sr))
        return;
    int back = rows_backward(dst, dx, dy, src, sr);
    for (int yy = 0; yy < sr.h; yy++) {
        int y = back ? sr.h - 1 - yy : yy;
        uint32_t *d = px_at(dst, dx, dy + y);
        const uint32_t *s = px_at(src, sr.x, sr.y + y);
        for (int xx = 0; xx < sr.w; xx++) {
            int x = back ? sr.w - 1 - xx : xx;
            uint32_t v = s[x];
            uint32_t a = v >> 24;
            if (!a)
                continue;
            d[x] = (mode == 0) ? v : sk__over(d[x], v, a);
        }
    }
}

void sk_blit_keyed(sk_canvas *dst, int dx, int dy, const sk_canvas *src, sk_rect sr)
{
    blit_px(dst, dx, dy, src, sr, 0);
}

void sk_blit_alpha(sk_canvas *dst, int dx, int dy, const sk_canvas *src, sk_rect sr)
{
    blit_px(dst, dx, dy, src, sr, 1);
}

void sk_draw_mask(sk_canvas *c, int x, int y, const uint8_t *mask, int mw, int mh,
                  int mask_stride, sk_color color)
{
    if (!clip_ok(c) || !mask || mw <= 0 || mh <= 0 || mask_stride < mw)
        return;
    uint32_t ca = SK_A(color);
    if (!ca)
        return;
    sk_rect d = sk_rect_intersect(sk_rect_make(x, y, mw, mh), c->clip);
    if (sk_rect_empty(d))
        return;
    for (int yy = d.y; yy < d.y + d.h; yy++) {
        const uint8_t *m = mask + (size_t)(yy - y) * mask_stride + (d.x - x);
        uint32_t *p = px_at(c, d.x, yy);
        for (int i = 0; i < d.w; i++) {
            uint32_t v = m[i];
            if (!v)
                continue;
            if (v == 255 && ca == 255)
                p[i] = color;
            else
                p[i] = sk__over(p[i], color, v == 255 ? ca : sk__mul255(v, ca));
        }
    }
}

/* 最近邻缩放：目标像素 (x,y) 取源里“同一相对位置”的像素。
 * 用 16.16 定点数步进，避免每个像素做除法。 */
void sk_blit_scaled(sk_canvas *dst, sk_rect dr, const sk_canvas *src, sk_rect sr)
{
    if (!clip_ok(dst) || !src || !src->pixels || sk_rect_empty(dr) || sk_rect_empty(sr))
        return;
    sk_rect d = sk_rect_intersect(dr, dst->clip);
    if (sk_rect_empty(d))
        return;
    int64_t stepx = ((int64_t)sr.w << 16) / dr.w;
    int64_t stepy = ((int64_t)sr.h << 16) / dr.h;
    for (int y = d.y; y < d.y + d.h; y++) {
        int64_t sy = sr.y + ((((int64_t)(y - dr.y) * stepy) + stepy / 2) >> 16);
        if (sy < 0 || sy >= src->h)
            continue;
        const uint32_t *srow = px_at(src, 0, (int)sy);
        uint32_t *drow = px_at(dst, 0, y);
        int64_t fx = (int64_t)(d.x - dr.x) * stepx + stepx / 2;
        for (int x = d.x; x < d.x + d.w; x++, fx += stepx) {
            int64_t sx = sr.x + (fx >> 16);
            if (sx >= 0 && sx < src->w)
                drow[x] = srow[sx];
        }
    }
}

/* ======================================================================
 * PPM 图片（P6：一个文本头 + 原始 RGB 字节）
 * 选它是因为格式简单到几十行就能读写，调试和“画板保存”都够用。
 * ====================================================================== */

static int ppm_token(FILE *f, int *out)
{
    int ch;
    for (;;) {
        ch = fgetc(f);
        if (ch == EOF)
            return -1;
        if (ch == '#') {                      /* 注释一直到行尾 */
            while ((ch = fgetc(f)) != '\n' && ch != EOF)
                ;
            continue;
        }
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            break;
    }
    if (ch < '0' || ch > '9')
        return -1;
    long v = 0;
    while (ch >= '0' && ch <= '9') {
        v = v * 10 + (ch - '0');
        if (v > 65535)
            return -1;
        ch = fgetc(f);
    }
    /* 数字后面必须正好跟一个空白；最后一个数（maxval）后面紧接着就是像素数据 */
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
        return -1;
    *out = (int)v;
    return 0;
}

sk_canvas *sk_image_load_ppm(const char *path)
{
    if (!path)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    sk_canvas *c = NULL;
    unsigned char *row = NULL;
    int w, h, maxval;
    if (fgetc(f) != 'P' || fgetc(f) != '6')
        goto out;
    if (ppm_token(f, &w) || ppm_token(f, &h) || ppm_token(f, &maxval))
        goto out;
    if (w <= 0 || h <= 0 || maxval <= 0 || maxval > 65535)
        goto out;
    c = sk_canvas_new(w, h);
    if (!c)
        goto out;
    int bps = maxval > 255 ? 2 : 1;           /* 每个分量的字节数 */
    size_t rowbytes = (size_t)w * 3 * bps;
    row = malloc(rowbytes);
    if (!row) {
        sk_canvas_free(c);
        c = NULL;
        goto out;
    }
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, rowbytes, f) != rowbytes) {
            sk_canvas_free(c);
            c = NULL;
            goto out;
        }
        uint32_t *p = px_at(c, 0, y);
        for (int x = 0; x < w; x++) {
            uint32_t rgb[3];
            for (int k = 0; k < 3; k++) {
                uint32_t v = bps == 2 ? ((uint32_t)row[(x * 3 + k) * 2] << 8) | row[(x * 3 + k) * 2 + 1]
                                      : row[x * 3 + k];
                if ((int)v > maxval) v = (uint32_t)maxval;
                rgb[k] = maxval == 255 ? v : (v * 255u + (uint32_t)maxval / 2) / (uint32_t)maxval;
            }
            p[x] = SK_RGB(rgb[0], rgb[1], rgb[2]);
        }
    }
out:
    free(row);
    fclose(f);
    return c;
}

int sk_image_save_ppm(const sk_canvas *c, const char *path)
{
    if (!c || !c->pixels || c->w <= 0 || c->h <= 0 || !path)
        return -1;
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fprintf(f, "P6\n%d %d\n255\n", c->w, c->h) > 0;
    unsigned char *row = malloc((size_t)c->w * 3);
    if (!row)
        ok = 0;
    for (int y = 0; ok && y < c->h; y++) {
        const uint32_t *p = px_at(c, 0, y);
        for (int x = 0; x < c->w; x++) {
            row[x * 3 + 0] = (unsigned char)SK_R(p[x]);
            row[x * 3 + 1] = (unsigned char)SK_G(p[x]);
            row[x * 3 + 2] = (unsigned char)SK_B(p[x]);
        }
        if (fwrite(row, 1, (size_t)c->w * 3, f) != (size_t)c->w * 3)
            ok = 0;
    }
    free(row);
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}
