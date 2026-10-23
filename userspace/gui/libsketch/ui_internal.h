/*
 * ui_internal.h — libsketch 控件库的“内部接口”（只给 libsketch 自己的 .c 文件用）
 *
 * 公共接口在 sketch/ui.h 和 sketch/client.h。这里补充的是“写一个控件类”时需要、
 * 但普通应用不该直接碰的东西：
 *
 *   1. 控件可以调的 sk_ui 核心服务：局部失效、光标形状、焦点/捕获/抓取查询、
 *      输入光标(caret)闪烁、控件私有定时器、剪贴板请求路由、置顶。
 *   2. client.c 提供给 ui.c 的窗口内部信息（窗口 id、窗口是否有焦点、销毁钩子、模态窗口）。
 *   3. 文本编辑控件共用的 UTF-8 / 命中测试小工具。
 *
 * ======================= 控件类会收到哪些事件 =======================
 * 控件类的 event(w, e) 收到的事件（坐标都是窗口内容区坐标）：
 *
 *   SK_EVENT_MOUSE
 *     - SK_MOUSE_ENTER / SK_MOUSE_LEAVE：由 sk_ui 合成，表示“悬停控件”变了。
 *       （同时 sk_ui 会设置/清除 SK_STATE_HOVER；除非控件设了 SK_WF_NO_AUTO_REDRAW，
 *         sk_ui 会顺便让整个控件失效重画，控件自己不用管。）
 *     - SK_MOUSE_MOVE：鼠标在控件上移动；或者控件正“捕获”鼠标（按下后拖到外面也照样收到）。
 *     - SK_MOUSE_DOWN：在控件上按下。sk_ui 在投递之前已经：
 *         设置捕获（直到所有按钮松开）、左键时设置 SK_STATE_PRESSED、
 *         对可获得焦点且没有 SK_WF_NO_CLICK_FOCUS 的控件设置键盘焦点。
 *     - SK_MOUSE_UP：捕获期间的松开。投递时 SK_STATE_PRESSED 还在，投递之后才清除，
 *       所以控件可以用 sk_widget_is_click(w, e) 判断“这是不是一次完整的点击”。
 *     - SK_MOUSE_WHEEL：鼠标在控件上滚轮（或者正在捕获）。
 *   SK_EVENT_KEY        有键盘焦点（或抓取了输入）时收到。返回 0 表示没处理：
 *                       Tab 会被 sk_ui 用来切换焦点，其它交给应用回调。
 *   SK_EVENT_FOCUS      e->focused = 1/0：控件的“有效焦点”变了。有效焦点 =
 *                       它是本窗口的焦点控件 并且 窗口本身拥有键盘焦点。
 *                       （SK_STATE_FOCUSED 状态位表示的也是有效焦点。）
 *   SK_EVENT_CLIPBOARD  先交给调用过 sk_widget_request_clipboard() 的控件，
 *                       其次是焦点控件，最后是应用回调。
 *
 * 返回 1 表示“已处理”。鼠标事件返回 0 会被交给应用回调（sk_ui_set_handler）。
 *
 * ======================= 生命周期与安全 =======================
 * - 在任何回调里调用 sk_widget_destroy() 都是安全的：控件先被标记为“已死亡”
 *   （所有 sk_widget_* 操作对它变成空操作），真正的 destroy/free 推迟到
 *   事件分发结束之后。控件类的 event() 在调用 on_click/on_change 之后
 *   可以用 sk_widget_alive(w) 检查自己是否还活着，死了就立刻 return 1，别再碰 priv。
 * - 控件销毁时，sk_ui 会自动取消它的定时器、caret、剪贴板请求、捕获、抓取和焦点。
 *
 * 线程：整个库是单线程的，所有回调都在 sk_app_run 的那个线程里执行。
 */
#ifndef SKETCH_UI_INTERNAL_H
#define SKETCH_UI_INTERNAL_H

#include <stdint.h>
#include "sketch/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====================== 1. 控件类可用的 sk_ui 核心服务 ====================== */

/* ---- 每个控件的内部标志 ---- */
enum sk_widget_flag {
    /* 悬停/按下状态变化时 sk_ui 不自动重画整个控件（控件自己根据 ENTER/LEAVE 等事件决定）。
     * 自定义控件（sk_custom_new）默认带这个标志：终端、画布这种大控件不该因为鼠标进出而整块重画。 */
    SK_WF_NO_AUTO_REDRAW = 1u << 0,
    /* 不画 sk_ui 统一的焦点虚线框（输入框、编辑器有自己的焦点外观） */
    SK_WF_NO_FOCUS_RING  = 1u << 1,
    /* 鼠标点击时不夺取键盘焦点（按钮、复选框、标签页：只能用 Tab 键聚焦，
     * 这样点工具栏按钮时编辑器里的光标不会丢失——和 macOS 的做法一样） */
    SK_WF_NO_CLICK_FOCUS = 1u << 2,
};
/* 先清除 clear 中的位，再设置 set 中的位 */
void     sk_widget_set_flags(sk_widget *w, unsigned set, unsigned clear);
unsigned sk_widget_flags(const sk_widget *w);

/* 鼠标停在这个控件上时显示的光标（enum sk_cursor_id，默认 SK_CURSOR_ARROW）。
 * 可以随时调用（例如列表在表头上显示箭头、在分隔线上显示别的），
 * 如果鼠标此刻正在这个控件上，会立即生效。实际报文只在光标真的变化时才发。 */
void sk_widget_set_cursor(sk_widget *w, int cursor);
int  sk_widget_cursor(const sk_widget *w);

/* 只让控件的一部分失效（窗口坐标，会被裁剪到控件矩形外扩 SK_WIDGET_BLEED 的范围）。
 * 例如编辑器只重画光标所在的一行。 */
void sk_widget_invalidate_rect(sk_widget *w, sk_rect r);

/* 控件还活着吗（没有被 sk_widget_destroy）？w 为 NULL 返回 0 */
int  sk_widget_alive(const sk_widget *w);
/* 控件是不是本窗口的焦点控件（不考虑窗口本身是否有焦点） */
int  sk_widget_has_focus(const sk_widget *w);
/* e 是不是对 w 的一次完整左键点击：SK_MOUSE_UP + 左键 + 按下时就在 w 上 + 松开时仍在 w 内 */
int  sk_widget_is_click(const sk_widget *w, const sk_event *e);
/* 控件当前使用的字体（sk_font_get(w->font)） */
const sk_font *sk_widget_get_font(const sk_widget *w);
/* 文字墨色：禁用时返回 SK_INK_FAINT，否则 normal */
sk_color sk_widget_ink(const sk_widget *w, sk_color normal);

/* 窗口本身是否拥有键盘焦点 */
int  sk_ui_window_focused(sk_ui *ui);
/* 是否应该画焦点框：只有用键盘（Tab）移动过焦点之后才显示，鼠标一点击就隐藏
 * （相当于 CSS 的 :focus-visible）。控件自己画焦点外观时也可以参考它。 */
int  sk_ui_focus_visible(sk_ui *ui);
sk_widget *sk_ui_hovered(sk_ui *ui);
sk_widget *sk_ui_pressed(sk_ui *ui);      /* 正在捕获鼠标的控件 */

/* ---- 抓取（grab）：弹出菜单、下拉框之类的“临时模态”控件用 ----
 * 抓取期间，本窗口所有鼠标和键盘事件都只交给 w（不管鼠标在哪儿），
 * 其它控件不会有悬停效果。w=NULL 释放抓取。控件销毁时自动释放。 */
void       sk_ui_grab(sk_ui *ui, sk_widget *w);
sk_widget *sk_ui_grabbed(sk_ui *ui);

/* 把控件移到列表末尾（= 最上层，最后画、最先接收鼠标） */
void sk_widget_raise(sk_widget *w);

/* ---- 输入光标（caret）闪烁服务 ----
 * 整个窗口同一时刻最多一个闪烁的光标，由 sk_ui 统一管理一个定时器：
 *   - 控件在获得有效焦点后（以及每次光标移动后）调用 sk_ui_caret_set(w, rect)
 *     告诉 sk_ui 光标的矩形（窗口坐标）。位置变化时旧位置和新位置都会失效，
 *     并且闪烁重新从“可见”开始（打字时光标不闪）。
 *   - 控件的 draw() 里用 sk_ui_caret_visible(w) 决定此刻画不画光标。
 *   - 失去有效焦点或控件销毁时 sk_ui 自动停止闪烁并擦掉光标。
 *   - 为了让空闲的系统真正睡眠（网页模拟器里很重要），连续闪烁约 10 秒没有输入后
 *     光标停在“可见”状态、定时器停止；下一次 sk_ui_caret_set/reset 会重新开始闪。
 * w 不是焦点控件时这些调用被忽略。 */
void sk_ui_caret_set(sk_widget *w, sk_rect caret);
void sk_ui_caret_reset(sk_widget *w);     /* 光标位置没变，但希望它立刻显示并重新计时（例如按键后） */
void sk_ui_caret_hide(sk_widget *w);      /* 不显示光标（例如只读编辑器）；再次 set 时恢复 */
int  sk_ui_caret_visible(const sk_widget *w);

/* ---- 控件私有定时器（控件销毁时自动取消） ---- */
typedef void (*sk_widget_timer_fn)(sk_widget *w);
/* 返回 id（>0），失败返回 0。repeat=0 的定时器触发一次后自动注销 */
int  sk_widget_add_timer(sk_widget *w, int interval_ms, int repeat, sk_widget_timer_fn fn);
void sk_widget_remove_timer(sk_widget *w, int id);

/* ---- 剪贴板 ----
 * 向服务器要剪贴板内容；回复到达时，SK_EVENT_CLIPBOARD 优先交给 w 的 event()。
 * （多个请求按先后顺序一一对应回复。） */
void sk_widget_request_clipboard(sk_widget *w);
/* 把文字放进系统剪贴板（len<0 表示用 strlen） */
void sk_widget_set_clipboard(sk_widget *w, const char *text, int len);

/* ---- 文字修改钩子 ----
 * 应用调用公共的 sk_widget_set_text(w, text) 时，如果控件类设置了这个钩子，
 * 就改为调用钩子（例如输入框要同时重置光标和滚动位置）。 */
typedef void (*sk_widget_text_hook)(sk_widget *w, const char *text);
void sk_widget_set_text_hook(sk_widget *w, sk_widget_text_hook hook);

/* ====================== 2. client.c 提供给控件库的窗口内部信息 ====================== */
uint32_t    sk_window_id(const sk_window *w);          /* 服务器分配的窗口 id */
int         sk_window_is_focused(const sk_window *w);  /* 最近一次 FOCUS 事件的状态（新窗口默认 1） */
const char *sk_window_title(const sk_window *w);
/* 窗口被销毁时（无论是应用主动销毁还是默认的关闭处理）调用 fn。每个窗口只有一个钩子，
 * fn=NULL 取消。sk_ui 用它在窗口消失后自动“断开”，避免访问已释放的画布。 */
typedef void (*sk_window_destroy_fn)(sk_window *w, void *user);
void        sk_window_set_destroy_hook(sk_window *w, sk_window_destroy_fn fn, void *user);
/* 模态窗口：设置后，其它窗口收不到鼠标和键盘事件（焦点、关闭、改变大小照常处理）。
 * 对话框在嵌套事件循环前设置、结束后恢复。返回之前的模态窗口（可以嵌套）。w=NULL 取消。
 * 模态窗口被销毁时自动取消。 */
sk_window  *sk_app_set_modal(sk_app *app, sk_window *w);
sk_window  *sk_app_modal(const sk_app *app);
/* 同步读取剪贴板（不需要窗口；命令行工具 sketch 用）。阻塞最多 timeout_ms 毫秒，
 * 返回 malloc 的、以 NUL 结尾的文本（*len 为字节数，可为 NULL），失败返回 NULL。
 * 等待期间到达的其它事件留在队列里，之后照常分发。 */
char       *sk_clipboard_get_sync(sk_app *app, int timeout_ms, int *len);

/* ====================== 3. 文本编辑小工具 ====================== */
/* s 的前 len 字节里，pos 之前一个码点的起始字节位置（pos<=0 返回 0） */
int  sk_ui_utf8_prev(const char *s, int pos);
/* pos 之后一个码点的起始字节位置（不超过 len） */
int  sk_ui_utf8_next(const char *s, int len, int pos);
/* “单词”字符：字母、数字、下划线、所有非 ASCII 字符（中文按连续汉字算一个词） */
int  sk_ui_is_word_char(uint32_t cp);
/* 按单词向左/右移动（Ctrl+←/→、双击选词用）：跳过空白再跳过一个词 */
int  sk_ui_word_left(const char *s, int pos);
int  sk_ui_word_right(const char *s, int len, int pos);
/* 命中测试：文字 s（前 len 字节）从 x=0 开始画，像素位置 x 最靠近哪个码点边界？返回字节偏移 */
int  sk_ui_text_hit(const sk_font *f, const char *s, int len, int x);

#ifdef __cplusplus
}
#endif
#endif /* SKETCH_UI_INTERNAL_H */
