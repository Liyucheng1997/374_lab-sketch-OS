/*
 * reset — 把终端恢复到正常状态
 *
 * 程序异常退出时可能把终端留在“原始模式”（不回显、按键不等回车）。
 * reset 做两件事：
 *   1. 用 tcsetattr() 把内核 tty 驱动的设置（行规程）恢复成正常值（相当于 stty sane）；
 *   2. 输出 ESC c（RIS，完全重置），让终端程序自己也恢复默认的颜色和字符集。
 */
#include "skbox.h"

#include <termios.h>
#include <unistd.h>

SKBOX_APPLET(reset, "恢复终端设置并清屏")

static const char usage[] =
    "用法: reset\n"
    "恢复终端的正常设置（回显、行编辑、Ctrl+C 等），并重置终端显示。\n"
    "程序崩溃后终端不回显或显示乱码时使用。\n";

int applet_reset(int argc, char **argv)
{
    if (sb_help(argc, argv, usage))
        return 0;
    struct termios t;
    int fd = isatty(STDIN_FILENO) ? STDIN_FILENO : STDOUT_FILENO;
    if (tcgetattr(fd, &t) == 0) {
        /* 输入：回车翻译成换行、支持 Ctrl+S/Ctrl+Q 流控、UTF-8 退格 */
        t.c_iflag |= BRKINT | ICRNL | IXON | IMAXBEL | IUTF8;
        t.c_iflag &= ~(tcflag_t)(INLCR | IGNCR | IXOFF);
        /* 输出：换行翻译成“回车+换行” */
        t.c_oflag |= OPOST | ONLCR;
        t.c_oflag &= ~(tcflag_t)(OCRNL | ONOCR | ONLRET);
        /* 本地：规范模式（按行读）、回显、信号键 */
        t.c_lflag |= ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE;
        t.c_lflag &= ~(tcflag_t)(ECHONL | NOFLSH | TOSTOP | ECHOPRT);
        t.c_cc[VINTR] = 3;      /* Ctrl+C */
        t.c_cc[VQUIT] = 28;     /* Ctrl+\ */
        t.c_cc[VERASE] = 127;   /* 退格 */
        t.c_cc[VKILL] = 21;     /* Ctrl+U */
        t.c_cc[VEOF] = 4;       /* Ctrl+D */
        t.c_cc[VSUSP] = 26;     /* Ctrl+Z */
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSADRAIN, &t) != 0)
            sb_perr("无法恢复终端设置");
    }
    /* ESC c：重置；ESC[?25h：显示光标；ESC[0m：恢复默认颜色 */
    fputs("\033c\033[?25h\033[0m", stdout);
    return 0;
}
