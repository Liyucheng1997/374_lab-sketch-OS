/*
 * sketch/rough.h — 手绘风格图元 (libsketch)
 *
 * 这是 SketchOS “手绘外观”的核心。算法思路参考 rough.js（MIT）：
 *   - 直线：不是一条精确的线，而是一条两端略微偏移、中间轻微弯曲（bowing）
 *     的三次贝塞尔曲线；double_stroke 时再用不同随机数画第二遍，形成铅笔“描两次”的效果。
 *   - 矩形 / 多边形：每条边各自是一条手绘线，并在角上稍微“出头”（overshoot）。
 *   - 椭圆：沿椭圆取若干点，对半径做随机扰动，起点随机，首尾重叠一小段，
 *     用 Catmull-Rom 样条连成闭合曲线。
 *   - 填充：hachure（斜线排线）、cross-hatch（交叉排线）、zigzag、dots、
 *     solid（边缘不规则的实心）、marker（半透明荧光笔涂抹）。
 *
 * 确定性：所有随机都来自 style.seed，同一个 seed 每次画出来完全一样，
 * 所以界面重绘时不会“抖动”。不同控件请用不同 seed（例如 sk_hash_str(id)）。
 *
 * 性能（v86 网页模拟器里 CPU 很慢）：
 *   - 笔触用“预计算的圆形抗锯齿笔刷印章”沿曲线按 ~0.6*宽度 的间距盖章，
 *     或用 sk_line_aa 分段绘制；曲线按长度自适应分段（约每 3px 一段）。
 *   - 排线填充的每条线只画一遍（不 double stroke）。
 */
#ifndef SKETCH_ROUGH_H
#define SKETCH_ROUGH_H

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 确定性随机数 ---------- */
typedef struct { uint32_t s; } sk_rng;
void     sk_rng_seed(sk_rng *r, uint32_t seed);   /* seed==0 也要能用 */
uint32_t sk_rng_u32(sk_rng *r);                   /* xorshift32 / PCG 均可 */
float    sk_rng_float(sk_rng *r);                 /* [0,1) */
float    sk_rng_range(sk_rng *r, float lo, float hi);
uint32_t sk_hash_str(const char *s);              /* FNV-1a，用于从字符串得到 seed */
uint32_t sk_hash_u32(uint32_t a, uint32_t b);     /* 混合两个整数得到 seed */

/* ---------- 样式 ---------- */
enum sk_fill_style {
    SK_FILL_NONE = 0,
    SK_FILL_SOLID,        /* 实心，边缘跟随手绘轮廓 */
    SK_FILL_HACHURE,      /* 平行斜线排线 */
    SK_FILL_CROSS_HATCH,  /* 交叉排线 */
    SK_FILL_ZIGZAG,       /* 来回折线 */
    SK_FILL_DOTS,         /* 点 */
    SK_FILL_MARKER,       /* 荧光笔：粗的半透明笔触来回涂 */
};

typedef struct sk_rough_style {
    float    roughness;       /* 0=完全规整, 1=默认, 2=很潦草 */
    float    bowing;          /* 线条弯曲程度，默认 1 */
    float    stroke_width;    /* 笔触宽度，默认 1.6；<=0 表示不画轮廓 */
    sk_color stroke;          /* 笔触颜色（可带 alpha） */
    int      double_stroke;   /* 1=描两遍（默认），0=只描一遍 */
    int      fill_style;      /* enum sk_fill_style */
    sk_color fill;            /* 填充颜色 */
    float    hachure_angle;   /* 排线角度（度），默认 -41 */
    float    hachure_gap;     /* 排线间距，默认 6 */
    float    fill_weight;     /* 排线笔触宽度，默认 1.2 */
    uint32_t seed;            /* 随机种子 */
} sk_rough_style;

/* 默认样式：ink 黑色笔触、无填充、roughness=1、seed=1 */
void sk_rough_default(sk_rough_style *s);

/* ---------- 图元 ---------- */
void sk_rough_line(sk_canvas *c, float x1, float y1, float x2, float y2, const sk_rough_style *s);
void sk_rough_rect(sk_canvas *c, float x, float y, float w, float h, const sk_rough_style *s);
/* 圆角矩形（r=圆角半径） */
void sk_rough_round_rect(sk_canvas *c, float x, float y, float w, float h, float r, const sk_rough_style *s);
/* 椭圆：中心 (cx,cy)，直径 w,h */
void sk_rough_ellipse(sk_canvas *c, float cx, float cy, float w, float h, const sk_rough_style *s);
void sk_rough_circle(sk_canvas *c, float cx, float cy, float diameter, const sk_rough_style *s);
/* 闭合多边形 */
void sk_rough_polygon(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s);
/* 不闭合折线（每段一条手绘线） */
void sk_rough_polyline(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s);
/* 平滑曲线：Catmull-Rom 样条穿过所有点，带轻微抖动 */
void sk_rough_curve(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s);
/* 圆弧：角度单位为弧度，0 指向 +x，顺时针为正（屏幕坐标系）。closed=1 时画成扇形 */
void sk_rough_arc(sk_canvas *c, float cx, float cy, float w, float h,
                  float start, float stop, int closed, const sk_rough_style *s);

/* 只做填充（不画轮廓）：对任意多边形做 hachure/solid 等填充。轮廓用 sk_rough_polygon。 */
void sk_rough_fill_polygon(sk_canvas *c, const sk_pointf *pts, int n, const sk_rough_style *s);

/* ---------- 自由笔触（画板、签名式文字下划线等） ---------- */
/* 一条连续手绘笔画：穿过 pts 的平滑曲线，宽度从 w0 渐变到 w1（模拟起笔/收笔），
 * pencil=1 时加一点颗粒感（按 seed 稀疏降低 alpha，像铅笔在纸上）。 */
void sk_brush_stroke(sk_canvas *c, const sk_pointf *pts, int n, float w0, float w1,
                     sk_color color, int pencil, uint32_t seed);

/* 手绘下划线 / 删除线：一条略带波浪、末端轻微上挑的线 */
void sk_rough_underline(sk_canvas *c, float x1, float x2, float y, sk_color color, float width, uint32_t seed);

/* 荧光笔高亮：在矩形区域涂一道半透明的宽笔触（选中文字的背景） */
void sk_marker_highlight(sk_canvas *c, sk_rect r, sk_color color, uint32_t seed);

/* 纸张纹理：在 r 内填充底色 base，并叠加确定性的细微颗粒噪点（strength 0..20）。
 * 噪点只由 (seed, 像素坐标 x, y) 决定（坐标相对画布原点），所以只重绘一个小矩形时
 * 结果与整块重绘完全一致——局部刷新不会出现“补丁”。 */
void sk_paper_fill(sk_canvas *c, sk_rect r, sk_color base, int strength, uint32_t seed);

#ifdef __cplusplus
}
#endif
#endif /* SKETCH_ROUGH_H */
