/*
 * sketch/theme.h — SketchOS 的视觉语言：调色板、标准控件外观、手绘图标
 *
 * 风格：“铅笔 + 彩色蜡笔 + 方格纸”。
 *   - 纸：暖白色，带极淡的颗粒；桌面是浅蓝色方格纸。
 *   - 墨：炭黑色铅笔线，略微潦草、描两遍。
 *   - 彩色：蜡笔/马克笔色，用排线(hachure)或荧光笔方式上色，而不是平涂。
 * 所有应用都通过这里的函数画标准元素，保证整个系统风格统一。
 * sketchd（窗口边框、任务栏）和 libsketch 的控件(ui.h)都用它。
 */
#ifndef SKETCH_THEME_H
#define SKETCH_THEME_H

#include "gfx.h"
#include "rough.h"
#include "font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 调色板 ---------- */
#define SK_PAPER         SK_RGB(0xFF, 0xFD, 0xF7)  /* 窗口内容的纸 */
#define SK_PAPER_DARK    SK_RGB(0xF4, 0xEF, 0xE1)  /* 次级面板、工具栏 */
#define SK_DESK_PAPER    SK_RGB(0xFB, 0xF8, 0xEF)  /* 桌面方格纸底色 */
#define SK_DESK_GRID     SK_RGB(0xDD, 0xE8, 0xF3)  /* 方格线 */
#define SK_INK           SK_RGB(0x2B, 0x2B, 0x2B)  /* 主墨色（铅笔） */
#define SK_INK_SOFT      SK_RGB(0x5C, 0x5C, 0x5C)  /* 次要文字 */
#define SK_INK_FAINT     SK_RGB(0xA0, 0xA0, 0xA0)  /* 禁用、提示文字 */
#define SK_SHADOW        SK_RGB(0xB4, 0xB4, 0xB4)  /* 排线阴影 */

#define SK_BLUE          SK_RGB(0x4A, 0x7B, 0xD0)
#define SK_RED           SK_RGB(0xE0, 0x60, 0x5A)
#define SK_GREEN         SK_RGB(0x5B, 0xAA, 0x62)
#define SK_YELLOW        SK_RGB(0xF2, 0xC9, 0x4C)
#define SK_ORANGE        SK_RGB(0xF2, 0x9E, 0x4C)
#define SK_PURPLE        SK_RGB(0x9B, 0x7B, 0xD4)
#define SK_PINK          SK_RGB(0xF2, 0x8D, 0xB2)
#define SK_TEAL          SK_RGB(0x3F, 0xB0, 0xA8)
#define SK_HIGHLIGHT     SK_RGBA(0xF7, 0xE4, 0x63, 0x90)  /* 荧光笔（选中） */
#define SK_FOCUS         SK_RGB(0x4A, 0x7B, 0xD0)         /* 焦点虚线 */

/* 终端 16 色（ANSI 0..15），在纸色背景上可读 */
extern const sk_color sk_ansi_palette[16];

/* ---------- 预设样式 ---------- */
/* 普通铅笔轮廓 */
void sk_style_pencil(sk_rough_style *s, uint32_t seed);
/* 排线填充 + 铅笔轮廓 */
void sk_style_hatched(sk_rough_style *s, sk_color fill, uint32_t seed);

/* ---------- 标准控件外观 ---------- */
enum sk_widget_state {
    SK_STATE_NORMAL   = 0,
    SK_STATE_HOVER    = 1 << 0,
    SK_STATE_PRESSED  = 1 << 1,
    SK_STATE_FOCUSED  = 1 << 2,
    SK_STATE_DISABLED = 1 << 3,
    SK_STATE_CHECKED  = 1 << 4,   /* 开关/选中的标签页/工具按钮 */
};
/* 控件绘制函数不负责擦背景：调用者（ui.h 的重绘流程）会先把背景重画好。
 * accent 是悬停/按下时的上色颜色。seed 决定手绘抖动，同一控件请保持不变。 */
void sk_draw_button(sk_canvas *c, sk_rect r, const char *label, int state, sk_color accent, uint32_t seed);
/* 只画按钮外框和上色，不画文字（图标按钮用） */
void sk_draw_button_frame(sk_canvas *c, sk_rect r, int state, sk_color accent, uint32_t seed);
void sk_draw_checkbox(sk_canvas *c, sk_rect r, const char *label, int state, uint32_t seed);
/* 输入框外框（像纸上的手写格子） */
void sk_draw_textbox_frame(sk_canvas *c, sk_rect r, int state, uint32_t seed);
/* 进度条/占用率条：value 0..1000（千分比） */
void sk_draw_meter(sk_canvas *c, sk_rect r, int permille, sk_color fill, uint32_t seed);
/* 标签页：一排手绘标签，active 是当前页 */
void sk_draw_tabs(sk_canvas *c, sk_rect r, const char *const *labels, int n, int active, int hover, uint32_t seed);
/* 返回 sk_draw_tabs 布局下第 i 个标签的矩形（用于命中测试） */
sk_rect sk_tabs_item_rect(sk_rect r, const char *const *labels, int n, int i);
/* 滚动条：total 内容高度、visible 可见高度、offset 当前偏移 */
void sk_draw_scrollbar(sk_canvas *c, sk_rect r, int total, int visible, int offset, int state, uint32_t seed);
/* 焦点框：手绘虚线 */
void sk_draw_focus_ring(sk_canvas *c, sk_rect r, uint32_t seed);
/* 手绘分隔线 */
void sk_draw_separator(sk_canvas *c, int x1, int x2, int y, uint32_t seed);

/* ---------- 窗口装饰（sketchd 使用） ---------- */
#define SK_TITLE_H      34   /* 标题栏高度 */
#define SK_BORDER       6    /* 窗口四周边框宽度（手绘线会在这一圈内抖动） */
#define SK_SHADOW_OFF   7    /* 排线阴影偏移 */
/* 给定内容区矩形 content（屏幕坐标），整个装饰（含阴影）占用的外接矩形 */
sk_rect sk_frame_outer_rect(sk_rect content);
/* 标题栏按钮的命中矩形（屏幕坐标）。which: 0=关闭 1=最大化 2=最小化 */
sk_rect sk_frame_button_rect(sk_rect content, int which);
/* 在 c 上画窗口装饰。c 的坐标系与 content 相同（通常是屏幕坐标或边框层的局部坐标）。
 * 只画边框、标题栏、按钮、阴影，不碰 content 内部。
 * hover_button: -1 无，0/1/2 同上。accent 是该应用的主题色（标题栏排线颜色）。 */
void sk_draw_window_frame(sk_canvas *c, sk_rect content, const char *title, int focused,
                          int hover_button, sk_color accent, uint32_t seed);

/* ---------- 手绘图标（全部由代码画出，没有图片文件） ---------- */
enum sk_icon {
    SK_ICON_TERMINAL = 0, SK_ICON_FOLDER, SK_ICON_FILE, SK_ICON_TEXT, SK_ICON_IMAGE,
    SK_ICON_PROGRAM, SK_ICON_NOTEPAD, SK_ICON_PAINT, SK_ICON_MONITOR, SK_ICON_CLOCK,
    SK_ICON_ABOUT, SK_ICON_SETTINGS, SK_ICON_POWER, SK_ICON_REBOOT, SK_ICON_HOME,
    SK_ICON_UP, SK_ICON_BACK, SK_ICON_NEW_FOLDER, SK_ICON_TRASH, SK_ICON_SAVE,
    SK_ICON_OPEN, SK_ICON_PENCIL, SK_ICON_ERASER, SK_ICON_BRUSH, SK_ICON_LINE,
    SK_ICON_RECT, SK_ICON_CIRCLE, SK_ICON_FILL, SK_ICON_KERNEL, SK_ICON_CHIP,
    SK_ICON_MEMORY, SK_ICON_PROCESS, SK_ICON_SKETCH_LOGO, SK_ICON_BOOK, SK_ICON_REFRESH,
    SK_ICON_COUNT
};
/* 在 (x,y) 为左上角、边长 size 的正方形里画图标 */
void sk_draw_icon(sk_canvas *c, int icon, int x, int y, int size, uint32_t seed);
/* 名称 → 图标（"terminal"、"folder" 等，用于 /etc/sketch/apps.conf），未知返回 SK_ICON_PROGRAM */
int  sk_icon_from_name(const char *name);

#ifdef __cplusplus
}
#endif
#endif /* SKETCH_THEME_H */
