/*
 * skterm.h — 终端窗口程序内部共用的结构和函数（main.c / pty.c / draw.c / input.c）
 *
 * 模块分工：
 *   vt.c     终端仿真器：解析转义序列，维护字符网格（不碰图形，见 term.h）
 *   pty.c    伪终端：创建 pty、启动 shell 子进程、窗口尺寸、挂断
 *   draw.c   把网格画到窗口上：只画变化的格子、滚屏时直接搬像素、光标、选区、滚动条
 *   input.c  键盘 → 字节序列、鼠标选择/滚轮/滚动条、剪贴板
 *   main.c   启动流程、事件循环里的各种回调、子进程退出、调试钩子
 */
#ifndef SKTERM_H
#define SKTERM_H

#include <sketch/client.h>
#include <sketch/font.h>
#include <sketch/gfx.h>
#include <sketch/rough.h>
#include <sketch/theme.h>

#include <stdint.h>
#include <sys/types.h>

#include "term.h"

#define SKT_SCROLLBACK   1000   /* 滚动历史保留的行数 */
#define SKT_COLS         80     /* 默认 80×24：VT100 以来终端的“标准尺寸” */
#define SKT_ROWS         24
#define SKT_MIN_COLS     24
#define SKT_MIN_ROWS     6

/* 窗口内的版面（像素）：左边留出一条“笔记本红线”的位置，右边是滚动条 */
#define SKT_PAD_L        16
#define SKT_PAD_T        6
#define SKT_PAD_B        6
#define SKT_SBAR_W       12
#define SKT_PAD_R        (SKT_SBAR_W + 8)
#define SKT_MARGIN_X     7      /* 红线的 x */

typedef struct skterm {
    sk_app    *app;
    sk_window *win;
    term      *t;

    /* 字体与版面 */
    const sk_font *font;
    int        cw, lh, asc;       /* 格子宽、行高、基线到格子顶的距离 */
    int        ox, oy;            /* 文字区左上角 */
    sk_rect    sbar;              /* 滚动条矩形 */
    uint32_t   seed;              /* 纸张纹理和手绘笔触的种子（固定 → 每次重画一模一样） */

    /* 子进程与 pty */
    int        master;            /* pty 主机端；-1 = 没有/已关闭 */
    pid_t      child;             /* shell 的进程号；0 = 没有 */
    int        child_running;
    int        child_status;      /* waitpid 得到的状态 */
    int        command_mode;      /* 1 = 运行的是 “shell -c 命令”，结束后窗口保留 */
    int        finished;          /* 已显示“程序已结束”，等一个按键关窗口 */
    uint64_t   start_ms;
    char      *wq;                /* 还没写进 pty 的输入（pty 缓冲区满时排队） */
    size_t     wq_len, wq_cap;

    /* 视图：view = 往回翻了多少行（0 = 正在看最新的屏幕） */
    int        view;
    int        vshift;            /* 这一帧里 view 变了多少（画的时候直接搬像素） */
    long long  seen_pushed;

    /* 选区：两个端点（绝对行号 + 列边界 0..cols），anchor 是按下的位置，head 跟着鼠标 */
    int        sel_on, sel_drag;
    long long  sa_l, sh_l;
    int        sa_c, sh_c;

    /* 重绘状态：每个显示行一个脏列区间；rows_all = 整个文字区；chrome = 边距、红线、滚动条 */
    int       *vlo, *vhi;
    int        vrows;
    int        rows_all, chrome;
    int        cur_on, cur_x, cur_y, cur_w, cur_style;   /* 屏幕上画着的光标 */
    int        sb_total, sb_off, sb_state, sb_valid;     /* 屏幕上画着的滚动条 */
    int        sbar_hover, sbar_drag;
    int        focused;
    int        bell_timer;

    char       title_buf[SK_TITLE_MAX * 2];
    int        sigpipe[2];        /* 信号处理函数 → 事件循环 的“自管道” */
    int        debug;             /* SKTERM_DEBUG=1 */
} skterm;

/* ---------- pty.c ---------- */
/* 打开 pty、启动子进程：argv[0] 是要执行的程序，dir 是工作目录。成功 0，失败 -1（errno） */
int  skt_spawn(skterm *s, char *const argv[], const char *dir);
/* 把当前行列数告诉内核（内核会给前台进程组发 SIGWINCH） */
void skt_set_winsize(skterm *s);
/* 关窗口时“挂断”：给子进程所在的进程组发 SIGHUP */
void skt_hangup(skterm *s);

/* ---------- draw.c ---------- */
/* 窗口 w×h 能放下多少列、多少行，并设置版面 */
void skt_layout(skterm *s, int w, int h, int *cols, int *rows);
/* 窗口尺寸 ↔ 行列数 */
int  skt_win_w(const skterm *s, int cols);
int  skt_win_h(const skterm *s, int rows);
/* 显示行数变了（改变窗口大小后）：重新分配脏区数组 */
int  skt_view_alloc(skterm *s, int rows);
/* 画出所有变化的地方（每轮事件循环调用一次） */
void skt_render(skterm *s);
/* 整窗重画（窗口尺寸改变后） */
void skt_render_all(skterm *s);
void skt_mark_rows(skterm *s, int r0, int r1);
void skt_mark_all(skterm *s);
void skt_bell(skterm *s);
/* 显示行 r 对应的绝对行号 */
long long skt_view_line(const skterm *s, int r);
/* 调色：TC_* 编码 → 颜色 */
sk_color skt_color(uint32_t c, sk_color def);

/* ---------- input.c ---------- */
int  skt_key(skterm *s, const sk_event *e);
int  skt_mouse(skterm *s, const sk_event *e);
void skt_paste(skterm *s, const char *text, int len);
void skt_send(skterm *s, const void *data, size_t n);
void skt_flush_input(skterm *s);
void skt_scroll_view(skterm *s, int delta);
void skt_set_view(skterm *s, int view);
void skt_clear_selection(skterm *s);
void skt_mark_selection(skterm *s);
/* 选区在第 L 行上覆盖的列区间 [*a, *b)，没有返回 0 */
int  skt_sel_cols(const skterm *s, long long L, int *a, int *b);
char *skt_sel_text(const skterm *s, int *len);

/* ---------- main.c ---------- */
void skt_close(skterm *s);

#endif /* SKTERM_H */
