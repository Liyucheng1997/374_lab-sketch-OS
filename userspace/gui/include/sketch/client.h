/*
 * sketch/client.h — 应用程序连接显示服务器的客户端库 (libsketch)
 *
 * 典型用法（底层 API；大多数应用用 ui.h 的控件库，更省事）：
 *
 *   sk_app *app = sk_app_connect("hello");
 *   sk_window *w = sk_window_create(app, "你好", 400, 300, SK_WIN_CENTER, SK_BLUE);
 *   sk_canvas *c = sk_window_canvas(w);
 *   sk_clear(c, SK_PAPER);  ... 画 ...
 *   sk_window_damage_all(w);
 *   sk_window_set_handler(w, on_event, userdata);
 *   return sk_app_run(app);       // 事件循环，直到 sk_app_quit 或所有窗口关闭
 *
 * 事件循环基于 poll()：同时等待显示服务器的套接字、应用注册的额外 fd
 * （例如终端的 pty）和定时器。全部是单线程的。
 */
#ifndef SKETCH_CLIENT_H
#define SKETCH_CLIENT_H

#include "gfx.h"
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sk_app sk_app;
typedef struct sk_window sk_window;

/* 统一的事件结构（由服务器报文翻译而来） */
typedef enum {
    SK_EVENT_MOUSE = 1,     /* 鼠标：e->mouse */
    SK_EVENT_KEY,           /* 键盘：e->key */
    SK_EVENT_FOCUS,         /* 焦点：e->focused */
    SK_EVENT_RESIZE,        /* 窗口尺寸已改变：库已经换好新缓冲区，e->w/e->h 是新尺寸，
                               canvas 内容是未定义的，应用必须整窗重画 */
    SK_EVENT_CLOSE,         /* 用户请求关闭；默认处理（handler 返回 0）= 销毁窗口 */
    SK_EVENT_CLIPBOARD,     /* 剪贴板内容到达：e->text / e->text_len */
} sk_event_type;

typedef struct {
    sk_event_type type;
    sk_window *win;
    union {
        struct { uint32_t kind; int x, y; uint32_t buttons, button; int wheel;
                 uint32_t clicks, modifiers; } mouse;
        struct { uint32_t keycode, state, modifiers, codepoint; } key;
        int focused;
        struct { int w, h; } size;
        struct { const char *text; int text_len; } clip;
    };
} sk_event;

/* 返回 1 表示已处理；对 SK_EVENT_CLOSE 返回 0 会执行默认动作（销毁窗口） */
typedef int (*sk_event_fn)(sk_window *w, const sk_event *e, void *user);
typedef void (*sk_fd_fn)(sk_app *app, int fd, short revents, void *user);
typedef void (*sk_timer_fn)(sk_app *app, void *user);

/* ---------- 连接 ---------- */
/* 连接显示服务器（路径取 $SKETCH_SOCKET，默认 SK_SOCKET_PATH）。失败返回 NULL 并在 stderr 说明原因 */
sk_app *sk_app_connect(const char *app_name);
void    sk_app_disconnect(sk_app *app);
int     sk_app_screen_w(const sk_app *app);
int     sk_app_screen_h(const sk_app *app);
/* 运行事件循环。返回值作为进程退出码使用。所有窗口都销毁后自动返回 0。
 * 与服务器的连接断开时也返回（退出码 1）。 */
int     sk_app_run(sk_app *app);
void    sk_app_quit(sk_app *app, int code);
/* 嵌套事件循环：一直处理事件直到 *flag != 0（或连接断开 / sk_app_quit）。
 * 对话框（ui.h 的 sk_dialog_*）就是用它实现“阻塞式”调用的。返回 0=flag 置位，-1=其它原因退出 */
int     sk_app_run_until(sk_app *app, volatile int *flag);

/* 空闲回调：每轮事件循环处理完事件、调用 poll() 之前执行（ui.h 用它统一重绘） */
typedef void (*sk_idle_fn)(sk_app *app, void *user);
int  sk_app_add_idle(sk_app *app, sk_idle_fn fn, void *user);
void sk_app_remove_idle(sk_app *app, int id);

/* 额外的文件描述符（例如 pty）。events 为 POLLIN 等 */
int  sk_app_add_fd(sk_app *app, int fd, short events, sk_fd_fn fn, void *user);
void sk_app_remove_fd(sk_app *app, int fd);
/* 定时器：interval_ms 后触发；repeat=1 时周期触发。返回 id（>0） */
int  sk_app_add_timer(sk_app *app, int interval_ms, int repeat, sk_timer_fn fn, void *user);
void sk_app_remove_timer(sk_app *app, int id);
/* 单调时钟毫秒数 */
uint64_t sk_now_ms(void);

/* 剪贴板（系统全局，由 sketchd 保存） */
void sk_clipboard_set(sk_app *app, const char *text, int len);
void sk_clipboard_request(sk_app *app, sk_window *w);   /* 结果作为 SK_EVENT_CLIPBOARD 送给 w */
/* 请服务器启动另一个应用（apps.conf 中的 id），arg 可为 NULL */
void sk_app_launch(sk_app *app, const char *app_id, const char *arg);

/* ---------- 窗口 ---------- */
/* 同步创建窗口（等待服务器确认）。accent=0 用默认蓝色。失败返回 NULL */
sk_window *sk_window_create(sk_app *app, const char *title, int w, int h,
                            unsigned flags, sk_color accent);
/* 设置最小尺寸（仅对 SK_WIN_RESIZABLE 有意义）；须在 create 之后立刻调用 */
void       sk_window_set_min_size(sk_window *w, int min_w, int min_h);
void       sk_window_destroy(sk_window *w);
sk_canvas *sk_window_canvas(sk_window *w);      /* 指向共享内存的画布；RESIZE 后指针不变，内容变 */
int        sk_window_width(const sk_window *w);
int        sk_window_height(const sk_window *w);
sk_app    *sk_window_app(const sk_window *w);
void       sk_window_set_handler(sk_window *w, sk_event_fn fn, void *user);
void       sk_window_set_title(sk_window *w, const char *title);
void       sk_window_set_cursor(sk_window *w, int cursor);
void       sk_window_show(sk_window *w);        /* 置顶并取得焦点 */
void       sk_window_move(sk_window *w, int x, int y);
/* 通知服务器这块区域画好了（会被合并：同一轮事件循环内多次 damage 只发一次报文） */
void       sk_window_damage(sk_window *w, sk_rect r);
void       sk_window_damage_all(sk_window *w);
/* 立即把积累的 damage 发出去（一般不需要手动调用，事件循环每轮结束时自动 flush） */
void       sk_window_flush(sk_window *w);
void      *sk_window_userdata(const sk_window *w);

#ifdef __cplusplus
}
#endif
#endif /* SKETCH_CLIENT_H */
