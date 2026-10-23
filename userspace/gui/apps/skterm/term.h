/*
 * term.h — skterm 的“终端仿真器”核心：字符网格 + 转义序列解析（vt.c）
 *
 * 终端仿真器做的事情可以一句话说完：**把程序写出来的字节流，变成一格一格的字符画面。**
 *
 *   shell / ls / top 等程序          内核 pty               skterm
 *   printf("\033[31m红\033[0m") ─▶ 从机端 ─ 线路规程 ─▶ 主机端 ─▶ vt.c 解析 ─▶ 网格 ─▶ draw.c 画到窗口
 *
 * 程序不知道（也不关心）自己面对的是真正的硬件终端、串口还是一个图形窗口，它们只会往
 * 标准输出写“普通字符 + 控制序列”。控制序列是 1970 年代 DEC VT100 终端定下的“协议”，
 * 后来 xterm 又扩展了很多（256 色、备用屏幕……），环境变量 TERM=xterm-256color 就是在
 * 告诉程序“对面是一个 xterm 兼容的终端，可以用这些序列”。
 *
 * 这个模块不依赖图形库：它只维护数据（哪一格是什么字、什么颜色、光标在哪），并记录
 * “哪些格子变了”（脏区），由图形部分决定怎么画、画哪些。这样的分层让渲染可以做到
 * “只重画变化的格子”——网页版（v86 模拟器）里这是终端流畅的关键。
 */
#ifndef SKTERM_TERM_H
#define SKTERM_TERM_H

#include <stddef.h>
#include <stdint.h>

/* ---------- 一个格子 ---------- */

/* 属性位（SGR 序列设置的“画笔”属性，外加宽字符的两个标记） */
enum {
    TA_BOLD      = 1 << 0,
    TA_DIM       = 1 << 1,
    TA_ITALIC    = 1 << 2,
    TA_UNDERLINE = 1 << 3,
    TA_BLINK     = 1 << 4,
    TA_REVERSE   = 1 << 5,   /* 反显：前景色和背景色对调（more 的提示、top 的表头） */
    TA_HIDDEN    = 1 << 6,
    TA_STRIKE    = 1 << 7,
    TA_WIDE      = 1 << 8,   /* 宽字符（汉字）的左半格：字形画在这一格，占两格宽 */
    TA_CONT      = 1 << 9,   /* 宽字符的右半格：本身没有字，只表示“被左边的字占着” */
};
#define TA_PEN_MASK  0x00FF  /* 画笔属性（不含宽字符标记） */

/* 颜色编码：不直接存 RGB，因为“默认色”和 16 色的具体颜色要到画的时候才决定
 * （同一个“红色”，在纸色主题里是蜡笔红；默认前景是铅笔墨色）。 */
#define TC_DEFAULT   0u
#define TC_INDEXED   0x01000000u   /* | 调色板序号 0..255 */
#define TC_RGB       0x02000000u   /* | 0xRRGGBB（真彩色 SGR 38;2;r;g;b） */
#define TC_KIND(c)   ((c) & 0xFF000000u)

typedef struct {
    uint32_t cp;        /* Unicode 码点；0 = 空格子（画出来和空格一样） */
    uint32_t fg, bg;    /* TC_* 编码 */
    uint16_t attr;      /* TA_* */
    uint16_t pad;       /* 总是 0（格子用 memcmp 比较） */
} tcell;

/* 一行：格子数在创建时固定（改变窗口宽度时整行重新分配） */
typedef struct {
    int   len;          /* 格子数；历史区里的行会去掉行尾空白，所以可能比屏幕窄 */
    int   wrapped;      /* 1 = 这一行写满后自动折到了下一行（复制文字时两行之间不加换行） */
    tcell c[];
} tline;

/* ---------- 回调：终端仿真器需要“对外”做的几件事 ---------- */
typedef struct {
    /* 回答程序的询问（光标位置报告 DSR、设备属性 DA）：这些字节要写回 pty，
     * 程序从自己的标准输入读到它们 */
    void (*reply)(void *user, const char *s, size_t n);
    /* OSC 0 / OSC 2：程序设置窗口标题 */
    void (*title)(void *user, const char *utf8);
    /* BEL（\a）：响铃 */
    void (*bell)(void *user);
    void *user;
} term_callbacks;

/* ---------- 终端状态 ---------- */
#define TERM_MAX_PARAMS 32
#define TERM_STR_MAX    1024

typedef struct {
    int   cx, cy, wrap_pending, origin, gl;
    int   charset[2];
    tcell pen;
} term_saved;

typedef struct term {
    int cols, rows;

    /* 两块屏幕：主屏幕（有滚动历史）和备用屏幕（全屏程序用，没有历史）。
     * scr 指向当前使用的那一块。每块是 rows 个行指针——滚动时只需要挪指针，不用搬格子。 */
    tline **scr, **pri, **alt;
    int     alt_on;

    /* 滚动历史：环形数组，从屏幕顶端滚出去的行进到这里 */
    tline   **sb;
    int       sb_cap, sb_head, sb_count;
    long long sb_dropped;   /* 历史区满了以后被丢掉的行数（用来给每一行一个稳定的“绝对行号”） */
    long long sb_pushed;    /* 累计进入历史区的行数 */

    /* 光标与画笔 */
    int   cx, cy;
    int   wrap_pending;     /* 刚在最后一列写了字：下一个字先换行（xterm 的“延迟折行”） */
    tcell pen;
    int   top, bot;         /* 滚动区域（DECSTBM），闭区间 */

    /* 模式 */
    int autowrap;           /* ?7  自动折行 */
    int cursor_visible;     /* ?25 显示光标 */
    int app_cursor;         /* ?1  方向键发 ESC O A 而不是 ESC [ A（DECCKM） */
    int insert;             /* 4   插入模式 */
    int origin;             /* ?6  光标坐标相对滚动区域 */
    int bracketed_paste;    /* ?2004 粘贴的内容用 ESC[200~ … ESC[201~ 包起来 */
    int cursor_shape;       /* DECSCUSR：0/1/2 方块，3/4 下划线，5/6 竖线 */
    int charset[2], gl;     /* G0/G1 字符集（1 = DEC 画线字符），gl 选当前用哪个 */
    term_saved saved[2];    /* ESC 7 保存的光标：主屏幕、备用屏幕各一份 */
    uint8_t   *tabs;        /* 每列一个字节：1 = 这里有制表位 */
    uint32_t   last_cp;     /* 最近画的字（REP 用） */

    /* 解析器状态（序列可能被 read() 从任意位置切断，所以状态必须跨调用保存） */
    int      state;
    int      params[TERM_MAX_PARAMS];
    uint8_t  sub[TERM_MAX_PARAMS];   /* 1 = 这个参数前面是冒号（38:2:r:g:b 这种子参数） */
    int      np, any_param;
    char     priv, inter;
    char     str[TERM_STR_MAX];
    int      str_len, str_kind, str_esc;
    uint32_t u8_cp, u8_min;
    int      u8_need;

    /* 脏区：每个屏幕行一个 [lo, hi) 列区间（lo >= hi 表示这行没变）。
     * shift = 整屏向上滚动了多少行还没画（图形部分可以直接把像素往上挪，不用重画文字）；
     * full = 整屏都要重画。 */
    int *dlo, *dhi;
    int  shift, full;

    term_callbacks cb;
} term;

/* 创建 cols×rows 的终端，历史区最多 scrollback 行。失败返回 NULL */
term *term_new(int cols, int rows, int scrollback, const term_callbacks *cb);
void  term_free(term *t);

/* 把程序输出的字节喂给终端（可以是任意长度、在任意位置切断） */
void  term_write(term *t, const void *data, size_t n);

/* 改变尺寸：尽量保留内容（光标所在行保持可见，多出来的行进历史区）。成功 0 */
int   term_resize(term *t, int cols, int rows);

/* 绝对行号：历史区最老的一行是 term_first_line()，屏幕第 0 行是 term_screen_line()。
 * 内容滚动时，同一行文字的绝对行号不变（选区就是用它记录的）。 */
long long term_first_line(const term *t);
long long term_screen_line(const term *t);
/* 取绝对行号对应的行，不存在返回 NULL。行可能比 cols 窄（历史区的行），缺的部分当空白 */
const tline *term_line(const term *t, long long abs);

/* 脏区操作 */
void  term_dirty_cells(term *t, int row, int c0, int c1);
void  term_clear_dirty(term *t);

#endif /* SKTERM_TERM_H */
