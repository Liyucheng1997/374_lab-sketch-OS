/*
 * rough.c — 手绘风格图元（SketchOS 外观的核心）
 *
 * 为什么手绘线看起来像“手画的”？人手画直线时：起笔落笔的位置有偏差，手腕的弧线让线
 * 中间微微鼓起（bowing），画完一遍觉得不够又顺着描一遍（double stroke），画矩形时
 * 每条边都画得稍长一点、在角上交叉“出头”。这里的算法照着 rough.js（MIT 许可）的思路，
 * 用随机数把这些“不完美”模拟出来：
 *   直线 = 一条三次贝塞尔曲线：两端加随机偏移，两个控制点在 20%~40% / 40%~80% 处，
 *          再整体垂直于线的方向偏一点（弯曲）。随机幅度和 roughness、线长成比例。
 *   椭圆 = 沿椭圆取若干点、每点加随机扰动，从随机角度起笔，最后多画一小段重叠，
 *          用 Catmull-Rom 样条把这些点平滑地串起来。
 *   填充 = 一组平行斜线（hachure）和图形求交，每条斜线本身也是一条略微抖动的手绘线。
 *
 * 确定性：随机数只来自 style.seed（以及排线的行号），同一个 seed 永远画出同一个图形，
 * 所以界面局部重绘时不会“抖”。轮廓和填充用两条独立的随机数流——给按钮加上悬停排线，
 * 它的轮廓线一像素都不会动。
 *
 * 所有线条最终都交给 gfx.c 的“笔触累积器”（圆形笔刷盖章 + 覆盖率蒙版）光栅化。
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sketch/rough.h"
#include "sketch/theme.h"
#include "gfx_internal.h"

#define SK_PI 3.14159265358979f
#define ROUGH_MAXOFF 2.0f     /* rough.js 的 maxRandomnessOffset：随机偏移的基本幅度（像素） */
#define COORD_OK(v) (fabsf(v) < 1.0e7f)   /* 同时排除 NaN、无穷大和离谱的坐标 */

/* ======================================================================
 * 确定性随机数
 * ====================================================================== */

/* murmur3 的收尾混合函数：把相近的输入（1、2、3……）打散成毫不相干的输出 */
static inline uint32_t fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

void sk_rng_seed(sk_rng *r, uint32_t seed)
{
    /* 先打散：seed 1 和 seed 2 不应该得到“错开一位”的同一个序列 */
    r->s = fmix32(seed ^ 0x5BD1E995u);
}

/* mulberry32：状态只有 32 位，任何状态（包括 0）都能正常工作，质量足够画画用 */
uint32_t sk_rng_u32(sk_rng *r)
{
    uint32_t z = (r->s += 0x6D2B79F5u);
    z = (z ^ (z >> 15)) * (z | 1u);
    z ^= z + (z ^ (z >> 7)) * (z | 61u);
    return z ^ (z >> 14);
}

float sk_rng_float(sk_rng *r)
{
    return (float)(sk_rng_u32(r) >> 8) * (1.0f / 16777216.0f);
}

float sk_rng_range(sk_rng *r, float lo, float hi)
{
    return lo + (hi - lo) * sk_rng_float(r);
}

uint32_t sk_hash_str(const char *s)
{
    uint32_t h = 2166136261u;                 /* FNV-1a */
    if (!s)
        return h;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

uint32_t sk_hash_u32(uint32_t a, uint32_t b)
{
    return fmix32(a * 0x9E3779B1u ^ fmix32(b + 0x7F4A7C15u));
}

/* ======================================================================
 * 样式
 * ====================================================================== */

void sk_rough_default(sk_rough_style *s)
{
    if (!s)
        return;
    s->roughness = 1.0f;
    s->bowing = 1.0f;
    s->stroke_width = 1.6f;
    s->stroke = SK_INK;
    s->double_stroke = 1;
    s->fill_style = SK_FILL_NONE;
    s->fill = SK_BLUE;
    s->hachure_angle = -41.0f;
    s->hachure_gap = 6.0f;
    s->fill_weight = 1.2f;
    s->seed = 1;
}

/* 样式参数的合法化：调用者可能传 0、负数甚至 NaN */
static float st_rough(const sk_rough_style *s)
{
    float r = s->roughness;
    return (r >= 0.0f && r <= 10.0f) ? r : (r > 10.0f ? 10.0f : 0.0f);
}

static float st_bowing(const sk_rough_style *s)
{
    float b = s->bowing;
    return (b >= -10.0f && b <= 10.0f) ? b : 0.0f;
}

static int st_has_stroke(const sk_rough_style *s)
{
    return s->stroke_width > 0.0f && SK_A(s->stroke) != 0;
}

static float st_radius(const sk_rough_style *s)
{
    float w = s->stroke_width;
    if (w > 200.0f) w = 200.0f;
    return w * 0.5f;
}

static float st_gap(const sk_rough_style *s)
{
    float g = s->hachure_gap;
    if (!(g >= 1.0f)) {
        float fw = s->fill_weight > 0.0f ? s->fill_weight : 1.2f;
        g = fw * 4.0f;
        if (!(g >= 1.0f)) g = 6.0f;
    }
    return g > 500.0f ? 500.0f : g;
}

static float st_fill_weight(const sk_rough_style *s)
{
    float w = s->fill_weight;
    if (!(w > 0.0f)) w = 1.2f;
    return w > 100.0f ? 100.0f : w;
}

/* 填充用独立的随机数流（见文件开头） */
static uint32_t fill_seed(const sk_rough_style *s)
{
    return sk_hash_u32(s->seed, 0xF1110001u);
}

/* ======================================================================
 * 路径：一笔由若干段三次贝塞尔曲线组成
 * ====================================================================== */

typedef struct { sk_pointf p0, p1, p2, p3; } bez_t;
typedef struct { bez_t b; int move; } seg_t;
typedef struct { seg_t *s; int n; int ok; } path_t;

static inline sk_pointf P(float x, float y)
{
    sk_pointf p = { x, y };
    return p;
}

static void path_begin(path_t *p)
{
    p->s = NULL;
    p->n = 0;
    p->ok = 1;
}

static void path_add(path_t *p, sk_pointf p0, sk_pointf p1, sk_pointf p2, sk_pointf p3, int move)
{
    if (!p->ok)
        return;
    seg_t *s = sk__scratch(SK__SCRATCH_PATH, sizeof(seg_t) * ((size_t)p->n + 1));
    if (!s) {
        p->ok = 0;
        return;
    }
    p->s = s;
    s[p->n].b.p0 = p0;
    s[p->n].b.p1 = p1;
    s[p->n].b.p2 = p2;
    s[p->n].b.p3 = p3;
    s[p->n].move = move || p->n == 0;
    p->n++;
}

/* 把一条路径作为“一笔”画出来：整条路径先盖到蒙版上，最后一次混合 */
static void path_stroke(sk_canvas *c, const path_t *p, float r, sk_color color, int grain, uint32_t gseed)
{
    if (!p->ok || p->n == 0 || !(r > 0.0f) || SK_A(color) == 0)
        return;
    float x0 = p->s[0].b.p0.x, x1 = x0, y0 = p->s[0].b.p0.y, y1 = y0;
    for (int i = 0; i < p->n; i++) {
        const sk_pointf *q = &p->s[i].b.p0;
        for (int k = 0; k < 4; k++) {
            if (!COORD_OK(q[k].x) || !COORD_OK(q[k].y))
                return;
            if (q[k].x < x0) x0 = q[k].x;
            if (q[k].x > x1) x1 = q[k].x;
            if (q[k].y < y0) y0 = q[k].y;
            if (q[k].y > y1) y1 = q[k].y;
        }
    }
    float pad = r + 2.0f;
    sk__stroke st;
    if (!sk__stroke_begin(&st, c, x0 - pad, y0 - pad, x1 + pad, y1 + pad))
        return;
    for (int i = 0; i < p->n; i++) {
        const bez_t *b = &p->s[i].b;
        if (p->s[i].move)
            sk__stroke_move(&st, b->p0.x, b->p0.y, r);
        sk__stroke_bezier(&st, b->p1.x, b->p1.y, b->p2.x, b->p2.y, b->p3.x, b->p3.y, r);
    }
    sk__stroke_end(&st, color, grain, gseed);
}

/* Catmull-Rom 样条 → 贝塞尔（rough.js 的 _curve）。
 * 样条穿过 pt[1] .. pt[len-2]，pt[0] 和 pt[len-1] 只用来决定两端的切线方向。
 * 每段 pt[i]→pt[i+1] 的两个贝塞尔控制点 = 端点 ± (相邻两点连线)/6。 */
static void path_curve(path_t *p, const sk_pointf *pt, int len)
{
    if (len > 3) {
        for (int i = 1; i + 2 < len; i++) {
            sk_pointf b1 = P(pt[i].x + (pt[i + 1].x - pt[i - 1].x) / 6.0f,
                             pt[i].y + (pt[i + 1].y - pt[i - 1].y) / 6.0f);
            sk_pointf b2 = P(pt[i + 1].x - (pt[i + 2].x - pt[i].x) / 6.0f,
                             pt[i + 1].y - (pt[i + 2].y - pt[i].y) / 6.0f);
            path_add(p, pt[i], b1, b2, pt[i + 1], i == 1);
        }
    } else if (len == 3) {
        path_add(p, pt[1], pt[1], pt[2], pt[2], 1);
    } else if (len == 2) {
        path_add(p, pt[0], pt[0], pt[1], pt[1], 1);
    }
}

/* ======================================================================
 * 手绘直线（rough.js 的 _line）
 * ====================================================================== */

/* [-x, x] 里的均匀随机数，再乘以 roughness×增益 */
static inline float roff(sk_rng *g, float x, float rg)
{
    return rg * (sk_rng_float(g) * 2.0f * x - x);
}

/* 长线条的随机幅度要相对收敛，否则 800 像素的窗口边会像波浪 */
static inline float length_gain(float len)
{
    if (len < 200.0f) return 1.0f;
    if (len > 500.0f) return 0.4f;
    return -0.0016668f * len + 1.233334f;
}

/* overlay=0 是第一遍（随机幅度满额），overlay=1 是第二遍（幅度减半，贴着第一遍描） */
static void rough_line_bez(sk_rng *g, float rough, float bowing, float x1, float y1,
                           float x2, float y2, int overlay, bez_t *b)
{
    float dx = x2 - x1, dy = y2 - y1;
    float len2 = dx * dx + dy * dy, len = sqrtf(len2);
    float rg = rough * length_gain(len);
    float offset = ROUGH_MAXOFF;
    if (offset * offset * 100.0f > len2)
        offset = len / 10.0f;                 /* 短线的抖动按长度缩小 */
    float R = overlay ? offset * 0.5f : offset;
    float diverge = 0.2f + sk_rng_float(g) * rough * 0.2f;
    float mdx = bowing * ROUGH_MAXOFF * dy / 200.0f;
    float mdy = bowing * ROUGH_MAXOFF * -dx / 200.0f;
    mdx = roff(g, mdx, rg);
    mdy = roff(g, mdy, rg);
    b->p0.x = x1 + roff(g, R, rg);
    b->p0.y = y1 + roff(g, R, rg);
    b->p1.x = mdx + x1 + dx * diverge + roff(g, R, rg);
    b->p1.y = mdy + y1 + dy * diverge + roff(g, R, rg);
    b->p2.x = mdx + x1 + 2.0f * dx * diverge + roff(g, R, rg);
    b->p2.y = mdy + y1 + 2.0f * dy * diverge + roff(g, R, rg);
    b->p3.x = x2 + roff(g, R, rg);
    b->p3.y = y2 + roff(g, R, rg);
}

/* 端点固定、只让中间弯一点的“软”曲线：用在需要首尾相接的地方（圆角矩形的边、折线填充） */
static void soft_bez(sk_rng *g, float rough, float bowing, sk_pointf a, sk_pointf b, bez_t *o)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    float rg = rough * length_gain(len);
    float off = len / 10.0f < ROUGH_MAXOFF ? len / 10.0f : ROUGH_MAXOFF;
    float bow = roff(g, bowing * ROUGH_MAXOFF * len / 200.0f, rg);
    float px = 0.0f, py = 0.0f;
    if (len > 1.0e-4f) {
        px = -dy / len;
        py = dx / len;
    }
    float t1 = 1.0f / 3.0f + roff(g, 0.06f, rough), t2 = 2.0f / 3.0f + roff(g, 0.06f, rough);
    o->p0 = a;
    o->p1 = P(a.x + dx * t1 + px * bow + roff(g, off * 0.5f, rg), a.y + dy * t1 + py * bow + roff(g, off * 0.5f, rg));
    o->p2 = P(a.x + dx * t2 + px * bow + roff(g, off * 0.5f, rg), a.y + dy * t2 + py * bow + roff(g, off * 0.5f, rg));
    o->p3 = b;
}

static void stroke_bez(sk_canvas *c, const bez_t *b, float r, sk_color color)
{
    path_t p;
    path_begin(&p);
    path_add(&p, b->p0, b->p1, b->p2, b->p3, 1);
    path_stroke(c, &p, r, color, 0, 0);
}

/* 一条完整的手绘边：可选地在两端“出头”，然后描一遍或两遍 */
static void rough_edge(sk_canvas *c, sk_rng *g, const sk_rough_style *s,
                       float x1, float y1, float x2, float y2, int overshoot)
{
    float rough = st_rough(s), bowing = st_bowing(s), r = st_radius(s);
    if (overshoot && rough > 0.0f) {
        float dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
        if (len > 1.0f) {
            /* 出头量：随长度增加但有上限，两端各自随机 */
            float ov = len * 0.035f;
            if (ov > 2.6f) ov = 2.6f;
            if (ov < 0.6f) ov = 0.6f;
            ov *= rough;
            float e0 = ov * (0.3f + 0.7f * sk_rng_float(g));
            float e1 = ov * (0.3f + 0.7f * sk_rng_float(g));
            float ux = dx / len, uy = dy / len;
            x1 -= ux * e0;
            y1 -= uy * e0;
            x2 += ux * e1;
            y2 += uy * e1;
        }
    }
    bez_t b;
    rough_line_bez(g, rough, bowing, x1, y1, x2, y2, 0, &b);
    stroke_bez(c, &b, r, s->stroke);
    if (s->double_stroke && rough > 0.0f) {
        rough_line_bez(g, rough, bowing, x1, y1, x2, y2, 1, &b);
        stroke_bez(c, &b, r, s->stroke);
    }
}

/* ======================================================================
 * 排线类填充（hachure / cross-hatch / zigzag / dots / marker）
 *
 * 共同的第一步是“扫描”：用一组方向为 angle、间距为 gap 的平行线去切多边形，
 * 每条线和多边形各边求交点，交点排序后两两配对（偶奇规则），每对之间就是一段在
 * 图形内部的线段。平行线的位置锁定在“以第一个顶点为原点、间距 gap 的格子”上，
 * 每条线的随机数只由 (seed, 行号) 决定：图形变宽（进度条增长）时已有的排线不会挪动。
 * ====================================================================== */

typedef struct {
    sk_pointf u, n;     /* 排线方向、法向 */
    float d;            /* 这条线的位置（沿法向） */
    int k;              /* 行号 */
} scan_line;

typedef void (*scan_fn)(void *ctx, const scan_line *ln, int j, int nseg, sk_pointf a, sk_pointf b);

static int float_cmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* area 是只关心的区域（画布坐标）：扫描线只在它覆盖的范围里生成，
 * 所以对一个巨大的图形做局部重绘时，代价只和局部区域有关。 */
static void hachure_scan(const sk_pointf *pts, int n, float angle_deg, float gap, sk_rect area,
                         float pad, scan_fn fn, void *ctx)
{
    if (n < 3 || sk_rect_empty(area))
        return;
    float a = angle_deg * (SK_PI / 180.0f);
    scan_line ln;
    ln.u = P(cosf(a), sinf(a));
    ln.n = P(-ln.u.y, ln.u.x);
    float d0 = pts[0].x * ln.n.x + pts[0].y * ln.n.y;
    float dmin = d0, dmax = d0;
    for (int i = 1; i < n; i++) {
        float d = pts[i].x * ln.n.x + pts[i].y * ln.n.y;
        if (d < dmin) dmin = d;
        if (d > dmax) dmax = d;
    }
    /* 和关心区域的四个角投影求交，裁掉看不见的排线 */
    float cx0 = (float)area.x - pad, cy0 = (float)area.y - pad;
    float cx1 = (float)(area.x + area.w) + pad, cy1 = (float)(area.y + area.h) + pad;
    float c0 = cx0 * ln.n.x + cy0 * ln.n.y, c1 = cx1 * ln.n.x + cy0 * ln.n.y;
    float c2 = cx0 * ln.n.x + cy1 * ln.n.y, c3 = cx1 * ln.n.x + cy1 * ln.n.y;
    float amin = fminf(fminf(c0, c1), fminf(c2, c3)), amax = fmaxf(fmaxf(c0, c1), fmaxf(c2, c3));
    if (amin > dmin) dmin = amin;
    if (amax < dmax) dmax = amax;
    if (!(dmax > dmin))
        return;
    float kf0 = ceilf((dmin - d0) / gap - 0.5f), kf1 = floorf((dmax - d0) / gap - 0.5f);
    if (!(kf1 >= kf0) || kf1 - kf0 > 20000.0f)
        return;
    float *xs = sk__scratch(SK__SCRATCH_XS, sizeof(float) * ((size_t)n + 1));
    if (!xs)
        return;
    for (int k = (int)kf0; k <= (int)kf1; k++) {
        float d = d0 + ((float)k + 0.5f) * gap;
        int nx = 0;
        for (int i = 0; i < n; i++) {
            sk_pointf p = pts[i], q = pts[(i + 1) % n];
            float dp = p.x * ln.n.x + p.y * ln.n.y, dq = q.x * ln.n.x + q.y * ln.n.y;
            if ((dp <= d) == (dq <= d))
                continue;
            float t = (d - dp) / (dq - dp);
            float ix = p.x + t * (q.x - p.x), iy = p.y + t * (q.y - p.y);
            xs[nx++] = ix * ln.u.x + iy * ln.u.y;
        }
        if (nx < 2)
            continue;
        if (nx == 2) {
            if (xs[0] > xs[1]) { float t = xs[0]; xs[0] = xs[1]; xs[1] = t; }
        } else {
            qsort(xs, (size_t)nx, sizeof(float), float_cmp);
        }
        ln.d = d;
        ln.k = k;
        int nseg = nx / 2;
        for (int j = 0; j < nseg; j++) {
            float s0 = xs[2 * j], s1 = xs[2 * j + 1];
            sk_pointf A = P(ln.n.x * d + ln.u.x * s0, ln.n.y * d + ln.u.y * s0);
            sk_pointf B = P(ln.n.x * d + ln.u.x * s1, ln.n.y * d + ln.u.y * s1);
            fn(ctx, &ln, j, nseg, A, B);
        }
    }
}

typedef struct {
    sk__stroke st;
    const sk_rough_style *s;
    uint32_t seed;
    float rough, bowing, r, gap;
    float pad;          /* 一条排线最多能偏离理想位置多远（抖动 + 弯曲 + 笔宽） */
    /* zigzag 的连笔状态 */
    int zk, zn;
    sk_pointf zend[8];
    int zhave[8];
} fill_ctx;

static void line_rng(sk_rng *g, uint32_t seed, int k, int j)
{
    sk_rng_seed(g, sk_hash_u32(seed, (uint32_t)k * 0x9E3779B1u + (uint32_t)j));
}

/* 线段两端各往里收一点，免得排线的抖动戳出轮廓太多 */
static int inset_seg(sk_pointf *a, sk_pointf *b, float inset)
{
    float dx = b->x - a->x, dy = b->y - a->y, len = sqrtf(dx * dx + dy * dy);
    if (len <= 2.0f * inset + 0.5f)
        return 0;
    float ux = dx / len, uy = dy / len;
    a->x += ux * inset;
    a->y += uy * inset;
    b->x -= ux * inset;
    b->y -= uy * inset;
    return 1;
}

static void hachure_cb(void *vctx, const scan_line *ln, int j, int nseg, sk_pointf a, sk_pointf b)
{
    fill_ctx *f = vctx;
    if (!inset_seg(&a, &b, 0.6f + f->r * 0.5f))
        return;
    sk_rng g;
    line_rng(&g, f->seed, ln->k, j);
    bez_t bz;
    /* 排线比轮廓规整一些：太乱的排线会让颜色看起来很脏 */
    rough_line_bez(&g, f->rough * 0.75f, f->bowing, a.x, a.y, b.x, b.y, 0, &bz);
    sk__stroke_move(&f->st, bz.p0.x, bz.p0.y, f->r);
    sk__stroke_bezier(&f->st, bz.p1.x, bz.p1.y, bz.p2.x, bz.p2.y, bz.p3.x, bz.p3.y, f->r);
}

static int poly_bbox(const sk_pointf *pts, int n, float *x0, float *y0, float *x1, float *y1)
{
    *x0 = *x1 = pts[0].x;
    *y0 = *y1 = pts[0].y;
    for (int i = 0; i < n; i++) {
        if (!COORD_OK(pts[i].x) || !COORD_OK(pts[i].y))
            return 0;
        if (pts[i].x < *x0) *x0 = pts[i].x;
        if (pts[i].x > *x1) *x1 = pts[i].x;
        if (pts[i].y < *y0) *y0 = pts[i].y;
        if (pts[i].y > *y1) *y1 = pts[i].y;
    }
    return 1;
}

static int fill_begin(fill_ctx *f, sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s,
                      uint32_t seed, float r)
{
    float x0, y0, x1, y1;
    if (!poly_bbox(pts, n, &x0, &y0, &x1, &y1))
        return 0;
    f->s = s;
    f->seed = seed;
    f->rough = st_rough(s);
    f->bowing = st_bowing(s);
    f->r = r;
    f->gap = st_gap(s);
    /* 排线的随机偏移：端点和控制点 ≤ 2px×roughness，弯曲 ≤ 线长的 1%×bowing×roughness，
     * zigzag 的端点再抖 15% 的间距。扫描范围和蒙版范围都要按这个外扩，
     * 否则局部重绘时，一条“理想位置在区域外、实际抖进区域里”的排线会被漏掉。 */
    float diag = hypotf(x1 - x0, y1 - y0);
    f->pad = r + 2.0f + f->rough * (2.0f * ROUGH_MAXOFF + 0.01f * fabsf(f->bowing) * diag + 0.15f * f->gap);
    float pad = f->pad + 1.0f;
    if (!sk__stroke_begin(&f->st, c, x0 - pad, y0 - pad, x1 + pad, y1 + pad))
        return 0;
    /* 蜡笔颗粒以图形的第一个顶点为原点：图形整体平移（窗口移动、画在不同的画布上）时颗粒跟着走 */
    f->st.gx = (int)floorf(pts[0].x);
    f->st.gy = (int)floorf(pts[0].y);
    return 1;
}

static void fill_hachure(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s,
                         float angle, uint32_t seed)
{
    fill_ctx f;
    if (!fill_begin(&f, c, pts, n, s, seed, st_fill_weight(s) * 0.5f))
        return;
    hachure_scan(pts, n, angle, f.gap, f.st.r, f.pad, hachure_cb, &f);
    /* 彩色排线带一点点蜡笔颗粒 */
    sk__stroke_end(&f.st, s->fill, 1, seed);
}

/* zigzag：像拿蜡笔来回涂——同一笔在相邻两条扫描线之间来回折返 */
static void zigzag_cb(void *vctx, const scan_line *ln, int j, int nseg, sk_pointf a, sk_pointf b)
{
    fill_ctx *f = vctx;
    if (!inset_seg(&a, &b, 0.6f + f->r * 0.5f))
        return;
    if (j >= 8)
        return;
    /* 扫描线上的段数变了（凹多边形）或者跳过了一行，连笔就断开重新起笔 */
    if (ln->k != f->zk + 1 || nseg != f->zn) {
        for (int i = 0; i < 8; i++)
            f->zhave[i] = 0;
    }
    if (j == nseg - 1) {
        f->zk = ln->k;
        f->zn = nseg;
    }
    sk_rng g;
    line_rng(&g, f->seed, ln->k, j);
    float jit = f->gap * 0.15f * f->rough;
    a.x += roff(&g, jit, 1.0f);
    a.y += roff(&g, jit, 1.0f);
    b.x += roff(&g, jit, 1.0f);
    b.y += roff(&g, jit, 1.0f);
    /* 方向只由行号的奇偶决定（不取决于从哪一行开始画），局部重绘时和整体重绘一致 */
    int dir = (ln->k + j) & 1;
    sk_pointf s0 = dir ? b : a, s1 = dir ? a : b;
    bez_t bz;
    if (f->zhave[j]) {
        /* 把笔尖放回本链的末端（同一扫描线上有多条链时会交替画），折回来接着涂 */
        sk__stroke_jump(&f->st, f->zend[j].x, f->zend[j].y, f->r);
        soft_bez(&g, f->rough * 0.5f, f->bowing, f->zend[j], s0, &bz);
        sk__stroke_bezier(&f->st, bz.p1.x, bz.p1.y, bz.p2.x, bz.p2.y, bz.p3.x, bz.p3.y, f->r);
    } else {
        sk__stroke_move(&f->st, s0.x, s0.y, f->r);
    }
    soft_bez(&g, f->rough * 0.75f, f->bowing, s0, s1, &bz);
    sk__stroke_bezier(&f->st, bz.p1.x, bz.p1.y, bz.p2.x, bz.p2.y, bz.p3.x, bz.p3.y, f->r);
    f->zend[j] = s1;
    f->zhave[j] = 1;
}

static void fill_zigzag(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s, uint32_t seed)
{
    fill_ctx f;
    if (!fill_begin(&f, c, pts, n, s, seed, st_fill_weight(s) * 0.5f))
        return;
    f.zk = -1000000;
    f.zn = -1;
    memset(f.zhave, 0, sizeof f.zhave);
    /* 多扫一行：区域边上那一行的“折返连笔”来自上一行 */
    hachure_scan(pts, n, s->hachure_angle, f.gap, f.st.r, f.pad + f.gap, zigzag_cb, &f);
    sk__stroke_end(&f.st, s->fill, 1, seed);
}

/* dots：沿扫描线每隔 gap 点一个小圆点，位置带一点随机 */
static void dots_cb(void *vctx, const scan_line *ln, int j, int nseg, sk_pointf a, sk_pointf b)
{
    fill_ctx *f = vctx;
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f)
        return;
    float ux = dx / len, uy = dy / len;
    sk_rng g;
    line_rng(&g, f->seed, ln->k, j);
    /* 点的位置也锁定在格子上（沿排线方向的投影取整），不随图形宽度变化 */
    float s0 = a.x * ln->u.x + a.y * ln->u.y;
    float first = ceilf(s0 / f->gap - 0.5f) * f->gap + f->gap * 0.5f - s0;
    float jit = f->gap * 0.22f * (f->rough > 1.0f ? 1.0f : f->rough);
    for (float t = first; t < len; t += f->gap) {
        float px = a.x + ux * t + roff(&g, jit, 1.0f);
        float py = a.y + uy * t + roff(&g, jit, 1.0f);
        float rr = f->r * (0.85f + 0.3f * sk_rng_float(&g));
        sk__stroke_dot(&f->st, px, py, rr);
    }
}

static void fill_dots(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s, uint32_t seed)
{
    fill_ctx f;
    float r = st_fill_weight(s) * 0.75f;
    if (r < 0.7f) r = 0.7f;
    if (!fill_begin(&f, c, pts, n, s, seed, r))
        return;
    hachure_scan(pts, n, s->hachure_angle, f.gap, f.st.r, f.pad + f.gap * 0.25f, dots_cb, &f);
    sk__stroke_end(&f.st, s->fill, 0, seed);
}

/* marker：荧光笔/马克笔——粗的半透明笔触一道一道涂。
 * 每一道单独混合，相邻两道重叠的地方颜色会深一点，这正是真实马克笔的样子。 */
typedef struct {
    sk_canvas *c;
    const sk_rough_style *s;
    uint32_t seed;
    float w, rough;
    sk_color color;
} marker_ctx;

static void marker_cb(void *vctx, const scan_line *ln, int j, int nseg, sk_pointf a, sk_pointf b)
{
    marker_ctx *m = vctx;
    float r = m->w * 0.5f;
    sk_rng g;
    line_rng(&g, m->seed, ln->k, j);
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    sk__stroke st;
    float pad = r + 2.0f;
    if (len <= 1.0f) {
        float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f;
        if (sk__stroke_begin(&st, m->c, cx - pad, cy - pad, cx + pad, cy + pad)) {
            sk__stroke_dot(&st, cx, cy, r * 0.9f);
            sk__stroke_end(&st, m->color, 0, 0);
        }
        return;
    }
    /* 扫描用的多边形已经往里收过一个笔刷半径（见 fill_marker），圆形笔头正好涂到边。
     * 两端各自沿法向随机偏一点：每一笔的方向都略有不同，不像尺子画的平行线 */
    float jit = r * 0.25f * (m->rough < 1.0f ? m->rough : 1.0f);
    float ja = roff(&g, jit, 1.0f), jb = roff(&g, jit, 1.0f);
    a.x += ln->n.x * ja;
    a.y += ln->n.y * ja;
    b.x += ln->n.x * jb;
    b.y += ln->n.y * jb;
    bez_t bz;
    soft_bez(&g, m->rough * 0.6f, 1.0f, a, b, &bz);
    float r0 = r * (0.92f + 0.08f * sk_rng_float(&g)), r1 = r * (0.85f + 0.1f * sk_rng_float(&g));
    /* 蒙版范围取贝塞尔四个控制点的包围盒（曲线一定在控制点的凸包里） */
    float bx0 = fminf(fminf(bz.p0.x, bz.p1.x), fminf(bz.p2.x, bz.p3.x));
    float bx1 = fmaxf(fmaxf(bz.p0.x, bz.p1.x), fmaxf(bz.p2.x, bz.p3.x));
    float by0 = fminf(fminf(bz.p0.y, bz.p1.y), fminf(bz.p2.y, bz.p3.y));
    float by1 = fmaxf(fmaxf(bz.p0.y, bz.p1.y), fmaxf(bz.p2.y, bz.p3.y));
    if (!sk__stroke_begin(&st, m->c, bx0 - pad, by0 - pad, bx1 + pad, by1 + pad))
        return;
    sk__stroke_move(&st, bz.p0.x, bz.p0.y, r0);
    sk__stroke_bezier(&st, bz.p1.x, bz.p1.y, bz.p2.x, bz.p2.y, bz.p3.x, bz.p3.y, r1);
    sk__stroke_end(&st, m->color, 0, 0);
}

static void fill_marker(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s, uint32_t seed)
{
    float x0, y0, x1, y1;
    if (!poly_bbox(pts, n, &x0, &y0, &x1, &y1))
        return;
    marker_ctx m;
    m.c = c;
    m.s = s;
    m.seed = seed;
    m.rough = st_rough(s);
    float gap = st_gap(s);
    /* 笔宽和 hachure_gap 成比例，但不超过图形短边的 45%（保证至少来回涂两三道） */
    float bw = x1 - x0, bh = y1 - y0;
    m.w = gap * 1.6f;
    float minside = fminf(bw, bh);
    if (m.w > minside * 0.45f) m.w = minside * 0.45f;
    if (m.w < 2.0f) m.w = 2.0f;
    m.color = SK_A(s->fill) == 255 ? SK_WITH_ALPHA(s->fill, 0x78) : s->fill;
    /* 笔触中心线只在“往里收了一个笔刷半径”的区域里走，粗笔头就不会涂出图形：
     * 把多边形朝包围盒中心按比例缩小（对凸图形足够好）。 */
    float r = m.w * 0.5f * 0.9f;
    float sx = bw > 2.0f * r ? (bw - 2.0f * r) / bw : 0.05f;
    float sy = bh > 2.0f * r ? (bh - 2.0f * r) / bh : 0.05f;
    float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    /* 人涂色时顺着图形的长边来回涂，只略微倾斜：一笔从这头涂到那头，上下漂移不超过
     * 笔宽的 35%，否则图形的两个角会漏涂 */
    float iw = bw - 2.0f * r, ih = bh - 2.0f * r;
    if (iw < 1.0f) iw = 1.0f;
    if (ih < 1.0f) ih = 1.0f;
    float angle;
    if (bw >= bh)
        angle = -fminf(9.0f, atanf(0.35f * m.w / iw) * (180.0f / SK_PI));
    else
        angle = -90.0f + fminf(9.0f, atanf(0.35f * m.w / ih) * (180.0f / SK_PI));
    float a = angle * (SK_PI / 180.0f);
    float nx = -sinf(a), ny = cosf(a);
    sk_pointf *q = sk__scratch(SK__SCRATCH_PTS, sizeof(sk_pointf) * (size_t)n);
    if (!q)
        return;
    /* 缩小后的多边形；顶点顺序从“法向投影最小”的那个开始，让排线格子从图形的一边起算 */
    int first = 0;
    float dmin = 1e30f, dmax = -1e30f;
    for (int i = 0; i < n; i++) {
        sk_pointf p = P(mx + (pts[i].x - mx) * sx, my + (pts[i].y - my) * sy);
        float d = p.x * nx + p.y * ny;
        if (d < dmin) { dmin = d; first = i; }
        if (d > dmax) dmax = d;
    }
    for (int i = 0; i < n; i++) {
        sk_pointf p = pts[(first + i) % n];
        q[i] = P(mx + (p.x - mx) * sx, my + (p.y - my) * sy);
    }
    /* 道数取整、间距均分：上下两边留得一样多 */
    float extent = dmax - dmin;
    int bands = (int)ceilf(extent / (m.w * 0.72f) - 0.15f);
    if (bands < 1) bands = 1;
    /* 再保险：整个填充裁剪在图形的包围盒里 */
    sk_rect box = sk_rect_make((int)floorf(x0), (int)floorf(y0), (int)ceilf(x1) - (int)floorf(x0) + 1,
                               (int)ceilf(y1) - (int)floorf(y0) + 1);
    sk_rect old = sk_push_clip(c, box);
    if (extent < 0.5f) {
        /* 图形太小，缩完只剩一个点：点一下就好 */
        scan_line ln = { P(cosf(a), sinf(a)), P(nx, ny), 0.0f, 0 };
        marker_cb(&m, &ln, 0, 1, P(mx, my), P(mx, my));
    } else {
        hachure_scan(q, n, angle, extent / (float)bands, c->clip,
                     m.w + m.rough * (2.0f + 0.01f * hypotf(bw, bh)), marker_cb, &m);
    }
    sk_pop_clip(c, old);
}

static int pts_ok(const sk_pointf *pts, int n)
{
    for (int i = 0; i < n; i++)
        if (!COORD_OK(pts[i].x) || !COORD_OK(pts[i].y))
            return 0;
    return 1;
}

/* 按 style 的填充方式填充一个多边形（seed 是填充流的种子） */
static void fill_dispatch(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s, uint32_t seed)
{
    if (!c || !pts || n < 3 || SK_A(s->fill) == 0 || !pts_ok(pts, n))
        return;
    switch (s->fill_style) {
    case SK_FILL_SOLID:
        sk_fill_polygon(c, pts, n, s->fill);
        break;
    case SK_FILL_HACHURE:
        fill_hachure(c, pts, n, s, s->hachure_angle, seed);
        break;
    case SK_FILL_CROSS_HATCH:
        fill_hachure(c, pts, n, s, s->hachure_angle, seed);
        fill_hachure(c, pts, n, s, s->hachure_angle + 90.0f, sk_hash_u32(seed, 0xC4055u));
        break;
    case SK_FILL_ZIGZAG:
        fill_zigzag(c, pts, n, s, seed);
        break;
    case SK_FILL_DOTS:
        fill_dots(c, pts, n, s, seed);
        break;
    case SK_FILL_MARKER:
        fill_marker(c, pts, n, s, seed);
        break;
    default:
        break;
    }
}

void sk_rough_fill_polygon(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s)
{
    if (!c || !s || !pts || n < 3 || s->fill_style == SK_FILL_NONE)
        return;
    fill_dispatch(c, pts, n, s, fill_seed(s));
}

/* 实心填充的多边形：顶点加一点随机偏移，填充和轮廓不会严丝合缝——像随手涂的颜色 */
static void solid_jitter(sk_pointf *pts, int n, const sk_rough_style *s, uint32_t seed)
{
    sk_rng g;
    sk_rng_seed(&g, sk_hash_u32(seed, 0x50710u));
    float j = 0.8f * st_rough(s);
    if (j > 2.0f) j = 2.0f;
    for (int i = 0; i < n; i++) {
        pts[i].x += roff(&g, j, 1.0f);
        pts[i].y += roff(&g, j, 1.0f);
    }
}

/* ======================================================================
 * 图元
 * ====================================================================== */

void sk_rough_line(sk_canvas *c, float x1, float y1, float x2, float y2, const sk_rough_style *s)
{
    if (!c || !s || !st_has_stroke(s))
        return;
    if (!COORD_OK(x1) || !COORD_OK(y1) || !COORD_OK(x2) || !COORD_OK(y2))
        return;
    sk_rng g;
    sk_rng_seed(&g, s->seed);
    rough_edge(c, &g, s, x1, y1, x2, y2, 0);
}

void sk_rough_rect(sk_canvas *c, float x, float y, float w, float h, const sk_rough_style *s)
{
    if (!c || !s || !COORD_OK(x) || !COORD_OK(y) || !COORD_OK(w) || !COORD_OK(h))
        return;
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (s->fill_style != SK_FILL_NONE) {
        sk_pointf q[4] = { P(x, y), P(x + w, y), P(x + w, y + h), P(x, y + h) };
        uint32_t fs = fill_seed(s);
        if (s->fill_style == SK_FILL_SOLID)
            solid_jitter(q, 4, s, fs);
        fill_dispatch(c, q, 4, s, fs);
    }
    if (!st_has_stroke(s))
        return;
    sk_rng g;
    sk_rng_seed(&g, s->seed);
    rough_edge(c, &g, s, x, y, x + w, y, 1);
    rough_edge(c, &g, s, x + w, y, x + w, y + h, 1);
    rough_edge(c, &g, s, x + w, y + h, x, y + h, 1);
    rough_edge(c, &g, s, x, y + h, x, y, 1);
}

/* 圆角矩形的“精确”外形（填充用）：直边 + 每个角 5 个点的圆弧 */
static int round_rect_outline(sk_pointf *o, float x, float y, float w, float h, float r)
{
    static const float cs[5] = { 1.0f, 0.9238795f, 0.7071068f, 0.3826834f, 0.0f };
    static const float sn[5] = { 0.0f, 0.3826834f, 0.7071068f, 0.9238795f, 1.0f };
    int n = 0;
    /* 四个角的圆心和起始方向：右上、右下、左下、左上（顺时针） */
    float cx[4] = { x + w - r, x + w - r, x + r, x + r };
    float cy[4] = { y + r, y + h - r, y + h - r, y + r };
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 5; i++) {
            float ux, uy;     /* 从 -90° 开始，每个角转 90° */
            switch (k) {
            case 0: ux = sn[i];  uy = -cs[i]; break;
            case 1: ux = cs[i];  uy = sn[i];  break;
            case 2: ux = -sn[i]; uy = cs[i];  break;
            default: ux = -cs[i]; uy = -sn[i]; break;
            }
            o[n++] = P(cx[k] + ux * r, cy[k] + uy * r);
        }
    }
    return n;
}

/* 圆角矩形的一遍轮廓：8 个锚点（直边与圆角的交接处）各自抖动，直边是“软”曲线，
 * 圆角是四分之一圆的贝塞尔近似；从 start 号边起笔，绕一圈后再多画一小段收笔。 */
static void round_rect_pass(path_t *p, sk_rng *g, float rough, float bowing, float x, float y,
                            float w, float h, float r, float jit, int start)
{
    const float K = 0.5523f;    /* 四分之一圆的三次贝塞尔近似常数 4(√2-1)/3 */
    sk_pointf A[8] = {
        P(x + r, y), P(x + w - r, y), P(x + w, y + r), P(x + w, y + h - r),
        P(x + w - r, y + h), P(x + r, y + h), P(x, y + h - r), P(x, y + r),
    };
    /* 每条直边的方向：上 +x，右 +y，下 -x，左 -y */
    static const float TX[4] = { 1, 0, -1, 0 }, TY[4] = { 0, 1, 0, -1 };
    for (int i = 0; i < 8; i++) {
        A[i].x += roff(g, jit, rough);
        A[i].y += roff(g, jit, rough);
    }
    bez_t bz;
    for (int s = 0; s < 8; s++) {
        int i = (start + s) & 7;
        sk_pointf a = A[i], b = A[(i + 1) & 7];
        if ((i & 1) == 0) {
            soft_bez(g, rough, bowing, a, b, &bz);
        } else {
            int ein = i >> 1, eout = ((i + 1) >> 1) & 3;
            float kr = K * r * (1.0f + roff(g, 0.15f, rough));
            bz.p0 = a;
            bz.p1 = P(a.x + TX[ein] * kr, a.y + TY[ein] * kr);
            bz.p2 = P(b.x - TX[eout] * kr, b.y - TY[eout] * kr);
            bz.p3 = b;
        }
        path_add(p, bz.p0, bz.p1, bz.p2, bz.p3, s == 0);
    }
    /* 收笔：顺着起笔那条边再往前画一点点，并稍微偏离一点 */
    if (rough > 0.0f) {
        int e = start >> 1;
        sk_pointf a = A[start & 7];
        float ov = (2.0f + 3.0f * sk_rng_float(g)) * (rough < 1.5f ? rough : 1.5f);
        float drift = roff(g, 0.8f, rough);
        sk_pointf b = P(a.x + TX[e & 3] * ov - TY[e & 3] * drift, a.y + TY[e & 3] * ov + TX[e & 3] * drift);
        path_add(p, a, P(a.x + (b.x - a.x) / 3.0f, a.y + (b.y - a.y) / 3.0f),
                 P(a.x + (b.x - a.x) * 2.0f / 3.0f, a.y + (b.y - a.y) * 2.0f / 3.0f), b, 0);
    }
}

void sk_rough_round_rect(sk_canvas *c, float x, float y, float w, float h, float r, const sk_rough_style *s)
{
    if (!c || !s || !COORD_OK(x) || !COORD_OK(y) || !COORD_OK(w) || !COORD_OK(h) || !COORD_OK(r))
        return;
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    float rmax = fminf(w, h) * 0.5f;
    if (r > rmax) r = rmax;
    if (r < 0.5f) {
        sk_rough_rect(c, x, y, w, h, s);
        return;
    }
    if (s->fill_style != SK_FILL_NONE) {
        sk_pointf q[20];
        int n = round_rect_outline(q, x, y, w, h, r);
        uint32_t fs = fill_seed(s);
        if (s->fill_style == SK_FILL_SOLID)
            solid_jitter(q, n, s, fs);
        fill_dispatch(c, q, n, s, fs);
    }
    if (!st_has_stroke(s))
        return;
    float rough = st_rough(s), bowing = st_bowing(s), rad = st_radius(s);
    sk_rng g;
    sk_rng_seed(&g, s->seed);
    /* 锚点抖动：大约是普通直线端点抖动的一半，圆角处太乱会显得“破” */
    float side = fminf(w, h);
    float jit = side < 20.0f ? side / 20.0f : 1.0f;
    path_t p;
    path_begin(&p);
    round_rect_pass(&p, &g, rough, bowing, x, y, w, h, r, jit, (int)(sk_rng_u32(&g) & 3) * 2);
    path_stroke(c, &p, rad, s->stroke, 0, 0);
    if (s->double_stroke && rough > 0.0f) {
        path_begin(&p);
        round_rect_pass(&p, &g, rough, bowing, x, y, w, h, r, jit * 0.6f, (int)(sk_rng_u32(&g) & 3) * 2);
        path_stroke(c, &p, rad, s->stroke, 0, 0);
    }
}

/* ---- 椭圆（rough.js 的 _computeEllipsePoints） ---- */

#define ELLIPSE_MAX_STEPS 72

static int ellipse_points(sk_rng *g, float rough, float cx, float cy, float rx, float ry, float inc,
                          float offset, float overlap, sk_pointf *out, sk_pointf *core, int *ncore)
{
    int n = 0, nc = 0;
    float rad = roff(g, 0.5f, rough) - SK_PI / 2.0f;     /* 随机的起笔角度（大致在顶部） */
    out[n].x = roff(g, offset, rough) + cx + 0.9f * rx * cosf(rad - inc);
    out[n].y = roff(g, offset, rough) + cy + 0.9f * ry * sinf(rad - inc);
    n++;
    float end = 2.0f * SK_PI + rad - 0.01f;
    for (int i = 0; i < ELLIPSE_MAX_STEPS + 1; i++) {
        float a = rad + inc * (float)i;
        if (a >= end)
            break;
        out[n].x = roff(g, offset, rough) + cx + rx * cosf(a);
        out[n].y = roff(g, offset, rough) + cy + ry * sinf(a);
        if (core)
            core[nc++] = out[n];
        n++;
    }
    /* 收尾：越过起点再画一点（overlap），并且半径略微收小，像笔画首尾交叠 */
    out[n].x = roff(g, offset, rough) + cx + rx * cosf(rad + 2.0f * SK_PI + overlap * 0.5f);
    out[n].y = roff(g, offset, rough) + cy + ry * sinf(rad + 2.0f * SK_PI + overlap * 0.5f);
    n++;
    out[n].x = roff(g, offset, rough) + cx + 0.98f * rx * cosf(rad + overlap);
    out[n].y = roff(g, offset, rough) + cy + 0.98f * ry * sinf(rad + overlap);
    n++;
    out[n].x = roff(g, offset, rough) + cx + 0.9f * rx * cosf(rad + overlap * 0.5f);
    out[n].y = roff(g, offset, rough) + cy + 0.9f * ry * sinf(rad + overlap * 0.5f);
    n++;
    if (ncore)
        *ncore = nc;
    return n;
}

/* 闭合 Catmull-Rom 曲线展开成多边形（填充用）：每段取 per 个点 */
static int closed_curve_flatten(const sk_pointf *p, int n, int per, sk_pointf *out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        sk_pointf p0 = p[(i + n - 1) % n], p1 = p[i], p2 = p[(i + 1) % n], p3 = p[(i + 2) % n];
        sk_pointf b1 = P(p1.x + (p2.x - p0.x) / 6.0f, p1.y + (p2.y - p0.y) / 6.0f);
        sk_pointf b2 = P(p2.x - (p3.x - p1.x) / 6.0f, p2.y - (p3.y - p1.y) / 6.0f);
        for (int k = 0; k < per; k++) {
            float t = (float)k / (float)per, mt = 1.0f - t;
            float a = mt * mt * mt, b = 3 * mt * mt * t, cc = 3 * mt * t * t, d = t * t * t;
            out[m++] = P(a * p1.x + b * b1.x + cc * b2.x + d * p2.x, a * p1.y + b * b1.y + cc * b2.y + d * p2.y);
        }
    }
    return m;
}

void sk_rough_ellipse(sk_canvas *c, float cx, float cy, float w, float h, const sk_rough_style *s)
{
    if (!c || !s || !COORD_OK(cx) || !COORD_OK(cy) || !COORD_OK(w) || !COORD_OK(h))
        return;
    float rough = st_rough(s);
    float rx = fabsf(w) * 0.5f, ry = fabsf(h) * 0.5f;
    sk_rng g;
    sk_rng_seed(&g, s->seed);
    /* 分段数随周长的平方根增长（大圆多几个点），这是 rough.js 的经验公式 */
    float psq = sqrtf(2.0f * SK_PI * sqrtf((rx * rx + ry * ry) * 0.5f));
    float steps = ceilf(fmaxf(9.0f, 9.0f / sqrtf(200.0f) * psq));
    if (steps > ELLIPSE_MAX_STEPS) steps = ELLIPSE_MAX_STEPS;
    float inc = 2.0f * SK_PI / steps;
    /* curveFitting=0.95：半径本身也随机偏 ±5%×roughness，圆就不是正圆了 */
    rx += roff(&g, rx * 0.05f, rough);
    ry += roff(&g, ry * 0.05f, rough);

    sk_pointf pts1[ELLIPSE_MAX_STEPS + 8], core[ELLIPSE_MAX_STEPS + 8], pts2[ELLIPSE_MAX_STEPS + 8];
    int ncore = 0;
    float ov_in = rough * (sk_rng_float(&g) * 0.6f + 0.4f);
    float overlap = inc * rough * (sk_rng_float(&g) * (ov_in - 0.1f) + 0.1f);
    int n1 = ellipse_points(&g, rough, cx, cy, rx, ry, inc, 1.0f, overlap, pts1, core, &ncore);

    if (s->fill_style != SK_FILL_NONE && ncore >= 3) {
        sk_pointf *fp = sk__scratch(SK__SCRATCH_FILLPTS, sizeof(sk_pointf) * (size_t)ncore * 4);
        if (fp) {
            int nf = closed_curve_flatten(core, ncore, 4, fp);
            uint32_t fs = fill_seed(s);
            fill_dispatch(c, fp, nf, s, fs);
        }
    }
    if (!st_has_stroke(s))
        return;
    float r = st_radius(s);
    path_t p;
    path_begin(&p);
    path_curve(&p, pts1, n1);
    path_stroke(c, &p, r, s->stroke, 0, 0);
    if (s->double_stroke && rough > 0.0f) {
        int n2 = ellipse_points(&g, rough, cx, cy, rx, ry, inc, 1.5f, 0.0f, pts2, NULL, NULL);
        path_begin(&p);
        path_curve(&p, pts2, n2);
        path_stroke(c, &p, r, s->stroke, 0, 0);
    }
}

void sk_rough_circle(sk_canvas *c, float cx, float cy, float diameter, const sk_rough_style *s)
{
    sk_rough_ellipse(c, cx, cy, diameter, diameter, s);
}

void sk_rough_polygon(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s)
{
    if (!c || !s || !pts || n < 2 || !pts_ok(pts, n))
        return;
    if (n >= 3 && s->fill_style != SK_FILL_NONE) {
        uint32_t fs = fill_seed(s);
        if (s->fill_style == SK_FILL_SOLID) {
            sk_pointf *q = sk__scratch(SK__SCRATCH_FILLPTS, sizeof(sk_pointf) * (size_t)n);
            if (q) {
                memcpy(q, pts, sizeof(sk_pointf) * (size_t)n);
                solid_jitter(q, n, s, fs);
                fill_dispatch(c, q, n, s, fs);
            }
        } else {
            fill_dispatch(c, pts, n, s, fs);
        }
    }
    if (!st_has_stroke(s))
        return;
    sk_rng g;
    sk_rng_seed(&g, s->seed);
    int edges = n == 2 ? 1 : n;
    for (int i = 0; i < edges; i++) {
        sk_pointf a = pts[i], b = pts[(i + 1) % n];
        rough_edge(c, &g, s, a.x, a.y, b.x, b.y, 1);
    }
}

void sk_rough_polyline(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s)
{
    if (!c || !s || !pts || n < 2 || !st_has_stroke(s) || !pts_ok(pts, n))
        return;
    sk_rng g;
    sk_rng_seed(&g, s->seed);
    for (int i = 0; i + 1 < n; i++)
        rough_edge(c, &g, s, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, 0);
}

/* rough.js 的 curve：每个点加随机偏移（首尾点重复一次作为切线参考），再串成样条 */
static void curve_pass(sk_canvas *c, sk_rng *g, const sk_pointf *pts, int n, float offset,
                       float rough, float r, sk_color color)
{
    sk_pointf *ps = sk__scratch(SK__SCRATCH_PTS, sizeof(sk_pointf) * ((size_t)n + 2));
    if (!ps)
        return;
    int m = 0;
    ps[m++] = P(pts[0].x + roff(g, offset, rough), pts[0].y + roff(g, offset, rough));
    ps[m++] = P(pts[0].x + roff(g, offset, rough), pts[0].y + roff(g, offset, rough));
    for (int i = 1; i < n; i++) {
        ps[m++] = P(pts[i].x + roff(g, offset, rough), pts[i].y + roff(g, offset, rough));
        if (i == n - 1)
            ps[m++] = P(pts[i].x + roff(g, offset, rough), pts[i].y + roff(g, offset, rough));
    }
    path_t p;
    path_begin(&p);
    path_curve(&p, ps, m);
    path_stroke(c, &p, r, color, 0, 0);
}

void sk_rough_curve(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s)
{
    if (!c || !s || !pts || n < 2 || !st_has_stroke(s) || !pts_ok(pts, n))
        return;
    float rough = st_rough(s), r = st_radius(s);
    sk_rng g;
    sk_rng_seed(&g, s->seed);
    curve_pass(c, &g, pts, n, 1.0f * (1.0f + rough * 0.2f), rough, r, s->stroke);
    if (s->double_stroke && rough > 0.0f)
        curve_pass(c, &g, pts, n, 1.5f * (1.0f + rough * 0.22f), rough, r, s->stroke);
}

/* ---- 圆弧（rough.js 的 arc / _arc） ---- */

static int arc_points(sk_rng *g, float rough, float cx, float cy, float rx, float ry, float strt,
                      float stp, float inc, float offset, sk_pointf *out, int max)
{
    int n = 0;
    float rad = strt + roff(g, 0.1f, rough);
    out[n++] = P(roff(g, offset, rough) + cx + 0.9f * rx * cosf(rad - inc),
                 roff(g, offset, rough) + cy + 0.9f * ry * sinf(rad - inc));
    for (int i = 0; n < max - 2; i++) {
        float a = rad + inc * (float)i;
        if (a > stp)
            break;
        out[n++] = P(roff(g, offset, rough) + cx + rx * cosf(a), roff(g, offset, rough) + cy + ry * sinf(a));
    }
    out[n++] = P(cx + rx * cosf(stp), cy + ry * sinf(stp));
    out[n++] = P(cx + rx * cosf(stp), cy + ry * sinf(stp));
    return n;
}

void sk_rough_arc(sk_canvas *c, float cx, float cy, float w, float h,
                  float start, float stop, int closed, const sk_rough_style *s)
{
    if (!c || !s || !COORD_OK(cx) || !COORD_OK(cy) || !COORD_OK(w) || !COORD_OK(h) ||
        !(fabsf(start) < 1.0e4f) || !(fabsf(stop) < 1.0e4f))
        return;
    float rough = st_rough(s);
    float rx = fabsf(w) * 0.5f, ry = fabsf(h) * 0.5f;
    sk_rng g;
    sk_rng_seed(&g, s->seed);
    rx += roff(&g, rx * 0.01f, rough);
    ry += roff(&g, ry * 0.01f, rough);
    float strt = start, stp = stop;
    if (stp < strt) { float t = strt; strt = stp; stp = t; }
    while (strt < 0.0f) {
        strt += 2.0f * SK_PI;
        stp += 2.0f * SK_PI;
    }
    if (stp - strt > 2.0f * SK_PI) {
        strt = 0.0f;
        stp = 2.0f * SK_PI;
    }
    if (stp - strt < 1.0e-4f)
        return;
    float ellipse_inc = 2.0f * SK_PI / 9.0f;
    float inc = fminf(ellipse_inc * 0.5f, (stp - strt) * 0.5f);

    if (s->fill_style != SK_FILL_NONE) {
        /* 扇形（closed）或弓形（不闭合）区域 */
        sk_pointf q[48];
        int nq = 0;
        if (closed)
            q[nq++] = P(cx, cy);
        int segs = (int)ceilf((stp - strt) / (SK_PI / 20.0f));
        if (segs > 40) segs = 40;
        if (segs < 2) segs = 2;
        for (int i = 0; i <= segs; i++) {
            float a = strt + (stp - strt) * (float)i / (float)segs;
            q[nq++] = P(cx + rx * cosf(a), cy + ry * sinf(a));
        }
        fill_dispatch(c, q, nq, s, fill_seed(s));
    }
    if (!st_has_stroke(s))
        return;
    float r = st_radius(s);
    sk_pointf pts[48];
    path_t p;
    int n = arc_points(&g, rough, cx, cy, rx, ry, strt, stp, inc, 1.0f, pts, 48);
    path_begin(&p);
    path_curve(&p, pts, n);
    path_stroke(c, &p, r, s->stroke, 0, 0);
    if (s->double_stroke && rough > 0.0f) {
        n = arc_points(&g, rough, cx, cy, rx, ry, strt, stp, inc, 1.5f, pts, 48);
        path_begin(&p);
        path_curve(&p, pts, n);
        path_stroke(c, &p, r, s->stroke, 0, 0);
    }
    if (closed) {
        rough_edge(c, &g, s, cx, cy, cx + rx * cosf(strt), cy + ry * sinf(strt), 0);
        rough_edge(c, &g, s, cx, cy, cx + rx * cosf(stp), cy + ry * sinf(stp), 0);
    }
}

/* ======================================================================
 * 自由笔触
 * ====================================================================== */

void sk_brush_stroke(sk_canvas *c, const sk_pointf *pts, int n, float w0, float w1,
                     sk_color color, int pencil, uint32_t seed)
{
    if (!c || !pts || n <= 0 || SK_A(color) == 0 || !pts_ok(pts, n))
        return;
    if (!(w0 > 0.0f)) w0 = 1.0f;
    if (!(w1 > 0.0f)) w1 = w0;
    if (w0 > 400.0f) w0 = 400.0f;
    if (w1 > 400.0f) w1 = 400.0f;
    float x0, y0, x1, y1;
    poly_bbox(pts, n, &x0, &y0, &x1, &y1);
    /* Catmull-Rom 曲线可能比点的包围盒稍微鼓出去一点，多留些余量 */
    float pad = fmaxf(w0, w1) * 0.5f + 2.0f + 0.15f * fmaxf(x1 - x0, y1 - y0);
    sk__stroke st;
    if (!sk__stroke_begin(&st, c, x0 - pad, y0 - pad, x1 + pad, y1 + pad))
        return;
    int grain = pencil ? 2 : 0;
    if (n == 1) {
        sk__stroke_dot(&st, pts[0].x, pts[0].y, w0 * 0.5f);
        sk__stroke_end(&st, color, grain, seed);
        return;
    }
    float total = 0.0f;
    for (int i = 0; i + 1 < n; i++)
        total += hypotf(pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y);
    sk__stroke_move(&st, pts[0].x, pts[0].y, w0 * 0.5f);
    float cum = 0.0f;
    for (int i = 0; i + 1 < n; i++) {
        sk_pointf pm = pts[i > 0 ? i - 1 : 0], p0 = pts[i], p1 = pts[i + 1], p2 = pts[i + 2 < n ? i + 2 : n - 1];
        sk_pointf b1 = P(p0.x + (p1.x - pm.x) / 6.0f, p0.y + (p1.y - pm.y) / 6.0f);
        sk_pointf b2 = P(p1.x - (p2.x - p0.x) / 6.0f, p1.y - (p2.y - p0.y) / 6.0f);
        cum += hypotf(p1.x - p0.x, p1.y - p0.y);
        float t = total > 0.0f ? cum / total : 1.0f;
        sk__stroke_bezier(&st, b1.x, b1.y, b2.x, b2.y, p1.x, p1.y, (w0 + (w1 - w0) * t) * 0.5f);
    }
    sk__stroke_end(&st, color, grain, seed);
}

void sk_rough_underline(sk_canvas *c, float x1, float x2, float y, sk_color color, float width, uint32_t seed)
{
    if (!c || !COORD_OK(x1) || !COORD_OK(x2) || !COORD_OK(y))
        return;
    if (x2 < x1) { float t = x1; x1 = x2; x2 = t; }
    float len = x2 - x1;
    if (len < 1.0f)
        return;
    if (!(width > 0.0f)) width = 1.6f;
    sk_rng g;
    sk_rng_seed(&g, seed);
    int m = (int)(len / 36.0f) + 2;
    if (m > 24) m = 24;
    sk_pointf pts[26];
    float wave = fminf(1.1f, len * 0.02f);
    float lift = fminf(2.6f, len * 0.05f);
    pts[0] = P(x1 - 1.0f + roff(&g, 0.8f, 1.0f), y + roff(&g, wave * 0.5f, 1.0f));
    for (int i = 1; i < m; i++) {
        float t = (float)i / (float)m;
        /* 越往后越往上抬一点：人写完字划线时手腕会自然上扬 */
        pts[i] = P(x1 + len * t + roff(&g, len / (float)m * 0.15f, 1.0f),
                   y + roff(&g, wave, 1.0f) - lift * t * t * 0.4f);
    }
    pts[m] = P(x2 + 1.5f, y - lift);
    sk_brush_stroke(c, pts, m + 1, width * 1.05f, width * 0.6f, color, 0, seed);
}

/* 荧光笔高亮：一道两端斜切（凿形笔头）、上下边缘略有起伏的半透明色带 */
void sk_marker_highlight(sk_canvas *c, sk_rect r, sk_color color, uint32_t seed)
{
    if (!c || sk_rect_empty(r) || SK_A(color) == 0)
        return;
    sk_rect old = sk_push_clip(c, r);
    if (sk_rect_empty(c->clip)) {
        sk_pop_clip(c, old);
        return;
    }
    sk_rng g;
    sk_rng_seed(&g, seed);
    float h = (float)r.h, w = (float)r.w;
    int m = (int)(w / 28.0f) + 2;
    if (m > 40) m = 40;
    sk_pointf q[2 * 41 + 4];
    int n = 0;
    float slant = fminf(h * 0.22f, w * 0.15f);
    float top = (float)r.y + h * 0.10f, bot = (float)r.y + h * 0.92f;
    float wob = h * 0.05f;
    for (int i = 0; i <= m; i++) {
        float t = (float)i / (float)m;
        float x = (float)r.x + slant + (w - 1.5f * slant) * t;
        q[n++] = P(x, top + roff(&g, wob, 1.0f));
    }
    for (int i = m; i >= 0; i--) {
        float t = (float)i / (float)m;
        float x = (float)r.x + 0.5f * slant + (w - 1.5f * slant) * t;
        q[n++] = P(x, bot + roff(&g, wob, 1.0f));
    }
    sk_fill_polygon(c, q, n, color);
    sk_pop_clip(c, old);
}

/* ======================================================================
 * 纸张纹理
 *
 * 纹理来自一块 256×256 的“母版”（只生成一次）：细颗粒 + 两层平滑起伏（纸浆不均匀）
 * + 一些短纤维。不同的 seed 只是在母版上取不同的偏移位置。所以每个像素的颜色只由
 * (seed, x, y) 决定，和这次画的矩形多大、从哪开始都无关——局部重绘不会出现“补丁”。
 * 母版四边无缝衔接，256 像素的重复周期配上很淡的颗粒，人眼看不出来。
 * ====================================================================== */

#define PAPER_N 256
static uint8_t *g_paper;     /* 每个纹素 0..63：这一点纸面比底色“暗”多少 */

/* 周期性的平滑值噪声：cell 像素一个随机格点，双线性 + smoothstep 插值，首尾相接 */
static float value_noise(int x, int y, int cell, uint32_t salt)
{
    int cells = PAPER_N / cell;
    int gx = x / cell, gy = y / cell;
    float fx = (float)(x % cell) / (float)cell, fy = (float)(y % cell) / (float)cell;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    float v[4];
    for (int k = 0; k < 4; k++) {
        int ix = (gx + (k & 1)) % cells, iy = (gy + (k >> 1)) % cells;
        v[k] = (float)(sk__hash3((uint32_t)ix, (uint32_t)iy, salt) & 0xFFFF) / 65535.0f;
    }
    float a = v[0] + (v[1] - v[0]) * fx, b = v[2] + (v[3] - v[2]) * fx;
    return a + (b - a) * fy;
}

static const uint8_t *paper_tile(void)
{
    if (g_paper)
        return g_paper;
    uint8_t *t = malloc(PAPER_N * PAPER_N);
    if (!t)
        return NULL;
    for (int y = 0; y < PAPER_N; y++) {
        for (int x = 0; x < PAPER_N; x++) {
            uint32_t h = sk__hash3((uint32_t)x, (uint32_t)y, 0x9A9E5u);
            float fine = (float)((h & 0xFF) * ((h >> 8) & 0xFF)) / 65025.0f;    /* 偏小的随机 */
            float med = value_noise(x, y, 4, 0x11u);
            float big = value_noise(x, y, 32, 0x22u);
            float v = 0.50f * fine + 0.22f * med * med + 0.30f * big * big;
            int iv = (int)(v * 63.0f + 0.5f);
            t[y * PAPER_N + x] = (uint8_t)(iv > 63 ? 63 : iv);
        }
    }
    /* 纸纤维：一些很短的细线，比周围稍暗 */
    sk_rng g;
    sk_rng_seed(&g, 0xF1BE5u);
    for (int i = 0; i < 90; i++) {
        float x = sk_rng_float(&g) * PAPER_N, y = sk_rng_float(&g) * PAPER_N;
        float a = sk_rng_float(&g) * 2.0f * SK_PI;
        float len = 3.0f + sk_rng_float(&g) * 9.0f;
        int dark = 26 + (int)(sk_rng_float(&g) * 22.0f);
        float bend = roff(&g, 0.08f, 1.0f);      /* 纤维微微弯曲 */
        for (float s = 0.0f; s < len; s += 0.7f) {
            int px = ((int)(x + cosf(a) * s) % PAPER_N + PAPER_N) % PAPER_N;
            int py = ((int)(y + sinf(a) * s) % PAPER_N + PAPER_N) % PAPER_N;
            uint8_t *p = &t[py * PAPER_N + px];
            if (*p < dark) *p = (uint8_t)dark;
            a += bend;
        }
    }
    g_paper = t;
    return t;
}

void sk_paper_fill(sk_canvas *c, sk_rect r, sk_color base, int strength, uint32_t seed)
{
    sk__paper_fill_at(c, r, base, strength, seed, 0, 0);
}

void sk__paper_fill_at(sk_canvas *c, sk_rect r, sk_color base, int strength, uint32_t seed, int ox, int oy)
{
    if (!c || !c->pixels)
        return;
    sk_rect d = sk_rect_intersect(r, c->clip);
    if (sk_rect_empty(d))
        return;
    base |= 0xFF000000u;
    if (strength > 20) strength = 20;
    const uint8_t *tile = strength > 0 ? paper_tile() : NULL;
    if (!tile) {
        sk_fill_rect(c, d, base);
        return;
    }
    /* 64 级颜色表：暗的程度 0..strength（在 255 级里），偏暖（蓝色降得多一点，像旧纸） */
    uint32_t lut[64];
    for (int i = 0; i < 64; i++) {
        int k = (i * strength * 8 + 31) / 63;            /* 1/8 级精度 */
        int rr = (int)SK_R(base) - (k * 7) / 64;
        int gg = (int)SK_G(base) - (k * 8) / 64;
        int bb = (int)SK_B(base) - (k * 10) / 64;
        if (rr < 0) rr = 0;
        if (gg < 0) gg = 0;
        if (bb < 0) bb = 0;
        lut[i] = SK_RGB(rr, gg, bb);
    }
    /* seed 决定在母版上从哪里开始取；(ox,oy) 是纹理坐标系的原点。用无符号数做加减，
     * 负数会自然地按 2 的补码回绕，再 & 255 就是正确的周期位置。 */
    uint32_t h = sk_hash_u32(seed, 0x9A9E2u);
    unsigned sx = (h & (PAPER_N - 1)) - (unsigned)ox, sy = ((h >> 8) & (PAPER_N - 1)) - (unsigned)oy;
    for (int y = d.y; y < d.y + d.h; y++) {
        const uint8_t *trow = tile + (((unsigned)y + sy) & (PAPER_N - 1)) * PAPER_N;
        uint32_t *p = c->pixels + ((intptr_t)y * c->stride + d.x);
        unsigned tx = ((unsigned)d.x + sx) & (PAPER_N - 1);
        for (int i = 0; i < d.w; i++) {
            p[i] = lut[trow[tx]];
            tx = (tx + 1) & (PAPER_N - 1);
        }
    }
}
