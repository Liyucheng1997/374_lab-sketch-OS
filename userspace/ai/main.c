/*
 * skaid — SketchOS 的 AI 服务（守护进程）
 *
 * 开机时由 sinit 按 /etc/sketch/services.conf 启动（策略 respawn：崩溃了会被重新拉起来）。
 * 它同时盯着三类文件描述符，全部放进一个 poll() 里，单线程处理：
 *   1. 监听套接字 /run/sketch/ai.sock：有新的客户端（命令行 ai、图形应用 skai）连进来；
 *   2. 每个客户端的连接：客户端发来的问题、取消、清空对话；
 *   3. 串口 /dev/ttyS1：网页上的 AI 桥送回来的流式回应（见 bridge.c）。
 * 没有事件时进程睡在 poll() 里，不占 CPU（见开发规范“性能规范”）。
 *
 * 同一个程序有两种身份（像 skbox 一样看 argv[0]）：
 *   skaid [-d]       守护进程；-d 打印调试信息到标准错误（服务的日志在 /var/log/skaid.log）
 *   ai ...           命令行客户端（/bin/ai 是指向 skaid 的符号链接），见 cli.c
 */
#include "skaid.h"

#include <skutil.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int g_debug;

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void dbg(const char *fmt, ...)
{
    if (!g_debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "skaid: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

const char *clip(const char *s, int cols)
{
    /* 日志里只放开头一小段；用 sk_utf8_fit 截断，不会把一个汉字切成两半 */
    static char buf[4][256];
    static int k;
    char *b = buf[k++ & 3];
    size_t n = sk_utf8_fit(s, SK_MIN(cols, 80), NULL);
    memcpy(b, s, n);
    b[n] = 0;
    return b;
}

static int listen_socket(const char *path)
{
    char dir[108];
    sk_strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        sk_mkdir_p(dir, 0755);
    }
    /* 上一次运行留下的套接字文件还在的话，bind 会失败（EADDRINUSE）：先删掉 */
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        sk_log("socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    sk_strlcpy(addr.sun_path, path, sizeof addr.sun_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 8) < 0) {
        sk_log("监听 %s 失败: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0666);
    return fd;
}

static void on_client_readable(session *s)
{
    long n = skai_reader_fill(&s->rd, s->fd);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        s->dead = 1;                /* 客户端关掉了连接（窗口关了、ai 命令结束了） */
        return;
    }
    char *line;
    while (!s->dead && (line = skai_reader_next(&s->rd))) {
        char *arg;
        const char *cmd = skai_split(line, &arg);
        dbg("客户端 %d: %s %s", s->fd, cmd, clip(arg, 60));
        agent_on_client(s, cmd, arg);
        free(line);
    }
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (!strcmp(base, "ai"))
        return cli_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "ai"))         /* skaid ai ...：同样当客户端 */
        return cli_main(argc - 1, argv + 1);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d"))
            g_debug = 1;
        else {
            fprintf(stderr, "用法: skaid [-d]        （命令行客户端请用 ai）\n");
            return 2;
        }
    }
    sk_log_init("skaid");
    /* 客户端随时可能断开；往断开的连接写数据时内核默认发 SIGPIPE 把我们杀掉，
     * 忽略它以后 write() 只是返回 EPIPE，我们关掉那个会话就好 */
    signal(SIGPIPE, SIG_IGN);

    const char *path = getenv(SKAI_SOCKET_ENV);
    if (!path || !*path)
        path = SKAI_SOCKET_PATH;
    int lfd = listen_socket(path);
    if (lfd < 0)
        return 1;
    agent_init();
    bridge_open();
    sk_log_console("ready socket=%s bridge=%s", path, bridge_fd() >= 0 ? SKAID_BRIDGE_DEV : "none");

    session *clients[SKAID_MAX_SESSIONS];
    int nclients = 0;
    for (;;) {
        struct pollfd pf[2 + SKAID_MAX_SESSIONS];
        int n = 0;
        pf[n++] = (struct pollfd){ lfd, POLLIN, 0 };
        int bfd = bridge_fd();
        if (bfd >= 0)
            pf[n++] = (struct pollfd){ bfd, POLLIN, 0 };
        int first_client = n;
        for (int i = 0; i < nclients; i++)
            pf[n++] = (struct pollfd){ clients[i]->fd, POLLIN, 0 };

        int r = poll(pf, (nfds_t)n, bridge_next_timeout(now_ms()));
        if (r < 0 && errno != EINTR) {
            sk_log("poll: %s", strerror(errno));
            return 1;
        }
        bridge_tick(now_ms());
        if (r <= 0)
            continue;

        if (bfd >= 0 && (pf[1].revents & POLLIN))
            bridge_on_readable();
        for (int i = 0; i < nclients; i++)
            if (pf[first_client + i].revents & (POLLIN | POLLHUP | POLLERR))
                on_client_readable(clients[i]);
        if (pf[0].revents & POLLIN) {
            int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (cfd >= 0) {
                session *s = nclients < SKAID_MAX_SESSIONS ? agent_new_session(cfd) : NULL;
                if (s) {
                    clients[nclients++] = s;
                    dbg("新客户端 fd=%d", cfd);
                } else {
                    skai_send(cfd, "ERROR", "同时打开的 AI 对话太多了");
                    close(cfd);
                }
            }
        }
        /* 回收断开的会话（倒着删，下标才不会乱） */
        for (int i = nclients - 1; i >= 0; i--) {
            if (clients[i]->dead) {
                dbg("客户端 fd=%d 断开", clients[i]->fd);
                agent_close_session(clients[i]);
                clients[i] = clients[--nclients];
            }
        }
    }
}
