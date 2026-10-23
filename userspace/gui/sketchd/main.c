/*
 * main.c — sketchd 的入口：初始化各个模块，然后进入事件循环
 *
 * 用法：sketchd [--backend=auto|kms|fbdev|headless] [--mode=宽x高] [--no-splash]
 *               [--no-input] [--socket=路径] [--apps=apps.conf 路径] [--debug]
 * 环境变量：SKETCHD_BACKEND、SKETCHD_DEBUG=1、SKETCH_SOCKET、SKETCHD_APPS、SKETCHD_SHOT（SIGUSR1 截图路径）
 * 内核命令行：sketch.mode=1024x768  sketch.nosplash  sketch.autostart=a,b  sketch.backend=fbdev
 *
 * sinit 在 /dev/tty1 上启动我们（标准输入输出都是 tty1，tty1 也是“控制终端”）。
 * sketchd 退出后 sinit 会重新启动它（services.conf 里是 respawn），
 * 所以 Ctrl+Alt+Backspace “退出图形界面”实际上等于“重启图形界面”。
 *
 * 信号的处理方式：把 SIGCHLD/SIGTERM/SIGINT/SIGHUP/SIGUSR1 “屏蔽”掉，再用 signalfd 把它们
 * 变成一个可以 poll 的文件描述符。这样信号就像普通事件一样在主循环里按顺序处理，
 * 不会在任意时刻打断程序（传统的信号处理函数里几乎什么都不能做）。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

int skd_debug;

static volatile sig_atomic_t g_quit;
static skd_display g_disp;
static int g_have_display;

uint64_t skd_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void skd_quit(void) { g_quit = 1; }
int skd_screen_w(void) { return g_disp.w; }
int skd_screen_h(void) { return g_disp.h; }
sk_rect skd_work_area(void) { return sk_rect_make(0, 0, g_disp.w, g_disp.h - SKD_TASKBAR_H); }

void skd_desktop_ready(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    /* 自动化测试等的就是这一行（docs/开发规范.md 4.6） */
    sk_log_console("ready %dx%d backend=%s mouse=%s", g_disp.w, g_disp.h, g_disp.name,
                   input_mouse_kind());
    launch_autostart();
}

/* ---------- 参数 ---------- */

typedef struct {
    char backend[16];
    int  mode_w, mode_h;
    int  no_splash;
    int  no_input;
    char socket[108];
    char apps[256];
} options;

static void usage(void)
{
    fprintf(stderr,
            "用法: sketchd [--backend=auto|kms|fbdev|headless] [--mode=宽x高] [--no-splash]\n"
            "              [--no-input] [--socket=路径] [--apps=apps.conf 路径] [--debug]\n");
}

static int parse_mode(const char *s, int *w, int *h)
{
    int a, b;
    if (sscanf(s, "%dx%d", &a, &b) == 2 && a >= 320 && b >= 240 && a <= 4096 && b <= 4096) {
        *w = a;
        *h = b;
        return 0;
    }
    return -1;
}

static int parse_options(int argc, char **argv, options *o)
{
    char buf[64];
    memset(o, 0, sizeof *o);
    sk_strlcpy(o->backend, "auto", sizeof o->backend);
    const char *env = getenv("SKETCH_SOCKET");
    sk_strlcpy(o->socket, env && *env ? env : SK_SOCKET_PATH, sizeof o->socket);
    env = getenv("SKETCHD_APPS");
    sk_strlcpy(o->apps, env && *env ? env : "/etc/sketch/apps.conf", sizeof o->apps);
    if ((env = getenv("SKETCHD_BACKEND")) && *env)
        sk_strlcpy(o->backend, env, sizeof o->backend);
    if ((env = getenv("SKETCHD_DEBUG")) && *env && strcmp(env, "0") != 0)
        skd_debug = 1;
    /* 内核命令行（sinit 启动我们时没法传参数，测试时改命令行最方便） */
    if (sk_cmdline_get("sketch.mode", buf, sizeof buf) && parse_mode(buf, &o->mode_w, &o->mode_h) < 0)
        sk_log("sketch.mode=%s 格式不对（应为 宽x高）", buf);
    if (sk_cmdline_get("sketch.nosplash", buf, sizeof buf))
        o->no_splash = 1;
    if (sk_cmdline_get("sketch.backend", buf, sizeof buf) && buf[0])
        sk_strlcpy(o->backend, buf, sizeof o->backend);
    if (sk_cmdline_get("sketch.debug", buf, sizeof buf))
        skd_debug = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (sk_startswith(a, "--backend="))
            sk_strlcpy(o->backend, a + 10, sizeof o->backend);
        else if (sk_startswith(a, "--mode=")) {
            if (parse_mode(a + 7, &o->mode_w, &o->mode_h) < 0) {
                fprintf(stderr, "sketchd: 分辨率格式不对: %s\n", a + 7);
                return -1;
            }
        } else if (strcmp(a, "--no-splash") == 0)
            o->no_splash = 1;
        else if (strcmp(a, "--no-input") == 0)
            o->no_input = 1;
        else if (sk_startswith(a, "--socket="))
            sk_strlcpy(o->socket, a + 9, sizeof o->socket);
        else if (sk_startswith(a, "--apps="))
            sk_strlcpy(o->apps, a + 7, sizeof o->apps);
        else if (strcmp(a, "--debug") == 0)
            skd_debug = 1;
        else {
            usage();
            return -1;
        }
    }
    if (strcmp(o->backend, "auto") && strcmp(o->backend, "kms") && strcmp(o->backend, "fbdev") &&
        strcmp(o->backend, "headless")) {
        fprintf(stderr, "sketchd: 未知的显示后端 \"%s\"\n", o->backend);
        return -1;
    }
    if (strcmp(o->backend, "headless") == 0)
        o->no_input = 1;
    return 0;
}

/* ---------- 打开显示设备 ---------- */

static int open_display(const options *o)
{
    if (strcmp(o->backend, "headless") == 0)
        return display_headless_open(&g_disp, o->mode_w, o->mode_h);
    if (strcmp(o->backend, "fbdev") != 0) {
        if (display_kms_open(&g_disp, o->mode_w, o->mode_h) == 0)
            return 0;
        if (strcmp(o->backend, "kms") == 0)
            return -1;
        sk_log("KMS 不可用，改用 fbdev");
    }
    return display_fb_open(&g_disp);
}

/* 实在没有能用的显示设备：退回文字界面，在这个终端上运行 shell */
static void text_fallback(void)
{
    vt_restore();
    sk_log_console("没有可用的显示设备，改用文字界面 (/bin/sksh -l)");
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
    execl("/bin/sksh", "sksh", "-l", (char *)NULL);
    sk_log("exec /bin/sksh 失败: %s", strerror(errno));
    exit(1);
}

/* ---------- 信号 ---------- */

static void save_debug_shot(void)
{
    const char *path = getenv("SKETCHD_SHOT");
    if (!path || !*path)
        path = "/tmp/sketchd-shot.ppm";
    sk_canvas *c = sk_canvas_new(g_disp.w, g_disp.h);
    if (!c)
        return;
    comp_snapshot(c);
    if (sk_image_save_ppm(c, path) == 0)
        sk_log("已把画面保存到 %s", path);
    sk_canvas_free(c);
}

static void handle_signals(int sfd)
{
    struct signalfd_siginfo si;
    while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
        switch (si.ssi_signo) {
        case SIGCHLD:
            launch_reap_children();
            break;
        case SIGTERM:
        case SIGINT:
        case SIGHUP:
            sk_log("收到信号 %u，退出", si.ssi_signo);
            g_quit = 1;
            break;
        case SIGUSR1:
            save_debug_shot();
            break;
        default:
            break;
        }
    }
}

static int min_timeout(int a, int b)
{
    if (a < 0)
        return b;
    if (b < 0)
        return a;
    return a < b ? a : b;
}

/* ---------- 主循环 ---------- */

#define MAX_POLL 96

static void event_loop(int sfd)
{
    while (!g_quit) {
        struct pollfd pfd[MAX_POLL];
        int n = 0;
        int i_sig = n;
        pfd[n++] = (struct pollfd){ .fd = sfd, .events = POLLIN };
        int i_clock = -1, i_meter = -1;
        if (taskbar_clock_fd() >= 0) {
            i_clock = n;
            pfd[n++] = (struct pollfd){ .fd = taskbar_clock_fd(), .events = POLLIN };
        }
        if (taskbar_meter_fd() >= 0) {
            i_meter = n;
            pfd[n++] = (struct pollfd){ .fd = taskbar_meter_fd(), .events = POLLIN };
        }
        int i_in = n, nin = input_pollfds(pfd + n, MAX_POLL - n);
        n += nin;
        int i_srv = n, nsrv = server_pollfds(pfd + n, MAX_POLL - n);
        n += nsrv;

        /* 没有动画、没有按住的鼠标、没有提示条要消失时，timeout = -1：无限期睡眠，
         * 直到有输入、有客户端消息、有信号或定时器到点。空闲时 CPU 占用为 0。 */
        int timeout = -1;
        timeout = min_timeout(timeout, splash_timeout_ms());
        timeout = min_timeout(timeout, menu_timeout_ms());
        timeout = min_timeout(timeout, input_timeout_ms());
        if (comp_has_work())
            timeout = 0;

        int r = poll(pfd, (nfds_t)n, timeout);
        if (r < 0 && errno != EINTR) {
            sk_log("poll 失败: %s", strerror(errno));
            break;
        }
        if (r > 0 && pfd[i_sig].revents)
            handle_signals(sfd);
        /* 先处理完所有输入，再合成：拖动窗口时一大串鼠标移动只合成一次 */
        if (r > 0 && nin)
            input_dispatch(pfd + i_in, nin);
        if (input_timeout_ms() >= 0)
            input_tick();
        if (r > 0 && nsrv)
            server_dispatch(pfd + i_srv, nsrv);
        if (i_clock >= 0 && (pfd[i_clock].revents & POLLIN))
            taskbar_clock_tick();
        if (i_meter >= 0 && (pfd[i_meter].revents & POLLIN))
            taskbar_meter_tick();
        splash_tick();
        menu_tick();
        comp_frame();      /* 每轮循环最多合成一次 */
    }
}

int main(int argc, char **argv)
{
    sk_log_init("sketchd");
    options opt;
    if (parse_options(argc, argv, &opt) < 0)
        return 2;

    /* 信号：致命信号用处理函数尽力恢复控制台；其余的交给 signalfd */
    vt_install_crash_handlers();
    signal(SIGPIPE, SIG_IGN);     /* 写已断开的套接字返回 EPIPE，而不是直接杀死我们 */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    int sfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        sk_log("signalfd 失败: %s", strerror(errno));
        return 1;
    }

    int headless = strcmp(opt.backend, "headless") == 0;
    if (!headless)
        vt_setup(STDIN_FILENO);

    if (open_display(&opt) < 0) {
        if (headless)
            return 1;
        text_fallback();
    }
    g_have_display = 1;
    if (comp_init(&g_disp) < 0) {
        g_disp.close(&g_disp);
        text_fallback();
    }

    apps_load(opt.apps);
    /* 提前加载字体（第一次用到时才 mmap，放在这里让启动动画第一帧不卡顿） */
    for (int i = 0; i < SK_FONT_COUNT; i++)
        sk_font_get(i);
    cursor_init();
    desktop_init(g_disp.w, g_disp.h);
    taskbar_init(g_disp.w, g_disp.h);
    menu_init(g_disp.w, g_disp.h);
    wm_init();
    if (!opt.no_input)
        input_init(g_disp.w, g_disp.h);

    if (server_init(opt.socket) < 0)
        sk_log("显示协议套接字创建失败：应用将无法连接");
    launch_set_socket_path(opt.socket);
    setenv(SK_SOCKET_ENV, opt.socket, 1);

    if (opt.no_splash) {
        comp_damage_all();
        comp_frame();
        skd_desktop_ready();
    } else {
        splash_start(g_disp.w, g_disp.h);
    }

    event_loop(sfd);

    /* ---- 清理：顺序与初始化相反 ---- */
    sk_log("正在退出");
    menu_shutdown();
    server_shutdown();
    wm_shutdown();
    input_shutdown();
    taskbar_shutdown();
    desktop_shutdown();
    comp_shutdown();
    if (g_have_display && g_disp.close)
        g_disp.close(&g_disp);
    vt_restore();
    close(sfd);
    return 0;
}
