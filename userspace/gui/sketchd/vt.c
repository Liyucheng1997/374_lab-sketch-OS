/*
 * vt.c — 虚拟终端 (VT) 的图形模式切换，以及崩溃时恢复文字控制台
 *
 * Linux 开机后屏幕上那个黑底白字的登录界面叫“虚拟终端”(/dev/tty1 …)。
 * 内核里的 fbcon 负责把终端的文字画到帧缓冲上，键盘处理程序 (keyboard.c) 负责把按键
 * 翻译成字符送进终端的输入队列。图形界面接管屏幕和键盘时，要跟这两位“打招呼”：
 *
 *   ioctl(KDSETMODE, KD_GRAPHICS)
 *       告诉 VT 层：“这个终端现在是图形模式”。fbcon 就不再往屏幕上画文字、
 *       不再画闪烁的光标，也不会在内核打印消息时把画面冲掉。
 *
 *   ioctl(KDSKBMODE, K_OFF)
 *       关闭这个终端的键盘输入。我们直接从 /dev/input/event* 读键盘，
 *       如果不关掉，按下的键还会被翻译成字符堆在 tty1 的输入缓冲区里；
 *       Alt+F2 之类的组合键还会触发切换终端。
 *
 * 这两项设置是“粘”在终端上的，不会因为进程退出而自动恢复！如果 sketchd 崩溃时
 * 不恢复，屏幕会停留在最后一帧、键盘失灵，用户只能重启。所以：
 *   - 正常退出、SIGTERM/SIGINT/SIGHUP（main.c 用 signalfd 处理）时调用 vt_restore()；
 *   - SIGSEGV/SIGBUS/SIGABRT 等“致命信号”的处理函数里也尽力恢复，然后再把信号
 *     按默认方式重新触发（让进程照常崩溃、产生正确的退出状态，sinit 好重新拉起它）。
 * 信号处理函数里只能调用“异步信号安全”的函数（ioctl、write 可以；printf、malloc 不行），
 * 因为信号可能在任意时刻打断主程序——比如正好在 malloc 内部持有锁的时候。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <errno.h>
#include <linux/kd.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int g_vt_fd = -1;
static int g_old_kbmode = -1;
static volatile sig_atomic_t g_vt_graphics = 0;

int vt_setup(int fd)
{
    int mode = 0;
    /* KDGETMODE 只对虚拟终端有效：用它判断 fd 是不是 VT（串口、管道、WSL 终端都不是） */
    if (ioctl(fd, KDGETMODE, &mode) < 0) {
        sk_log("vt: fd %d 不是虚拟终端（%s），跳过图形模式切换", fd, strerror(errno));
        return -1;
    }
    g_vt_fd = fd;
    if (ioctl(fd, KDGKBMODE, &g_old_kbmode) < 0) {
        sk_log("vt: KDGKBMODE 失败: %s", strerror(errno));
        g_old_kbmode = -1;
    }
    if (ioctl(fd, KDSETMODE, KD_GRAPHICS) < 0)
        sk_log("vt: KDSETMODE KD_GRAPHICS 失败: %s", strerror(errno));
    if (g_old_kbmode >= 0 && ioctl(fd, KDSKBMODE, K_OFF) < 0)
        sk_log("vt: KDSKBMODE K_OFF 失败: %s", strerror(errno));
    /* 丢掉切换之前残留在终端输入队列里的按键 */
    tcflush(fd, TCIFLUSH);
    g_vt_graphics = 1;
    SKD_DBG("vt: 已切换到图形模式（原键盘模式 %d）", g_old_kbmode);
    return 0;
}

void vt_restore(void)
{
    if (!g_vt_graphics || g_vt_fd < 0)
        return;
    g_vt_graphics = 0;
    if (g_old_kbmode >= 0)
        ioctl(g_vt_fd, KDSKBMODE, g_old_kbmode);
    /* 回到文字模式：fbcon 会把整个终端重画一遍，登录提示又出现了 */
    ioctl(g_vt_fd, KDSETMODE, KD_TEXT);
}

/* ---------- 致命信号处理 ---------- */

static void crash_handler(int sig)
{
    static const char msg[] = "sketchd: 收到致命信号，正在恢复文字控制台\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof msg - 1);
    (void)r;
    display_kms_emergency_restore();
    vt_restore();
    /* 安装时用了 SA_RESETHAND：处理函数执行时信号处理方式已经恢复成默认（终止进程）。
     * 这里再发一次给自己；信号在处理函数返回后送达，进程按默认方式终止。 */
    raise(sig);
}

void vt_install_crash_handlers(void)
{
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = crash_handler;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < SK_ARRAY_LEN(sigs); i++)
        sigaction(sigs[i], &sa, NULL);
}
