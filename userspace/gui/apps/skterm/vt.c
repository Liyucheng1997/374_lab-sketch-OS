/*
 * vt.c — VT100/xterm 转义序列解析 + 字符网格（skterm 的终端仿真器核心）
 *
 * ┌──────────────────────── 程序输出的字节流长什么样 ────────────────────────┐
 *
 *   普通字符     "hello"、"你好"（UTF-8：一个汉字 3 个字节）→ 写到光标处，光标右移
 *   C0 控制字符  \r 回到行首  \n 下移一行  \b 左移一格  \t 跳到下一个制表位  \a 响铃
 *   ESC 序列     ESC 7 / ESC 8 保存/恢复光标、ESC M 反向换行、ESC c 重置终端……
 *   CSI 序列     ESC [ 参数 ; 参数 … 结尾字母，最常用的一类：
 *                  ESC[2;5H 光标到第 2 行第 5 列     ESC[K 擦到行尾     ESC[2J 清屏
 *                  ESC[31m  前景红色                 ESC[0m 恢复默认    ESC[?1049h 备用屏幕
 *   OSC 序列     ESC ] 0 ; 标题 BEL —— 设置窗口标题
 *
 * └────────────────────────────────────────────────────────────────────────┘
 *
 * 解析器是一个“状态机”：每来一个字节，根据当前状态（普通文字 / 刚收到 ESC / 正在读 CSI 参数
 * / 正在读 OSC 字符串）决定怎么处理。程序一次 write() 的数据可能被内核拆成好几次 read()，
 * 一个转义序列、甚至一个汉字的 3 个字节都可能被切成两半——所以所有“读到一半”的状态
 * （包括 UTF-8 解码到第几个字节）都保存在 term 结构里，下一次接着来。
 *
 * 两个容易忽略的细节：
 *   - 延迟折行（pending wrap）：在最后一列写一个字后，光标**不**立刻跳到下一行，而是停在
 *     最后一列并记下“欠一次换行”；下一个字到来时才真正换行。如果接下来是 \r\n，就不会
 *     多出一个空行。sksh 的行编辑器正是按这个规则计算光标位置的。
 *   - 宽字符：汉字占两格。左格存字（TA_WIDE），右格只是占位（TA_CONT）。覆盖其中一半时，
 *     另一半必须清掉，否则屏幕上会留下“半个汉字”。如果一行只剩最后一格，汉字放不下，
 *     就把它挪到下一行（最后一格留空）。
 */
#include "term.h"

#include <sketch/font.h>    /* sk_wcwidth：码点在终端里占几格 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S_GROUND, S_ESC, S_ESC_INTER, S_CSI, S_CSI_IGNORE, S_STR };
enum { STR_OSC, STR_IGNORE };

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ======================================================================
 * 行与格子
 * ====================================================================== */

/* 空白格：擦除时用当前画笔的背景色（xterm 的“背景色擦除”BCE，TERM=xterm-256color 声明了它） */
static tcell blank_cell(const term *t)
{
    tcell c;
    memset(&c, 0, sizeof c);
    c.bg = t->pen.bg;
    return c;
}

static tcell default_cell(void)
{
    tcell c;
    memset(&c, 0, sizeof c);
    return c;
}

static tline *line_new(int len, tcell fill)
{
    tline *l = malloc(sizeof *l + (size_t)len * sizeof(tcell));
    if (!l)
        return NULL;
    l->len = len;
    l->wrapped = 0;
    for (int i = 0; i < len; i++)
        l->c[i] = fill;
    return l;
}

/* 把一行改成 len 格宽（返回新指针；内存不够时返回原来的行） */
static tline *line_resize(tline *l, int len)
{
    int old = l->len;
    if (old == len)
        return l;
    tline *n = realloc(l, sizeof *l + (size_t)len * sizeof(tcell));
    if (!n)
        return l;
    tcell d = default_cell();
    for (int i = old; i < len; i++)
        n->c[i] = d;
    n->len = len;
    /* 宽字符的右半格被截掉了：左半格也不能留着 */
    if (len > 0 && (n->c[len - 1].attr & TA_WIDE))
        n->c[len - 1] = d;
    return n;
}

/* ======================================================================
 * 脏区
 * ====================================================================== */

void term_dirty_cells(term *t, int row, int c0, int c1)
{
    if (row < 0 || row >= t->rows || t->full)
        return;
    c0 = imax(c0, 0);
    c1 = imin(c1, t->cols);
    if (c0 >= c1)
        return;
    if (t->dlo[row] >= t->dhi[row]) {
        t->dlo[row] = c0;
        t->dhi[row] = c1;
    } else {
        t->dlo[row] = imin(t->dlo[row], c0);
        t->dhi[row] = imax(t->dhi[row], c1);
    }
}

static void dirty_rows(term *t, int r0, int r1)
{
    for (int r = imax(r0, 0); r <= r1 && r < t->rows; r++) {
        t->dlo[r] = 0;
        t->dhi[r] = t->cols;
    }
}

void term_clear_dirty(term *t)
{
    for (int r = 0; r < t->rows; r++) {
        t->dlo[r] = t->cols;
        t->dhi[r] = 0;
    }
    t->shift = 0;
    t->full = 0;
}

/* 整屏向上滚了 n 行：已有的脏标记跟着内容一起上移，底部新出现的行整行脏。
 * 图形部分会把窗口像素整体上移 shift 行，所以没变的行完全不用重画。 */
static void dirty_shift(term *t, int n)
{
    if (t->full)
        return;
    if (n >= t->rows || t->shift + n >= t->rows) {
        t->full = 1;
        return;
    }
    memmove(t->dlo, t->dlo + n, (size_t)(t->rows - n) * sizeof *t->dlo);
    memmove(t->dhi, t->dhi + n, (size_t)(t->rows - n) * sizeof *t->dhi);
    dirty_rows(t, t->rows - n, t->rows - 1);
    t->shift += n;
}

/* 写一个格子：内容真的变了才标脏（程序重画同样的内容时——比如 top 每 3 秒刷新一次、
 * 行编辑器整行重画——没变的格子不会被重新绘制） */
static void set_cell(term *t, int y, int x, tcell c)
{
    tline *l = t->scr[y];
    if (x < 0 || x >= l->len)
        return;
    if (memcmp(&l->c[x], &c, sizeof c) != 0) {
        l->c[x] = c;
        term_dirty_cells(t, y, x, x + 1);
    }
}

/* 将要改写第 y 行的 [x0, x1) 格：如果边界切开了一个宽字符，把它的另一半清掉 */
static void fix_wide(term *t, int y, int x0, int x1)
{
    tline *l = t->scr[y];
    tcell b = blank_cell(t);
    if (x0 > 0 && x0 < l->len && (l->c[x0].attr & TA_CONT)) {
        b.bg = l->c[x0 - 1].bg;
        set_cell(t, y, x0 - 1, b);
    }
    if (x1 > 0 && x1 < l->len && (l->c[x1].attr & TA_CONT)) {
        b.bg = l->c[x1].bg;
        set_cell(t, y, x1, b);
    }
}

static void erase_cells(term *t, int y, int x0, int x1)
{
    x0 = imax(x0, 0);
    x1 = imin(x1, t->cols);
    if (x0 >= x1)
        return;
    fix_wide(t, y, x0, x1);
    tcell b = blank_cell(t);
    for (int x = x0; x < x1; x++)
        set_cell(t, y, x, b);
    if (x1 >= t->cols)
        t->scr[y]->wrapped = 0;
}

static void erase_rows(term *t, int r0, int r1)
{
    for (int r = imax(r0, 0); r <= r1 && r < t->rows; r++)
        erase_cells(t, r, 0, t->cols);
}

/* ======================================================================
 * 滚动历史
 * ====================================================================== */

static tline *sb_get(const term *t, int i)
{
    return t->sb[(t->sb_head + i) % t->sb_cap];
}

/* 屏幕顶端滚出去的一行进入历史区。行尾的空白格不保存（大多数行很短，省内存） */
static void sb_push(term *t, tline *l)
{
    if (t->sb_cap <= 0) {
        free(l);
        return;
    }
    int n = l->len;
    while (n > 0) {
        const tcell *c = &l->c[n - 1];
        if (c->cp || c->bg != TC_DEFAULT || (c->attr & (TA_REVERSE | TA_UNDERLINE | TA_STRIKE)))
            break;
        n--;
    }
    if (n < l->len) {
        tline *s = realloc(l, sizeof *l + (size_t)n * sizeof(tcell));
        if (s)
            l = s;
        l->len = n;
    }
    if (t->sb_count == t->sb_cap) {
        /* 满了：丢掉最老的一行 */
        free(t->sb[t->sb_head]);
        t->sb[t->sb_head] = l;
        t->sb_head = (t->sb_head + 1) % t->sb_cap;
        t->sb_dropped++;
    } else {
        t->sb[(t->sb_head + t->sb_count) % t->sb_cap] = l;
        t->sb_count++;
    }
    t->sb_pushed++;
}

static void sb_clear(term *t)
{
    for (int i = 0; i < t->sb_count; i++)
        free(sb_get(t, i));
    t->sb_dropped += t->sb_count;
    t->sb_head = 0;
    t->sb_count = 0;
}

long long term_first_line(const term *t) { return t->sb_dropped; }
long long term_screen_line(const term *t) { return t->sb_dropped + t->sb_count; }

const tline *term_line(const term *t, long long abs)
{
    long long i = abs - t->sb_dropped;
    if (i < 0)
        return NULL;
    if (i < t->sb_count)
        return sb_get(t, (int)i);
    i -= t->sb_count;
    if (i < t->rows)
        return t->scr[i];
    return NULL;
}

/* ======================================================================
 * 滚动
 * ====================================================================== */

/* 滚动区域 [top, bot] 向上滚 n 行（底部出现空行）。
 * history=1 时，主屏幕从第 0 行滚出去的行进入历史区（删除行 DL 不进） */
static void scroll_up(term *t, int top, int bot, int n, int history)
{
    int h = bot - top + 1;
    n = imin(n, h);
    if (n <= 0)
        return;
    int keep = history && !t->alt_on && top == 0;
    tline *gone[n > 0 ? n : 1];
    for (int i = 0; i < n; i++)
        gone[i] = t->scr[top + i];
    memmove(&t->scr[top], &t->scr[top + n], (size_t)(h - n) * sizeof *t->scr);
    tcell b = blank_cell(t);
    for (int i = 0; i < n; i++) {
        tline *nl = line_new(t->cols, b);
        if (!nl) {
            /* 内存不够：不进历史区，直接把滚出去的行清空后再利用 */
            nl = gone[i];
            gone[i] = NULL;
            for (int x = 0; x < nl->len; x++)
                nl->c[x] = b;
            nl->wrapped = 0;
        }
        t->scr[bot - n + 1 + i] = nl;
        if (gone[i]) {
            if (keep)
                sb_push(t, gone[i]);
            else
                free(gone[i]);
        }
    }
    if (top == 0 && bot == t->rows - 1)
        dirty_shift(t, n);
    else
        dirty_rows(t, top, bot);
}

/* 滚动区域 [top, bot] 向下滚 n 行（顶部出现空行，底部的行丢掉） */
static void scroll_down(term *t, int top, int bot, int n)
{
    int h = bot - top + 1;
    n = imin(n, h);
    if (n <= 0)
        return;
    tline *gone[n > 0 ? n : 1];
    for (int i = 0; i < n; i++)
        gone[i] = t->scr[bot - n + 1 + i];
    memmove(&t->scr[top + n], &t->scr[top], (size_t)(h - n) * sizeof *t->scr);
    tcell b = blank_cell(t);
    for (int i = 0; i < n; i++) {
        tline *l = gone[i];
        for (int x = 0; x < l->len; x++)
            l->c[x] = b;
        l->wrapped = 0;
        t->scr[top + i] = l;
    }
    dirty_rows(t, top, bot);
}

/* 换行（LF / IND）：光标在滚动区域底边时整个区域上滚，否则光标下移 */
static void index_down(term *t)
{
    if (t->cy == t->bot)
        scroll_up(t, t->top, t->bot, 1, 1);
    else if (t->cy < t->rows - 1)
        t->cy++;
}

/* 反向换行（ESC M）：光标在滚动区域顶边时整个区域下滚 */
static void index_up(term *t)
{
    if (t->cy == t->top)
        scroll_down(t, t->top, t->bot, 1);
    else if (t->cy > 0)
        t->cy--;
}

/* ======================================================================
 * 光标
 * ====================================================================== */

static void move_to(term *t, int x, int y)
{
    t->wrap_pending = 0;
    t->cx = iclamp(x, 0, t->cols - 1);
    if (t->origin)
        t->cy = iclamp(y + t->top, t->top, t->bot);
    else
        t->cy = iclamp(y, 0, t->rows - 1);
}

static void save_cursor(term *t)
{
    term_saved *s = &t->saved[t->alt_on];
    s->cx = t->cx;
    s->cy = t->cy;
    s->wrap_pending = t->wrap_pending;
    s->origin = t->origin;
    s->pen = t->pen;
    s->gl = t->gl;
    s->charset[0] = t->charset[0];
    s->charset[1] = t->charset[1];
}

static void restore_cursor(term *t)
{
    term_saved *s = &t->saved[t->alt_on];
    t->cx = iclamp(s->cx, 0, t->cols - 1);
    t->cy = iclamp(s->cy, 0, t->rows - 1);
    t->wrap_pending = s->wrap_pending;
    t->origin = s->origin;
    t->pen = s->pen;
    t->gl = s->gl;
    t->charset[0] = s->charset[0];
    t->charset[1] = s->charset[1];
}

static void reset_saved(term *t)
{
    for (int i = 0; i < 2; i++) {
        memset(&t->saved[i], 0, sizeof t->saved[i]);
    }
}

static void tab_reset(term *t)
{
    for (int x = 0; x < t->cols; x++)
        t->tabs[x] = (x % 8) == 0 && x > 0;
}

static void tab_forward(term *t, int n)
{
    int x = t->cx;
    while (n-- > 0) {
        x++;
        while (x < t->cols - 1 && !t->tabs[x])
            x++;
        if (x >= t->cols - 1) {
            x = t->cols - 1;
            break;
        }
    }
    t->cx = x;
}

static void tab_backward(term *t, int n)
{
    int x = t->cx;
    while (n-- > 0 && x > 0) {
        x--;
        while (x > 0 && !t->tabs[x])
            x--;
    }
    t->cx = x;
    t->wrap_pending = 0;
}

/* ======================================================================
 * 写字
 * ====================================================================== */

/* DEC 特殊图形字符集（ESC ( 0 之后，小写字母变成画线符号；老程序用它画表格边框） */
static uint32_t dec_graphics(uint32_t cp)
{
    static const uint16_t map[32] = {
        0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,   /* ` a b c d e f g */
        0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,   /* h i j k l m n o */
        0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,   /* p q r s t u v w */
        0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7, 0x0020,   /* x y z { | } ~ DEL */
    };
    if (cp >= 0x60 && cp <= 0x7E)
        return map[cp - 0x60];
    if (cp == 0x5F)
        return 0x0020;
    return cp;
}

static void insert_blanks(term *t, int n)
{
    int y = t->cy, x0 = t->cx;
    n = imin(n, t->cols - x0);
    if (n <= 0)
        return;
    fix_wide(t, y, x0, x0);
    tline *l = t->scr[y];
    for (int x = t->cols - 1; x >= x0 + n; x--)
        set_cell(t, y, x, l->c[x - n]);
    tcell b = blank_cell(t);
    for (int x = x0; x < x0 + n; x++)
        set_cell(t, y, x, b);
    /* 最右边被挤出去的可能是宽字符的右半格 */
    if (l->c[t->cols - 1].attr & TA_WIDE)
        set_cell(t, y, t->cols - 1, b);
}

static void delete_chars(term *t, int n)
{
    int y = t->cy, x0 = t->cx;
    n = imin(n, t->cols - x0);
    if (n <= 0)
        return;
    fix_wide(t, y, x0, x0 + n);
    tline *l = t->scr[y];
    for (int x = x0; x < t->cols - n; x++)
        set_cell(t, y, x, l->c[x + n]);
    tcell b = blank_cell(t);
    for (int x = t->cols - n; x < t->cols; x++)
        set_cell(t, y, x, b);
}

/* 在光标处写一个字符 */
static void put_char(term *t, uint32_t cp)
{
    if (t->charset[t->gl])
        cp = dec_graphics(cp);
    int w = sk_wcwidth(cp);
    if (w <= 0)
        return;                 /* 组合用的附加符号（零宽字符）：直接丢掉 */
    if (w > 2)
        w = 2;

    if (t->wrap_pending && t->autowrap) {
        /* 延迟折行：上一个字写在了最后一列，现在才真正换到下一行 */
        t->scr[t->cy]->wrapped = 1;
        t->cx = 0;
        index_down(t);
    }
    t->wrap_pending = 0;
    if (w == 2 && t->cx == t->cols - 1) {
        if (t->autowrap) {
            /* 汉字放不下最后一格：最后一格留空，挪到下一行 */
            erase_cells(t, t->cy, t->cx, t->cols);
            t->scr[t->cy]->wrapped = 1;
            t->cx = 0;
            index_down(t);
        } else {
            t->cx = imax(t->cols - 2, 0);
        }
    }
    if (t->insert)
        insert_blanks(t, w);

    int y = t->cy, x = t->cx;
    fix_wide(t, y, x, x + w);
    tcell c = t->pen;
    c.cp = cp;
    c.pad = 0;
    c.attr = (uint16_t)((t->pen.attr & TA_PEN_MASK) | (w == 2 ? TA_WIDE : 0));
    set_cell(t, y, x, c);
    if (w == 2 && x + 1 < t->cols) {
        c.cp = 0;
        c.attr = (uint16_t)((t->pen.attr & TA_PEN_MASK) | TA_CONT);
        set_cell(t, y, x + 1, c);
    }
    t->last_cp = cp;

    if (x + w >= t->cols) {
        t->cx = t->cols - 1;
        t->wrap_pending = t->autowrap;
    } else {
        t->cx = x + w;
    }
}

/* ======================================================================
 * 控制字符
 * ====================================================================== */

static void control(term *t, uint8_t b)
{
    switch (b) {
    case 0x07:                              /* BEL 响铃 */
        if (t->cb.bell)
            t->cb.bell(t->cb.user);
        break;
    case 0x08:                              /* BS 退格：只移动光标，不擦字 */
        if (t->cx > 0)
            t->cx--;
        t->wrap_pending = 0;
        break;
    case 0x09:                              /* HT 制表 */
        tab_forward(t, 1);
        break;
    case 0x0A: case 0x0B: case 0x0C:        /* LF VT FF：都当作换行 */
        /* 注意：程序写的 "\n" 在内核的线路规程里已经被（ONLCR）变成了 "\r\n"，
         * 所以到这里的 LF 只负责“下移一行”，不回行首 */
        index_down(t);
        break;
    case 0x0D:                              /* CR 回到行首 */
        t->cx = 0;
        t->wrap_pending = 0;
        break;
    case 0x0E:                              /* SO：切换到 G1 字符集 */
        t->gl = 1;
        break;
    case 0x0F:                              /* SI：切换回 G0 */
        t->gl = 0;
        break;
    case 0x18: case 0x1A:                   /* CAN SUB：取消正在解析的序列 */
        t->state = S_GROUND;
        break;
    case 0x1B:                              /* ESC：转义序列开始 */
        t->state = S_ESC;
        t->inter = 0;
        break;
    default:
        break;
    }
}

/* ======================================================================
 * 模式、复位
 * ====================================================================== */

static void set_alt(term *t, int on, int clear)
{
    if (on == t->alt_on)
        return;
    t->alt_on = on;
    t->scr = on ? t->alt : t->pri;
    if (on && clear) {
        tcell d = default_cell();
        for (int r = 0; r < t->rows; r++) {
            for (int x = 0; x < t->scr[r]->len; x++)
                t->scr[r]->c[x] = d;
            t->scr[r]->wrapped = 0;
        }
    }
    t->full = 1;
}

static void reset_modes(term *t)
{
    t->autowrap = 1;
    t->cursor_visible = 1;
    t->app_cursor = 0;
    t->insert = 0;
    t->origin = 0;
    t->bracketed_paste = 0;
    t->cursor_shape = 0;
    t->charset[0] = t->charset[1] = 0;
    t->gl = 0;
    memset(&t->pen, 0, sizeof t->pen);
    t->top = 0;
    t->bot = t->rows - 1;
}

/* ESC c：彻底复位（reset 命令就发它）。滚动历史保留 */
static void full_reset(term *t)
{
    set_alt(t, 0, 0);
    reset_modes(t);
    reset_saved(t);
    tab_reset(t);
    erase_rows(t, 0, t->rows - 1);
    t->cx = t->cy = 0;
    t->wrap_pending = 0;
    t->full = 1;
}

static void set_private_mode(term *t, int mode, int on)
{
    switch (mode) {
    case 1:    t->app_cursor = on; break;
    case 6:    t->origin = on; move_to(t, 0, 0); break;
    case 7:    t->autowrap = on; if (!on) t->wrap_pending = 0; break;
    case 25:   t->cursor_visible = on; break;
    case 47:                                         /* 备用屏幕（不保存光标） */
        set_alt(t, on, 0);
        break;
    case 1047:
        if (!on && t->alt_on)
            erase_rows(t, 0, t->rows - 1);
        set_alt(t, on, 0);
        break;
    case 1048:
        if (on)
            save_cursor(t);
        else
            restore_cursor(t);
        break;
    case 1049:
        /* 全屏程序（top、编辑器）启动时进入备用屏幕，退出时回到主屏幕：
         * 主屏幕上原来的内容（命令历史）完好无损地回来了 */
        if (on && !t->alt_on) {
            save_cursor(t);
            set_alt(t, 1, 1);
            t->saved[1] = t->saved[0];
            move_to(t, t->cx, t->cy);
        } else if (!on && t->alt_on) {
            set_alt(t, 0, 0);
            restore_cursor(t);
        }
        break;
    case 2004: t->bracketed_paste = on; break;
    default:   break;      /* 鼠标报告(1000…)、光标闪烁(12)、132 列(3)等：不支持，忽略 */
    }
}

/* ======================================================================
 * SGR：字的颜色和样式（ESC [ … m）
 * ====================================================================== */

static int P(const term *t, int i, int def)
{
    int n = t->any_param ? t->np + 1 : 0;
    return (i < n && t->params[i] > 0) ? t->params[i] : def;
}

/* 解析 38/48 后面的扩展颜色，返回用掉的参数个数（不含 38/48 本身） */
static int ext_color(const term *t, int i, int n, uint32_t *out)
{
    if (i + 1 >= n)
        return 0;
    if (t->sub[i + 1]) {
        /* 冒号形式：38:5:n、38:2:r:g:b 或 38:2:色彩空间:r:g:b */
        int j = i + 1;
        while (j < n && t->sub[j])
            j++;
        int k = j - (i + 1);
        const int *p = &t->params[i + 1];
        if (p[0] == 5 && k >= 2)
            *out = TC_INDEXED | (uint32_t)(p[1] & 0xFF);
        else if (p[0] == 2 && k >= 5)
            *out = TC_RGB | ((uint32_t)imin(p[2], 255) << 16) | ((uint32_t)imin(p[3], 255) << 8) |
                   (uint32_t)imin(p[4], 255);
        else if (p[0] == 2 && k == 4)
            *out = TC_RGB | ((uint32_t)imin(p[1], 255) << 16) | ((uint32_t)imin(p[2], 255) << 8) |
                   (uint32_t)imin(p[3], 255);
        return k;
    }
    /* 分号形式：38;5;n 或 38;2;r;g;b */
    int mode = t->params[i + 1];
    if (mode == 5 && i + 2 < n) {
        *out = TC_INDEXED | (uint32_t)(t->params[i + 2] & 0xFF);
        return 2;
    }
    if (mode == 2 && i + 4 < n) {
        *out = TC_RGB | ((uint32_t)imin(t->params[i + 2], 255) << 16) |
               ((uint32_t)imin(t->params[i + 3], 255) << 8) | (uint32_t)imin(t->params[i + 4], 255);
        return 4;
    }
    return 1;
}

static void sgr(term *t)
{
    int n = t->any_param ? t->np + 1 : 0;
    if (n == 0) {
        memset(&t->pen, 0, sizeof t->pen);
        return;
    }
    for (int i = 0; i < n; i++) {
        int p = t->params[i];
        uint16_t *a = &t->pen.attr;
        switch (p) {
        case 0:  memset(&t->pen, 0, sizeof t->pen); break;
        case 1:  *a |= TA_BOLD; break;
        case 2:  *a |= TA_DIM; break;
        case 3:  *a |= TA_ITALIC; break;
        case 4:
            /* 4:0 = 取消下划线，4:1..5 = 各种下划线（我们都画成一条线） */
            if (i + 1 < n && t->sub[i + 1]) {
                if (t->params[++i] == 0)
                    *a &= (uint16_t)~TA_UNDERLINE;
                else
                    *a |= TA_UNDERLINE;
                while (i + 1 < n && t->sub[i + 1])
                    i++;
            } else {
                *a |= TA_UNDERLINE;
            }
            break;
        case 5: case 6: *a |= TA_BLINK; break;
        case 7:  *a |= TA_REVERSE; break;
        case 8:  *a |= TA_HIDDEN; break;
        case 9:  *a |= TA_STRIKE; break;
        case 21: *a |= TA_UNDERLINE; break;
        case 22: *a &= (uint16_t)~(TA_BOLD | TA_DIM); break;
        case 23: *a &= (uint16_t)~TA_ITALIC; break;
        case 24: *a &= (uint16_t)~TA_UNDERLINE; break;
        case 25: *a &= (uint16_t)~TA_BLINK; break;
        case 27: *a &= (uint16_t)~TA_REVERSE; break;
        case 28: *a &= (uint16_t)~TA_HIDDEN; break;
        case 29: *a &= (uint16_t)~TA_STRIKE; break;
        case 38: i += ext_color(t, i, n, &t->pen.fg); break;
        case 39: t->pen.fg = TC_DEFAULT; break;
        case 48: i += ext_color(t, i, n, &t->pen.bg); break;
        case 49: t->pen.bg = TC_DEFAULT; break;
        default:
            if (p >= 30 && p <= 37)
                t->pen.fg = TC_INDEXED | (uint32_t)(p - 30);
            else if (p >= 40 && p <= 47)
                t->pen.bg = TC_INDEXED | (uint32_t)(p - 40);
            else if (p >= 90 && p <= 97)
                t->pen.fg = TC_INDEXED | (uint32_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107)
                t->pen.bg = TC_INDEXED | (uint32_t)(p - 100 + 8);
            break;
        }
    }
}

/* ======================================================================
 * CSI 序列
 * ====================================================================== */

static void reply(term *t, const char *s)
{
    if (t->cb.reply)
        t->cb.reply(t->cb.user, s, strlen(s));
}

static void csi_dispatch(term *t, uint8_t f)
{
    char buf[64];
    int n = t->any_param ? t->np + 1 : 0;

    if (t->priv == '?') {
        if (f == 'h' || f == 'l')
            for (int i = 0; i < (n ? n : 1); i++)
                set_private_mode(t, t->params[i], f == 'h');
        return;
    }
    if (t->priv == '>') {
        if (f == 'c')
            reply(t, "\033[>0;10;1c");          /* 第二设备属性 */
        return;
    }
    if (t->priv)
        return;
    if (t->inter == ' ' && f == 'q') {           /* DECSCUSR 光标形状 */
        t->cursor_shape = P(t, 0, 0);
        return;
    }
    if (t->inter == '!' && f == 'p') {           /* DECSTR 软复位 */
        int cx = t->cx, cy = t->cy;
        reset_modes(t);
        reset_saved(t);
        t->cx = cx;
        t->cy = cy;
        return;
    }
    if (t->inter)
        return;

    int y = t->cy, x = t->cx;
    switch (f) {
    case '@':                                    /* ICH 插入空格 */
        insert_blanks(t, P(t, 0, 1));
        break;
    case 'A': {                                  /* CUU 上移（不越过滚动区域顶边） */
        int lim = y >= t->top ? t->top : 0;
        t->cy = imax(y - P(t, 0, 1), lim);
        t->wrap_pending = 0;
        break;
    }
    case 'B': case 'e': {                        /* CUD 下移 */
        int lim = y <= t->bot ? t->bot : t->rows - 1;
        t->cy = imin(y + P(t, 0, 1), lim);
        t->wrap_pending = 0;
        break;
    }
    case 'C': case 'a':                          /* CUF 右移 */
        t->cx = imin(x + P(t, 0, 1), t->cols - 1);
        t->wrap_pending = 0;
        break;
    case 'D':                                    /* CUB 左移 */
        t->cx = imax(x - P(t, 0, 1), 0);
        t->wrap_pending = 0;
        break;
    case 'E':                                    /* CNL 下移 n 行并回到行首 */
        t->cy = imin(y + P(t, 0, 1), y <= t->bot ? t->bot : t->rows - 1);
        t->cx = 0;
        t->wrap_pending = 0;
        break;
    case 'F':                                    /* CPL 上移 n 行并回到行首 */
        t->cy = imax(y - P(t, 0, 1), y >= t->top ? t->top : 0);
        t->cx = 0;
        t->wrap_pending = 0;
        break;
    case 'G': case '`':                          /* CHA 光标到第 n 列 */
        t->cx = iclamp(P(t, 0, 1) - 1, 0, t->cols - 1);
        t->wrap_pending = 0;
        break;
    case 'H': case 'f':                          /* CUP 光标到第 r 行第 c 列（从 1 开始） */
        move_to(t, P(t, 1, 1) - 1, P(t, 0, 1) - 1);
        break;
    case 'I':                                    /* CHT 前进 n 个制表位 */
        tab_forward(t, P(t, 0, 1));
        break;
    case 'J':                                    /* ED 擦除屏幕 */
        switch (P(t, 0, 0)) {
        case 0:
            erase_cells(t, y, x, t->cols);
            erase_rows(t, y + 1, t->rows - 1);
            break;
        case 1:
            erase_rows(t, 0, y - 1);
            erase_cells(t, y, 0, x + 1);
            break;
        case 2:
            erase_rows(t, 0, t->rows - 1);
            break;
        case 3:                                  /* 连滚动历史一起清掉（clear 命令） */
            if (!t->alt_on && t->sb_count) {
                sb_clear(t);
                t->full = 1;
            }
            break;
        }
        break;
    case 'K':                                    /* EL 擦除行 */
        switch (P(t, 0, 0)) {
        case 0: erase_cells(t, y, x, t->cols); break;
        case 1: erase_cells(t, y, 0, x + 1); break;
        case 2: erase_cells(t, y, 0, t->cols); break;
        }
        break;
    case 'L':                                    /* IL 插入行（光标在滚动区域内才有效） */
        if (y >= t->top && y <= t->bot) {
            scroll_down(t, y, t->bot, P(t, 0, 1));
            t->cx = 0;
            t->wrap_pending = 0;
        }
        break;
    case 'M':                                    /* DL 删除行 */
        if (y >= t->top && y <= t->bot) {
            scroll_up(t, y, t->bot, P(t, 0, 1), 0);   /* 删掉的行不进历史区 */
            t->cx = 0;
            t->wrap_pending = 0;
        }
        break;
    case 'P':                                    /* DCH 删除字符 */
        delete_chars(t, P(t, 0, 1));
        break;
    case 'S':                                    /* SU 向上滚 */
        scroll_up(t, t->top, t->bot, P(t, 0, 1), 1);
        break;
    case 'T':                                    /* SD 向下滚 */
        if (n <= 1)
            scroll_down(t, t->top, t->bot, P(t, 0, 1));
        break;
    case 'X':                                    /* ECH 擦除 n 个字符（光标不动） */
        erase_cells(t, y, x, x + P(t, 0, 1));
        break;
    case 'Z':                                    /* CBT 后退 n 个制表位 */
        tab_backward(t, P(t, 0, 1));
        break;
    case 'b':                                    /* REP 重复上一个字 */
        if (t->last_cp) {
            int k = imin(P(t, 0, 1), t->cols * t->rows);
            uint32_t cp = t->last_cp;
            int cs = t->charset[t->gl];
            t->charset[t->gl] = 0;               /* last_cp 已经是转换后的字 */
            while (k-- > 0)
                put_char(t, cp);
            t->charset[t->gl] = cs;
        }
        break;
    case 'c':                                    /* DA 设备属性：自称 VT220 + ANSI 颜色 */
        if (P(t, 0, 0) == 0)
            reply(t, "\033[?62;22c");
        break;
    case 'd':                                    /* VPA 光标到第 n 行 */
        move_to(t, t->cx, P(t, 0, 1) - 1);
        break;
    case 'g':                                    /* TBC 清除制表位 */
        if (P(t, 0, 0) == 0)
            t->tabs[x] = 0;
        else if (P(t, 0, 0) == 3)
            memset(t->tabs, 0, (size_t)t->cols);
        break;
    case 'h': case 'l':                          /* SM/RM：标准模式只支持 4（插入） */
        for (int i = 0; i < n; i++)
            if (t->params[i] == 4)
                t->insert = f == 'h';
        break;
    case 'm':
        sgr(t);
        break;
    case 'n':                                    /* DSR 状态报告 */
        if (P(t, 0, 0) == 5) {
            reply(t, "\033[0n");
        } else if (P(t, 0, 0) == 6) {            /* 光标位置报告 */
            snprintf(buf, sizeof buf, "\033[%d;%dR", (t->origin ? y - t->top : y) + 1, x + 1);
            reply(t, buf);
        }
        break;
    case 'r': {                                  /* DECSTBM 设置滚动区域 */
        int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
        bot = imin(bot, t->rows - 1);
        if (top < bot) {
            t->top = top;
            t->bot = bot;
            move_to(t, 0, 0);
        }
        break;
    }
    case 's':                                    /* SCOSC 保存光标 */
        if (n == 0)
            save_cursor(t);
        break;
    case 'u':                                    /* SCORC 恢复光标 */
        if (n == 0)
            restore_cursor(t);
        break;
    case 't':                                    /* 窗口操作：只回答“文字区多少行多少列” */
        if (P(t, 0, 0) == 18) {
            snprintf(buf, sizeof buf, "\033[8;%d;%dt", t->rows, t->cols);
            reply(t, buf);
        }
        break;
    default:
        break;
    }
}

static void csi_byte(term *t, uint8_t b)
{
    if (b >= '0' && b <= '9') {
        int *p = &t->params[t->np];
        if (*p < 100000)
            *p = *p * 10 + (b - '0');
        t->any_param = 1;
    } else if (b == ';' || b == ':') {
        if (t->np < TERM_MAX_PARAMS - 1) {
            t->np++;
            t->sub[t->np] = b == ':';
        }
        t->any_param = 1;
    } else if (b >= '<' && b <= '?') {
        if (!t->any_param && !t->priv)
            t->priv = (char)b;
        else
            t->state = S_CSI_IGNORE;
    } else if (b >= 0x20 && b <= 0x2F) {
        t->inter = (char)b;
    } else if (b >= 0x40 && b <= 0x7E) {
        t->state = S_GROUND;
        csi_dispatch(t, b);
    } else {
        t->state = S_GROUND;
    }
}

/* ======================================================================
 * ESC 序列、OSC 字符串
 * ====================================================================== */

static void start_csi(term *t)
{
    t->state = S_CSI;
    memset(t->params, 0, sizeof t->params);
    memset(t->sub, 0, sizeof t->sub);
    t->np = 0;
    t->any_param = 0;
    t->priv = 0;
    t->inter = 0;
}

static void start_str(term *t, int kind)
{
    t->state = S_STR;
    t->str_kind = kind;
    t->str_len = 0;
    t->str_esc = 0;
}

/* OSC 字符串结束：目前只处理 0（图标名+标题）和 2（标题） */
static void str_finish(term *t)
{
    if (t->str_kind != STR_OSC)
        return;
    t->str[t->str_len] = 0;
    char *semi = strchr(t->str, ';');
    if (!semi)
        return;
    *semi = 0;
    int ps = atoi(t->str);
    if ((ps == 0 || ps == 2) && t->cb.title) {
        char *s = semi + 1;
        for (char *p = s; *p; p++)
            if ((unsigned char)*p < 0x20)
                *p = ' ';
        t->cb.title(t->cb.user, s);
    }
}

static void esc_dispatch(term *t, uint8_t b)
{
    t->state = S_GROUND;
    switch (b) {
    case '[': start_csi(t); break;
    case ']': start_str(t, STR_OSC); break;
    case 'P': case 'X': case '^': case '_': start_str(t, STR_IGNORE); break;
    case '(': case ')': case '*': case '+': case '#': case '%': case ' ':
        t->inter = (char)b;
        t->state = S_ESC_INTER;
        break;
    case '7': save_cursor(t); break;                         /* DECSC */
    case '8': restore_cursor(t); break;                      /* DECRC */
    case 'D': index_down(t); t->wrap_pending = 0; break;     /* IND */
    case 'E': t->cx = 0; index_down(t); t->wrap_pending = 0; break;   /* NEL */
    case 'M': index_up(t); t->wrap_pending = 0; break;       /* RI 反向换行 */
    case 'H': t->tabs[t->cx] = 1; break;                     /* HTS 设置制表位 */
    case 'c': full_reset(t); break;                          /* RIS */
    case 'Z': reply(t, "\033[?62;22c"); break;
    default: break;                                          /* ESC = / ESC > / ESC \ 等：忽略 */
    }
}

static void esc_inter(term *t, uint8_t b)
{
    t->state = S_GROUND;
    switch (t->inter) {
    case '(': t->charset[0] = b == '0'; break;
    case ')': t->charset[1] = b == '0'; break;
    case '#':
        if (b == '8') {                                      /* DECALN：满屏 E（屏幕校准图案） */
            tcell c = default_cell();
            c.cp = 'E';
            for (int y = 0; y < t->rows; y++)
                for (int x = 0; x < t->cols; x++)
                    set_cell(t, y, x, c);
            move_to(t, 0, 0);
        }
        break;
    default: break;
    }
}

/* ======================================================================
 * 字节入口
 * ====================================================================== */

/* 普通文字状态下的一个字节：UTF-8 解码（可能跨越两次 read()） */
static void ground_byte(term *t, uint8_t b)
{
    if (t->u8_need) {
        if ((b & 0xC0) == 0x80) {
            t->u8_cp = (t->u8_cp << 6) | (b & 0x3Fu);
            if (--t->u8_need == 0) {
                uint32_t cp = t->u8_cp;
                if (cp < t->u8_min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    cp = 0xFFFD;
                put_char(t, cp);
            }
            return;
        }
        /* 序列断了：先输出一个替换符号，这个字节重新处理 */
        t->u8_need = 0;
        put_char(t, 0xFFFD);
    }
    if (b < 0x80) {
        put_char(t, b);
    } else if ((b & 0xE0) == 0xC0) {
        t->u8_cp = b & 0x1Fu;
        t->u8_need = 1;
        t->u8_min = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
        t->u8_cp = b & 0x0Fu;
        t->u8_need = 2;
        t->u8_min = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
        t->u8_cp = b & 0x07u;
        t->u8_need = 3;
        t->u8_min = 0x10000;
    } else {
        put_char(t, 0xFFFD);
    }
}

static void feed(term *t, uint8_t b)
{
    if (t->state == S_STR) {
        if (t->str_esc) {
            /* 字符串以 ESC \（ST）结束；ESC 后面跟别的字符也视为结束，再按 ESC 序列处理 */
            t->str_esc = 0;
            str_finish(t);
            t->state = S_GROUND;
            if (b == '\\')
                return;
            t->state = S_ESC;
            t->inter = 0;
        } else if (b == 0x07) {
            str_finish(t);
            t->state = S_GROUND;
            return;
        } else if (b == 0x1B) {
            t->str_esc = 1;
            return;
        } else if (b == 0x18 || b == 0x1A) {
            t->state = S_GROUND;
            return;
        } else {
            if (t->str_len < TERM_STR_MAX - 1)
                t->str[t->str_len++] = (char)b;
            return;
        }
    }
    if (b < 0x20) {
        if (t->u8_need) {
            t->u8_need = 0;
            put_char(t, 0xFFFD);
        }
        control(t, b);
        return;
    }
    switch (t->state) {
    case S_GROUND:
        if (b != 0x7F)
            ground_byte(t, b);
        break;
    case S_ESC:
        if (b < 0x80)
            esc_dispatch(t, b);
        else
            t->state = S_GROUND;
        break;
    case S_ESC_INTER:
        esc_inter(t, b);
        break;
    case S_CSI:
        csi_byte(t, b);
        break;
    case S_CSI_IGNORE:
        if (b >= 0x40 && b <= 0x7E)
            t->state = S_GROUND;
        break;
    default:
        t->state = S_GROUND;
        break;
    }
}

void term_write(term *t, const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        /* 快速路径：普通文字状态下的可打印 ASCII（终端输出里最常见的字节）直接写格子 */
        if (t->state == S_GROUND && !t->u8_need && b >= 0x20 && b < 0x7F) {
            put_char(t, b);
            continue;
        }
        feed(t, b);
    }
}

/* ======================================================================
 * 创建、销毁、改变尺寸
 * ====================================================================== */

static tline **buf_new(int cols, int rows)
{
    tline **b = calloc((size_t)rows, sizeof *b);
    if (!b)
        return NULL;
    for (int r = 0; r < rows; r++) {
        b[r] = line_new(cols, default_cell());
        if (!b[r]) {
            while (r-- > 0)
                free(b[r]);
            free(b);
            return NULL;
        }
    }
    return b;
}

static void buf_free(tline **b, int rows)
{
    if (!b)
        return;
    for (int r = 0; r < rows; r++)
        free(b[r]);
    free(b);
}

term *term_new(int cols, int rows, int scrollback, const term_callbacks *cb)
{
    if (cols < 2 || rows < 1)
        return NULL;
    term *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->cols = cols;
    t->rows = rows;
    t->pri = buf_new(cols, rows);
    t->alt = buf_new(cols, rows);
    t->sb_cap = scrollback > 0 ? scrollback : 0;
    t->sb = t->sb_cap ? calloc((size_t)t->sb_cap, sizeof *t->sb) : NULL;
    t->tabs = malloc((size_t)cols);
    t->dlo = malloc((size_t)rows * sizeof *t->dlo);
    t->dhi = malloc((size_t)rows * sizeof *t->dhi);
    if (!t->pri || !t->alt || (t->sb_cap && !t->sb) || !t->tabs || !t->dlo || !t->dhi) {
        term_free(t);
        return NULL;
    }
    t->scr = t->pri;
    if (cb)
        t->cb = *cb;
    reset_modes(t);
    tab_reset(t);
    term_clear_dirty(t);
    t->full = 1;
    return t;
}

void term_free(term *t)
{
    if (!t)
        return;
    buf_free(t->pri, t->rows);
    buf_free(t->alt, t->rows);
    if (t->sb) {
        for (int i = 0; i < t->sb_count; i++)
            free(sb_get(t, i));
        free(t->sb);
    }
    free(t->tabs);
    free(t->dlo);
    free(t->dhi);
    free(t);
}

/* 把一块屏幕改成 cols×rows。*cy 是这块屏幕上的光标行（会被调整）。
 * 主屏幕：变矮时，光标上面多出来的行推进历史区（光标所在行保持可见）；
 *         变高且光标在最后一行时，从历史区拉回几行（就像把纸往下拉）。 */
static tline **buf_resize(term *t, tline **old, int oldrows, int cols, int rows, int *cy, int is_pri)
{
    tline **nb = calloc((size_t)rows, sizeof *nb);
    if (!nb)
        return NULL;
    int drop = *cy >= rows ? *cy - rows + 1 : 0;
    int pull = 0;
    if (is_pri && rows > oldrows && *cy == oldrows - 1)
        pull = imin(t->sb_count, rows - oldrows);
    int k = 0;
    for (int i = t->sb_count - pull; i < t->sb_count; i++)
        nb[k++] = line_resize(sb_get(t, i), cols);
    t->sb_count -= pull;
    for (int i = 0; i < drop; i++) {
        if (is_pri)
            sb_push(t, old[i]);
        else
            free(old[i]);
    }
    int i = drop;
    for (; i < oldrows && k < rows; i++)
        nb[k++] = line_resize(old[i], cols);
    for (; i < oldrows; i++)
        free(old[i]);
    for (; k < rows; k++) {
        nb[k] = line_new(cols, default_cell());
        if (!nb[k]) {
            while (k-- > 0)
                free(nb[k]);
            free(nb);
            return NULL;   /* 极端情况（内存耗尽），调用者会退出 */
        }
    }
    *cy = *cy - drop + pull;
    free(old);
    return nb;
}

int term_resize(term *t, int cols, int rows)
{
    if (cols < 2 || rows < 1)
        return -1;
    if (cols == t->cols && rows == t->rows)
        return 0;
    uint8_t *tabs = malloc((size_t)cols);
    int *dlo = malloc((size_t)rows * sizeof *dlo);
    int *dhi = malloc((size_t)rows * sizeof *dhi);
    if (!tabs || !dlo || !dhi) {
        free(tabs);
        free(dlo);
        free(dhi);
        return -1;
    }
    /* 主屏幕的光标：正在用备用屏幕时，是进入备用屏幕前保存的那个 */
    int pcy = t->alt_on ? t->saved[0].cy : t->cy;
    int acy = t->alt_on ? t->cy : t->saved[1].cy;
    int oldrows = t->rows;
    tline **pri = buf_resize(t, t->pri, oldrows, cols, rows, &pcy, 1);
    tline **alt = pri ? buf_resize(t, t->alt, oldrows, cols, rows, &acy, 0) : NULL;
    if (!pri || !alt) {
        free(tabs);
        free(dlo);
        free(dhi);
        return -1;
    }
    t->pri = pri;
    t->alt = alt;
    t->scr = t->alt_on ? alt : pri;

    for (int x = 0; x < cols; x++)
        tabs[x] = x < t->cols ? t->tabs[x] : ((x % 8) == 0 && x > 0);
    free(t->tabs);
    t->tabs = tabs;
    free(t->dlo);
    free(t->dhi);
    t->dlo = dlo;
    t->dhi = dhi;

    t->cols = cols;
    t->rows = rows;
    if (t->alt_on) {
        t->cy = acy;
        t->saved[0].cy = pcy;
    } else {
        t->cy = pcy;
        t->saved[1].cy = acy;
    }
    t->cx = iclamp(t->cx, 0, cols - 1);
    t->cy = iclamp(t->cy, 0, rows - 1);
    for (int i = 0; i < 2; i++) {
        t->saved[i].cx = iclamp(t->saved[i].cx, 0, cols - 1);
        t->saved[i].cy = iclamp(t->saved[i].cy, 0, rows - 1);
    }
    t->wrap_pending = 0;
    t->top = 0;
    t->bot = rows - 1;
    term_clear_dirty(t);
    t->full = 1;
    return 0;
}
