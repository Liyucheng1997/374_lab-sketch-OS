/*
 * skpaint.h — 画板内部各模块之间的接口（只给 skpaint 自己的 .c 文件用）
 *
 *   main.c   窗口、控件、鼠标键盘、打开/保存/新建的流程
 *   draw.c   画图工具的“笔”：铅笔线、圆头刷子、直线/矩形/椭圆、油漆桶填充
 *   undo.c   撤销/重做：按 64×64 的小块做“写时复制”（和内核 fork 的写时复制同一个思路）
 *   pnm.c    读写 Netpbm 图片：PPM(P3/P6)、PGM(P2/P5)、PBM(P1/P4)
 *
 * 坐标约定：本文件里的坐标都是“图片坐标”——以画布左上角为原点、单位是图片像素；
 * 换算成窗口坐标要加上画布在窗口里的位置（main.c 的 ox/oy）。
 */
#ifndef SKPAINT_H
#define SKPAINT_H

#include <sketch/gfx.h>

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- 工具 */

enum paint_tool {
    TOOL_PENCIL = 0,    /* 铅笔：1 像素硬边线 */
    TOOL_BRUSH,         /* 刷子：圆头、抗锯齿，粗细由滑块决定 */
    TOOL_ERASER,        /* 橡皮：用画布底色（白色）画的刷子 */
    TOOL_LINE,          /* 直线 */
    TOOL_RECT,          /* 矩形 */
    TOOL_ELLIPSE,       /* 椭圆 */
    TOOL_FILL,          /* 油漆桶（区域填充） */
    TOOL_COUNT
};

#define PAINT_WHITE SK_RGB(0xFF, 0xFF, 0xFF)   /* 画布的底色，也是橡皮的颜色 */

/* ---------------------------------------------------------------- draw.c */

/* 1 像素宽的硬边直线（Bresenham 算法），两端都包括。遵守 c->clip */
void draw_pencil_line(sk_canvas *c, int x0, int y0, int x1, int y1, sk_color color);
/* 圆头的抗锯齿粗线（刷子、橡皮）：从 (x0,y0) 到 (x1,y1)，宽 width 像素 */
void draw_brush_line(sk_canvas *c, float x0, float y0, float x1, float y1, float width, sk_color color);

/* 直线/矩形/椭圆：拖动时先预览，松开鼠标才真正画进图片 */
typedef struct {
    int      tool;              /* TOOL_LINE / TOOL_RECT / TOOL_ELLIPSE */
    int      x0, y0, x1, y1;    /* 按下点和当前点（图片坐标） */
    int      size;              /* 线宽 */
    sk_color color;
    int      rough;             /* 1 = 用 rough.h 的手绘风格 */
    uint32_t seed;              /* 手绘抖动的种子：同一个图形预览和定稿一模一样 */
} paint_shape;

/* 把图形画到 c 上；(ox, oy) 是图片原点在 c 里的位置（画进图片时为 0,0，预览时是画布在窗口里的位置） */
void    shape_draw(sk_canvas *c, const paint_shape *s, int ox, int oy);
/* 图形可能碰到的像素范围（图片坐标，留足了手绘抖动的余量） */
sk_rect shape_bounds(const paint_shape *s);

/* 油漆桶第一步：从 (x,y) 开始找出“和起点颜色相近、连成一片”的区域。
 * 返回 1 并给出 *mask（w*h 字节，1 = 要涂）和包围盒；0 = 不需要填；-1 = 内存不足 */
int  fill_compute(const sk_canvas *img, int x, int y, sk_color color, int tolerance,
                  uint8_t **mask, sk_rect *bbox);
/* 第二步：把 mask 里标记的像素涂成 color（调用者先把 bbox 交给撤销记录） */
void fill_apply(sk_canvas *img, const uint8_t *mask, sk_rect bbox, sk_color color);

/* ---------------------------------------------------------------- undo.c */

#define UNDO_TILE        64                   /* 块边长（像素） */
#define UNDO_MAX_LEVELS  64                   /* 最多保留多少步 */
#define UNDO_BUDGET      (20u * 1024 * 1024)  /* 撤销记录最多占用的内存（字节） */

typedef struct undo_op undo_op;

typedef struct {
    sk_canvas *img;             /* 被记录的图片 */
    int        tiles_x, tiles_y;
    uint8_t   *saved;           /* 当前操作里每个块是否已经保存过（tiles_x*tiles_y 个） */
    undo_op   *cur;             /* 正在进行的操作；NULL = 没有 */
    undo_op   *undo[UNDO_MAX_LEVELS];   /* [0] 最旧 */
    int        nundo;
    undo_op   *redo[UNDO_MAX_LEVELS];   /* [nredo-1] 是下一个要重做的 */
    int        nredo;
    size_t     bytes;           /* 所有记录占用的像素内存 */
    unsigned   version;         /* 当前图片内容的“版本号”（判断是否改过、是否需要保存） */
    unsigned   next_version;
    int        oom;             /* 当前操作中途内存不足 */
} undo_hist;

enum undo_result {
    UNDO_NOTHING = 0,   /* 这次操作没碰任何像素 */
    UNDO_RECORDED,      /* 记下了，可以撤销 */
    UNDO_TOO_BIG,       /* 这一步太大，超过内存预算，无法撤销（图片照样改了） */
    UNDO_NO_MEMORY,     /* 中途内存不足：撤销记录已清空 */
};

/* 换了一张图片（新建/打开）：清空记录，开始一个新版本。失败返回 -1 */
int  undo_reset(undo_hist *h, sk_canvas *img);
void undo_free(undo_hist *h);
void undo_begin(undo_hist *h);
/* 即将修改图片的 r 区域：把还没保存过的块先复制一份（必须在真正画之前调用） */
void undo_touch(undo_hist *h, sk_rect r);
int  undo_end(undo_hist *h);                 /* 返回 enum undo_result */
int  undo_in_progress(const undo_hist *h);
/* 撤销/重做一步：成功返回 1，并在 *changed 给出改动的范围（图片坐标） */
int  undo_undo(undo_hist *h, sk_rect *changed);
int  undo_redo(undo_hist *h, sk_rect *changed);

/* ---------------------------------------------------------------- pnm.c */

#define PNM_MAX_SIDE 4096    /* 能打开的图片最大边长 */

/* 读 PPM/PGM/PBM 图片（P1～P6），返回新画布（不透明）。失败返回 NULL，err 里是中文原因 */
sk_canvas *pnm_load(const char *path, char *err, size_t errlen);
/* 存成二进制 PPM（P6）：先写临时文件，再 rename() 换上去。成功返回 0，失败 -1（err 里是原因） */
int        pnm_save(const sk_canvas *c, const char *path, char *err, size_t errlen);

#endif /* SKPAINT_H */
