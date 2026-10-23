/*
 * sketch/gfx.h — SketchOS 基础光栅图形 (libsketch)
 *
 * 一切绘制都发生在 sk_canvas 上：一块 32 位像素内存 + 裁剪矩形。
 * 像素格式固定为 0xAARRGGBB（小端内存里是 B,G,R,A），与 DRM 的
 * XRGB8888 / ARGB8888 一致，合成到屏幕时可以直接 memcpy。
 *
 * 约定：
 *   - 坐标单位是像素，原点在左上角，x 向右，y 向下。
 *   - 所有绘制函数都遵守 canvas->clip，越界自动裁剪，永远不会写出缓冲区。
 *   - 颜色的 alpha < 255 时做 "source-over" 混合；alpha == 255 直接覆盖。
 *   - 画布本身的 alpha 通道：窗口内容缓冲区忽略 alpha（当作不透明）；
 *     sketchd 的窗口边框层用 alpha==0 表示透明（只有 0/非 0 两种，便于快速合成）。
 *   - 这个库不依赖任何操作系统接口，可以在宿主机上编译做离线渲染测试。
 */
#ifndef SKETCH_GFX_H
#define SKETCH_GFX_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t sk_color;           /* 0xAARRGGBB */

#define SK_RGB(r, g, b)      ((sk_color)(0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define SK_RGBA(r, g, b, a)  ((sk_color)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define SK_A(c) (((c) >> 24) & 0xFF)
#define SK_R(c) (((c) >> 16) & 0xFF)
#define SK_G(c) (((c) >> 8) & 0xFF)
#define SK_B(c) ((c) & 0xFF)
/* 把颜色 c 的 alpha 替换成 a */
#define SK_WITH_ALPHA(c, a)  (((c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

typedef struct { int x, y, w, h; } sk_rect;
typedef struct { float x, y; } sk_pointf;

typedef struct sk_canvas {
    uint32_t *pixels;   /* 第 0 行第 0 列 */
    int w, h;           /* 尺寸（像素） */
    int stride;         /* 每行像素数（不是字节数），>= w */
    sk_rect clip;       /* 当前裁剪区域，总是 ⊆ (0,0,w,h) */
} sk_canvas;

/* ---------- 矩形运算 ---------- */
static inline sk_rect sk_rect_make(int x, int y, int w, int h) { sk_rect r = { x, y, w, h }; return r; }
int     sk_rect_empty(sk_rect r);                       /* w<=0 || h<=0 */
sk_rect sk_rect_intersect(sk_rect a, sk_rect b);        /* 无交集时返回 w=h=0 */
sk_rect sk_rect_union(sk_rect a, sk_rect b);            /* 包围盒；空矩形不参与 */
int     sk_rect_contains(sk_rect r, int x, int y);
sk_rect sk_rect_inset(sk_rect r, int dx, int dy);       /* 四边各缩进 dx/dy（负数=外扩） */
sk_rect sk_rect_offset(sk_rect r, int dx, int dy);

/* ---------- 画布 ---------- */
/* 用已有内存初始化画布（不分配内存）。stride 以像素计。 */
void sk_canvas_init(sk_canvas *c, uint32_t *pixels, int w, int h, int stride);
/* 分配一块新画布（malloc），失败返回 NULL。用 sk_canvas_free 释放。 */
sk_canvas *sk_canvas_new(int w, int h);
void sk_canvas_free(sk_canvas *c);
/* 子画布：与 parent 共享像素，原点移到 r 的左上角，clip 为 r∩parent->clip（转成子坐标） */
void sk_canvas_sub(sk_canvas *sub, const sk_canvas *parent, sk_rect r);

void    sk_set_clip(sk_canvas *c, sk_rect r);   /* clip = r ∩ 画布范围 */
void    sk_reset_clip(sk_canvas *c);            /* clip = 整个画布 */
sk_rect sk_push_clip(sk_canvas *c, sk_rect r);  /* clip = clip ∩ r，返回旧 clip */
void    sk_pop_clip(sk_canvas *c, sk_rect old); /* 恢复 */

/* ---------- 颜色 ---------- */
sk_color sk_blend(sk_color dst, sk_color src);          /* source-over，结果 alpha=255 */
sk_color sk_mix(sk_color a, sk_color b, int t);         /* 线性插值，t: 0..255 (0=a) */
sk_color sk_lighten(sk_color c, int amount);            /* 向白色混合 amount/255 */
sk_color sk_darken(sk_color c, int amount);             /* 向黑色混合 amount/255 */

/* ---------- 基本绘制（精确、无手绘抖动） ---------- */
void sk_clear(sk_canvas *c, sk_color color);                       /* 填满整个 clip，忽略 alpha（直接写入） */
void sk_fill_rect(sk_canvas *c, sk_rect r, sk_color color);        /* alpha<255 时混合 */
void sk_put_pixel(sk_canvas *c, int x, int y, sk_color color);     /* 混合 */
/* 覆盖率混合：在 (x,y) 以 color 混合，额外乘以覆盖率 cov (0..255)。抗锯齿的基础。 */
void sk_blend_pixel_cov(sk_canvas *c, int x, int y, sk_color color, int cov);
void sk_hline(sk_canvas *c, int x0, int x1, int y, sk_color color);
void sk_vline(sk_canvas *c, int x, int y0, int y1, sk_color color);
void sk_rect_outline(sk_canvas *c, sk_rect r, sk_color color);     /* 1px 边框 */
/* 抗锯齿直线，任意粗细（>=0.5），圆头 */
void sk_line_aa(sk_canvas *c, float x0, float y0, float x1, float y1, float width, sk_color color);
/* 抗锯齿实心圆 / 圆环 */
void sk_fill_circle_aa(sk_canvas *c, float cx, float cy, float r, sk_color color);
void sk_circle_aa(sk_canvas *c, float cx, float cy, float r, float width, sk_color color);
/* 实心多边形（偶奇规则，扫描线，边缘 4x 纵向超采样抗锯齿） */
void sk_fill_polygon(sk_canvas *c, const sk_pointf *pts, int n, sk_color color);

/* 拷贝 src 的 sr 区域到 dst 的 (dx,dy)，不透明直接覆盖（忽略 alpha） */
void sk_blit(sk_canvas *dst, int dx, int dy, const sk_canvas *src, sk_rect sr);
/* 同上，但跳过 alpha==0 的像素（“抠像”合成，sketchd 边框层用） */
void sk_blit_keyed(sk_canvas *dst, int dx, int dy, const sk_canvas *src, sk_rect sr);
/* 同上，按每个像素的 alpha 做完整混合 */
void sk_blit_alpha(sk_canvas *dst, int dx, int dy, const sk_canvas *src, sk_rect sr);
/* 用 8 位 alpha 蒙版着色：mask[y*mask_stride+x] 是覆盖率。字形渲染就是它。 */
void sk_draw_mask(sk_canvas *c, int x, int y, const uint8_t *mask, int mw, int mh,
                  int mask_stride, sk_color color);
/* 最近邻缩放拷贝（画板缩略图、图标缩放用） */
void sk_blit_scaled(sk_canvas *dst, sk_rect dr, const sk_canvas *src, sk_rect sr);

/* ---------- 图像读写（PPM P6，便于调试和“画板”保存） ---------- */
/* 读 P6 PPM，返回新画布；失败 NULL。 */
sk_canvas *sk_image_load_ppm(const char *path);
/* 写 P6 PPM（不含 alpha）；成功返回 0。 */
int sk_image_save_ppm(const sk_canvas *c, const char *path);

#ifdef __cplusplus
}
#endif
#endif /* SKETCH_GFX_H */
