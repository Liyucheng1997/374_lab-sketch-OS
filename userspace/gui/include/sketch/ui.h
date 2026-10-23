/*
 * sketch/ui.h — 手绘风格控件库（保留模式，libsketch）
 *
 * 一个 sk_ui 管理一个窗口里的所有控件（一个扁平列表，后加入的在上层）。
 *   - 每个控件有矩形 r（窗口内容区坐标）、状态位（悬停/按下/焦点/禁用/选中）。
 *   - 状态变化时只把该控件的矩形标记为“脏”；每轮事件循环结束时（idle 回调）
 *     sk_ui 把脏区域合并，先画背景（默认纸色，可自定义），再按顺序画与之相交的控件，
 *     最后把这块区域 damage 给服务器。—— 在很慢的网页模拟器里也能流畅。
 *   - 鼠标事件交给鼠标下面最上层的可见控件；按下后该控件“捕获”鼠标直到松开。
 *     键盘事件交给有焦点的控件；Tab/Shift+Tab 在可获得焦点的控件间切换（除非焦点控件自己
 *     处理了 Tab，例如多行编辑器）。SK_EVENT_CLIPBOARD（剪贴板内容到达）也先交给有焦点的控件。
 *     控件不处理的事件交给 sk_ui_set_handler 设置的应用回调。
 *   - 控件可以在自己矩形外 SK_WIDGET_BLEED 像素内作画（手绘线条会“出头”、焦点虚线框），
 *     sk_ui 标记脏区域和设置裁剪时都会把控件矩形向外扩这么多。
 *   - 窗口尺寸改变时调用 layout 回调，应用在里面重新摆放控件。
 *
 * 示例：
 *   static void on_ok(sk_widget *b, void *u) { sk_app_quit(app, 0); }
 *   sk_ui *ui = sk_ui_new(win);
 *   sk_label_new(ui, sk_rect_make(20, 20, 200, 24), "你好，手绘世界！", SK_FONT_UI, SK_ALIGN_LEFT);
 *   sk_button_new(ui, sk_rect_make(20, 60, 100, 36), "确定", on_ok, NULL);
 *   sk_ui_invalidate_all(ui);
 *   sk_app_run(app);
 */
#ifndef SKETCH_UI_H
#define SKETCH_UI_H

#include <stdint.h>
#include "gfx.h"
#include "font.h"
#include "theme.h"
#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sk_ui sk_ui;
typedef struct sk_widget sk_widget;

#define SK_WIDGET_BLEED 4

typedef void (*sk_widget_fn)(sk_widget *w, void *user);

typedef struct sk_widget_class {
    const char *name;
    /* 画控件。c 是窗口画布，clip 已设为（脏区域 ∩ 控件矩形外扩 SK_WIDGET_BLEED），背景已经画好 */
    void (*draw)(sk_widget *w, sk_canvas *c);
    /* 处理事件；鼠标坐标是窗口内容区坐标。返回 1=已处理 */
    int  (*event)(sk_widget *w, const sk_event *e);
    /* 释放 priv 等私有数据（可为 NULL） */
    void (*destroy)(sk_widget *w);
    /* 矩形改变后调用（可为 NULL） */
    void (*resized)(sk_widget *w);
    int  focusable;
} sk_widget_class;

struct sk_widget {
    const sk_widget_class *cls;
    sk_ui    *ui;
    sk_rect   r;
    unsigned  state;        /* enum sk_widget_state (theme.h) */
    int       visible;
    uint32_t  seed;         /* 手绘随机种子（创建时自动分配，稳定不变） */
    sk_color  accent;       /* 悬停/选中时的上色，默认 SK_BLUE */
    int       font;         /* enum sk_font_id */
    char     *text;         /* 标签/按钮文字等（控件自己拥有，用 sk_widget_set_text 修改） */
    sk_widget_fn on_click;  /* 按钮点击、列表项激活（双击/回车）、输入框回车 */
    sk_widget_fn on_change; /* 内容/选中项/勾选状态/标签页/滑块值改变 */
    void     *user;         /* 回调的 user 参数 */
    void     *priv;         /* 控件类私有数据 */
    int       id;           /* 应用自定义 */
    sk_widget *next;        /* 内部链表 */
};

/* ---------- sk_ui ---------- */
sk_ui     *sk_ui_new(sk_window *win);        /* 接管 win 的事件回调 */
void       sk_ui_free(sk_ui *ui);            /* 销毁所有控件（不销毁窗口） */
sk_window *sk_ui_window(sk_ui *ui);
sk_app    *sk_ui_app(sk_ui *ui);
sk_canvas *sk_ui_canvas(sk_ui *ui);

typedef void (*sk_paint_fn)(sk_ui *ui, sk_canvas *c, sk_rect dirty, void *user);
typedef void (*sk_layout_fn)(sk_ui *ui, int w, int h, void *user);
/* 自定义背景绘制（默认：SK_PAPER 纸张纹理）。dirty 之外的地方不需要画 */
void sk_ui_set_background(sk_ui *ui, sk_paint_fn fn, void *user);
/* 窗口创建后和每次尺寸改变后调用；sk_ui_set_layout 本身会立即调用一次 */
void sk_ui_set_layout(sk_ui *ui, sk_layout_fn fn, void *user);
/* 控件没处理的事件（以及 FOCUS/CLOSE/CLIPBOARD）交给它；返回值含义同 sk_event_fn */
void sk_ui_set_handler(sk_ui *ui, sk_event_fn fn, void *user);

void sk_ui_invalidate(sk_ui *ui, sk_rect r);   /* 标记脏区域，本轮循环结束时重绘 */
void sk_ui_invalidate_all(sk_ui *ui);
void sk_ui_redraw_now(sk_ui *ui);              /* 立刻重绘脏区域并 damage */
void sk_ui_focus(sk_ui *ui, sk_widget *w);     /* w=NULL 取消焦点 */
sk_widget *sk_ui_focused(sk_ui *ui);
sk_widget *sk_ui_widget_at(sk_ui *ui, int x, int y);

/* ---------- 通用控件操作 ---------- */
sk_widget *sk_widget_new(sk_ui *ui, const sk_widget_class *cls, sk_rect r);  /* 自定义控件用 */
void sk_widget_destroy(sk_widget *w);
void sk_widget_invalidate(sk_widget *w);
void sk_widget_set_rect(sk_widget *w, sk_rect r);
void sk_widget_set_text(sk_widget *w, const char *text);
void sk_widget_set_visible(sk_widget *w, int visible);
void sk_widget_set_enabled(sk_widget *w, int enabled);
void sk_widget_set_checked(sk_widget *w, int checked);
int  sk_widget_checked(const sk_widget *w);
void sk_widget_set_accent(sk_widget *w, sk_color accent);
void sk_widget_set_font(sk_widget *w, int font_id);
void sk_widget_set_callbacks(sk_widget *w, sk_widget_fn on_click, sk_widget_fn on_change, void *user);

/* ---------- 标准控件 ---------- */
/* 静态文字。align: SK_ALIGN_*；文字过长时截断 */
sk_widget *sk_label_new(sk_ui *ui, sk_rect r, const char *text, int font_id, int align);
void       sk_label_set_color(sk_widget *w, sk_color color);
/* 多行静态文字（自动换行） */
sk_widget *sk_paragraph_new(sk_ui *ui, sk_rect r, const char *text, int font_id);

/* 按钮 */
sk_widget *sk_button_new(sk_ui *ui, sk_rect r, const char *label, sk_widget_fn on_click, void *user);
/* 图标按钮（工具栏）：label 可为 NULL；有 label 时图标在左、文字在右 */
sk_widget *sk_icon_button_new(sk_ui *ui, sk_rect r, int icon, const char *label,
                              sk_widget_fn on_click, void *user);
/* 颜色块按钮（画板调色盘）；选中（checked）时画圈 */
sk_widget *sk_swatch_new(sk_ui *ui, sk_rect r, sk_color color, sk_widget_fn on_click, void *user);
sk_color   sk_swatch_color(const sk_widget *w);

/* 复选框：点击切换 checked 并调用 on_change */
sk_widget *sk_checkbox_new(sk_ui *ui, sk_rect r, const char *label, int checked,
                           sk_widget_fn on_change, void *user);

/* 单行输入框：编辑时调用 on_change，回车时调用 on_click。支持 ←→ Home End 退格 Delete、
 * Ctrl+A/C/X/V（经由系统剪贴板）、鼠标点击定位光标。text 字段始终是当前内容 */
sk_widget  *sk_textbox_new(sk_ui *ui, sk_rect r, const char *initial,
                           sk_widget_fn on_change, void *user);
const char *sk_textbox_text(const sk_widget *w);
void        sk_textbox_set_text(sk_widget *w, const char *text);   /* 光标移到末尾 */
void        sk_textbox_set_placeholder(sk_widget *w, const char *hint);

/* 多行文本编辑器（记事本）：滚动、光标、Shift/鼠标拖动选择、复制粘贴、撤销(Ctrl+Z, 至少 1 级)、
 * 方向键/PageUp/PageDown/Home/End、Tab 插入 4 个空格。内部用 UTF-8 存储 */
sk_widget *sk_textarea_new(sk_ui *ui, sk_rect r, const char *initial);
char      *sk_textarea_get_text(const sk_widget *w);              /* malloc，调用者 free */
void       sk_textarea_set_text(sk_widget *w, const char *text);
int        sk_textarea_modified(const sk_widget *w);
void       sk_textarea_set_modified(sk_widget *w, int modified);
/* 光标所在行列（从 1 开始），用于状态栏 */
void       sk_textarea_cursor_pos(const sk_widget *w, int *line, int *col);
/* 选区用 sk_textarea_get_text 返回的文字里的字节偏移表示（行与行之间算一个 \n）。
 * select：锚点放在 start、光标放在 end（start == end 只移动光标），并滚动到光标可见；
 * 偏移会被截到文字范围内、对齐到 UTF-8 字符开头；不触发 on_change（查找、跳到某行用）。
 * get_selection：*start 是锚点、*end 是光标，没有选中时两者相等 */
void       sk_textarea_select(sk_widget *w, int start, int end);
void       sk_textarea_get_selection(const sk_widget *w, int *start, int *end);
void       sk_textarea_set_readonly(sk_widget *w, int readonly);
/* 自动换行（默认开启：按控件宽度折行，上下方向键按“显示行”移动）；关闭时可水平滚动 */
void       sk_textarea_set_wrap(sk_widget *w, int wrap);
/* 左侧行号栏（默认关闭） */
void       sk_textarea_set_line_numbers(sk_widget *w, int on);

/* 列表/表格：可选表头（点击表头调用 sort 回调）、图标列、多列文字、单选、
 * 滚轮/滚动条/方向键滚动。选中变化→on_change，双击或回车→on_click */
sk_widget *sk_list_new(sk_ui *ui, sk_rect r, sk_widget_fn on_change, sk_widget_fn on_click, void *user);
void       sk_list_set_columns(sk_widget *w, const char *const *titles, const int *widths, int ncols);
void       sk_list_set_sort_handler(sk_widget *w, void (*on_sort)(sk_widget *w, int col, void *user));
void       sk_list_set_row_height(sk_widget *w, int h);
void       sk_list_clear(sk_widget *w);
/* 添加一行：icon 为 SK_ICON_* 或 -1；cells 有 ncols 个字符串（会被复制）；data 是应用自定义值 */
int        sk_list_add(sk_widget *w, int icon, const char *const *cells, intptr_t data);
int        sk_list_count(const sk_widget *w);
int        sk_list_selected(const sk_widget *w);                  /* -1 = 无 */
void       sk_list_select(sk_widget *w, int row);                 /* 并滚动到可见 */
intptr_t   sk_list_data(const sk_widget *w, int row);
const char *sk_list_cell(const sk_widget *w, int row, int col);
void       sk_list_set_cell(sk_widget *w, int row, int col, const char *text);
int        sk_list_scroll(const sk_widget *w);                    /* 第一可见行 */
void       sk_list_set_scroll(sk_widget *w, int first_row);

/* 标签页条（只画标签本身；切换页面时应用在 on_change 里显示/隐藏对应控件） */
sk_widget *sk_tabs_new(sk_ui *ui, sk_rect r, const char *const *labels, int n,
                       sk_widget_fn on_change, void *user);
int        sk_tabs_active(const sk_widget *w);
void       sk_tabs_set_active(sk_widget *w, int index);

/* 占用率条：permille 0..1000，caption 显示在条上（可为 NULL） */
sk_widget *sk_meter_new(sk_ui *ui, sk_rect r, sk_color fill);
void       sk_meter_set(sk_widget *w, int permille, const char *caption);

/* 滑块 */
sk_widget *sk_slider_new(sk_ui *ui, sk_rect r, int min, int max, int value,
                         sk_widget_fn on_change, void *user);
int        sk_slider_value(const sk_widget *w);
void       sk_slider_set_value(sk_widget *w, int value);

/* 自定义绘制区域（图表、时钟表盘、画板画布、终端屏幕……）
 * draw 时 c 的 clip 已设好；event 返回 1 表示已处理。focusable=1 时可获得键盘焦点 */
typedef void (*sk_custom_draw_fn)(sk_widget *w, sk_canvas *c, void *user);
typedef int  (*sk_custom_event_fn)(sk_widget *w, const sk_event *e, void *user);
sk_widget *sk_custom_new(sk_ui *ui, sk_rect r, sk_custom_draw_fn draw, sk_custom_event_fn event,
                         int focusable, void *user);

/* ---------- 弹出菜单（在窗口内部，覆盖在其它控件之上） ---------- */
/* items 用 NULL 或 "-" 表示分隔线。选中后调用 on_pick(index)，点外面关闭（不回调） */
typedef void (*sk_menu_fn)(sk_ui *ui, int index, void *user);
void sk_menu_popup(sk_ui *ui, int x, int y, const char *const *items, int n, sk_menu_fn on_pick, void *user);

/* ---------- 对话框（独立的小窗口；嵌套事件循环，调用会阻塞直到对话框关闭） ---------- */
/* buttons 形如 "确定|取消"，返回被点击按钮的序号；直接关掉对话框返回 -1 */
int   sk_dialog_message(sk_app *app, const char *title, const char *text, const char *buttons);
/* 输入一行文字，返回 malloc 的字符串（取消返回 NULL） */
char *sk_dialog_input(sk_app *app, const char *title, const char *prompt, const char *initial);
/* 选择文件：save=0 打开（必须选已存在的文件），save=1 保存（可输入新文件名）。
 * 返回 malloc 的绝对路径，取消返回 NULL */
char *sk_dialog_file(sk_app *app, const char *title, const char *start_dir, int save,
                     const char *default_name);

#ifdef __cplusplus
}
#endif
#endif /* SKETCH_UI_H */
