/*
 * skdtest — sketchd 的“原始协议”测试客户端（不用 libsketch 的 client.c，直接收发报文）
 *
 * 用法：skdtest [--name 名字] [--windows N] [--size 宽x高] [--resizable] [--center]
 *               [--dialog] [--dialog2] [--nodecor] [--title 标题] [--ignore-close] [--cursor N]
 *   （--dialog2：第 2 个及以后的窗口是第 1 个窗口的对话框）
 *               [--clip 文本] [--launch 应用id] [--move X,Y] [--log-moves] [--min 宽x高]
 *
 * 它会：
 *   - 打开 N 个窗口，每个窗口画上醒目的颜色、编号和“最近收到的事件”，方便看截图验证；
 *   - 把收到的每个事件都打印到 /dev/console（QEMU 串口），格式稳定，测试脚本用正则匹配：
 *       skdtest[名字]: created serial=1 win=3
 *       skdtest[名字]: focus win=3 on=1
 *       skdtest[名字]: mouse win=3 kind=down x=10 y=20 buttons=1 button=1 clicks=1 mods=0
 *       skdtest[名字]: key win=3 code=30 state=1 cp=97 mods=0
 *       skdtest[名字]: configure win=3 640x480
 *       skdtest[名字]: close win=3
 *   - 收到 CONFIGURE 时按新尺寸重建共享内存缓冲区并 ATTACH（验证缩放/最大化流程）。
 */
#define _GNU_SOURCE
#include <sketch/font.h>
#include <sketch/gfx.h>
#include <sketch/proto.h>
#include <sketch/rough.h>
#include <sketch/theme.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAXW 8

typedef struct {
    uint32_t id, serial;
    int      w, h;
    uint32_t *px;
    size_t   len;
    char     title[SK_TITLE_MAX];
    char     last[128];        /* 最近一个事件（画在窗口里） */
    char     typed[64];        /* 收到的可打印字符 */
    int      focused;
    sk_color tint;
    int      alive;
} twin;

static int   g_sock = -1;
static FILE *g_log;
static char  g_name[SK_APPNAME_MAX] = "skdtest";
static twin  g_win[MAXW];
static int   g_nwin;
static int   g_ignore_close, g_log_moves;
static unsigned long g_moves;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(g_log, "skdtest[%s]: %s\n", g_name, buf);
    fflush(g_log);
}

static int send_fd(const void *msg, size_t len, int fd)
{
    struct iovec iov = { (void *)msg, len };
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        memset(cbuf, 0, sizeof cbuf);
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof cbuf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof fd);
    }
    return (int)sendmsg(g_sock, &mh, MSG_NOSIGNAL);
}

static int new_buffer(int w, int h, uint32_t **px, size_t *len)
{
    int fd = memfd_create("skdtest", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    *len = (size_t)w * h * 4;
    if (ftruncate(fd, (off_t)*len) < 0 || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
        close(fd);
        return -1;
    }
    *px = mmap(NULL, *len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (*px == MAP_FAILED) {
        close(fd);
        return -1;
    }
    return fd;
}

static void damage(twin *t, int x, int y, int w, int h)
{
    sk_req_damage d = { { SK_REQ_DAMAGE, sizeof d }, t->id, x, y, w, h };
    send(g_sock, &d, sizeof d, MSG_NOSIGNAL);
}

/* 窗口内容：浅色底 + 方格 + 大编号 + 尺寸 + 最近事件 + 输入的文字 */
static void redraw(twin *t)
{
    sk_canvas c;
    sk_canvas_init(&c, t->px, t->w, t->h, t->w);
    sk_clear(&c, sk_mix(SK_PAPER, t->tint, 60));
    for (int x = 0; x < t->w; x += 20)
        sk_vline(&c, x, 0, t->h - 1, sk_mix(SK_PAPER, t->tint, 110));
    for (int y = 0; y < t->h; y += 20)
        sk_hline(&c, 0, t->w - 1, y, sk_mix(SK_PAPER, t->tint, 110));
    sk_rough_style s;
    sk_rough_default(&s);
    s.stroke = t->tint;
    s.stroke_width = 3;
    s.seed = t->id;
    sk_rough_rect(&c, 4, 4, (float)t->w - 8, (float)t->h - 8, &s);
    sk_font *big = sk_font_get(SK_FONT_LARGE), *ui = sk_font_get(SK_FONT_UI);
    char line[160];
    snprintf(line, sizeof line, "%s #%u", g_name, t->id);
    sk_draw_text(&c, big, 16, 44, line, SK_INK);
    snprintf(line, sizeof line, "%dx%d %s", t->w, t->h, t->focused ? "（有焦点）" : "");
    sk_draw_text(&c, ui, 16, 70, line, SK_INK_SOFT);
    sk_draw_text(&c, ui, 16, 94, t->last, SK_INK);
    snprintf(line, sizeof line, "输入: %s", t->typed);
    sk_draw_text(&c, ui, 16, 118, line, SK_BLUE);
    damage(t, 0, 0, t->w, t->h);
}

static twin *find(uint32_t id)
{
    for (int i = 0; i < g_nwin; i++)
        if (g_win[i].alive && g_win[i].id == id)
            return &g_win[i];
    return NULL;
}

static int alive_count(void)
{
    int n = 0;
    for (int i = 0; i < g_nwin; i++)
        n += g_win[i].alive;
    return n;
}

static const char *kind_name(uint32_t k)
{
    static const char *const names[] = { "?", "move", "down", "up", "wheel", "enter", "leave" };
    return k < 7 ? names[k] : "?";
}

static void handle(const uint8_t *buf, ssize_t n)
{
    const sk_msg_header *h = (const sk_msg_header *)buf;
    if (n < (ssize_t)sizeof *h)
        return;
    switch (h->type) {
    case SK_EV_WINDOW_CREATED: {
        const sk_ev_window_created *e = (const void *)buf;
        for (int i = 0; i < g_nwin; i++)
            if (g_win[i].serial == e->serial && !g_win[i].id) {
                g_win[i].id = e->win;
                g_win[i].alive = 1;
                logf_("created serial=%u win=%u", e->serial, e->win);
                snprintf(g_win[i].last, sizeof g_win[i].last, "已创建，窗口 id = %u", e->win);
                redraw(&g_win[i]);
            }
        break;
    }
    case SK_EV_FOCUS: {
        const sk_ev_focus *e = (const void *)buf;
        logf_("focus win=%u on=%u", e->win, e->focused);
        twin *t = find(e->win);
        if (t) {
            t->focused = (int)e->focused;
            redraw(t);
        }
        break;
    }
    case SK_EV_MOUSE: {
        const sk_ev_mouse *e = (const void *)buf;
        twin *t = find(e->win);
        if (e->kind == SK_MOUSE_MOVE) {
            g_moves++;
            if (!g_log_moves)
                break;
        }
        logf_("mouse win=%u kind=%s x=%d y=%d buttons=%u button=%u clicks=%u wheel=%d mods=%u",
              e->win, kind_name(e->kind), e->x, e->y, e->buttons, e->button, e->clicks, e->wheel,
              e->modifiers);
        if (t && e->kind != SK_MOUSE_MOVE) {
            snprintf(t->last, sizeof t->last, "鼠标 %s (%d,%d) 按钮=%u 点击=%u 滚轮=%d",
                     kind_name(e->kind), e->x, e->y, e->button, e->clicks, e->wheel);
            redraw(t);
        }
        break;
    }
    case SK_EV_KEY: {
        const sk_ev_key *e = (const void *)buf;
        logf_("key win=%u code=%u state=%u cp=%u mods=%u", e->win, e->keycode, e->state, e->codepoint,
              e->modifiers);
        twin *t = find(e->win);
        if (t && e->state != 0) {
            if (e->codepoint >= 0x20 && e->codepoint < 0x7F && !(e->modifiers & SK_MOD_CTRL)) {
                size_t l = strlen(t->typed);
                if (l + 1 < sizeof t->typed) {
                    t->typed[l] = (char)e->codepoint;
                    t->typed[l + 1] = 0;
                }
            } else if (e->codepoint == 0x08 && t->typed[0]) {
                t->typed[strlen(t->typed) - 1] = 0;
            }
            snprintf(t->last, sizeof t->last, "按键 code=%u cp=%u mods=%u", e->keycode, e->codepoint, e->modifiers);
            redraw(t);
        }
        break;
    }
    case SK_EV_CONFIGURE: {
        const sk_ev_configure *e = (const void *)buf;
        logf_("configure win=%u %dx%d", e->win, e->w, e->h);
        twin *t = find(e->win);
        if (!t || e->w < 1 || e->h < 1)
            break;
        uint32_t *px;
        size_t len;
        int fd = new_buffer(e->w, e->h, &px, &len);
        if (fd < 0) {
            logf_("new_buffer failed: %s", strerror(errno));
            break;
        }
        munmap(t->px, t->len);
        t->px = px;
        t->len = len;
        t->w = e->w;
        t->h = e->h;
        snprintf(t->last, sizeof t->last, "收到 CONFIGURE %dx%d", e->w, e->h);
        sk_canvas c;
        sk_canvas_init(&c, t->px, t->w, t->h, t->w);
        sk_clear(&c, SK_PAPER);
        sk_req_attach_buffer a = { { SK_REQ_ATTACH_BUFFER, sizeof a }, t->id, t->w, t->h };
        /* 先画好再 ATTACH：服务器换上新缓冲区时内容已经完整 */
        redraw(t);
        send_fd(&a, sizeof a, fd);
        close(fd);
        damage(t, 0, 0, t->w, t->h);
        break;
    }
    case SK_EV_CLOSE: {
        const sk_ev_window *e = (const void *)buf;
        logf_("close win=%u%s", e->win, g_ignore_close ? " (ignored)" : "");
        if (g_ignore_close)
            break;
        twin *t = find(e->win);
        if (t) {
            sk_req_window d = { { SK_REQ_DESTROY_WINDOW, sizeof d }, t->id };
            send(g_sock, &d, sizeof d, MSG_NOSIGNAL);
            t->alive = 0;
            logf_("destroyed win=%u", t->id);
        }
        if (alive_count() == 0) {
            logf_("all windows closed, exiting (moves=%lu)", g_moves);
            exit(0);
        }
        break;
    }
    case SK_EV_CLIPBOARD: {
        const sk_ev_clipboard *e = (const void *)buf;
        int show = e->len < 200 ? (int)e->len : 200;
        logf_("clipboard len=%u text=%.*s", e->len, show, e->text);
        break;
    }
    case SK_EV_ERROR: {
        const sk_ev_error *e = (const void *)buf;
        logf_("error code=%u req=%u msg=%s", e->code, e->request_type, e->message);
        break;
    }
    default:
        logf_("event type=%u size=%zd", h->type, n);
    }
}

int main(int argc, char **argv)
{
    int nwin = 1, w = 420, h = 260, flags = 0, cursor = -1, move_x = -99999, move_y = 0;
    int min_w = 0, min_h = 0, dialog2 = 0;
    const char *title = NULL, *clip = NULL, *launch = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--name")) { snprintf(g_name, sizeof g_name, "%s", v); i++; }
        else if (!strcmp(a, "--windows")) { nwin = atoi(v); i++; }
        else if (!strcmp(a, "--size")) { sscanf(v, "%dx%d", &w, &h); i++; }
        else if (!strcmp(a, "--min")) { sscanf(v, "%dx%d", &min_w, &min_h); i++; }
        else if (!strcmp(a, "--title")) { title = v; i++; }
        else if (!strcmp(a, "--cursor")) { cursor = atoi(v); i++; }
        else if (!strcmp(a, "--clip")) { clip = v; i++; }
        else if (!strcmp(a, "--launch")) { launch = v; i++; }
        else if (!strcmp(a, "--move")) { sscanf(v, "%d,%d", &move_x, &move_y); i++; }
        else if (!strcmp(a, "--resizable")) flags |= SK_WIN_RESIZABLE;
        else if (!strcmp(a, "--center")) flags |= SK_WIN_CENTER;
        else if (!strcmp(a, "--dialog")) flags |= SK_WIN_DIALOG;
        else if (!strcmp(a, "--dialog2")) dialog2 = 1;
        else if (!strcmp(a, "--nodecor")) flags |= SK_WIN_NO_DECOR;
        else if (!strcmp(a, "--ignore-close")) g_ignore_close = 1;
        else if (!strcmp(a, "--log-moves")) g_log_moves = 1;
        else { fprintf(stderr, "skdtest: 未知参数 %s\n", a); return 2; }
    }
    if (nwin < 1) nwin = 1;
    if (nwin > MAXW) nwin = MAXW;
    /* 会话首进程打开终端设备时加 O_NOCTTY，避免它成为“控制终端”（/dev/console 内核本来就不会，
     * 但这是好习惯：打开别的 tty 时不加的话，进程退出时内核会挂断那个终端） */
    int cfd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    g_log = cfd >= 0 ? fdopen(cfd, "w") : NULL;
    if (!g_log)
        g_log = stdout;

    const char *path = getenv("SKETCH_SOCKET");
    if (!path || !*path)
        path = SK_SOCKET_PATH;
    g_sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (g_sock < 0 || connect(g_sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        logf_("connect %s failed: %s", path, strerror(errno));
        return 1;
    }
    sk_req_hello hello;
    memset(&hello, 0, sizeof hello);
    hello.hdr.type = SK_REQ_HELLO;
    hello.hdr.size = sizeof hello;
    hello.version = SK_PROTO_VERSION;
    hello.pid = (uint32_t)getpid();
    snprintf(hello.app_name, sizeof hello.app_name, "%s", g_name);
    send(g_sock, &hello, sizeof hello, MSG_NOSIGNAL);
    uint8_t buf[SK_MSG_MAX];
    ssize_t n = recv(g_sock, buf, sizeof buf, 0);
    const sk_ev_welcome *wel = (const void *)buf;
    if (n != (ssize_t)sizeof *wel || wel->hdr.type != SK_EV_WELCOME) {
        logf_("no WELCOME (n=%zd)", n);
        return 1;
    }
    logf_("welcome screen=%dx%d version=%u", wel->screen_w, wel->screen_h, wel->version);

    static const sk_color tints[] = { SK_BLUE, SK_RED, SK_GREEN, SK_ORANGE, SK_PURPLE, SK_TEAL, SK_PINK, SK_YELLOW };
    for (int i = 0; i < nwin; i++) {
        twin *t = &g_win[g_nwin++];
        memset(t, 0, sizeof *t);
        t->serial = 100 + (uint32_t)i;
        t->w = w - 30 * i;
        t->h = h - 20 * i;
        t->tint = tints[(i + (g_name[0] & 7)) % 8];
        if (title)
            snprintf(t->title, sizeof t->title, "%s", title);
        else
            snprintf(t->title, sizeof t->title, "测试窗口 %d（%s）", i + 1, g_name);
        int fd = new_buffer(t->w, t->h, &t->px, &t->len);
        if (fd < 0) {
            logf_("memfd failed: %s", strerror(errno));
            return 1;
        }
        sk_canvas c;
        sk_canvas_init(&c, t->px, t->w, t->h, t->w);
        sk_clear(&c, SK_PAPER);
        sk_req_create_window cw;
        memset(&cw, 0, sizeof cw);
        cw.hdr.type = SK_REQ_CREATE_WINDOW;
        cw.hdr.size = sizeof cw;
        cw.serial = t->serial;
        cw.w = t->w;
        cw.h = t->h;
        cw.flags = (uint32_t)flags | (dialog2 && i > 0 ? SK_WIN_DIALOG : 0);
        cw.accent = t->tint;
        cw.min_w = min_w;
        cw.min_h = min_h;
        snprintf(cw.title, sizeof cw.title, "%s", t->title);
        send_fd(&cw, sizeof cw, fd);
        close(fd);
    }

    int extras_done = 0;
    for (;;) {
        struct pollfd p = { g_sock, POLLIN, 0 };
        if (poll(&p, 1, -1) < 0 && errno != EINTR)
            break;
        n = recv(g_sock, buf, sizeof buf, MSG_DONTWAIT);
        if (n == 0) {
            logf_("disconnected (moves=%lu)", g_moves);
            return 1;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            logf_("recv failed: %s", strerror(errno));
            return 1;
        }
        handle(buf, n);
        /* 所有窗口都创建好以后，执行一次性的附加动作 */
        if (!extras_done && alive_count() == nwin) {
            extras_done = 1;
            twin *t = &g_win[0];
            if (cursor >= 0) {
                sk_req_set_cursor sc = { { SK_REQ_SET_CURSOR, sizeof sc }, t->id, (uint32_t)cursor };
                send(g_sock, &sc, sizeof sc, MSG_NOSIGNAL);
                logf_("set cursor %d", cursor);
            }
            if (move_x != -99999) {
                sk_req_move mv = { { SK_REQ_MOVE, sizeof mv }, t->id, move_x, move_y };
                send(g_sock, &mv, sizeof mv, MSG_NOSIGNAL);
                logf_("move to %d,%d", move_x, move_y);
            }
            if (clip) {
                sk_req_clipboard cb;
                memset(&cb, 0, sizeof cb);
                size_t l = strlen(clip);
                cb.hdr.type = SK_REQ_SET_CLIPBOARD;
                cb.hdr.size = (uint16_t)(offsetof(sk_req_clipboard, text) + l);
                cb.len = (uint32_t)l;
                memcpy(cb.text, clip, l);
                send(g_sock, &cb, cb.hdr.size, MSG_NOSIGNAL);
                sk_msg_header gc = { SK_REQ_GET_CLIPBOARD, sizeof gc };
                send(g_sock, &gc, sizeof gc, MSG_NOSIGNAL);
            }
            if (launch) {
                sk_req_launch l;
                memset(&l, 0, sizeof l);
                l.hdr.type = SK_REQ_LAUNCH;
                l.hdr.size = sizeof l;
                snprintf(l.app_id, sizeof l.app_id, "%s", launch);
                send(g_sock, &l, sizeof l, MSG_NOSIGNAL);
                logf_("launch request %s", launch);
            }
            {
                sk_req_set_title st;
                memset(&st, 0, sizeof st);
                st.hdr.type = SK_REQ_SET_TITLE;
                st.hdr.size = sizeof st;
                st.win = t->id;
                snprintf(st.title, sizeof st.title, "%s", t->title);
                send(g_sock, &st, sizeof st, MSG_NOSIGNAL);
            }
        }
    }
    return 0;
}
