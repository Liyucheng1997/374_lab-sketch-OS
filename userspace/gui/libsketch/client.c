/*
 * client.c — 应用程序一侧的显示协议实现（sketch/client.h）
 *
 * 一个图形应用和显示服务器 sketchd 之间只有两样东西：
 *
 *   1. 一条 Unix 域套接字（SOCK_SEQPACKET）。
 *      SEQPACKET = “可靠、有序、保留报文边界”：一次 send() 发出的一个报文，对方一次 recv()
 *      就完整收到这一个报文，不会像 TCP 字节流那样粘在一起或被拆开，所以我们不需要自己分帧。
 *      请求（创建窗口、提交 damage……）和事件（鼠标、键盘……）都走这条套接字。
 *
 *   2. 每个窗口一块共享内存（memfd）。
 *      memfd_create() 在内核里创建一个“匿名文件”（只存在于内存里，没有路径），
 *      ftruncate() 设定大小，mmap() 映射进自己的地址空间后就能当数组用。
 *      再通过套接字的 SCM_RIGHTS “辅助数据”把这个文件描述符**传给服务器进程**——
 *      内核会在服务器进程里新建一个 fd 指向同一个文件，服务器也 mmap 它，
 *      于是两个进程看到的是同一批物理内存页：应用画完，服务器直接读，一个像素都不用拷贝。
 *      我们还给它加上 F_SEAL_SHRINK/F_SEAL_GROW “封印”：之后谁都不能再改变它的大小，
 *      服务器因此不必担心客户端偷偷把文件缩小导致它读内存时收到 SIGBUS 而崩溃。
 *
 * 事件循环（sk_app_run）是单线程的，结构是经典的 “poll 循环”：
 *
 *     for (;;) {
 *         分发已经收到、还没处理的事件；
 *         触发到期的定时器；
 *         运行空闲回调（控件库在这里统一重画脏区域）；
 *         把本轮积累的 damage 合并后发给服务器；
 *         poll(套接字 + 应用注册的 fd, 超时 = 最近的定时器);   ← 没事做时进程在这里睡眠，不占 CPU
 *         读取套接字上所有到达的报文放进队列；调用就绪 fd 的回调；
 *     }
 *
 * 健壮性约定：
 *   - 回调里可以随意注册/注销 fd、定时器、空闲回调，可以销毁任何窗口（包括正在处理事件的窗口），
 *     可以开启嵌套事件循环（对话框）。被删除的条目只做标记，等没人遍历时再真正清理；
 *     正在处理事件的窗口推迟到处理完才释放内存。
 *   - 同一个 fd/定时器/空闲回调的回调函数不会被重入（嵌套循环里会跳过它）。
 *   - 写套接字用 MSG_NOSIGNAL：服务器崩溃时 send() 只返回 EPIPE，进程不会被 SIGPIPE 杀死；
 *     连接一断，所有事件循环都退出（sk_app_run 返回 1）。
 */
#define _GNU_SOURCE
#include "sketch/client.h"
#include "sketch/theme.h"
#include "ui_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

#define CONNECT_TIMEOUT_MS  5000   /* 等 WELCOME 的时间 */
#define CREATE_TIMEOUT_MS   5000   /* 等 WINDOW_CREATED 的时间 */
#define DAMAGE_RECTS        4      /* 每个窗口最多攒几块 damage 矩形 */
#define DAMAGE_MERGE_SLACK  2048   /* 合并两块矩形时允许“多画”的像素数 */

/* ---------------------------------------------------------------- 数据结构 */

/* 收到但还没分发的报文（FIFO 队列） */
typedef struct msg_node {
    struct msg_node *next;
    int len;
    unsigned char data[];
} msg_node;

struct sk_window {
    sk_app    *app;
    sk_window *next;
    uint32_t   id;
    sk_canvas  canvas;            /* 指向共享内存；RESIZE 后对象地址不变，内容/尺寸变 */
    void      *map;               /* mmap 的起始地址 */
    size_t     map_size;
    unsigned   flags;
    int        min_w, min_h;
    sk_event_fn handler;
    void      *user;
    sk_window_destroy_fn destroy_hook;
    void      *destroy_user;
    char       title[SK_TITLE_MAX];
    int        cursor;            /* 最近一次发给服务器的光标，-1=还没发过 */
    int        focused;
    sk_rect    damage[DAMAGE_RECTS];
    int        ndamage;
    int        busy;              /* >0：正在执行它的事件回调（可能嵌套） */
    int        dead;              /* 已销毁，等 busy 归零后释放 */
};

typedef struct {
    int fd;
    short events;
    sk_fd_fn fn;
    void *user;
    int removed;
    int running;
} fd_entry;

typedef struct {
    int id;
    int interval;
    int repeat;
    uint64_t due;                 /* 下次触发的单调时钟毫秒数 */
    sk_timer_fn fn;
    void *user;
    int removed;
    int running;
} timer_entry;

typedef struct {
    int id;
    sk_idle_fn fn;
    void *user;
    int removed;
    int running;
} idle_entry;

struct sk_app {
    int        fd;                /* 与服务器的套接字 */
    int        dead;              /* 连接已断开 */
    char       name[SK_APPNAME_MAX];
    int        screen_w, screen_h;

    sk_window *windows;           /* 活着的窗口 */
    int        nwindows;
    sk_window *graveyard;         /* 已销毁但还在回调里、暂不能释放的窗口 */
    sk_window *modal;

    msg_node  *q_head, *q_tail;   /* 事件队列 */
    uint32_t   next_serial;

    fd_entry    *fds;    int nfds,    cap_fds;
    timer_entry *timers; int ntimers, cap_timers;
    idle_entry  *idles;  int nidles,  cap_idles;
    int        walking;           /* >0 时有人在遍历上面三个数组，删除只做标记 */
    int        next_timer_id, next_idle_id;

    uint32_t  *clip_req;          /* 等待剪贴板回复的窗口 id（FIFO） */
    int        nclip, cap_clip;

    int        quit, quit_code;
    int        run_depth;         /* 嵌套事件循环层数 */
    int        free_pending;      /* 在循环内部调用了 sk_app_disconnect */
};

/* ---------------------------------------------------------------- 小工具 */

uint64_t sk_now_ms(void)
{
    /* CLOCK_MONOTONIC：开机后单调递增的时钟，不受用户修改系统时间影响，定时器必须用它 */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void app_warn(const sk_app *app, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void app_warn(const sk_app *app, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[%s] libsketch: ", app && app->name[0] ? app->name : "?");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* 把 UTF-8 字符串复制进定长数组：截断时不把一个多字节字符切成两半 */
static void copy_utf8(char *dst, size_t size, const char *src)
{
    size_t n = 0;
    if (!size)
        return;
    if (src) {
        n = strlen(src);
        if (n > size - 1) {
            n = size - 1;
            /* 10xxxxxx 是多字节字符的后续字节：退回到一个字符的开头 */
            while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
                n--;
        }
        memcpy(dst, src, n);
    }
    memset(dst + n, 0, size - n);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static long long rect_area(sk_rect r) { return (r.w > 0 && r.h > 0) ? (long long)r.w * r.h : 0; }

static sk_rect rect_bbox(sk_rect a, sk_rect b)
{
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return sk_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static sk_rect rect_clip(sk_rect r, int w, int h)
{
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > w ? w : r.x + r.w, y1 = r.y + r.h > h ? h : r.y + r.h;
    if (x1 <= x0 || y1 <= y0)
        return sk_rect_make(0, 0, 0, 0);
    return sk_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static int rect_inside(sk_rect outer, sk_rect in)
{
    return in.x >= outer.x && in.y >= outer.y &&
           in.x + in.w <= outer.x + outer.w && in.y + in.h <= outer.y + outer.h;
}

/* 数组扩容（成功 0） */
static int grow(void **arr, int *cap, int need, size_t elem)
{
    if (need <= *cap)
        return 0;
    int ncap = *cap ? *cap * 2 : 8;
    while (ncap < need)
        ncap *= 2;
    void *p = realloc(*arr, (size_t)ncap * elem);
    if (!p)
        return -1;
    *arr = p;
    *cap = ncap;
    return 0;
}

/* ---------------------------------------------------------------- 发送 */

static void mark_dead(sk_app *app, const char *why)
{
    if (app->dead)
        return;
    app->dead = 1;
    app_warn(app, "与显示服务器的连接已断开（%s）", why);
}

/* 发送一个报文，可附带一个 fd（SCM_RIGHTS）。成功 0 */
static int send_msg(sk_app *app, void *msg, size_t len, int fd)
{
    if (!app || app->dead || app->fd < 0)
        return -1;
    struct iovec iov = { msg, len };
    union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } ctl;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        /* “辅助数据”里放一个 SCM_RIGHTS 条目：内核会把 fd 复制到接收进程里 */
        memset(&ctl, 0, sizeof ctl);
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof ctl.buf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof fd);
    }
    for (;;) {
        ssize_t n = sendmsg(app->fd, &mh, MSG_NOSIGNAL);
        if (n >= 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN || errno == EBADF) {
            mark_dead(app, strerror(errno));
        } else {
            app_warn(app, "发送报文失败: %s", strerror(errno));
        }
        return -1;
    }
}

static void fill_hdr(sk_msg_header *h, int type, size_t size)
{
    h->type = (uint16_t)type;
    h->size = (uint16_t)size;
}

/* ---------------------------------------------------------------- 接收 */

/* 服务器发来的每种报文的最小长度；未知类型返回 0 */
static size_t ev_min_size(int type)
{
    switch (type) {
    case SK_EV_WELCOME:        return sizeof(sk_ev_welcome);
    case SK_EV_WINDOW_CREATED: return sizeof(sk_ev_window_created);
    case SK_EV_MOUSE:          return sizeof(sk_ev_mouse);
    case SK_EV_KEY:            return sizeof(sk_ev_key);
    case SK_EV_FOCUS:          return sizeof(sk_ev_focus);
    case SK_EV_CONFIGURE:      return sizeof(sk_ev_configure);
    case SK_EV_CLOSE:          return sizeof(sk_ev_window);
    case SK_EV_CLIPBOARD:      return offsetof(sk_ev_clipboard, text);
    case SK_EV_ERROR:          return offsetof(sk_ev_error, message);
    default:                   return 0;
    }
}

static void queue_push(sk_app *app, const void *data, int len)
{
    msg_node *m = malloc(sizeof *m + (size_t)len);
    if (!m) {
        app_warn(app, "内存不足，丢弃一个事件");
        return;
    }
    m->next = NULL;
    m->len = len;
    memcpy(m->data, data, (size_t)len);
    if (app->q_tail)
        app->q_tail->next = m;
    else
        app->q_head = m;
    app->q_tail = m;
}

/* 从队列里摘掉 m（prev 是它的前一个节点，可为 NULL） */
static void queue_unlink(sk_app *app, msg_node *prev, msg_node *m)
{
    if (prev)
        prev->next = m->next;
    else
        app->q_head = m->next;
    if (app->q_tail == m)
        app->q_tail = prev;
}

/* 非阻塞地读完套接字里所有已到达的报文，放进队列。返回读到的个数 */
static int read_socket(sk_app *app)
{
    unsigned char buf[SK_MSG_MAX];
    union { struct cmsghdr align; char b[CMSG_SPACE(sizeof(int) * 8)]; } ctl;
    int count = 0;
    while (!app->dead) {
        struct iovec iov = { buf, sizeof buf };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctl.b;
        mh.msg_controllen = sizeof ctl.b;
        ssize_t n = recvmsg(app->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                mark_dead(app, strerror(errno));
            break;
        }
        if (n == 0) {                       /* 对方关闭了连接 */
            mark_dead(app, "服务器关闭了连接");
            break;
        }
        /* 服务器不应该给我们发 fd；万一发了，关掉免得泄漏 */
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                int nfd = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < nfd; i++) {
                    int fd;
                    memcpy(&fd, CMSG_DATA(cm) + i * sizeof(int), sizeof fd);
                    close(fd);
                }
            }
        }
        if (mh.msg_flags & MSG_TRUNC) {
            app_warn(app, "收到过长的报文（>%d 字节），已丢弃", SK_MSG_MAX);
            continue;
        }
        const sk_msg_header *h = (const sk_msg_header *)buf;
        size_t need = n >= (ssize_t)sizeof *h ? ev_min_size(h->type) : 0;
        if (n < (ssize_t)sizeof *h || h->size != n || need == 0 || (size_t)n < need) {
            app_warn(app, "收到格式错误的报文（类型 %d，%d 字节），已丢弃",
                     n >= (ssize_t)sizeof *h ? h->type : -1, (int)n);
            continue;
        }
        queue_push(app, buf, (int)n);
        count++;
    }
    return count;
}

/* 等待套接字可读，最多 timeout_ms 毫秒。返回 >0 可读，0 超时，<0 出错 */
static int wait_readable(sk_app *app, int timeout_ms)
{
    struct pollfd p = { app->fd, POLLIN, 0 };
    for (;;) {
        int r = poll(&p, 1, timeout_ms);
        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

/* 同步等待某个报文：match() 返回 1 的那个报文从队列中取出并返回（调用者 free）。
 * 等待期间收到的其它报文留在队列里，之后由事件循环正常分发。超时返回 NULL */
typedef int (*msg_match_fn)(const msg_node *m, uint32_t arg);

static msg_node *wait_for(sk_app *app, msg_match_fn match, uint32_t arg, int timeout_ms)
{
    uint64_t deadline = sk_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        msg_node *prev = NULL;
        for (msg_node *m = app->q_head; m; prev = m, m = m->next) {
            if (match(m, arg)) {
                queue_unlink(app, prev, m);
                return m;
            }
        }
        if (app->dead)
            return NULL;
        uint64_t now = sk_now_ms();
        if (now >= deadline)
            return NULL;
        int r = wait_readable(app, (int)(deadline - now));
        if (r < 0) {
            mark_dead(app, strerror(errno));
            return NULL;
        }
        if (r > 0)
            read_socket(app);
    }
}

static int match_welcome(const msg_node *m, uint32_t arg)
{
    (void)arg;
    return ((const sk_msg_header *)m->data)->type == SK_EV_WELCOME;
}

/* WINDOW_CREATED（serial 匹配），或者针对 CREATE_WINDOW 的 ERROR */
static int match_created(const msg_node *m, uint32_t serial)
{
    const sk_msg_header *h = (const sk_msg_header *)m->data;
    if (h->type == SK_EV_WINDOW_CREATED)
        return ((const sk_ev_window_created *)m->data)->serial == serial;
    if (h->type == SK_EV_ERROR && m->len >= (int)offsetof(sk_ev_error, message))
        return ((const sk_ev_error *)m->data)->request_type == SK_REQ_CREATE_WINDOW;
    return 0;
}

static void print_error(sk_app *app, const msg_node *m)
{
    const sk_ev_error *e = (const sk_ev_error *)m->data;
    char text[sizeof e->message + 1] = "";
    if (m->len >= (int)sizeof *e) {
        memcpy(text, e->message, sizeof e->message);
        text[sizeof e->message] = 0;
    }
    app_warn(app, "服务器报告错误：请求 %u 失败（错误码 %u）%s%s",
             e->request_type, e->code, text[0] ? "：" : "", text);
}

/* ---------------------------------------------------------------- 连接 */

sk_app *sk_app_connect(const char *app_name)
{
    sk_app *app = calloc(1, sizeof *app);
    if (!app) {
        fprintf(stderr, "libsketch: 内存不足\n");
        return NULL;
    }
    copy_utf8(app->name, sizeof app->name, app_name && *app_name ? app_name : "app");
    app->next_serial = 1;
    app->next_timer_id = 1;
    app->next_idle_id = 1;

    const char *path = getenv(SK_SOCKET_ENV);
    if (!path || !*path)
        path = SK_SOCKET_PATH;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        app_warn(app, "套接字路径太长: %s", path);
        free(app);
        return NULL;
    }
    strcpy(addr.sun_path, path);

    /* SOCK_CLOEXEC：exec 别的程序时自动关闭，子进程（例如终端里运行的 shell）不会继承这条连接 */
    app->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (app->fd < 0) {
        app_warn(app, "创建套接字失败: %s", strerror(errno));
        free(app);
        return NULL;
    }
    if (connect(app->fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        app_warn(app, "无法连接显示服务器 %s: %s（sketchd 在运行吗？可以用环境变量 %s 指定套接字）",
                 path, strerror(errno), SK_SOCKET_ENV);
        close(app->fd);
        free(app);
        return NULL;
    }

    sk_req_hello hello;
    memset(&hello, 0, sizeof hello);
    fill_hdr(&hello.hdr, SK_REQ_HELLO, sizeof hello);
    hello.version = SK_PROTO_VERSION;
    hello.pid = (uint32_t)getpid();
    copy_utf8(hello.app_name, sizeof hello.app_name, app->name);
    if (send_msg(app, &hello, sizeof hello, -1) < 0) {
        app_warn(app, "向显示服务器发送 HELLO 失败");
        sk_app_disconnect(app);
        return NULL;
    }

    msg_node *m = wait_for(app, match_welcome, 0, CONNECT_TIMEOUT_MS);
    if (!m) {
        app_warn(app, "显示服务器 %s 在 %d 秒内没有回应 WELCOME（服务器卡住了，或者协议不兼容）",
                 path, CONNECT_TIMEOUT_MS / 1000);
        sk_app_disconnect(app);
        return NULL;
    }
    const sk_ev_welcome *wel = (const sk_ev_welcome *)m->data;
    if (wel->version != SK_PROTO_VERSION)
        app_warn(app, "协议版本不一致：服务器 %u，本程序 %d（继续尝试）", wel->version, SK_PROTO_VERSION);
    app->screen_w = wel->screen_w;
    app->screen_h = wel->screen_h;
    free(m);
    return app;
}

int sk_app_screen_w(const sk_app *app) { return app ? app->screen_w : 0; }
int sk_app_screen_h(const sk_app *app) { return app ? app->screen_h : 0; }

/* ---------------------------------------------------------------- 共享内存缓冲区 */

/* 新建一块 w*h*4 字节、加了封印的共享内存并映射。成功返回 fd（调用者发送后关闭） */
static int alloc_buffer(sk_app *app, int w, int h, void **map_out, size_t *size_out)
{
    size_t size = (size_t)w * (size_t)h * 4;
    int fd = memfd_create("sketch-window", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        app_warn(app, "memfd_create 失败: %s", strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        app_warn(app, "ftruncate(%zu) 失败: %s", size, strerror(errno));
        close(fd);
        return -1;
    }
    /* 封印：从此这个文件的大小不能再变（服务器会检查 F_SEAL_SHRINK） */
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) < 0) {
        app_warn(app, "给共享内存加封印失败: %s", strerror(errno));
        close(fd);
        return -1;
    }
    /* MAP_SHARED：写入直接进到共享的页里，服务器那边立刻能看到 */
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        app_warn(app, "mmap %dx%d 窗口缓冲区失败: %s", w, h, strerror(errno));
        close(fd);
        return -1;
    }
    *map_out = p;
    *size_out = size;
    return fd;
}

static void canvas_attach(sk_window *w, void *map, int width, int height)
{
    sk_canvas_init(&w->canvas, (uint32_t *)map, width, height, width);
}

/* ---------------------------------------------------------------- 窗口 */

static sk_window *find_window(sk_app *app, uint32_t id)
{
    for (sk_window *w = app->windows; w; w = w->next)
        if (w->id == id && !w->dead)
            return w;
    return NULL;
}

sk_window *sk_window_create(sk_app *app, const char *title, int w, int h,
                            unsigned flags, sk_color accent)
{
    if (!app || app->dead)
        return NULL;
    w = clampi(w, 1, SK_MAX_WIN_SIZE);
    h = clampi(h, 1, SK_MAX_WIN_SIZE);

    sk_window *win = calloc(1, sizeof *win);
    if (!win) {
        app_warn(app, "内存不足，无法创建窗口");
        return NULL;
    }
    win->app = app;
    win->flags = flags;
    win->cursor = -1;
    win->focused = 1;                  /* 新窗口通常立刻获得焦点；服务器随后会发 FOCUS 事件确认 */
    copy_utf8(win->title, sizeof win->title, title ? title : "");

    int fd = alloc_buffer(app, w, h, &win->map, &win->map_size);
    if (fd < 0) {
        free(win);
        return NULL;
    }
    canvas_attach(win, win->map, w, h);
    /* 内容先铺成纸色：服务器一拿到缓冲区就可能显示它（新 memfd 全是 0 = 黑色） */
    sk_clear(&win->canvas, SK_PAPER);

    sk_req_create_window req;
    memset(&req, 0, sizeof req);
    fill_hdr(&req.hdr, SK_REQ_CREATE_WINDOW, sizeof req);
    req.serial = app->next_serial++;
    req.w = w;
    req.h = h;
    req.flags = flags;
    req.accent = accent;
    copy_utf8(req.title, sizeof req.title, win->title);
    int rc = send_msg(app, &req, sizeof req, fd);
    close(fd);                         /* 已经传给服务器了；我们自己靠 mmap 访问，不再需要 fd */
    if (rc < 0) {
        munmap(win->map, win->map_size);
        free(win);
        return NULL;
    }

    /* 同步等待服务器确认；期间到达的其它事件留在队列里，稍后照常分发 */
    msg_node *m = wait_for(app, match_created, req.serial, CREATE_TIMEOUT_MS);
    if (!m || ((const sk_msg_header *)m->data)->type != SK_EV_WINDOW_CREATED) {
        if (m)
            print_error(app, m);
        else if (!app->dead)
            app_warn(app, "创建窗口「%s」超时：服务器 %d 秒内没有回应", win->title, CREATE_TIMEOUT_MS / 1000);
        free(m);
        munmap(win->map, win->map_size);
        free(win);
        return NULL;
    }
    win->id = ((const sk_ev_window_created *)m->data)->win;
    free(m);

    win->next = app->windows;
    app->windows = win;
    app->nwindows++;
    return win;
}

void sk_window_set_min_size(sk_window *w, int min_w, int min_h)
{
    if (!w || w->dead)
        return;
    /* 协议里最小尺寸只能随 CREATE_WINDOW 一起发送，而窗口已经建好了，
     * 所以这里在客户端执行：服务器要求的尺寸小于最小值时，我们按最小值分配缓冲区，
     * ATTACH_BUFFER 里报告的就是实际尺寸。 */
    w->min_w = clampi(min_w, 0, SK_MAX_WIN_SIZE);
    w->min_h = clampi(min_h, 0, SK_MAX_WIN_SIZE);
}

static void free_window(sk_window *w)
{
    if (w->map)
        munmap(w->map, w->map_size);
    free(w);
}

/* 把 w 从 graveyard 里取出并释放（busy 已归零时调用） */
static void reap_window(sk_app *app, sk_window *w)
{
    for (sk_window **pp = &app->graveyard; *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            free_window(w);
            return;
        }
    }
}

void sk_window_destroy(sk_window *w)
{
    if (!w || w->dead)
        return;
    sk_app *app = w->app;
    w->dead = 1;
    w->ndamage = 0;

    /* 先通知使用者（sk_ui），让它断开与这个窗口的联系 */
    sk_window_destroy_fn hook = w->destroy_hook;
    w->destroy_hook = NULL;
    if (hook)
        hook(w, w->destroy_user);

    if (!app->dead) {
        sk_req_window req;
        memset(&req, 0, sizeof req);
        fill_hdr(&req.hdr, SK_REQ_DESTROY_WINDOW, sizeof req);
        req.win = w->id;
        send_msg(app, &req, sizeof req, -1);
    }
    if (app->modal == w)
        app->modal = NULL;

    for (sk_window **pp = &app->windows; *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            app->nwindows--;
            break;
        }
    }
    if (w->busy > 0) {
        /* 正在执行它的事件回调（比如“关闭”按钮的回调里销毁了自己的窗口）：
         * 画布还可能被访问，先放进“墓地”，等回调返回后再释放内存 */
        w->next = app->graveyard;
        app->graveyard = w;
    } else {
        free_window(w);
    }
}

sk_canvas *sk_window_canvas(sk_window *w) { return w ? &w->canvas : NULL; }
int sk_window_width(const sk_window *w) { return w ? w->canvas.w : 0; }
int sk_window_height(const sk_window *w) { return w ? w->canvas.h : 0; }
sk_app *sk_window_app(const sk_window *w) { return w ? w->app : NULL; }
void *sk_window_userdata(const sk_window *w) { return w ? w->user : NULL; }

void sk_window_set_handler(sk_window *w, sk_event_fn fn, void *user)
{
    if (!w)
        return;
    w->handler = fn;
    w->user = user;
}

void sk_window_set_title(sk_window *w, const char *title)
{
    if (!w || w->dead)
        return;
    copy_utf8(w->title, sizeof w->title, title ? title : "");
    sk_req_set_title req;
    memset(&req, 0, sizeof req);
    fill_hdr(&req.hdr, SK_REQ_SET_TITLE, sizeof req);
    req.win = w->id;
    memcpy(req.title, w->title, sizeof req.title);
    send_msg(w->app, &req, sizeof req, -1);
}

void sk_window_set_cursor(sk_window *w, int cursor)
{
    if (!w || w->dead)
        return;
    if (cursor < 0 || cursor >= SK_CURSOR_COUNT)
        cursor = SK_CURSOR_ARROW;
    if (cursor == w->cursor)           /* 没变就不发：鼠标每动一下都可能调用到这里 */
        return;
    w->cursor = cursor;
    sk_req_set_cursor req;
    memset(&req, 0, sizeof req);
    fill_hdr(&req.hdr, SK_REQ_SET_CURSOR, sizeof req);
    req.win = w->id;
    req.cursor = (uint32_t)cursor;
    send_msg(w->app, &req, sizeof req, -1);
}

void sk_window_show(sk_window *w)
{
    if (!w || w->dead)
        return;
    sk_req_window req;
    memset(&req, 0, sizeof req);
    fill_hdr(&req.hdr, SK_REQ_SHOW, sizeof req);
    req.win = w->id;
    send_msg(w->app, &req, sizeof req, -1);
}

void sk_window_move(sk_window *w, int x, int y)
{
    if (!w || w->dead)
        return;
    sk_req_move req;
    memset(&req, 0, sizeof req);
    fill_hdr(&req.hdr, SK_REQ_MOVE, sizeof req);
    req.win = w->id;
    req.x = x;
    req.y = y;
    send_msg(w->app, &req, sizeof req, -1);
}

/* ---------------------------------------------------------------- damage 合并
 *
 * 应用每画一点东西就调用一次 sk_window_damage()。如果每次都立刻发报文，
 * 服务器就要为同一块区域合成很多遍。所以我们先把矩形攒起来：
 *   - 新矩形被已有矩形包含 → 丢掉；
 *   - 和某块合并后“多出来的面积”很小（相邻、重叠）→ 合并成包围盒；
 *   - 攒满 DAMAGE_RECTS 块 → 和“合并代价”最小的那块合并。
 * 每轮事件循环结束时一次性发出去。 */
static void damage_add(sk_window *w, sk_rect r)
{
    r = rect_clip(r, w->canvas.w, w->canvas.h);
    if (r.w <= 0 || r.h <= 0)
        return;
    for (;;) {
        int best = -1;
        long long best_cost = 0;
        for (int i = 0; i < w->ndamage; i++) {
            sk_rect d = w->damage[i];
            if (rect_inside(d, r))
                return;
            long long cost = rect_area(rect_bbox(d, r)) - rect_area(d) - rect_area(r);
            if (best < 0 || cost < best_cost) {
                best = i;
                best_cost = cost;
            }
        }
        if (best >= 0 && (best_cost <= DAMAGE_MERGE_SLACK || w->ndamage >= DAMAGE_RECTS)) {
            r = rect_bbox(w->damage[best], r);
            w->damage[best] = w->damage[--w->ndamage];
            continue;                  /* 合并后的大矩形可能又和别的块重叠，再检查一遍 */
        }
        w->damage[w->ndamage++] = r;
        return;
    }
}

void sk_window_damage(sk_window *w, sk_rect r)
{
    if (!w || w->dead)
        return;
    damage_add(w, r);
}

void sk_window_damage_all(sk_window *w)
{
    if (!w || w->dead)
        return;
    w->ndamage = 0;
    damage_add(w, sk_rect_make(0, 0, w->canvas.w, w->canvas.h));
}

void sk_window_flush(sk_window *w)
{
    if (!w || w->dead || !w->ndamage)
        return;
    int n = w->ndamage;
    w->ndamage = 0;
    for (int i = 0; i < n; i++) {
        sk_req_damage req;
        memset(&req, 0, sizeof req);
        fill_hdr(&req.hdr, SK_REQ_DAMAGE, sizeof req);
        req.win = w->id;
        req.x = w->damage[i].x;
        req.y = w->damage[i].y;
        req.w = w->damage[i].w;
        req.h = w->damage[i].h;
        if (send_msg(w->app, &req, sizeof req, -1) < 0)
            break;
    }
}

static void flush_all(sk_app *app)
{
    for (sk_window *w = app->windows; w; w = w->next)
        sk_window_flush(w);
}

/* ---------------------------------------------------------------- 内部接口（ui_internal.h） */

uint32_t sk_window_id(const sk_window *w) { return w ? w->id : 0; }
int sk_window_is_focused(const sk_window *w) { return w && !w->dead && w->focused; }
const char *sk_window_title(const sk_window *w) { return w ? w->title : ""; }

void sk_window_set_destroy_hook(sk_window *w, sk_window_destroy_fn fn, void *user)
{
    if (!w || w->dead)
        return;
    w->destroy_hook = fn;
    w->destroy_user = user;
}

sk_window *sk_app_set_modal(sk_app *app, sk_window *w)
{
    if (!app)
        return NULL;
    sk_window *old = app->modal;
    app->modal = (w && !w->dead) ? w : NULL;
    return old;
}

sk_window *sk_app_modal(const sk_app *app) { return app ? app->modal : NULL; }

/* ---------------------------------------------------------------- 剪贴板与启动 */

void sk_clipboard_set(sk_app *app, const char *text, int len)
{
    if (!app || app->dead)
        return;
    if (!text)
        text = "", len = 0;
    if (len < 0)
        len = (int)strlen(text);
    if (len > SK_CLIP_MAX) {
        len = SK_CLIP_MAX;
        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80)
            len--;                     /* 不切断多字节字符 */
    }
    sk_req_clipboard *req = malloc(sizeof *req);
    if (!req)
        return;
    size_t size = offsetof(sk_req_clipboard, text) + (size_t)len;   /* 变长报文：只发实际字节 */
    fill_hdr(&req->hdr, SK_REQ_SET_CLIPBOARD, size);
    req->len = (uint32_t)len;
    memcpy(req->text, text, (size_t)len);
    send_msg(app, req, size, -1);
    free(req);
}

void sk_clipboard_request(sk_app *app, sk_window *w)
{
    if (!app || app->dead || !w || w->dead)
        return;
    if (grow((void **)&app->clip_req, &app->cap_clip, app->nclip + 1, sizeof *app->clip_req) < 0)
        return;
    sk_msg_header req;
    fill_hdr(&req, SK_REQ_GET_CLIPBOARD, sizeof req);
    if (send_msg(app, &req, sizeof req, -1) < 0)
        return;
    /* 服务器按顺序回复，所以记下“第几个回复给哪个窗口” */
    app->clip_req[app->nclip++] = w->id;
}

char *sk_clipboard_get_sync(sk_app *app, int timeout_ms, int *len_out)
{
    if (len_out)
        *len_out = 0;
    if (!app || app->dead)
        return NULL;
    sk_msg_header req;
    fill_hdr(&req, SK_REQ_GET_CLIPBOARD, sizeof req);
    if (send_msg(app, &req, sizeof req, -1) < 0)
        return NULL;
    /* 回复按请求顺序到达：前面还有 nclip 个窗口在等回复，它们的回复留在队列里，
     * 我们要的是这之后的第一个 CLIPBOARD 报文 */
    int skip = app->nclip;
    uint64_t deadline = sk_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        int seen = 0;
        msg_node *prev = NULL;
        for (msg_node *m = app->q_head; m; prev = m, m = m->next) {
            if (((const sk_msg_header *)m->data)->type != SK_EV_CLIPBOARD)
                continue;
            if (seen++ < skip)
                continue;
            queue_unlink(app, prev, m);
            const sk_ev_clipboard *ev = (const sk_ev_clipboard *)m->data;
            int len = (int)ev->len;
            int avail = m->len - (int)offsetof(sk_ev_clipboard, text);
            if (len > avail)
                len = avail;
            if (len < 0)
                len = 0;
            char *text = malloc((size_t)len + 1);
            if (text) {
                memcpy(text, ev->text, (size_t)len);
                text[len] = 0;
                if (len_out)
                    *len_out = len;
            }
            free(m);
            return text;
        }
        uint64_t now = sk_now_ms();
        if (app->dead || now >= deadline)
            return NULL;
        int r = wait_readable(app, (int)(deadline - now));
        if (r < 0) {
            mark_dead(app, strerror(errno));
            return NULL;
        }
        if (r > 0)
            read_socket(app);
    }
}

void sk_app_launch(sk_app *app, const char *app_id, const char *arg)
{
    if (!app || app->dead || !app_id || !*app_id)
        return;
    sk_req_launch req;
    memset(&req, 0, sizeof req);
    fill_hdr(&req.hdr, SK_REQ_LAUNCH, sizeof req);
    copy_utf8(req.app_id, sizeof req.app_id, app_id);
    copy_utf8(req.arg, sizeof req.arg, arg ? arg : "");
    send_msg(app, &req, sizeof req, -1);
}

/* ---------------------------------------------------------------- fd / 定时器 / 空闲回调
 *
 * 三张表都是数组。遍历期间（walking>0）删除只打 removed 标记，
 * 遍历结束、没人在用时再压缩数组——这样回调里删除自己或别人都是安全的。 */

static void compact_tables(sk_app *app)
{
    if (app->walking > 0)
        return;
    int j = 0;
    for (int i = 0; i < app->nfds; i++)
        if (!app->fds[i].removed)
            app->fds[j++] = app->fds[i];
    app->nfds = j;
    j = 0;
    for (int i = 0; i < app->ntimers; i++)
        if (!app->timers[i].removed)
            app->timers[j++] = app->timers[i];
    app->ntimers = j;
    j = 0;
    for (int i = 0; i < app->nidles; i++)
        if (!app->idles[i].removed)
            app->idles[j++] = app->idles[i];
    app->nidles = j;
}

int sk_app_add_fd(sk_app *app, int fd, short events, sk_fd_fn fn, void *user)
{
    if (!app || fd < 0 || !fn)
        return -1;
    /* 同一个 fd 再注册一次 = 更新它的事件和回调 */
    for (int i = 0; i < app->nfds; i++) {
        fd_entry *e = &app->fds[i];
        if (!e->removed && e->fd == fd) {
            e->events = events;
            e->fn = fn;
            e->user = user;
            return 0;
        }
    }
    if (grow((void **)&app->fds, &app->cap_fds, app->nfds + 1, sizeof *app->fds) < 0)
        return -1;
    fd_entry *e = &app->fds[app->nfds++];
    memset(e, 0, sizeof *e);
    e->fd = fd;
    e->events = events;
    e->fn = fn;
    e->user = user;
    return 0;
}

void sk_app_remove_fd(sk_app *app, int fd)
{
    if (!app)
        return;
    for (int i = 0; i < app->nfds; i++)
        if (app->fds[i].fd == fd)
            app->fds[i].removed = 1;
    compact_tables(app);
}

int sk_app_add_timer(sk_app *app, int interval_ms, int repeat, sk_timer_fn fn, void *user)
{
    if (!app || !fn)
        return 0;
    if (interval_ms < 0)
        interval_ms = 0;
    if (repeat && interval_ms < 1)
        interval_ms = 1;               /* 周期 0 的重复定时器会让循环空转，至少 1ms */
    if (grow((void **)&app->timers, &app->cap_timers, app->ntimers + 1, sizeof *app->timers) < 0)
        return 0;
    timer_entry *t = &app->timers[app->ntimers++];
    memset(t, 0, sizeof *t);
    t->id = app->next_timer_id++;
    if (app->next_timer_id <= 0)
        app->next_timer_id = 1;
    t->interval = interval_ms;
    t->repeat = repeat ? 1 : 0;
    t->due = sk_now_ms() + (uint64_t)interval_ms;
    t->fn = fn;
    t->user = user;
    return t->id;
}

void sk_app_remove_timer(sk_app *app, int id)
{
    if (!app || id <= 0)
        return;
    for (int i = 0; i < app->ntimers; i++)
        if (app->timers[i].id == id)
            app->timers[i].removed = 1;
    compact_tables(app);
}

int sk_app_add_idle(sk_app *app, sk_idle_fn fn, void *user)
{
    if (!app || !fn)
        return 0;
    if (grow((void **)&app->idles, &app->cap_idles, app->nidles + 1, sizeof *app->idles) < 0)
        return 0;
    idle_entry *e = &app->idles[app->nidles++];
    memset(e, 0, sizeof *e);
    e->id = app->next_idle_id++;
    if (app->next_idle_id <= 0)
        app->next_idle_id = 1;
    e->fn = fn;
    e->user = user;
    return e->id;
}

void sk_app_remove_idle(sk_app *app, int id)
{
    if (!app || id <= 0)
        return;
    for (int i = 0; i < app->nidles; i++)
        if (app->idles[i].id == id)
            app->idles[i].removed = 1;
    compact_tables(app);
}

/* 触发所有到期的定时器 */
static void run_timers(sk_app *app)
{
    uint64_t now = sk_now_ms();
    app->walking++;
    int n = app->ntimers;              /* 回调里新加的定时器下一轮再说 */
    for (int i = 0; i < n && !app->dead; i++) {
        timer_entry *t = &app->timers[i];
        if (t->removed || t->running || t->due > now)
            continue;
        if (t->repeat) {
            /* 不累积误差：下次时间 = 上次“应该”触发的时间 + 周期，而不是 now + 周期。
             * 如果进程被耽搁、错过了好几个周期，就跳过错过的，只补触发这一次（不会连发一串）。 */
            t->due += (uint64_t)t->interval;
            if (t->due <= now)
                t->due += ((now - t->due) / (uint64_t)t->interval + 1) * (uint64_t)t->interval;
        } else {
            t->removed = 1;
        }
        sk_timer_fn fn = t->fn;
        void *user = t->user;
        int id = t->id;
        t->running = 1;
        fn(app, user);
        /* 回调里数组可能被 realloc，重新按下标取；用 id 确认还是同一个 */
        if (i < app->ntimers && app->timers[i].id == id)
            app->timers[i].running = 0;
    }
    app->walking--;
    compact_tables(app);
}

static void run_idles(sk_app *app)
{
    app->walking++;
    int n = app->nidles;
    for (int i = 0; i < n && !app->dead; i++) {
        idle_entry *e = &app->idles[i];
        if (e->removed || e->running)
            continue;
        sk_idle_fn fn = e->fn;
        void *user = e->user;
        int id = e->id;
        e->running = 1;
        fn(app, user);
        if (i < app->nidles && app->idles[i].id == id)
            app->idles[i].running = 0;
    }
    app->walking--;
    compact_tables(app);
}

/* poll 的超时：离最近一个定时器还有多少毫秒；没有定时器返回 -1（无限等待） */
static int poll_timeout(sk_app *app)
{
    uint64_t now = sk_now_ms();
    int64_t best = -1;
    for (int i = 0; i < app->ntimers; i++) {
        timer_entry *t = &app->timers[i];
        if (t->removed || t->running)
            continue;
        int64_t d = t->due > now ? (int64_t)(t->due - now) : 0;
        if (best < 0 || d < best)
            best = d;
    }
    if (best > INT_MAX)
        best = INT_MAX;
    return (int)best;
}

/* ---------------------------------------------------------------- 事件分发 */

static int deliver(sk_window *w, sk_event *e)
{
    if (!w->handler)
        return 0;
    w->busy++;
    int r = w->handler(w, e, w->user);
    w->busy--;
    return r;
}

/* 处理 CONFIGURE：换一块新尺寸的缓冲区，让应用重画，再 ATTACH 给服务器 */
static void handle_configure(sk_window *w, int nw, int nh)
{
    sk_app *app = w->app;
    nw = clampi(nw, w->min_w > 1 ? w->min_w : 1, SK_MAX_WIN_SIZE);
    nh = clampi(nh, w->min_h > 1 ? w->min_h : 1, SK_MAX_WIN_SIZE);

    void *map;
    size_t size;
    int fd = alloc_buffer(app, nw, nh, &map, &size);
    if (fd < 0)
        return;                        /* 内存不够：保持旧缓冲区，服务器继续显示旧内容 */

    void *old_map = w->map;
    size_t old_size = w->map_size;
    w->map = map;
    w->map_size = size;
    canvas_attach(w, map, nw, nh);     /* 同一个 sk_canvas 对象，换成新的像素/尺寸/裁剪 */
    w->ndamage = 0;                    /* 旧缓冲区上攒的 damage 作废 */

    /* 先让应用把新缓冲区画好，再交给服务器——这样服务器永远不会显示一块没画过的缓冲区 */
    sk_event e;
    memset(&e, 0, sizeof e);
    e.type = SK_EVENT_RESIZE;
    e.win = w;
    e.size.w = nw;
    e.size.h = nh;
    int handled = deliver(w, &e);

    if (!w->dead) {
        if (!handled) {
            /* 默认处理：铺一张白纸 */
            sk_reset_clip(&w->canvas);
            sk_clear(&w->canvas, SK_PAPER);
        }
        sk_req_attach_buffer req;
        memset(&req, 0, sizeof req);
        fill_hdr(&req.hdr, SK_REQ_ATTACH_BUFFER, sizeof req);
        req.win = w->id;
        req.w = nw;
        req.h = nh;
        send_msg(app, &req, sizeof req, fd);
        sk_window_damage_all(w);
    }
    close(fd);
    /* 旧缓冲区现在可以解除映射了：服务器那边有自己的映射，我们这边不再需要 */
    munmap(old_map, old_size);
}

static void handle_clipboard(sk_app *app, const msg_node *m)
{
    const sk_ev_clipboard *ev = (const sk_ev_clipboard *)m->data;
    if (app->nclip == 0)
        return;                        /* 没人要的回复 */
    uint32_t id = app->clip_req[0];
    memmove(app->clip_req, app->clip_req + 1, (size_t)(app->nclip - 1) * sizeof *app->clip_req);
    app->nclip--;
    sk_window *w = find_window(app, id);
    if (!w)
        return;
    int len = (int)ev->len;
    int avail = m->len - (int)offsetof(sk_ev_clipboard, text);
    if (len > avail)
        len = avail;
    if (len > SK_CLIP_MAX)
        len = SK_CLIP_MAX;
    if (len < 0)
        len = 0;
    char *text = malloc((size_t)len + 1);
    if (!text)
        return;
    memcpy(text, ev->text, (size_t)len);
    text[len] = 0;                     /* 给应用一个以 NUL 结尾的字符串 */
    sk_event e;
    memset(&e, 0, sizeof e);
    e.type = SK_EVENT_CLIPBOARD;
    e.win = w;
    e.clip.text = text;
    e.clip.text_len = len;
    deliver(w, &e);
    free(text);
}

/* 队列里 m 之后是否还有同一窗口的 CONFIGURE（有的话这个就没必要处理了） */
static int newer_configure_queued(const msg_node *m, uint32_t win)
{
    for (const msg_node *n = m->next; n; n = n->next) {
        const sk_msg_header *h = (const sk_msg_header *)n->data;
        if (h->type == SK_EV_CONFIGURE && ((const sk_ev_configure *)n->data)->win == win)
            return 1;
    }
    return 0;
}

/* 紧跟在后面的是不是同一窗口、同样按钮状态的鼠标移动（是的话跳过这一个）。
 * 只有应用处理不过来、事件堆积时才会发生——在很慢的模拟器里这能避免“鼠标拖影”式的卡顿。 */
static int newer_move_queued(const msg_node *m, const sk_ev_mouse *ev)
{
    const msg_node *n = m->next;
    if (!n || ((const sk_msg_header *)n->data)->type != SK_EV_MOUSE)
        return 0;
    const sk_ev_mouse *nx = (const sk_ev_mouse *)n->data;
    return nx->win == ev->win && nx->kind == SK_MOUSE_MOVE && nx->buttons == ev->buttons &&
           nx->modifiers == ev->modifiers;
}

static void dispatch_one(sk_app *app, msg_node *m)
{
    const sk_msg_header *h = (const sk_msg_header *)m->data;
    sk_event e;
    memset(&e, 0, sizeof e);
    sk_window *w = NULL;

    switch (h->type) {
    case SK_EV_MOUSE: {
        const sk_ev_mouse *ev = (const sk_ev_mouse *)m->data;
        if (ev->kind == SK_MOUSE_MOVE && newer_move_queued(m, ev))
            return;
        w = find_window(app, ev->win);
        if (!w || (app->modal && app->modal != w))
            return;                    /* 模态对话框打开时，其它窗口不接受输入 */
        e.type = SK_EVENT_MOUSE;
        e.mouse.kind = ev->kind;
        e.mouse.x = ev->x;
        e.mouse.y = ev->y;
        e.mouse.buttons = ev->buttons;
        e.mouse.button = ev->button;
        e.mouse.wheel = ev->wheel;
        e.mouse.clicks = ev->clicks;
        e.mouse.modifiers = ev->modifiers;
        break;
    }
    case SK_EV_KEY: {
        const sk_ev_key *ev = (const sk_ev_key *)m->data;
        w = find_window(app, ev->win);
        if (!w || (app->modal && app->modal != w))
            return;
        e.type = SK_EVENT_KEY;
        e.key.keycode = ev->keycode;
        e.key.state = ev->state;
        e.key.modifiers = ev->modifiers;
        e.key.codepoint = ev->codepoint;
        break;
    }
    case SK_EV_FOCUS: {
        const sk_ev_focus *ev = (const sk_ev_focus *)m->data;
        w = find_window(app, ev->win);
        if (!w)
            return;
        w->focused = ev->focused ? 1 : 0;
        e.type = SK_EVENT_FOCUS;
        e.focused = w->focused;
        break;
    }
    case SK_EV_CONFIGURE: {
        const sk_ev_configure *ev = (const sk_ev_configure *)m->data;
        w = find_window(app, ev->win);
        if (!w || newer_configure_queued(m, ev->win))
            return;                    /* 用户拖得比我们画得快：只处理最后一次尺寸 */
        w->busy++;
        handle_configure(w, ev->w, ev->h);
        w->busy--;
        goto reap;
    }
    case SK_EV_CLOSE: {
        const sk_ev_window *ev = (const sk_ev_window *)m->data;
        w = find_window(app, ev->win);
        if (!w)
            return;
        e.type = SK_EVENT_CLOSE;
        e.win = w;
        w->busy++;
        int handled = w->handler ? w->handler(w, &e, w->user) : 0;
        w->busy--;
        if (!handled)
            sk_window_destroy(w);      /* 默认动作：关掉窗口 */
        goto reap;
    }
    case SK_EV_CLIPBOARD:
        handle_clipboard(app, m);
        return;
    case SK_EV_ERROR:
        print_error(app, m);
        return;
    case SK_EV_WELCOME:
    case SK_EV_WINDOW_CREATED:
    default:
        return;                        /* 迟到的同步回复（已超时放弃），忽略 */
    }

    e.win = w;
    deliver(w, &e);
reap:
    if (w && w->dead && w->busy == 0)
        reap_window(app, w);
}

/* ---------------------------------------------------------------- 事件循环 */

enum { LOOP_FLAG, LOOP_QUIT, LOOP_NOWIN, LOOP_DEAD };

static int loop_should_exit(sk_app *app, volatile int *flag)
{
    if (flag && *flag)
        return LOOP_FLAG;
    if (app->quit)
        return LOOP_QUIT;
    if (app->dead)
        return LOOP_DEAD;
    if (app->nwindows <= 0)
        return LOOP_NOWIN;
    return -1;
}

static void app_free(sk_app *app);

static int app_loop(sk_app *app, volatile int *flag)
{
    struct pollfd *pfd = NULL;
    int *pidx = NULL;
    int cap = 0;
    int why;

    app->run_depth++;
    for (;;) {
        /* 1. 分发队列里的事件（每处理一个都检查是否该退出，让嵌套循环及时返回） */
        while ((why = loop_should_exit(app, flag)) < 0 && app->q_head) {
            msg_node *m = app->q_head;
            queue_unlink(app, NULL, m);
            dispatch_one(app, m);
            free(m);
        }
        if (why >= 0)
            break;

        /* 2. 定时器 */
        run_timers(app);
        if ((why = loop_should_exit(app, flag)) >= 0)
            break;

        /* 3. 空闲回调（控件库在这里重画），4. 把攒下的 damage 发出去 */
        run_idles(app);
        flush_all(app);
        if ((why = loop_should_exit(app, flag)) >= 0)
            break;

        /* 5. 睡眠等待：服务器的报文、应用的 fd、或者下一个定时器到期 */
        int need = 1 + app->nfds;
        if (need > cap) {
            struct pollfd *np = realloc(pfd, (size_t)need * sizeof *pfd);
            int *ni = realloc(pidx, (size_t)need * sizeof *pidx);
            if (np)
                pfd = np;
            if (ni)
                pidx = ni;
            if (!np || !ni) {
                app_warn(app, "内存不足");
                why = LOOP_DEAD;
                break;
            }
            cap = need;
        }
        int n = 0;
        pfd[n].fd = app->fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        pidx[n++] = -1;
        for (int i = 0; i < app->nfds; i++) {
            fd_entry *fe = &app->fds[i];
            if (fe->removed || fe->running)
                continue;
            pfd[n].fd = fe->fd;
            pfd[n].events = fe->events;
            pfd[n].revents = 0;
            pidx[n++] = i;
        }
        int timeout = app->q_head ? 0 : poll_timeout(app);
        int r = poll(pfd, (nfds_t)n, timeout);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            app_warn(app, "poll 失败: %s", strerror(errno));
            why = LOOP_DEAD;
            break;
        }
        if (r == 0)
            continue;

        /* 6. 读服务器报文（HUP 时也先把剩下的数据读完，read_socket 会发现 EOF） */
        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))
            read_socket(app);

        /* 7. 应用的 fd：按 fd 号重新找条目（回调可能增删了表项） */
        app->walking++;
        for (int k = 1; k < n && !app->dead; k++) {
            if (!pfd[k].revents)
                continue;
            int i = pidx[k];
            if (i >= app->nfds)
                continue;
            fd_entry *fe = &app->fds[i];
            if (fe->removed || fe->running || fe->fd != pfd[k].fd)
                continue;
            sk_fd_fn fn = fe->fn;
            void *user = fe->user;
            int fd = fe->fd;
            fe->running = 1;
            fn(app, fd, pfd[k].revents, user);
            if (i < app->nfds && app->fds[i].fd == fd)
                app->fds[i].running = 0;
        }
        app->walking--;
        compact_tables(app);
    }
    free(pfd);
    free(pidx);
    app->run_depth--;
    return why;
}

int sk_app_run(sk_app *app)
{
    if (!app)
        return 1;
    int why = app_loop(app, NULL);
    int code;
    switch (why) {
    case LOOP_QUIT:  code = app->quit_code; break;
    case LOOP_NOWIN: code = 0; break;
    default:         code = app->dead ? 1 : 0; break;
    }
    if (app->run_depth == 0) {
        app->quit = 0;                 /* 最外层循环结束：允许以后再次 sk_app_run */
        if (app->free_pending)
            app_free(app);
    }
    return code;
}

int sk_app_run_until(sk_app *app, volatile int *flag)
{
    if (!app || !flag)
        return -1;
    if (*flag)
        return 0;
    int why = app_loop(app, flag);
    if (app->run_depth == 0 && app->free_pending) {
        app_free(app);
        return -1;
    }
    return why == LOOP_FLAG ? 0 : -1;
}

void sk_app_quit(sk_app *app, int code)
{
    if (!app)
        return;
    /* 只设标志：当前回调返回后，每一层嵌套循环都会检查到它并依次退出 */
    app->quit = 1;
    app->quit_code = code;
}

/* ---------------------------------------------------------------- 断开 */

static void app_free(sk_app *app)
{
    while (app->windows)
        sk_window_destroy(app->windows);
    while (app->graveyard) {
        sk_window *w = app->graveyard;
        app->graveyard = w->next;
        free_window(w);
    }
    while (app->q_head) {
        msg_node *m = app->q_head;
        app->q_head = m->next;
        free(m);
    }
    if (app->fd >= 0)
        close(app->fd);
    free(app->fds);
    free(app->timers);
    free(app->idles);
    free(app->clip_req);
    free(app);
}

void sk_app_disconnect(sk_app *app)
{
    if (!app)
        return;
    if (app->run_depth > 0) {
        /* 在事件循环内部被调用：先让所有循环退出，等最外层返回时再真正释放 */
        while (app->windows)
            sk_window_destroy(app->windows);
        app->quit = 1;
        app->free_pending = 1;
        return;
    }
    app_free(app);
}
