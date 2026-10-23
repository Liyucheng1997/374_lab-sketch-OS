/*
 * skdbad — 故意不守规矩的客户端：验证 sketchd 能扛住任何客户端行为
 *
 * 用法：skdbad <模式>
 *   garbage   HELLO 之后发一堆随机字节            → 期望收到 ERROR 并被断开
 *   nohello   不发 HELLO 直接创建窗口              → ERROR + 断开
 *   badver    HELLO 版本号不对                     → ERROR + 断开
 *   unsealed  缓冲区没有 F_SEAL_SHRINK 封印          → ERROR + 断开
 *   small     缓冲区比 w*h*4 小                     → ERROR + 断开
 *   notmemfd  用 /dev/null 当缓冲区                 → ERROR + 断开
 *   fdspam    DAMAGE 报文附带 3 个 fd               → ERROR + 断开（fd 不会泄漏）
 *   nofd      CREATE_WINDOW 不带 fd                 → ERROR + 断开
 *   badsize   DAMAGE 长度少 1 字节                   → ERROR + 断开
 *   toobig    发 9000 字节的报文                     → ERROR + 断开
 *   wrongwin  销毁一个不属于自己的窗口               → ERROR + 断开
 *   oversize  创建 5000x10 的窗口                    → ERROR + 断开
 *   manywin   连续创建 20 个窗口                     → 第 17 个起收到非致命 ERROR
 *   shrink    正常创建窗口后尝试 ftruncate 缩小缓冲区 → 内核拒绝 (EPERM)，sketchd 不受影响
 *   flood     疯狂发送 20000 条 DAMAGE               → sketchd 照常响应
 *   hang      创建窗口后再也不读套接字（模拟卡死）   → 强制关闭 / 事件积压后被断开
 *   dragdie   创建窗口，收到第一次鼠标按下就立刻退出 → 拖动中途客户端消失
 *   crash     创建窗口后 2 秒段错误崩溃
 * 结果打印到 /dev/console：skdbad[模式]: ...
 */
#define _GNU_SOURCE
#include <sketch/proto.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int   g_sock = -1;
static FILE *g_log;
static const char *g_mode = "?";

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(g_log, "skdbad[%s]: %s\n", g_mode, buf);
    fflush(g_log);
}

static int sendmsg_fds(const void *msg, size_t len, const int *fds, int nfds)
{
    struct iovec iov = { (void *)msg, len };
    char cbuf[CMSG_SPACE(sizeof(int) * 8)];
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (nfds > 0) {
        memset(cbuf, 0, sizeof cbuf);
        mh.msg_control = cbuf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(cm), fds, sizeof(int) * (size_t)nfds);
    }
    return (int)sendmsg(g_sock, &mh, MSG_NOSIGNAL);
}

static void hello(uint32_t version)
{
    sk_req_hello h;
    memset(&h, 0, sizeof h);
    h.hdr.type = SK_REQ_HELLO;
    h.hdr.size = sizeof h;
    h.version = version;
    h.pid = 1;     /* 故意撒谎：服务器应该用 SO_PEERCRED 而不是相信它 */
    snprintf(h.app_name, sizeof h.app_name, "skdbad-%s", g_mode);
    send(g_sock, &h, sizeof h, MSG_NOSIGNAL);
}

static int memfd(size_t size, int seal)
{
    int fd = memfd_create("skdbad", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)size) < 0)
        return -1;
    if (seal)
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
    return fd;
}

static void create_window(uint32_t serial, int w, int h, int fd, const char *title)
{
    sk_req_create_window cw;
    memset(&cw, 0, sizeof cw);
    cw.hdr.type = SK_REQ_CREATE_WINDOW;
    cw.hdr.size = sizeof cw;
    cw.serial = serial;
    cw.w = w;
    cw.h = h;
    cw.flags = SK_WIN_RESIZABLE;
    cw.accent = 0xFFE0605A;
    snprintf(cw.title, sizeof cw.title, "%s", title);
    sendmsg_fds(&cw, sizeof cw, fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0);
}

/* 读事件直到断开或超时；返回 1 = 被服务器断开 */
static int drain(int timeout_ms, uint32_t *win_out)
{
    uint8_t buf[SK_MSG_MAX];
    for (;;) {
        struct pollfd p = { g_sock, POLLIN, 0 };
        int r = poll(&p, 1, timeout_ms);
        if (r == 0)
            return 0;
        ssize_t n = recv(g_sock, buf, sizeof buf, 0);
        if (n <= 0) {
            logf_("disconnected by server");
            return 1;
        }
        const sk_msg_header *h = (const void *)buf;
        if (h->type == SK_EV_ERROR) {
            const sk_ev_error *e = (const void *)buf;
            logf_("got ERROR code=%u req=%u msg=%s", e->code, e->request_type, e->message);
        } else if (h->type == SK_EV_WINDOW_CREATED) {
            const sk_ev_window_created *e = (const void *)buf;
            logf_("window created win=%u", e->win);
            if (win_out)
                *win_out = e->win;
        } else if (h->type == SK_EV_WELCOME) {
            logf_("welcome");
        }
    }
}

static void draw_and_damage(uint32_t win, int fd, int w, int h, uint32_t color)
{
    uint32_t *px = mmap(NULL, (size_t)w * h * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px != MAP_FAILED) {
        for (int i = 0; i < w * h; i++)
            px[i] = color ^ (uint32_t)((i / w / 16 + i % w / 16) % 2 ? 0x202020 : 0);
        munmap(px, (size_t)w * h * 4);
    }
    sk_req_damage d = { { SK_REQ_DAMAGE, sizeof d }, win, 0, 0, w, h };
    send(g_sock, &d, sizeof d, MSG_NOSIGNAL);
}

int main(int argc, char **argv)
{
    g_mode = argc > 1 ? argv[1] : "garbage";
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
    if (connect(g_sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        logf_("connect failed: %s", strerror(errno));
        return 1;
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    const char *m = g_mode;
    uint32_t win = 0;

    if (!strcmp(m, "nohello")) {
        int fd = memfd(100 * 100 * 4, 1);
        create_window(1, 100, 100, fd, "nohello");
    } else if (!strcmp(m, "badver")) {
        hello(99);
    } else {
        hello(SK_PROTO_VERSION);
    }

    if (!strcmp(m, "garbage")) {
        for (int i = 0; i < 16; i++) {
            uint8_t junk[300];
            size_t len = 1 + (size_t)(rand() % (int)sizeof junk);
            for (size_t k = 0; k < len; k++)
                junk[k] = (uint8_t)rand();
            send(g_sock, junk, len, MSG_NOSIGNAL);
        }
    } else if (!strcmp(m, "unsealed")) {
        create_window(1, 200, 100, memfd(200 * 100 * 4, 0), "unsealed");
    } else if (!strcmp(m, "small")) {
        create_window(1, 200, 100, memfd(200 * 100 * 4 - 1, 1), "small");
    } else if (!strcmp(m, "notmemfd")) {
        create_window(1, 200, 100, open("/dev/null", O_RDWR), "notmemfd");
    } else if (!strcmp(m, "nofd")) {
        create_window(1, 200, 100, -1, "nofd");
    } else if (!strcmp(m, "fdspam")) {
        int fds[3] = { memfd(4096, 1), memfd(4096, 1), open("/dev/null", O_RDONLY) };
        sk_req_damage d = { { SK_REQ_DAMAGE, sizeof d }, 1, 0, 0, 1, 1 };
        sendmsg_fds(&d, sizeof d, fds, 3);
    } else if (!strcmp(m, "badsize")) {
        sk_req_damage d = { { SK_REQ_DAMAGE, sizeof d - 1 }, 1, 0, 0, 1, 1 };
        send(g_sock, &d, sizeof d - 1, MSG_NOSIGNAL);
    } else if (!strcmp(m, "toobig")) {
        static uint8_t big[9000];
        sk_msg_header *h = (sk_msg_header *)big;
        h->type = SK_REQ_DAMAGE;
        h->size = 9000 & 0xFFFF;
        send(g_sock, big, sizeof big, MSG_NOSIGNAL);
    } else if (!strcmp(m, "wrongwin")) {
        sk_req_window d = { { SK_REQ_DESTROY_WINDOW, sizeof d }, 99999 };
        send(g_sock, &d, sizeof d, MSG_NOSIGNAL);
    } else if (!strcmp(m, "oversize")) {
        create_window(1, 5000, 10, memfd(5000 * 10 * 4, 1), "oversize");
    } else if (!strcmp(m, "manywin")) {
        for (int i = 0; i < 20; i++) {
            int fd = memfd(64 * 48 * 4, 1);
            create_window((uint32_t)i, 64, 48, fd, "many");
            close(fd);
        }
        drain(1500, NULL);
        logf_("done");
        return 0;
    } else if (!strcmp(m, "shrink")) {
        int fd = memfd(300 * 200 * 4, 1);
        create_window(1, 300, 200, fd, "shrink 测试");
        drain(500, &win);
        draw_and_damage(win, fd, 300, 200, 0xFF5BAA62);
        if (ftruncate(fd, 0) < 0)
            logf_("ftruncate refused: %s (good)", strerror(errno));
        else
            logf_("ftruncate succeeded?!");
        drain(1000, NULL);
        logf_("still alive");
        return 0;
    } else if (!strcmp(m, "flood")) {
        int fd = memfd(200 * 150 * 4, 1);
        create_window(1, 200, 150, fd, "flood 测试");
        drain(500, &win);
        draw_and_damage(win, fd, 200, 150, 0xFFF29E4C);
        int sent = 0;
        for (int i = 0; i < 20000; i++) {
            sk_req_damage d = { { SK_REQ_DAMAGE, sizeof d }, win, rand() % 200, rand() % 150, 30, 30 };
            if (send(g_sock, &d, sizeof d, MSG_NOSIGNAL) == (ssize_t)sizeof d)
                sent++;
        }
        logf_("flood sent=%d", sent);
        drain(2000, NULL);
        logf_("done");
        return 0;
    } else if (!strcmp(m, "hang") || !strcmp(m, "dragdie") || !strcmp(m, "crash")) {
        int fd = memfd(360 * 220 * 4, 1);
        char title[64];
        snprintf(title, sizeof title, "%s 测试", m);
        create_window(1, 360, 220, fd, title);
        drain(500, &win);
        /* 移到固定位置（内容区左上角 320,180），测试脚本才知道该点哪里 */
        sk_req_move mv = { { SK_REQ_MOVE, sizeof mv }, win, 320, 180 };
        send(g_sock, &mv, sizeof mv, MSG_NOSIGNAL);
        draw_and_damage(win, fd, 360, 220, !strcmp(m, "hang") ? 0xFFF2C94C : 0xFF9B7BD4);
        logf_("window ready win=%u", win);
        if (!strcmp(m, "hang")) {
            /* 再也不读套接字：服务器发来的事件会堆在内核缓冲区里 */
            for (;;)
                pause();
        }
        if (!strcmp(m, "crash")) {
            sleep(2);
            logf_("crashing now");
            raise(SIGSEGV);
        }
        uint8_t buf[SK_MSG_MAX];
        for (;;) {
            ssize_t n = recv(g_sock, buf, sizeof buf, 0);
            if (n <= 0)
                return 1;
            const sk_ev_mouse *e = (const void *)buf;
            if (e->hdr.type == SK_EV_MOUSE && e->kind == SK_MOUSE_DOWN) {
                logf_("got mouse down, dying mid-drag");
                _exit(3);
            }
        }
    } else if (strcmp(m, "nohello") && strcmp(m, "badver")) {
        logf_("unknown mode");
        return 2;
    }
    int dropped = drain(3000, NULL);
    logf_("%s", dropped ? "result: disconnected" : "result: still connected");
    return 0;
}
