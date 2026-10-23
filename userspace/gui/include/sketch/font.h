/*
 * sketch/font.h — 位图字体 (libsketch)
 *
 * 字体来源：霞鹜文楷 LXGW WenKai（SIL OFL 1.1，手写楷体风格，含中文）。
 * 构建时 tools/mkfont.py 用 FreeType(Pillow) 把 TTF 预先光栅化成 8 位灰度位图，
 * 存成 SketchOS 自己的 .skf 格式，放在 /usr/share/fonts/。运行时 mmap 进来直接用，
 * 系统里不需要 FreeType。
 *
 * .skf 文件格式（全部小端）：
 *   struct skf_header {            // 32 字节
 *       char     magic[4];         // "SKF1"
 *       uint16_t pixel_size;       // 生成时的字号（像素）
 *       int16_t  ascent;           // 基线到最高点（正数）
 *       int16_t  descent;          // 基线到最低点（正数）
 *       uint16_t line_height;      // 推荐行高
 *       uint32_t glyph_count;
 *       uint32_t index_offset;     // glyph 索引表在文件中的偏移
 *       uint32_t bitmap_offset;    // 位图数据区偏移
 *       uint16_t flags;            // bit0: 等宽字体
 *       uint16_t cell_width;       // 等宽字体的半角宽度（ASCII 宽度），否则 0
 *       uint32_t reserved;
 *   };
 *   struct skf_glyph {             // 16 字节，按 codepoint 升序排列（二分查找）
 *       uint32_t codepoint;
 *       uint32_t bitmap;           // 相对 bitmap_offset 的偏移
 *       uint8_t  w, h;             // 位图尺寸
 *       int8_t   left;             // 笔位到位图左边的距离
 *       int8_t   top;              // 基线到位图顶边的距离（向上为正）
 *       uint8_t  advance;          // 前进宽度
 *       uint8_t  pad[3];
 *   };
 *   位图：w*h 字节，每字节是覆盖率 0..255，行优先，无填充。
 */
#ifndef SKETCH_FONT_H
#define SKETCH_FONT_H

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sk_font sk_font;

/* 系统字体（路径固定，sk_font_get 第一次调用时加载；离线测试可用环境变量
 * SKETCH_FONT_DIR 覆盖目录，默认 /usr/share/fonts） */
enum sk_font_id {
    SK_FONT_UI = 0,     /* 正文 16px:  sans-16.skf */
    SK_FONT_TITLE,      /* 标题/按钮 20px: sans-20.skf */
    SK_FONT_LARGE,      /* 大字 32px:  sans-32.skf（启动画面、时钟） */
    SK_FONT_MONO,       /* 终端等宽 16px: mono-16.skf */
    SK_FONT_COUNT
};
/* 取系统字体；加载失败时返回内置的 8x16 后备字体（只含 ASCII），永远不返回 NULL */
sk_font *sk_font_get(int id);

sk_font *sk_font_load(const char *path);     /* 失败返回 NULL */
void     sk_font_free(sk_font *f);
/* 缺字时去 fallback 里找；都没有就画一个方框 */
void     sk_font_set_fallback(sk_font *f, sk_font *fallback);

int sk_font_ascent(const sk_font *f);
int sk_font_descent(const sk_font *f);
int sk_font_line_height(const sk_font *f);
int sk_font_is_mono(const sk_font *f);
int sk_font_cell_width(const sk_font *f);    /* 等宽字体的半角宽度；非等宽返回平均宽度 */

/* ---------- UTF-8 ---------- */
/* 从 *s 解码一个码点并前移指针；遇到非法字节返回 0xFFFD 并前移 1 字节；到末尾返回 0 */
uint32_t sk_utf8_next(const char **s);
/* 把码点编码进 out（至少 4 字节），返回字节数 */
int      sk_utf8_encode(uint32_t cp, char *out);
/* 字符串中的码点个数 */
int      sk_utf8_len(const char *s);
/* 码点在终端里占几格：CJK/全角为 2，组合字符 0，其它 1 */
int      sk_wcwidth(uint32_t cp);

/* ---------- 测量与绘制 ---------- */
int sk_glyph_advance(const sk_font *f, uint32_t cp);
int sk_text_width(const sk_font *f, const char *utf8);
int sk_text_width_n(const sk_font *f, const char *utf8, int nbytes);
/* 在基线 (x, baseline) 处绘制文字，返回绘制结束时的 x */
int sk_draw_text(sk_canvas *c, const sk_font *f, int x, int baseline, const char *utf8, sk_color color);
int sk_draw_text_n(sk_canvas *c, const sk_font *f, int x, int baseline, const char *utf8, int nbytes, sk_color color);
/* 绘制单个码点（终端按格子画字用），返回 advance */
int sk_draw_glyph(sk_canvas *c, const sk_font *f, int x, int baseline, uint32_t cp, sk_color color);

enum sk_align { SK_ALIGN_LEFT = 0, SK_ALIGN_CENTER = 1, SK_ALIGN_RIGHT = 2 };
/* 在矩形 r 内按水平对齐方式画一行文字，垂直居中；过长时用 “…” 截断 */
void sk_draw_text_in_rect(sk_canvas *c, const sk_font *f, sk_rect r, const char *utf8,
                          sk_color color, int align);
/* 自动换行（中文按字、英文按词），从 r.y 开始逐行绘制，返回用掉的高度。
 * 只想测量时 c 传 NULL。 */
int  sk_draw_text_wrapped(sk_canvas *c, const sk_font *f, sk_rect r, const char *utf8,
                          sk_color color, int line_gap);

#ifdef __cplusplus
}
#endif
#endif /* SKETCH_FONT_H */
