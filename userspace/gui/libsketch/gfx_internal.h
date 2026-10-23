/*
 * gfx_internal.h — libsketch 图形核心的内部接口（不是公共契约，应用不要包含它）
 *
 * gfx.c 实现，rough.c / theme.c / icons.c 共用：
 *   1. 像素混合的内联函数（整数运算，热路径用）；
 *   2. 按槽位复用的临时缓冲区（避免在绘制热路径里反复 malloc/free）；
 *   3. “笔触累积器”：手绘线条的光栅化引擎。
 *
 * 为什么需要“笔触累积器”？
 *   一条手绘线是一条曲线，我们用一个抗锯齿的圆形“笔刷印章”沿着曲线密密地盖章。
 *   如果每个章都直接和画布混合，相邻的章互相重叠，半透明的边缘会被叠加好几次，
 *   线条边缘变硬、半透明颜色出现一串“珠子”。所以先把整条笔画盖到一张 8 位覆盖率
 *   蒙版上（每个像素取所有章的最大值），整条笔画画完后再一次性按覆盖率混合到画布上。
 *   这样同一笔画内部永远只混合一次——和真实的笔在纸上划过一次的效果一致；
 *   而两笔之间（比如“描两遍”）会自然叠加变深，正像铅笔来回描。
 */
#ifndef SKETCH_GFX_INTERNAL_H
#define SKETCH_GFX_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "sketch/gfx.h"

/* ---------- 像素混合 ---------- */

/*
 * source-over 混合：把颜色 s（只用它的 RGB）以不透明度 a (0..255) 盖到 d 上，结果不透明。
 *   out = s*a + d*(1-a)
 * 为了不做除法，把 a 映射到 0..256（a + (a>>7)），乘完右移 8 位；再利用“R 和 B 在
 * 32 位里相隔 16 位、互不进位”的老技巧，一次乘法同时算 R、B 两个通道。
 * 目标像素的 alpha 不参与计算（gfx.h 的约定：内容缓冲区忽略 alpha；边框层的 alpha 只表示
 * “有没有画过”）。所以在透明图层上画画之前，应该先把 RGB 填成“将来会露出来的底色”
 * （sketchd 用 SK_WITH_ALPHA(SK_DESK_PAPER, 0) 清屏），抗锯齿边缘就会和那个颜色混合。
 */
static inline uint32_t sk__over(uint32_t d, uint32_t s, uint32_t a)
{
    if (a == 0)
        return d;
    if (a >= 255)
        return s | 0xFF000000u;
    uint32_t a2 = a + (a >> 7);
    uint32_t ia = 256 - a2;
    uint32_t rb = ((s & 0xFF00FFu) * a2 + (d & 0xFF00FFu) * ia) >> 8;
    uint32_t g  = ((s & 0x00FF00u) * a2 + (d & 0x00FF00u) * ia) >> 8;
    return 0xFF000000u | (rb & 0xFF00FFu) | (g & 0x00FF00u);
}

/* 纸张纹理的内部版本：颗粒坐标 = (x - ox, y - oy)。窗口边框用内容区左上角作原点，
 * 这样无论边框画在哪块画布上（sketchd 把边框拆成四条小画布）、窗口移到哪里，纹理都连续。 */
void sk__paper_fill_at(sk_canvas *c, sk_rect r, sk_color base, int strength, uint32_t seed, int ox, int oy);

/* 两个 0..255 的数相乘再除以 255（四舍五入），整数实现 */
static inline uint32_t sk__mul255(uint32_t a, uint32_t b)
{
    uint32_t t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

/* ---------- 临时缓冲区 ----------
 * 每个槽位一块只增不减的内存，绘制函数用完不释放，下次直接复用。
 * 同一时刻同一槽位只能有一个使用者，所以不同模块用不同槽位。失败返回 NULL。 */
enum {
    SK__SCRATCH_POLY = 0,   /* sk_fill_polygon：边表 + 行覆盖率 */
    SK__SCRATCH_SPAN,       /* 笔触累积器：每行的脏列范围 */
    SK__SCRATCH_PTS,        /* rough.c：轮廓点、曲线控制点 */
    SK__SCRATCH_XS,         /* rough.c：排线与多边形的交点 */
    SK__SCRATCH_FILLPTS,    /* rough.c：填充多边形的点 */
    SK__SCRATCH_PATH,       /* rough.c：一笔里的贝塞尔曲线段 */
    SK__SCRATCH_COUNT
};
void *sk__scratch(int slot, size_t bytes);

/* ---------- 笔触累积器 ---------- */
typedef struct sk__stroke {
    sk_canvas *c;
    sk_rect    r;         /* 蒙版对应的画布区域（画布坐标，⊆ c->clip） */
    uint8_t   *m;         /* 覆盖率蒙版，行距 r.w，用完必须清零 */
    int       *lo, *hi;   /* 每行写过的列范围（相对 r.x），lo>hi 表示这一行没写过 */
    float      px, py, pr;/* 当前笔尖位置和半径 */
    int        have_pt;   /* 是否已有当前点 */
    float      maxr;      /* 本次笔触见过的最大半径（用于线段裁剪外扩） */
    int        gx, gy;    /* 颗粒纹理的原点（默认画布原点；填充用图形自己的顶点，跟着图形平移） */
} sk__stroke;

/* 开始一笔：(x0,y0)-(x1,y1) 是这一笔可能碰到的范围（已经包含笔宽）。
 * 返回 0 表示完全落在裁剪区外（或内存不足），这一笔可以直接跳过，不必调用 end。 */
int  sk__stroke_begin(sk__stroke *st, sk_canvas *c, float x0, float y0, float x1, float y1);
/* 抬笔移动到 (x,y)，并在那里盖一个半径 r 的章 */
void sk__stroke_move(sk__stroke *st, float x, float y, float r);
/* 从当前点画直线到 (x,y)，半径从当前半径线性变到 r */
void sk__stroke_line(sk__stroke *st, float x, float y, float r);
/* 从当前点画三次贝塞尔曲线（控制点 1、2，终点 3），按长度自适应细分，半径渐变到 r */
void sk__stroke_bezier(sk__stroke *st, float x1, float y1, float x2, float y2,
                       float x3, float y3, float r);
/* 把笔尖挪到 (x,y) 但不盖章（那里之前已经画过，接着从那儿继续画） */
void sk__stroke_jump(sk__stroke *st, float x, float y, float r);
/* 只盖一个章（点画填充用），不改变当前点 */
void sk__stroke_dot(sk__stroke *st, float x, float y, float r);
/* 把蒙版按覆盖率混合到画布并清零蒙版。
 * grain: 0=无颗粒 1=彩色铅笔/蜡笔的轻微颗粒 2=铅笔颗粒（较明显）；颗粒只由 (seed,x,y) 决定 */
void sk__stroke_end(sk__stroke *st, sk_color color, int grain, uint32_t seed);

/* 坐标“哈希噪声”：同样的 (x, y, seed) 永远得到同样的 32 位值（纸张颗粒、铅笔颗粒用） */
static inline uint32_t sk__hash3(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

#endif /* SKETCH_GFX_INTERNAL_H */
