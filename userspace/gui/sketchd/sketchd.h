/*
 * sketchd.h — sketchd 内部共享的数据结构与模块接口
 *
 * sketchd = 显示服务器 + 窗口管理器 + 桌面外壳。它是一个**单线程、事件驱动**的程序：
 *
 *         ┌──────────────── poll() 睡眠等待 ────────────────┐
 *         │ /dev/input/event*  键盘鼠标        ← input.c / keymap.c / vmmouse.c
 *         │ 监听套接字 + 客户端套接字（协议）  ← server.c
 *         │ signalfd：SIGCHLD / SIGTERM …     ← main.c / launch.c
 *         │ timerfd：时钟（整分钟）、系统监视  ← taskbar.c
 *         └───────────────────────┬─────────────────────────┘
 *                                 ▼
 *           wm.c 更新窗口状态（焦点、层叠、拖动……），记录“损坏区域” damage
 *                                 ▼
 *           compositor.c 每轮事件循环只合成一次、只合成损坏的矩形：
 *             桌面背景 → 各窗口（边框层 + 内容）→ 任务栏 → 菜单/弹窗 → 缩放虚线框
 *           然后把这些矩形连同鼠标光标一起拷到屏幕（display_kms.c / display_fb.c）
 *
 * 为什么不用多线程？显示服务器的所有状态（窗口列表、焦点、损坏区域）都是共享的，
 * 多线程就要到处加锁；而它的工作几乎都是“等待事件 → 处理很快 → 继续等待”，
 * 单线程 + poll() 既简单又不会有竞争条件。X11、Wayland 合成器也大都是这样做的。
 */
#ifndef SKETCHD_H
#define SKETCHD_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <sketch/gfx.h>
#include <sketch/rough.h>
#include <sketch/font.h>
#include <sketch/theme.h>
#include <sketch/proto.h>
#include "skutil.h"

/* ====================================================================== */
/*                               全局常量                                  */
/* ====================================================================== */

#define SKD_TASKBAR_H         44     /* 任务栏高度（不含顶部撕纸边） */
#define SKD_TEAR_H            6      /* 任务栏顶部“撕纸”锯齿的最大高度 */
#define SKD_MAX_CLIENTS       32     /* 同时连接的客户端上限 */
#define SKD_MAX_WINDOWS       128    /* 全部窗口上限 */
#define SKD_MAX_WIN_PER_CLIENT 16    /* 每个客户端的窗口上限（防止恶意程序耗尽内存） */
#define SKD_MAX_APPS          32     /* apps.conf 最多几个应用 */
#define SKD_DEFAULT_MIN_W     120    /* 默认最小内容尺寸（客户端没指定时） */
#define SKD_DEFAULT_MIN_H     60
#define SKD_DCLICK_MS         400    /* 双击：两次按下间隔 ≤ 400ms 且距离 ≤ 4px */
#define SKD_DCLICK_DIST       4
#define SKD_FORCE_CLOSE_MS    3000   /* 关闭请求 3 秒没反应，再点关闭就强制结束进程 */

/* 系统调色板之外的派生色（都由 theme.h 的调色板混合得到，保持风格统一） */
#define SKD_KRAFT  SK_RGB(0xD9, 0xBE, 0x93)   /* 牛皮纸：SK_ORANGE 与 SK_PAPER_DARK 混合的结果 */

/* ====================================================================== */
/*                          main.c — 公共小工具                            */
/* ====================================================================== */

extern int skd_debug;                 /* SKETCHD_DEBUG=1：输出调试日志、合成耗时 */
uint64_t skd_now_ms(void);            /* 单调时钟（开机以来的毫秒数，不受改系统时间影响） */
void     skd_quit(void);              /* 请求退出事件循环（Ctrl+Alt+Backspace、SIGTERM） */
int      skd_screen_w(void);
int      skd_screen_h(void);
/* 工作区 = 屏幕去掉任务栏，最大化窗口和初始摆放都限制在这里面 */
sk_rect  skd_work_area(void);
/* 调试日志：只有 skd_debug 时才输出 */
#define SKD_DBG(...) do { if (skd_debug) sk_log(__VA_ARGS__); } while (0)

/* ====================================================================== */
/*                  display_*.c — 显示后端（屏幕在哪里）                   */
/* ====================================================================== */

/*
 * 后端只负责“拿到一块能显示到屏幕上的内存”（scanout buffer，扫描输出缓冲区：
 * 显卡按固定频率一行行读它，送到显示器）。合成在内存里的后备缓冲区 (back buffer)
 * 中完成，再由 compositor.c 把变化的矩形拷过去。这样做的原因：
 *   1. 显存通常是“写合并 (write-combining)”映射，读它非常慢；合成需要反复读写像素，
 *      必须在普通内存里做。
 *   2. 合成过程中的中间状态不会被显示器看到（不会闪烁）。
 */
typedef struct skd_display {
    const char *name;        /* "kms" / "fbdev" / "headless"（写进 ready 日志） */
    int      w, h;           /* 分辨率 */
    uint8_t *map;            /* 扫描输出缓冲区的映射 */
    int      pitch;          /* 每行字节数（可能大于 w*bpp/8，必须按它换行！） */
    int      bpp;            /* 32（XRGB8888）或 16（RGB565） */
    /* 拷贝完一批矩形之后调用（KMS 用 DIRTYFB 告诉驱动哪里变了）；可为 NULL */
    void   (*commit)(struct skd_display *d, const sk_rect *rects, int n);
    /* 恢复原来的显示状态并释放资源 */
    void   (*close)(struct skd_display *d);
    void    *priv;
} skd_display;

/* 失败返回 -1（已打印原因）。want_w/want_h 是期望的分辨率（0 = 自动） */
int display_kms_open(skd_display *d, int want_w, int want_h);
int display_fb_open(skd_display *d);
int display_headless_open(skd_display *d, int w, int h);
/* 崩溃时尽力恢复（只做异步信号安全的 ioctl），见 vt.c */
void display_kms_emergency_restore(void);

/* ====================================================================== */
/*                           vt.c — 虚拟终端                               */
/* ====================================================================== */

int  vt_setup(int fd);          /* 切到图形模式、关闭终端键盘；fd 不是 VT 时返回 -1（不算错误） */
void vt_restore(void);          /* 恢复文字模式（可重复调用） */
void vt_install_crash_handlers(void);

/* ====================================================================== */
/*                compositor.c — 损坏区域 + 合成 + 刷到屏幕                */
/* ====================================================================== */

int        comp_init(skd_display *d);
void       comp_shutdown(void);
sk_canvas *comp_back(void);               /* 后备缓冲区（合成结果，不含光标） */
void       comp_damage(sk_rect r);         /* 这块需要重新合成 */
void       comp_damage_all(void);
void       comp_flush_only(sk_rect r);     /* 只需要重新拷到屏幕（光标移动） */
int        comp_has_work(void);
void       comp_frame(void);               /* 合成所有损坏区域并刷到屏幕 */
/* 把整个画面（不含光标）合成到 dst（截图用） */
void       comp_snapshot(sk_canvas *dst);

/* ====================================================================== */
/*                        cursor.c — 手绘鼠标光标                          */
/* ====================================================================== */

void        cursor_init(void);
void        cursor_set_shape(int id);      /* enum sk_cursor_id */
int         cursor_shape(void);
void        cursor_set_pos(int x, int y);
void        cursor_set_visible(int on);
sk_rect     cursor_rect(void);             /* 当前光标在屏幕上占的矩形（不可见时为空） */
/* 光标图像（ARGB，带真正的 alpha），左上角对应 cursor_rect() 的左上角 */
const sk_canvas *cursor_image(void);

/* ====================================================================== */
/*                    launch.c — 应用列表与启动进程                        */
/* ====================================================================== */

typedef struct skd_app {
    char     id[SK_APPNAME_MAX];   /* apps.conf 第 1 列，例如 "terminal" */
    char     icon_name[32];
    int      icon;                 /* SK_ICON_* */
    char     name[64];             /* 显示名，例如 "终端" */
    sk_color color;                /* 主题色 */
    char     command[256];         /* 命令行（可含 %f） */
} skd_app;

int            apps_load(const char *path);      /* 返回读到的应用个数 */
int            apps_count(void);
const skd_app *apps_get(int i);
const skd_app *apps_find(const char *id);
/* 启动应用；arg 可为 NULL/空串。成功返回 pid，失败 -1（会弹出提示） */
pid_t          launch_app(const char *id, const char *arg);
void           launch_reap_children(void);       /* 收到 SIGCHLD 后调用：回收僵尸进程 */
void           launch_autostart(void);           /* sketch.autostart=a,b,c */
void           launch_set_socket_path(const char *path);
/* 某个进程是不是我们启动的应用（返回应用 id，否则 NULL）——用于给窗口找图标 */
const char    *launch_app_of_pid(pid_t pid);

/* ====================================================================== */
/*                     server.c — 显示协议（服务器端）                     */
/* ====================================================================== */

typedef struct skd_client skd_client;     /* 定义在 server.c 里（其它模块不需要知道细节） */

/* 客户端共享给我们的一块像素缓冲区（memfd 的只读映射；映射建立后 fd 已关闭） */
typedef struct skd_buffer {
    uint32_t *pixels;
    size_t    map_len;
    int       w, h;
} skd_buffer;

int         server_init(const char *path);
void        server_shutdown(void);
/* 把需要 poll 的 fd 填进 pfd（返回个数）；server_dispatch 处理 poll 结果 */
int         server_pollfds(struct pollfd *pfd, int max);
void        server_dispatch(const struct pollfd *pfd, int n);
const char *server_client_app(const skd_client *c);
pid_t       server_client_pid(const skd_client *c);   /* 来自 SO_PEERCRED，可信 */
void        server_kill_client(skd_client *c);        /* SIGKILL 它的进程（强制关闭） */
/* 发送事件（失败时内部处理排队/断开，调用者不用管） */
void server_send_mouse(skd_client *c, uint32_t win, uint32_t kind, int x, int y,
                       uint32_t buttons, uint32_t button, int wheel, uint32_t clicks,
                       uint32_t mods);
void server_send_key(skd_client *c, uint32_t win, uint32_t keycode, uint32_t state,
                     uint32_t mods, uint32_t codepoint);
void server_send_focus(skd_client *c, uint32_t win, int focused);
void server_send_configure(skd_client *c, uint32_t win, int w, int h);
void server_send_close(skd_client *c, uint32_t win);
void buffer_release(skd_buffer *b);
/* wm 销毁窗口 / 换缓冲区时通知 server 归还该客户端的配额 */
void server_window_gone(skd_client *c, size_t bytes);
void server_buffer_replaced(skd_client *c, size_t old_bytes);

/* ====================================================================== */
/*                         wm.c — 窗口管理器                               */
/* ====================================================================== */

enum { SKD_STRIP_TOP = 0, SKD_STRIP_BOTTOM, SKD_STRIP_LEFT, SKD_STRIP_RIGHT, SKD_STRIP_COUNT };

typedef struct skd_window {
    uint32_t    id;
    skd_client *client;
    char        title[SK_TITLE_MAX];
    char        app[SK_APPNAME_MAX];   /* HELLO 里的应用名 */
    int         icon;                  /* 任务栏图标（按 apps.conf 推断） */
    uint32_t    flags;                 /* enum sk_window_flags */
    sk_color    accent;
    int         min_w, min_h;
    int         cursor;                /* 客户端设置的光标（内容区内显示） */
    uint32_t    seed;                  /* 手绘随机种子：同一窗口每次重画都一样 */

    skd_buffer  buf;
    sk_canvas   content;               /* 指向 buf 的画布，只作为 blit 的源 */
    sk_rect     rect;                  /* 内容区在屏幕上的位置；尺寸 = 缓冲区尺寸 */

    int         mapped;                /* 收到第一次 DAMAGE/SHOW 后才显示（避免先闪一下黑框） */
    int         minimized;
    int         maximized;
    sk_rect     restore_rect;          /* 最大化之前的内容区 */
    uint32_t    order;                 /* 创建顺序（任务栏按它排列） */

    /* 边框层：四条边各一块小画布（中间被内容挡住，不必保存），坐标相对内容区左上角 */
    sk_canvas  *strip[SKD_STRIP_COUNT];
    sk_rect     strip_off[SKD_STRIP_COUNT];
    int         frame_valid;
    int         frame_focused, frame_hover, frame_w, frame_h;   /* 上次渲染时的状态 */
    char        frame_title[SK_TITLE_MAX];

    uint64_t    close_req_ms;          /* 上次发送 CLOSE 的时刻（强制关闭用） */
} skd_window;

void        wm_init(void);
void        wm_shutdown(void);
/* 由 server.c 调用（协议请求已经验证过） */
skd_window *wm_create_window(skd_client *c, const char *app, const sk_req_create_window *req,
                             skd_buffer *buf);
void        wm_window_announced(skd_window *w);   /* 已回复 WINDOW_CREATED：给焦点、补发 CONFIGURE */
void        wm_destroy_window(skd_window *w);
void        wm_destroy_client_windows(skd_client *c);   /* 客户端断开时 */
skd_window *wm_find(uint32_t id);
void        wm_attach_buffer(skd_window *w, skd_buffer *buf);
void        wm_damage_content(skd_window *w, sk_rect r);
void        wm_set_title(skd_window *w, const char *title);
void        wm_show_window(skd_window *w);
void        wm_move_window(skd_window *w, int x, int y);
void        wm_set_cursor(skd_window *w, int cursor);
/* 输入（由 input.c 调用） */
void        wm_pointer_motion(int x, int y);
void        wm_pointer_button(uint32_t button, int pressed);
void        wm_pointer_wheel(int delta);
void        wm_key(uint32_t keycode, uint32_t state, uint32_t mods, uint32_t codepoint);
/* 合成（由 compositor.c 调用） */
int         wm_covering_index(sk_rect r);   /* 完全盖住 r 的最上层窗口的层号，没有返回 -1 */
void        wm_paint(sk_canvas *dst, sk_rect r, int from_index);
void        wm_paint_overlay(sk_canvas *dst, sk_rect r);   /* 缩放虚线框 */
/* 查询与操作（任务栏、菜单使用） */
int         wm_task_list(skd_window **out, int max);       /* 任务栏上的窗口，按创建顺序 */
skd_window *wm_focused(void);
void        wm_activate(skd_window *w);    /* 恢复（如果最小化）+ 置顶 + 焦点 */
void        wm_minimize(skd_window *w);
void        wm_toggle_maximize(skd_window *w);
void        wm_request_close(skd_window *w);
void        wm_set_focus(skd_window *w);   /* NULL = 桌面获得焦点 */
int         wm_window_count(void);
void        wm_refresh_cursor(void);       /* 重新判断光标形状（弹窗开关后） */
void        wm_cancel_grabs(void);         /* 弹出菜单时取消正在进行的拖动 */
int         wm_pointer_x(void);
int         wm_pointer_y(void);

/* ====================================================================== */
/*                  desktop.c — 桌面背景、图标、右键菜单                    */
/* ====================================================================== */

void desktop_init(int w, int h);
void desktop_shutdown(void);
void desktop_paint(sk_canvas *dst, sk_rect r);
/* 鼠标在桌面空白处（没有窗口、不在任务栏）按下 */
void desktop_pointer_down(int x, int y, uint32_t button, int clicks);
void desktop_cycle_wallpaper(void);
void desktop_clear_selection(void);
const char *desktop_wallpaper_name(void);

/* ====================================================================== */
/*                         taskbar.c — 任务栏                              */
/* ====================================================================== */

void    taskbar_init(int w, int h);
void    taskbar_shutdown(void);
int     taskbar_contains(int x, int y);
int     taskbar_covers(sk_rect r);          /* r 完全落在任务栏不透明部分内 */
void    taskbar_paint(sk_canvas *dst, sk_rect r);
void    taskbar_pointer_motion(int x, int y);   /* x<0 表示指针离开任务栏 */
void    taskbar_pointer_down(int x, int y, uint32_t button);
void    taskbar_pointer_up(int x, int y, uint32_t button);
void    taskbar_windows_changed(void);      /* 窗口增删、标题/焦点/最小化变化 */
void    taskbar_set_start_pressed(int on);  /* 开始菜单打开时按钮保持按下 */
sk_rect taskbar_start_rect(void);
int     taskbar_clock_fd(void);             /* timerfd：每个整分钟触发 */
int     taskbar_meter_fd(void);             /* timerfd：每 2 秒触发 */
void    taskbar_clock_tick(void);
void    taskbar_meter_tick(void);

/* ====================================================================== */
/*          menu.c — 开始菜单、右键菜单、确认框、Alt+Tab、提示条            */
/* ====================================================================== */

void menu_init(int w, int h);
void menu_shutdown(void);
void menu_paint(sk_canvas *dst, sk_rect r);
int  menu_is_open(void);                    /* 有需要抢占鼠标/键盘的弹窗 */
/* 弹窗打开时，鼠标/键盘事件先交给菜单；返回 1 表示已处理（不再往下传） */
int  menu_pointer_motion(int x, int y);
int  menu_pointer_down(int x, int y, uint32_t button);
int  menu_pointer_up(int x, int y, uint32_t button);
int  menu_key(uint32_t keycode, uint32_t state, uint32_t mods);
void menu_toggle_start(void);
void menu_open_desktop(int x, int y);       /* 桌面右键菜单 */
void menu_confirm_power(int reboot);        /* “确定要关机吗？” */
void menu_close_all(void);
/* Alt+Tab 切换窗口的浮层 */
void menu_switcher_show(skd_window **wins, int n, int sel);
void menu_switcher_hide(void);
/* 右下角的小提示条，ms 毫秒后自动消失 */
void menu_toast(const char *text, int ms);
int  menu_timeout_ms(void);                 /* 下一次需要醒来的时间（-1 = 不需要） */
void menu_tick(void);

/* ====================================================================== */
/*                         splash.c — 启动动画                             */
/* ====================================================================== */

void splash_start(int w, int h);
int  splash_active(void);
int  splash_timeout_ms(void);   /* 距离下一帧的毫秒数（-1 = 动画已结束） */
void splash_tick(void);         /* 推进动画；结束时自动切换到桌面 */
void splash_skip(void);         /* 按键/点击跳过 */
void splash_paint(sk_canvas *dst, sk_rect r);
/* 动画结束（或被跳过、或 nosplash）时 main.c 要做的事：打印 ready、自动启动应用 */
void skd_desktop_ready(void);

/* ====================================================================== */
/*                input.c / keymap.c / vmmouse.c — 输入设备                */
/* ====================================================================== */

int         input_init(int screen_w, int screen_h);
void        input_shutdown(void);
int         input_pollfds(struct pollfd *pfd, int max);
void        input_dispatch(const struct pollfd *pfd, int n);
int         input_timeout_ms(void);   /* vmmouse 按住按钮时的安全网轮询 */
void        input_tick(void);
const char *input_mouse_kind(void);   /* "vmmouse" / "evdev-abs" / "evdev-rel" / "none" */
uint32_t    input_modifiers(void);    /* 当前修饰键（enum sk_modifier） */
uint32_t    input_buttons(void);      /* 当前按住的鼠标按钮 */

/* keymap.c：把 evdev 键码翻译成字符 */
typedef struct skd_keymap_state {
    int shift_l, shift_r, ctrl_l, ctrl_r, alt_l, alt_r, meta_l, meta_r;
    int caps_lock, num_lock;
} skd_keymap_state;
/* 更新修饰键状态；返回 1 表示 LED 状态（Caps/Num Lock）变了 */
int      keymap_update(skd_keymap_state *st, uint32_t keycode, int value);
uint32_t keymap_modifiers(const skd_keymap_state *st);
/* 键码 → Unicode（不可打印键返回 0）。*keycode 可能被改写（小键盘在 NumLock 关时变成方向键） */
uint32_t keymap_translate(const skd_keymap_state *st, uint32_t *keycode);

/* vmmouse.c：VMware 绝对坐标鼠标（用户态驱动） */
typedef struct skd_vmmouse_packet {
    int      relative;   /* 1 = 相对移动包（dx,dy），0 = 绝对坐标 */
    int      x, y;       /* 绝对：0..0xFFFF；相对：带符号增量 */
    int      wheel;      /* 正数 = 向上滚 */
    uint32_t buttons;    /* SK_BUTTON_* */
} skd_vmmouse_packet;
int  vmmouse_init(void);                 /* 0 = 成功启用绝对坐标模式 */
/* 取出队列里的数据包，返回个数（0 = 没有），出错时自动重新启用并返回 -1 */
int  vmmouse_read(skd_vmmouse_packet *out, int max);
void vmmouse_shutdown(void);

#endif /* SKETCHD_H */
