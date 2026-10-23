/*
 * server.c — 显示协议的服务器端（协议定义见 sketch/proto.h）
 *
 * 传输层：Unix 域套接字 (AF_UNIX) + SOCK_SEQPACKET
 *   - Unix 域套接字是同一台机器上进程之间的“本地网络”，地址是文件系统里的一个路径
 *     (/run/sketch/display.sock)，用 connect() 连接，和 TCP 用法几乎一样。
 *   - SEQPACKET = “有序、可靠、保留报文边界”：客户端 send 一次，我们 recv 一次就正好
 *     拿到完整的一条报文，不会像 TCP 字节流那样粘包/拆包，解析非常简单。
 *   - 它还能用 SCM_RIGHTS 传递“文件描述符”：客户端把 memfd 的 fd 塞进辅助数据
 *     (control message)，内核在我们的进程里新建一个指向同一个文件的 fd。
 *     这就是共享内存窗口缓冲区的由来（零拷贝：应用画在这块内存里，我们直接读）。
 *
 * 安全与健壮性：客户端是不可信的（可能有 bug、可能恶意、可能随时崩溃）。
 *   - 每条报文都严格检查长度、字段范围、窗口归属；违反协议 → 发 SK_EV_ERROR 并断开。
 *   - 只接受报文“应该带”的 fd 个数，多余的立即关闭（否则会泄漏 fd，最终耗尽）。
 *   - 共享内存必须加了 F_SEAL_SHRINK 封印，见 import_buffer() 里的解释。
 *   - 发送事件用 MSG_DONTWAIT：卡住的客户端不能卡住整个显示服务器。
 *   - 客户端的 pid 用内核提供的 SO_PEERCRED，而不是相信 HELLO 里自报的 pid。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define OUTQ_MAX          256                  /* 每个客户端最多积压的事件数 */
#define OUTQ_MOVE_LIMIT   (OUTQ_MAX / 2)       /* 积压超过一半时直接丢弃鼠标移动事件 */
#define STUCK_MS          10000                /* 积压持续这么久没有进展 → 断开 */
#define MAX_FDS_PER_MSG   8
#define MAX_CLIENT_BYTES  (192u << 20)         /* 每个客户端最多映射 192MB 窗口缓冲区 */
#define MAX_MSGS_PER_ROUND 64                  /* 每轮最多处理一个客户端的报文数（公平） */

typedef struct {
    uint16_t len;
    uint8_t  is_move;
    uint32_t win;
    uint8_t *data;
} outmsg;

struct skd_client {
    int      fd;
    pid_t    pid;                  /* SO_PEERCRED：内核担保的对端进程 */
    int      hello;
    int      dead;                 /* 已决定断开，等本轮结束统一清理 */
    char     app[SK_APPNAME_MAX];
    int      nwin;
    size_t   mapped_bytes;
    outmsg   q[OUTQ_MAX];          /* 发送队列（环形） */
    int      qhead, qcount;
    uint64_t stuck_since;
};

static skd_client *g_clients[SKD_MAX_CLIENTS];
static int         g_listen = -1;
static char        g_path[108];
static char        g_clip[SK_CLIP_MAX];
static uint32_t    g_clip_len;

/* 接收缓冲区：用 uint32_t 数组保证对齐，可以直接当结构体访问 */
static uint32_t g_rx[SK_MSG_MAX / 4 + 4];

/* ---------- 基础 ---------- */

const char *server_client_app(const skd_client *c) { return c ? c->app : "?"; }
pid_t server_client_pid(const skd_client *c) { return c ? c->pid : -1; }

void buffer_release(skd_buffer *b)
{
    if (b->pixels && b->map_len)
        munmap(b->pixels, b->map_len);
    memset(b, 0, sizeof *b);
}

static void q_clear(skd_client *c)
{
    while (c->qcount > 0) {
        free(c->q[c->qhead].data);
        c->qhead = (c->qhead + 1) % OUTQ_MAX;
        c->qcount--;
    }
    c->qhead = 0;
}

static void mark_dead(skd_client *c, const char *why)
{
    if (c->dead)
        return;
    c->dead = 1;
    sk_log("server: 断开客户端 %s (pid %d): %s", c->app[0] ? c->app : "?", (int)c->pid, why);
}

/* 把队列里积压的报文按顺序发出去，直到发完或者对方的接收缓冲区又满了 */
static void q_flush(skd_client *c)
{
    while (c->qcount > 0 && !c->dead) {
        outmsg *m = &c->q[c->qhead];
        ssize_t r = send(c->fd, m->data, m->len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            mark_dead(c, strerror(errno));
            return;
        }
        free(m->data);
        m->data = NULL;
        c->qhead = (c->qhead + 1) % OUTQ_MAX;
        c->qcount--;
        c->stuck_since = skd_now_ms();   /* 有进展 */
    }
}

/*
 * 发送一条事件。正常情况下直接 send 成功；对方处理不过来（接收缓冲区满，EAGAIN）时：
 *   - 鼠标移动事件可以合并/丢弃：只有“最后的位置”有意义；
 *   - 按键、按钮、焦点等事件绝不能悄悄丢掉（否则应用会以为某个键一直按着），
 *     先排队，等 POLLOUT 时再发；排队太多或太久说明客户端卡死了，断开它。
 */
static void send_msg(skd_client *c, const void *msg, uint16_t len, int is_move, uint32_t win)
{
    if (!c || c->dead)
        return;
    if (c->qcount == 0) {
        ssize_t r = send(c->fd, msg, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (r == (ssize_t)len)
            return;
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            mark_dead(c, strerror(errno));     /* EPIPE/ECONNRESET：对方已经退出 */
            return;
        }
    }
    if (is_move) {
        if (c->qcount > 0) {
            outmsg *last = &c->q[(c->qhead + c->qcount - 1) % OUTQ_MAX];
            if (last->is_move && last->win == win && last->len == len) {
                memcpy(last->data, msg, len);   /* 合并：用新位置覆盖还没发出去的旧位置 */
                return;
            }
        }
        if (c->qcount >= OUTQ_MOVE_LIMIT)
            return;                              /* 丢弃：移动事件不重要 */
    }
    if (c->qcount >= OUTQ_MAX) {
        mark_dead(c, "事件积压太多（客户端不读套接字）");
        return;
    }
    uint8_t *copy = malloc(len);
    if (!copy) {
        mark_dead(c, "内存不足");
        return;
    }
    memcpy(copy, msg, len);
    outmsg *m = &c->q[(c->qhead + c->qcount) % OUTQ_MAX];
    m->len = len;
    m->is_move = (uint8_t)is_move;
    m->win = win;
    m->data = copy;
    if (c->qcount == 0)
        c->stuck_since = skd_now_ms();
    c->qcount++;
}

/* 发送 SK_EV_ERROR。fatal=1 时随后断开（协议违规），0 时只是告知（例如资源不足） */
static void send_error(skd_client *c, uint16_t req, uint32_t code, int fatal, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static void send_error(skd_client *c, uint16_t req, uint32_t code, int fatal, const char *fmt, ...)
{
    sk_ev_error e;
    memset(&e, 0, sizeof e);
    e.hdr.type = SK_EV_ERROR;
    e.hdr.size = sizeof e;
    e.code = code;
    e.request_type = req;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.message, sizeof e.message, fmt, ap);
    va_end(ap);
    sk_log("server: 客户端 %s (pid %d) 请求 %u 出错: %s", c->app[0] ? c->app : "?",
           (int)c->pid, req, e.message);
    if (fatal) {
        /* 绕过队列直接发：反正马上要断开了，发不出去也无所谓 */
        send(c->fd, &e, sizeof e, MSG_DONTWAIT | MSG_NOSIGNAL);
        mark_dead(c, "协议错误");
    } else {
        send_msg(c, &e, sizeof e, 0, 0);
    }
}

/* ---------- 事件发送（给 wm 用） ---------- */

void server_send_mouse(skd_client *c, uint32_t win, uint32_t kind, int x, int y,
                       uint32_t buttons, uint32_t button, int wheel, uint32_t clicks,
                       uint32_t mods)
{
    sk_ev_mouse m;
    memset(&m, 0, sizeof m);
    m.hdr.type = SK_EV_MOUSE;
    m.hdr.size = sizeof m;
    m.win = win;
    m.kind = kind;
    m.x = x;
    m.y = y;
    m.buttons = buttons;
    m.button = button;
    m.wheel = wheel;
    m.clicks = clicks;
    m.modifiers = mods;
    send_msg(c, &m, sizeof m, kind == SK_MOUSE_MOVE, win);
}

void server_send_key(skd_client *c, uint32_t win, uint32_t keycode, uint32_t state,
                     uint32_t mods, uint32_t codepoint)
{
    sk_ev_key k;
    memset(&k, 0, sizeof k);
    k.hdr.type = SK_EV_KEY;
    k.hdr.size = sizeof k;
    k.win = win;
    k.keycode = keycode;
    k.state = state;
    k.modifiers = mods;
    k.codepoint = codepoint;
    send_msg(c, &k, sizeof k, 0, win);
}

void server_send_focus(skd_client *c, uint32_t win, int focused)
{
    sk_ev_focus f;
    memset(&f, 0, sizeof f);
    f.hdr.type = SK_EV_FOCUS;
    f.hdr.size = sizeof f;
    f.win = win;
    f.focused = focused ? 1 : 0;
    send_msg(c, &f, sizeof f, 0, win);
}

void server_send_configure(skd_client *c, uint32_t win, int w, int h)
{
    sk_ev_configure e;
    memset(&e, 0, sizeof e);
    e.hdr.type = SK_EV_CONFIGURE;
    e.hdr.size = sizeof e;
    e.win = win;
    e.w = w;
    e.h = h;
    send_msg(c, &e, sizeof e, 0, win);
}

void server_send_close(skd_client *c, uint32_t win)
{
    sk_ev_window e;
    memset(&e, 0, sizeof e);
    e.hdr.type = SK_EV_CLOSE;
    e.hdr.size = sizeof e;
    e.win = win;
    send_msg(c, &e, sizeof e, 0, win);
}

void server_kill_client(skd_client *c)
{
    if (!c)
        return;
    if (c->pid > 1 && c->pid != getpid()) {
        sk_log("server: 强制结束 %s (pid %d)", c->app, (int)c->pid);
        kill(c->pid, SIGKILL);
    }
    mark_dead(c, "强制关闭");
}

/* ---------- 请求处理 ---------- */

/* 固定长度字符数组必须在范围内以 NUL 结尾 */
static int has_nul(const char *s, size_t n) { return memchr(s, 0, n) != NULL; }

/* 标题/名字里的控制字符（换行、制表符……）换成空格，避免把界面和日志搞乱 */
static void sanitize(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7F)
            *s = ' ';
}

/*
 * 导入客户端的共享内存缓冲区。
 *
 * 为什么要求 F_SEAL_SHRINK？
 *   mmap 一个文件后，如果文件被截短 (ftruncate 变小)，再访问超出新长度的那部分映射，
 *   CPU 找不到对应的页，内核会给访问者发 SIGBUS 信号——默认动作是终止进程。
 *   也就是说，一个恶意客户端只要把自己的 memfd 截短，就能让 sketchd 在合成时崩溃！
 *   memfd 支持“封印”(seal)：加上 F_SEAL_SHRINK 之后，这个文件永远不能再变小，
 *   连创建者自己也不行。我们检查封印存在、且当前大小足够，就可以放心地读它了。
 */
static int import_buffer(skd_client *c, uint16_t req, int fd, int w, int h, skd_buffer *out)
{
    if (w < 1 || h < 1 || w > SK_MAX_WIN_SIZE || h > SK_MAX_WIN_SIZE) {
        send_error(c, req, EINVAL, 1, "缓冲区尺寸 %dx%d 超出 1..%d", w, h, SK_MAX_WIN_SIZE);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        send_error(c, req, EBADF, 1, "附带的 fd 不是共享内存文件");
        return -1;
    }
    size_t need = (size_t)w * (size_t)h * 4;
    if ((uint64_t)st.st_size < (uint64_t)need) {
        send_error(c, req, EINVAL, 1, "缓冲区只有 %lld 字节，需要 %zu", (long long)st.st_size, need);
        return -1;
    }
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0) {
        send_error(c, req, EINVAL, 1, "缓冲区不是 memfd（读不到封印）");
        return -1;
    }
    if (!(seals & F_SEAL_SHRINK)) {
        send_error(c, req, EPERM, 1, "缓冲区没有 F_SEAL_SHRINK 封印");
        return -1;
    }
    if (c->mapped_bytes + need > MAX_CLIENT_BYTES) {
        send_error(c, req, ENOMEM, 0, "窗口缓冲区总量超过上限");
        return -1;
    }
    /* 只读映射：我们只读客户端画好的像素，绝不往里写 */
    void *p = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        send_error(c, req, ENOMEM, 0, "映射缓冲区失败: %s", strerror(errno));
        return -1;
    }
    out->pixels = p;
    out->map_len = need;
    out->w = w;
    out->h = h;
    c->mapped_bytes += need;
    /* 映射建立之后 fd 就不需要了：映射本身持有对这块内存的引用 */
    return 0;
}

static skd_window *owned_window(skd_client *c, uint16_t req, uint32_t id)
{
    skd_window *w = wm_find(id);
    if (!w || w->client != c) {
        send_error(c, req, ENOENT, 1, "窗口 %u 不存在或不属于你", id);
        return NULL;
    }
    return w;
}

static void handle_hello(skd_client *c, const sk_req_hello *m)
{
    if (m->version != SK_PROTO_VERSION) {
        send_error(c, SK_REQ_HELLO, EPROTO, 1, "协议版本 %u 不支持（服务器是 %d）",
                   m->version, SK_PROTO_VERSION);
        return;
    }
    if (!has_nul(m->app_name, sizeof m->app_name)) {
        send_error(c, SK_REQ_HELLO, EINVAL, 1, "app_name 没有 NUL 结尾");
        return;
    }
    sk_strlcpy(c->app, m->app_name[0] ? m->app_name : "app", sizeof c->app);
    sanitize(c->app);
    c->hello = 1;
    sk_ev_welcome w;
    memset(&w, 0, sizeof w);
    w.hdr.type = SK_EV_WELCOME;
    w.hdr.size = sizeof w;
    w.version = SK_PROTO_VERSION;
    w.screen_w = skd_screen_w();
    w.screen_h = skd_screen_h();
    send_msg(c, &w, sizeof w, 0, 0);
    SKD_DBG("server: HELLO %s 自报 pid=%u 实际 pid=%d", c->app, m->pid, (int)c->pid);
}

static void handle_create(skd_client *c, const sk_req_create_window *m, int fd)
{
    const uint32_t known = SK_WIN_RESIZABLE | SK_WIN_CENTER | SK_WIN_NO_DECOR | SK_WIN_DIALOG;
    if (m->flags & ~known) {
        send_error(c, SK_REQ_CREATE_WINDOW, EINVAL, 1, "未知的窗口标志 %#x", m->flags);
        return;
    }
    if (!has_nul(m->title, sizeof m->title)) {
        send_error(c, SK_REQ_CREATE_WINDOW, EINVAL, 1, "标题没有 NUL 结尾");
        return;
    }
    if (m->min_w < 0 || m->min_h < 0 || m->min_w > SK_MAX_WIN_SIZE || m->min_h > SK_MAX_WIN_SIZE) {
        send_error(c, SK_REQ_CREATE_WINDOW, EINVAL, 1, "最小尺寸 %dx%d 非法", m->min_w, m->min_h);
        return;
    }
    if (c->nwin >= SKD_MAX_WIN_PER_CLIENT || wm_window_count() >= SKD_MAX_WINDOWS) {
        send_error(c, SK_REQ_CREATE_WINDOW, EMFILE, 0, "窗口太多");
        return;
    }
    skd_buffer buf;
    memset(&buf, 0, sizeof buf);
    if (import_buffer(c, SK_REQ_CREATE_WINDOW, fd, m->w, m->h, &buf) < 0)
        return;
    sk_req_create_window req = *m;
    sanitize(req.title);
    skd_window *w = wm_create_window(c, c->app, &req, &buf);
    if (!w) {
        c->mapped_bytes -= buf.map_len;
        buffer_release(&buf);
        send_error(c, SK_REQ_CREATE_WINDOW, ENOMEM, 0, "创建窗口失败");
        return;
    }
    c->nwin++;
    sk_ev_window_created e;
    memset(&e, 0, sizeof e);
    e.hdr.type = SK_EV_WINDOW_CREATED;
    e.hdr.size = sizeof e;
    e.serial = m->serial;
    e.win = w->id;
    send_msg(c, &e, sizeof e, 0, w->id);
    /* 新窗口获得焦点（先回复 WINDOW_CREATED，客户端才认识这个窗口 id，再发 FOCUS） */
    wm_window_announced(w);
}

/* 窗口销毁时由 wm 回调：归还这个客户端的配额 */
void server_window_gone(skd_client *c, size_t bytes)
{
    if (!c)
        return;
    if (c->nwin > 0)
        c->nwin--;
    c->mapped_bytes = c->mapped_bytes > bytes ? c->mapped_bytes - bytes : 0;
}

void server_buffer_replaced(skd_client *c, size_t old_bytes)
{
    if (c)
        c->mapped_bytes = c->mapped_bytes > old_bytes ? c->mapped_bytes - old_bytes : 0;
}

static void handle_message(skd_client *c, const uint8_t *data, size_t n, int *fds, int nfds)
{
    const sk_msg_header *h = (const sk_msg_header *)data;
    if (n < sizeof *h) {
        send_error(c, 0, EINVAL, 1, "报文太短（%zu 字节）", n);
        return;
    }
    uint16_t type = h->type;
    if (h->size != n) {
        send_error(c, type, EINVAL, 1, "报文头 size=%u 与实际长度 %zu 不符", h->size, n);
        return;
    }
    /* 每种请求的长度是固定的（剪贴板除外），差一个字节都不行 */
    size_t expect = 0;
    int want_fds = 0;
    switch (type) {
    case SK_REQ_HELLO:          expect = sizeof(sk_req_hello); break;
    case SK_REQ_CREATE_WINDOW:  expect = sizeof(sk_req_create_window); want_fds = 1; break;
    case SK_REQ_DESTROY_WINDOW: expect = sizeof(sk_req_window); break;
    case SK_REQ_ATTACH_BUFFER:  expect = sizeof(sk_req_attach_buffer); want_fds = 1; break;
    case SK_REQ_DAMAGE:         expect = sizeof(sk_req_damage); break;
    case SK_REQ_SET_TITLE:      expect = sizeof(sk_req_set_title); break;
    case SK_REQ_SHOW:           expect = sizeof(sk_req_window); break;
    case SK_REQ_MOVE:           expect = sizeof(sk_req_move); break;
    case SK_REQ_SET_CURSOR:     expect = sizeof(sk_req_set_cursor); break;
    case SK_REQ_SET_CLIPBOARD:  expect = 0; break;    /* 变长，下面单独检查 */
    case SK_REQ_GET_CLIPBOARD:  expect = sizeof(sk_msg_header); break;
    case SK_REQ_LAUNCH:         expect = sizeof(sk_req_launch); break;
    default:
        send_error(c, type, ENOSYS, 1, "未知的请求类型 %u", type);
        return;
    }
    if (expect && n != expect) {
        send_error(c, type, EINVAL, 1, "请求 %u 长度应为 %zu，实际 %zu", type, expect, n);
        return;
    }
    if (nfds != want_fds) {
        send_error(c, type, EINVAL, 1, "请求 %u 应附带 %d 个 fd，实际 %d 个", type, want_fds, nfds);
        return;
    }
    if (!c->hello && type != SK_REQ_HELLO) {
        send_error(c, type, EPROTO, 1, "第一条请求必须是 HELLO");
        return;
    }
    if (c->hello && type == SK_REQ_HELLO) {
        send_error(c, type, EPROTO, 1, "重复的 HELLO");
        return;
    }

    switch (type) {
    case SK_REQ_HELLO:
        handle_hello(c, (const sk_req_hello *)data);
        break;
    case SK_REQ_CREATE_WINDOW:
        handle_create(c, (const sk_req_create_window *)data, fds[0]);
        break;
    case SK_REQ_DESTROY_WINDOW: {
        skd_window *w = owned_window(c, type, ((const sk_req_window *)data)->win);
        if (w)
            wm_destroy_window(w);
        break;
    }
    case SK_REQ_ATTACH_BUFFER: {
        const sk_req_attach_buffer *m = (const sk_req_attach_buffer *)data;
        skd_window *w = owned_window(c, type, m->win);
        skd_buffer buf;
        memset(&buf, 0, sizeof buf);
        if (w && import_buffer(c, type, fds[0], m->w, m->h, &buf) == 0)
            wm_attach_buffer(w, &buf);
        break;
    }
    case SK_REQ_DAMAGE: {
        const sk_req_damage *m = (const sk_req_damage *)data;
        skd_window *w = owned_window(c, type, m->win);
        if (w)
            wm_damage_content(w, sk_rect_make(m->x, m->y, m->w, m->h));
        break;
    }
    case SK_REQ_SET_TITLE: {
        const sk_req_set_title *m = (const sk_req_set_title *)data;
        if (!has_nul(m->title, sizeof m->title)) {
            send_error(c, type, EINVAL, 1, "标题没有 NUL 结尾");
            break;
        }
        skd_window *w = owned_window(c, type, m->win);
        if (w) {
            char t[SK_TITLE_MAX];
            sk_strlcpy(t, m->title, sizeof t);
            sanitize(t);
            wm_set_title(w, t);
        }
        break;
    }
    case SK_REQ_SHOW: {
        skd_window *w = owned_window(c, type, ((const sk_req_window *)data)->win);
        if (w)
            wm_show_window(w);
        break;
    }
    case SK_REQ_MOVE: {
        const sk_req_move *m = (const sk_req_move *)data;
        skd_window *w = owned_window(c, type, m->win);
        if (w)
            wm_move_window(w, m->x, m->y);
        break;
    }
    case SK_REQ_SET_CURSOR: {
        const sk_req_set_cursor *m = (const sk_req_set_cursor *)data;
        if (m->cursor >= SK_CURSOR_COUNT) {
            send_error(c, type, EINVAL, 1, "光标编号 %u 非法", m->cursor);
            break;
        }
        skd_window *w = owned_window(c, type, m->win);
        if (w)
            wm_set_cursor(w, (int)m->cursor);
        break;
    }
    case SK_REQ_SET_CLIPBOARD: {
        const sk_req_clipboard *m = (const sk_req_clipboard *)data;
        size_t base = offsetof(sk_req_clipboard, text);
        if (n < base || m->len > SK_CLIP_MAX || n != base + m->len) {
            send_error(c, type, EINVAL, 1, "剪贴板报文长度不对");
            break;
        }
        memcpy(g_clip, m->text, m->len);
        g_clip_len = m->len;
        SKD_DBG("server: %s 设置剪贴板 %u 字节", c->app, m->len);
        break;
    }
    case SK_REQ_GET_CLIPBOARD: {
        static sk_ev_clipboard e;    /* 4KB，不放在栈上 */
        e.hdr.type = SK_EV_CLIPBOARD;
        e.hdr.size = (uint16_t)(offsetof(sk_ev_clipboard, text) + g_clip_len);
        e.len = g_clip_len;
        memcpy(e.text, g_clip, g_clip_len);
        send_msg(c, &e, e.hdr.size, 0, 0);
        break;
    }
    case SK_REQ_LAUNCH: {
        const sk_req_launch *m = (const sk_req_launch *)data;
        if (!has_nul(m->app_id, sizeof m->app_id) || !has_nul(m->arg, sizeof m->arg)) {
            send_error(c, type, EINVAL, 1, "app_id/arg 没有 NUL 结尾");
            break;
        }
        launch_app(m->app_id, m->arg);
        break;
    }
    }
}

/* ---------- 连接管理 ---------- */

static void client_free(int idx)
{
    skd_client *c = g_clients[idx];
    wm_destroy_client_windows(c);
    q_clear(c);
    close(c->fd);
    free(c);
    g_clients[idx] = NULL;
}

static void reap_dead(void)
{
    uint64_t now = skd_now_ms();
    for (int i = 0; i < SKD_MAX_CLIENTS; i++) {
        skd_client *c = g_clients[i];
        if (!c)
            continue;
        if (!c->dead && c->qcount > 0 && now - c->stuck_since > STUCK_MS)
            mark_dead(c, "超过 10 秒不读事件");
        if (c->dead)
            client_free(i);
    }
}

static void accept_clients(void)
{
    for (;;) {
        int fd = accept4(g_listen, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                sk_log("server: accept 失败: %s", strerror(errno));
            return;
        }
        int slot = -1;
        for (int i = 0; i < SKD_MAX_CLIENTS; i++)
            if (!g_clients[i]) {
                slot = i;
                break;
            }
        skd_client *c = slot >= 0 ? calloc(1, sizeof *c) : NULL;
        if (!c) {
            sk_ev_error e;
            memset(&e, 0, sizeof e);
            e.hdr.type = SK_EV_ERROR;
            e.hdr.size = sizeof e;
            e.code = EMFILE;
            sk_strlcpy(e.message, "连接的应用太多了", sizeof e.message);
            send(fd, &e, sizeof e, MSG_DONTWAIT | MSG_NOSIGNAL);
            close(fd);
            sk_log("server: 客户端已满（%d 个），拒绝新连接", SKD_MAX_CLIENTS);
            continue;
        }
        c->fd = fd;
        c->pid = -1;
        /* 问内核“连过来的是哪个进程”：这个信息由内核在 connect 时记录，客户端无法伪造 */
        struct ucred cr;
        socklen_t len = sizeof cr;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0)
            c->pid = cr.pid;
        g_clients[slot] = c;
        SKD_DBG("server: 新客户端 pid=%d", (int)c->pid);
    }
}

static void client_read(skd_client *c)
{
    for (int round = 0; round < MAX_MSGS_PER_ROUND && !c->dead; round++) {
        union {
            struct cmsghdr align;
            char buf[CMSG_SPACE(sizeof(int) * MAX_FDS_PER_MSG)];
        } ctl;
        struct iovec iov = { g_rx, SK_MSG_MAX };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof ctl.buf;
        /* MSG_CMSG_CLOEXEC：收到的 fd 自动带 CLOEXEC，fork 出的应用不会继承它们 */
        ssize_t n = recvmsg(c->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                mark_dead(c, strerror(errno));
            return;
        }
        /* 先把收到的 fd 全部取出来：无论报文是否合法，都必须保证每个 fd 最终被关闭 */
        int fds[MAX_FDS_PER_MSG];
        int nfds = 0;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
                continue;
            int cnt = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int j = 0; j < cnt; j++) {
                int fd;
                memcpy(&fd, CMSG_DATA(cm) + (size_t)j * sizeof(int), sizeof fd);
                if (nfds < MAX_FDS_PER_MSG)
                    fds[nfds++] = fd;
                else
                    close(fd);
            }
        }
        if (n == 0) {
            /* SEQPACKET 读到 0 字节 = 对方关闭了连接（正常退出或崩溃） */
            for (int j = 0; j < nfds; j++)
                close(fds[j]);
            mark_dead(c, "连接已关闭");
            return;
        }
        uint16_t type = 0;
        if (n >= (ssize_t)sizeof type)
            memcpy(&type, g_rx, sizeof type);
        if (mh.msg_flags & MSG_TRUNC)
            send_error(c, type, EMSGSIZE, 1, "报文超过 %d 字节", SK_MSG_MAX);
        else if (mh.msg_flags & MSG_CTRUNC)
            send_error(c, type, EINVAL, 1, "附带的 fd 太多");
        else
            handle_message(c, (const uint8_t *)g_rx, (size_t)n, fds, nfds);
        /* 映射建立后 fd 就没用了；出错时也要关掉 */
        for (int j = 0; j < nfds; j++)
            close(fds[j]);
    }
}

int server_init(const char *path)
{
    sk_strlcpy(g_path, path, sizeof g_path);
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        sk_log("server: 套接字路径太长: %s", path);
        return -1;
    }
    /* 确保目录存在（/run 是 tmpfs，每次开机都是空的） */
    char dir[108];
    sk_strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        sk_mkdir_p(dir, 0755);
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        sk_log("server: socket 失败: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    sk_strlcpy(addr.sun_path, path, sizeof addr.sun_path);
    /* 上一次 sketchd 崩溃时留下的套接字文件还在，bind 会失败（EADDRINUSE），先删掉 */
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        sk_log("server: bind %s 失败: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    /* 所有用户都能连接（系统里目前只有 root，但这是显示服务器的惯例） */
    chmod(path, 0666);
    if (listen(fd, 16) < 0) {
        sk_log("server: listen 失败: %s", strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    g_listen = fd;
    sk_log("server: 在 %s 上监听", path);
    return 0;
}

void server_shutdown(void)
{
    for (int i = 0; i < SKD_MAX_CLIENTS; i++)
        if (g_clients[i])
            client_free(i);
    if (g_listen >= 0) {
        close(g_listen);
        g_listen = -1;
        unlink(g_path);
    }
}

int server_pollfds(struct pollfd *pfd, int max)
{
    reap_dead();
    int n = 0;
    if (g_listen >= 0 && n < max) {
        pfd[n].fd = g_listen;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
    }
    for (int i = 0; i < SKD_MAX_CLIENTS && n < max; i++) {
        skd_client *c = g_clients[i];
        if (!c)
            continue;
        pfd[n].fd = c->fd;
        pfd[n].events = (short)(POLLIN | (c->qcount ? POLLOUT : 0));
        pfd[n].revents = 0;
        n++;
    }
    return n;
}

void server_dispatch(const struct pollfd *pfd, int n)
{
    for (int k = 0; k < n; k++) {
        short re = pfd[k].revents;
        if (!re)
            continue;
        if (pfd[k].fd == g_listen) {
            accept_clients();
            continue;
        }
        skd_client *c = NULL;
        for (int i = 0; i < SKD_MAX_CLIENTS; i++)
            if (g_clients[i] && g_clients[i]->fd == pfd[k].fd) {
                c = g_clients[i];
                break;
            }
        if (!c || c->dead)
            continue;
        if (re & POLLOUT)
            q_flush(c);
        if (re & POLLIN)
            client_read(c);
        else if (re & (POLLHUP | POLLERR | POLLNVAL))
            mark_dead(c, "连接挂断");
    }
    reap_dead();
}
