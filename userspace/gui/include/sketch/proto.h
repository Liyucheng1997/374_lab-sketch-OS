/*
 * sketch/proto.h — SketchOS 显示协议（sketchd ⇄ 应用程序）
 *
 * 架构（与 Wayland 思路相同，简化版）：
 *
 *   应用进程                                   sketchd 进程（显示服务器 + 窗口管理器）
 *   ┌──────────────┐  Unix 域套接字 (SEQPACKET) ┌─────────────────────────────┐
 *   │ 在共享内存里  │ ── 请求：创建窗口/提交损坏区 ─▶│ 合成所有窗口 → 屏幕(KMS/fbdev)│
 *   │ 画自己的窗口  │ ◀─ 事件：鼠标/键盘/焦点/关闭 ── │ 读取键盘鼠标(evdev/vmmouse)  │
 *   └──────────────┘   memfd 通过 SCM_RIGHTS 传递  └─────────────────────────────┘
 *
 *   1. 应用 connect() 到 SK_SOCKET_PATH，发送 SK_REQ_HELLO，收到 SK_EV_WELCOME。
 *   2. 应用用 memfd_create() 创建 w*h*4 字节的共享内存，ftruncate 后加上
 *      F_SEAL_SHRINK 封印（防止它之后把内存缩小导致服务器访问时 SIGBUS），
 *      然后发送 SK_REQ_CREATE_WINDOW，并通过 SCM_RIGHTS 附带这个 fd。
 *   3. 服务器 mmap 这块内存（只读），回复 SK_EV_WINDOW_CREATED（带窗口 id）。
 *   4. 应用在共享内存里画图，画完发送 SK_REQ_DAMAGE 告诉服务器哪块变了；
 *      服务器只重新合成那一块。
 *   5. 用户拖动窗口右下角改变大小 → 服务器发 SK_EV_CONFIGURE(w,h) → 应用新建一块
 *      共享内存、重画，发送 SK_REQ_ATTACH_BUFFER（附带新 fd）。
 *
 * 编码：每个 SEQPACKET 报文 = sk_msg_header + 对应结构体。所有整数都是本机字节序
 * （同一台机器上的进程间通信）。报文最大 SK_MSG_MAX 字节。
 * 服务器收到格式错误的报文会断开该客户端（客户端崩溃不会影响服务器）。
 */
#ifndef SKETCH_PROTO_H
#define SKETCH_PROTO_H

#include <stdint.h>

#define SK_SOCKET_PATH     "/run/sketch/display.sock"
#define SK_SOCKET_ENV      "SKETCH_SOCKET"     /* 环境变量可覆盖套接字路径 */
#define SK_PROTO_VERSION   1
#define SK_MSG_MAX         8192
#define SK_TITLE_MAX       64
#define SK_APPNAME_MAX     32
#define SK_CLIP_MAX        4096                /* 剪贴板文本最大字节数 */
#define SK_MAX_WIN_SIZE    4096                /* 窗口宽/高上限 */

enum sk_msg_type {
    /* ---- 客户端 → 服务器 ---- */
    SK_REQ_HELLO = 1,          /* sk_req_hello */
    SK_REQ_CREATE_WINDOW,      /* sk_req_create_window + fd */
    SK_REQ_DESTROY_WINDOW,     /* sk_req_window */
    SK_REQ_ATTACH_BUFFER,      /* sk_req_attach_buffer + fd */
    SK_REQ_DAMAGE,             /* sk_req_damage */
    SK_REQ_SET_TITLE,          /* sk_req_set_title */
    SK_REQ_SHOW,               /* sk_req_window：显示/恢复并置顶 */
    SK_REQ_MOVE,               /* sk_req_move：移动窗口（内容区左上角，屏幕坐标） */
    SK_REQ_SET_CURSOR,         /* sk_req_set_cursor */
    SK_REQ_SET_CLIPBOARD,      /* sk_req_clipboard */
    SK_REQ_GET_CLIPBOARD,      /* 无负载，服务器回 SK_EV_CLIPBOARD */
    SK_REQ_LAUNCH,             /* sk_req_launch：让服务器启动一个应用（像桌面双击） */

    /* ---- 服务器 → 客户端 ---- */
    SK_EV_WELCOME = 100,       /* sk_ev_welcome */
    SK_EV_WINDOW_CREATED,      /* sk_ev_window_created */
    SK_EV_MOUSE,               /* sk_ev_mouse */
    SK_EV_KEY,                 /* sk_ev_key */
    SK_EV_FOCUS,               /* sk_ev_focus */
    SK_EV_CONFIGURE,           /* sk_ev_configure */
    SK_EV_CLOSE,               /* sk_ev_window：用户点了关闭按钮 / Alt+F4 */
    SK_EV_CLIPBOARD,           /* sk_ev_clipboard */
    SK_EV_ERROR,               /* sk_ev_error */
};

typedef struct {
    uint16_t type;   /* enum sk_msg_type */
    uint16_t size;   /* 整个报文字节数（含头） */
} sk_msg_header;

/* ---------- 请求 ---------- */
typedef struct {
    sk_msg_header hdr;
    uint32_t version;                    /* SK_PROTO_VERSION */
    uint32_t pid;
    char     app_name[SK_APPNAME_MAX];   /* 例如 "skterm"，NUL 结尾 */
} sk_req_hello;

enum sk_window_flags {
    SK_WIN_RESIZABLE = 1 << 0,   /* 允许用户拖动改变大小 */
    SK_WIN_CENTER    = 1 << 1,   /* 初始位置居中（否则由服务器层叠摆放） */
    SK_WIN_NO_DECOR  = 1 << 2,   /* 没有边框和标题栏（启动画面等） */
    SK_WIN_DIALOG    = 1 << 3,   /* 对话框：不在任务栏显示，总在父应用其它窗口之上 */
};

typedef struct {
    sk_msg_header hdr;
    uint32_t serial;             /* 客户端自选的序号，回复里原样带回 */
    int32_t  w, h;               /* 内容区尺寸（不含边框），1..SK_MAX_WIN_SIZE */
    uint32_t flags;              /* enum sk_window_flags */
    uint32_t accent;             /* 标题栏主题色 sk_color；0=默认蓝 */
    int32_t  min_w, min_h;       /* 允许缩放到的最小内容尺寸（0=不限） */
    char     title[SK_TITLE_MAX];
    /* 附带 1 个 fd：memfd，大小 >= w*h*4，像素格式 0xAARRGGBB（忽略 A），stride = w */
} sk_req_create_window;

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
} sk_req_window;

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
    int32_t  w, h;               /* 新缓冲区尺寸，stride = w；附带 1 个 memfd */
} sk_req_attach_buffer;

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
    int32_t  x, y, w, h;         /* 内容区坐标；会被裁剪到缓冲区范围 */
} sk_req_damage;

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
    char     title[SK_TITLE_MAX];
} sk_req_set_title;

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
    int32_t  x, y;
} sk_req_move;

enum sk_cursor_id {
    SK_CURSOR_ARROW = 0,   /* 手绘箭头 */
    SK_CURSOR_TEXT,        /* 文字竖线 I */
    SK_CURSOR_HAND,        /* 小手（可点击） */
    SK_CURSOR_CROSS,       /* 十字（画板） */
    SK_CURSOR_PENCIL,      /* 铅笔（画板） */
    SK_CURSOR_MOVE,        /* 四向箭头 */
    SK_CURSOR_RESIZE,      /* 斜向双箭头（右下角缩放） */
    SK_CURSOR_WAIT,        /* 沙漏 */
    SK_CURSOR_COUNT
};

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
    uint32_t cursor;             /* 鼠标在该窗口内容区内时显示的光标 */
} sk_req_set_cursor;

typedef struct {
    sk_msg_header hdr;
    uint32_t len;                /* 文本字节数 <= SK_CLIP_MAX */
    char     text[SK_CLIP_MAX];  /* 报文实际长度 = offsetof(text) + len */
} sk_req_clipboard;

typedef struct {
    sk_msg_header hdr;
    char     app_id[SK_APPNAME_MAX];   /* /etc/sketch/apps.conf 里的 id，例如 "notepad" */
    char     arg[256];                 /* 传给应用的一个参数（例如文件路径），可为空 */
} sk_req_launch;

/* ---------- 事件 ---------- */
typedef struct {
    sk_msg_header hdr;
    uint32_t version;
    int32_t  screen_w, screen_h;
} sk_ev_welcome;

typedef struct {
    sk_msg_header hdr;
    uint32_t serial;             /* 对应 sk_req_create_window.serial */
    uint32_t win;                /* 服务器分配的窗口 id（>0） */
} sk_ev_window_created;

enum sk_mouse_kind {
    SK_MOUSE_MOVE = 1,
    SK_MOUSE_DOWN,
    SK_MOUSE_UP,
    SK_MOUSE_WHEEL,
    SK_MOUSE_ENTER,
    SK_MOUSE_LEAVE,
};
enum sk_mouse_button {
    SK_BUTTON_LEFT   = 1 << 0,
    SK_BUTTON_RIGHT  = 1 << 1,
    SK_BUTTON_MIDDLE = 1 << 2,
};

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
    uint32_t kind;               /* enum sk_mouse_kind */
    int32_t  x, y;               /* 内容区坐标（按下拖动时可能超出窗口范围） */
    uint32_t buttons;            /* 当前按住的按钮位掩码 */
    uint32_t button;             /* DOWN/UP：这次变化的按钮 */
    int32_t  wheel;              /* WHEEL：正数=向上滚 */
    uint32_t clicks;             /* DOWN：1=单击 2=双击（400ms 内同一位置） */
    uint32_t modifiers;          /* enum sk_modifier */
} sk_ev_mouse;

enum sk_modifier {
    SK_MOD_SHIFT = 1 << 0,
    SK_MOD_CTRL  = 1 << 1,
    SK_MOD_ALT   = 1 << 2,
    SK_MOD_SUPER = 1 << 3,
    SK_MOD_CAPS  = 1 << 4,
};

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
    uint32_t keycode;            /* Linux evdev 键码（KEY_A 等，见 <linux/input-event-codes.h>） */
    uint32_t state;              /* 0=松开 1=按下 2=自动重复 */
    uint32_t modifiers;          /* enum sk_modifier */
    uint32_t codepoint;          /* 按美式键盘布局翻译出的 Unicode 字符；不可打印键为 0。
                                    回车=0x0D 退格=0x08 Tab=0x09 Esc=0x1B；Ctrl+字母 仍给出字母本身 */
} sk_ev_key;

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
    uint32_t focused;            /* 1=获得键盘焦点 0=失去 */
} sk_ev_focus;

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
    int32_t  w, h;               /* 服务器希望的新内容尺寸；客户端应重建缓冲区并 ATTACH */
} sk_ev_configure;

typedef struct {
    sk_msg_header hdr;
    uint32_t win;
} sk_ev_window;

typedef struct {
    sk_msg_header hdr;
    uint32_t len;
    char     text[SK_CLIP_MAX];
} sk_ev_clipboard;

typedef struct {
    sk_msg_header hdr;
    uint32_t code;               /* errno 风格的错误码 */
    uint32_t request_type;       /* 出错的请求类型 */
    char     message[128];
} sk_ev_error;

#endif /* SKETCH_PROTO_H */
