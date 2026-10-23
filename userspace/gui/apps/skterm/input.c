/*
 * input.c — 键盘、鼠标、剪贴板
 *
 * 键盘：sketchd 发来的是“按键事件”（键码 + 修饰键 + 翻译好的字符），而 shell 想要的是
 * 字节流——和当年的硬件终端从串口线上发出的字节一模一样：
 *   - 普通字符 → UTF-8 字节；回车 → \r（线路规程的 ICRNL 再把它变成程序读到的 \n）；
 *     退格 → 0x7F（DEL，线路规程把它当作“删一个字”）；
 *   - Ctrl+字母 → 控制字符：Ctrl+C = 0x03，线路规程看到它就给前台进程组发 SIGINT——
 *     终端程序自己并不“杀”进程，它只是发出一个字节；
 *   - 方向键、F1… → 转义序列，例如 ↑ = ESC [ A（程序用 ESC[?1h 打开“应用光标键模式”后是 ESC O A）；
 *   - Alt+键 → 先发一个 ESC，再发这个键。
 * Ctrl+Shift+C / Ctrl+Shift+V 是终端自己的复制粘贴（Ctrl+C 已经被 SIGINT 占用了），
 * Shift+PageUp/PageDown 翻看滚动历史。
 *
 * 鼠标：拖动选择文字（荧光笔高亮），双击选一个词，右键：有选区时复制、没有时粘贴；
 * 滚轮翻看历史；右边的滚动条可以拖动。
 */
#define _GNU_SOURCE
#include "skterm.h"

#include <skutil.h>

#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WHEEL_LINES 3
#define WQ_MAX      (1 << 20)   /* 最多排队 1MB 还没写进 pty 的输入 */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ======================================================================
 * 写入 pty
 * ====================================================================== */

/* 还没写完的输入（粘贴一大段时，内核的终端输入缓冲区可能装不下）继续写 */
void skt_flush_input(skterm *s)
{
    while (s->wq_len > 0 && s->master >= 0) {
        ssize_t n = write(s->master, s->wq, s->wq_len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN)
                s->wq_len = 0;          /* 对面已经关了（EIO）：丢弃 */
            break;
        }
        memmove(s->wq, s->wq + n, s->wq_len - (size_t)n);
        s->wq_len -= (size_t)n;
    }
}

void skt_send(skterm *s, const void *data, size_t n)
{
    if (s->master < 0 || n == 0)
        return;
    const char *p = data;
    if (s->wq_len == 0) {
        while (n > 0) {
            ssize_t w = write(s->master, p, n);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                if (errno != EAGAIN)
                    return;
                break;
            }
            p += w;
            n -= (size_t)w;
        }
        if (n == 0)
            return;
    }
    /* 写不进去的部分排队，等 pty 可写（POLLOUT）时再写 */
    if (s->wq_len + n > WQ_MAX)
        return;
    if (s->wq_len + n > s->wq_cap) {
        size_t cap = s->wq_cap ? s->wq_cap * 2 : 4096;
        while (cap < s->wq_len + n)
            cap *= 2;
        char *q = realloc(s->wq, cap);
        if (!q)
            return;
        s->wq = q;
        s->wq_cap = cap;
    }
    memcpy(s->wq + s->wq_len, p, n);
    s->wq_len += n;
}

/* ======================================================================
 * 视图（翻看历史）
 * ====================================================================== */

void skt_set_view(skterm *s, int view)
{
    int maxv = s->t->alt_on ? 0 : s->t->sb_count;
    view = view < 0 ? 0 : view > maxv ? maxv : view;
    s->vshift += view - s->view;     /* 内容整体移动了这么多行：draw.c 搬像素，只画新露出来的行 */
    s->view = view;
}

void skt_scroll_view(skterm *s, int delta)
{
    skt_set_view(s, s->view + delta);
}

/* ======================================================================
 * 选区
 * ====================================================================== */

/* 端点比较：先比行，再比列 */
static int pt_cmp(long long l1, int c1, long long l2, int c2)
{
    if (l1 != l2)
        return l1 < l2 ? -1 : 1;
    return (c1 > c2) - (c1 < c2);
}

static void sel_range(const skterm *s, long long *l0, int *c0, long long *l1, int *c1)
{
    if (pt_cmp(s->sa_l, s->sa_c, s->sh_l, s->sh_c) <= 0) {
        *l0 = s->sa_l; *c0 = s->sa_c; *l1 = s->sh_l; *c1 = s->sh_c;
    } else {
        *l0 = s->sh_l; *c0 = s->sh_c; *l1 = s->sa_l; *c1 = s->sa_c;
    }
}

/* 一行文字的实际长度（行尾的空白不算） */
static int line_text_len(const tline *l)
{
    if (!l)
        return 0;
    int n = l->len;
    while (n > 0 && l->c[n - 1].cp == 0 && !(l->c[n - 1].attr & TA_CONT))
        n--;
    return n;
}

int skt_sel_cols(const skterm *s, long long L, int *a, int *b)
{
    if (!s->sel_on)
        return 0;
    long long l0, l1;
    int c0, c1;
    sel_range(s, &l0, &c0, &l1, &c1);
    if (L < l0 || L > l1)
        return 0;
    const tline *ln = term_line(s->t, L);
    int cols = s->t->cols;
    int x0 = L == l0 ? c0 : 0;
    /* 中间的行涂到文字结尾再多一格（表示“换行也选上了”），不涂满整行 */
    int x1 = L == l1 ? c1 : imin(imax(line_text_len(ln), x0) + 1, cols);
    /* 不把宽字符切成两半 */
    if (ln && x0 > 0 && x0 < ln->len && (ln->c[x0].attr & TA_CONT))
        x0--;
    if (ln && x1 > 0 && x1 < ln->len && (ln->c[x1].attr & TA_CONT))
        x1++;
    x1 = imin(x1, cols);
    if (x0 >= x1)
        return 0;
    *a = x0;
    *b = x1;
    return 1;
}

/* 选中的文字（malloc，UTF-8）。行尾空白去掉；自动折行接起来的两行之间不加换行 */
char *skt_sel_text(const skterm *s, int *len)
{
    *len = 0;
    if (!s->sel_on)
        return NULL;
    long long l0, l1;
    int c0, c1;
    sel_range(s, &l0, &c0, &l1, &c1);
    size_t cap = 256, n = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    for (long long L = l0; L <= l1; L++) {
        int a = 0, b = 0;
        const tline *ln = term_line(s->t, L);
        size_t line_start = n;
        if (ln && skt_sel_cols(s, L, &a, &b)) {
            for (int x = a; x < b && x < ln->len; x++) {
                const tcell *c = &ln->c[x];
                if (c->attr & TA_CONT)
                    continue;
                if (n + 8 > cap) {
                    char *q = realloc(out, cap *= 2);
                    if (!q) {
                        free(out);
                        return NULL;
                    }
                    out = q;
                }
                n += (size_t)sk_utf8_encode(c->cp ? c->cp : ' ', out + n);
            }
        }
        while (n > line_start && out[n - 1] == ' ')
            n--;
        if (L < l1 && !(ln && ln->wrapped)) {
            if (n + 2 > cap) {
                char *q = realloc(out, cap *= 2);
                if (!q) {
                    free(out);
                    return NULL;
                }
                out = q;
            }
            out[n++] = '\n';
        }
    }
    out[n] = 0;
    *len = (int)n;
    return out;
}

/* 选区覆盖的显示行标脏（选区变化时，新旧两个选区各调用一次） */
static void sel_mark_current(skterm *s)
{
    if (!s->sel_on)
        return;
    long long l0, l1;
    int c0, c1;
    sel_range(s, &l0, &c0, &l1, &c1);
    long long top = skt_view_line(s, 0), rows = s->t->rows;
    long long r0 = l0 - top, r1 = l1 - top;
    if (r1 < 0 || r0 >= rows)
        return;                                  /* 不在画面上 */
    skt_mark_rows(s, r0 < 0 ? 0 : (int)r0, r1 >= rows ? (int)rows - 1 : (int)r1);
}

void skt_mark_selection(skterm *s)
{
    sel_mark_current(s);
}

void skt_clear_selection(skterm *s)
{
    if (!s->sel_on)
        return;
    sel_mark_current(s);
    s->sel_on = 0;
    s->sel_drag = 0;
}

static void copy_selection(skterm *s)
{
    int len;
    char *text = skt_sel_text(s, &len);
    if (text && len > 0)
        sk_clipboard_set(s->app, text, len);
    free(text);
}

/* 窗口坐标 → (绝对行号, 列边界)。列边界取离鼠标最近的格子边线，选字更自然 */
static void point_at(const skterm *s, int x, int y, long long *L, int *c)
{
    int r = (y - s->oy) / s->lh;
    if (y < s->oy)
        r = -1 - (s->oy - y - 1) / s->lh;
    r = imax(imin(r, s->t->rows - 1), 0);
    int col = (x - s->ox + s->cw / 2) / s->cw;
    if (x < s->ox)
        col = 0;
    *c = imax(imin(col, s->t->cols), 0);
    *L = skt_view_line(s, r);
}

static int is_word_char(uint32_t cp)
{
    if (cp == 0 || cp == ' ' || cp == '\t')
        return 0;
    return !strchr("\"'`()[]{}<>|;,", (int)(cp < 128 ? cp : 'a'));
}

/* 双击：选中鼠标下面的一个“词”（连续的非空白、非括号字符） */
static void select_word(skterm *s, long long L, int x)
{
    const tline *ln = term_line(s->t, L);
    if (!ln || x >= ln->len)
        return;
    if (ln->c[x].attr & TA_CONT && x > 0)
        x--;
    if (!is_word_char(ln->c[x].cp))
        return;
    int a = x, b = x + 1;
    while (a > 0 && ((ln->c[a - 1].attr & TA_CONT) || is_word_char(ln->c[a - 1].cp)))
        a--;
    while (b < ln->len && ((ln->c[b].attr & TA_CONT) || is_word_char(ln->c[b].cp)))
        b++;
    skt_clear_selection(s);
    s->sel_on = 1;
    s->sa_l = s->sh_l = L;
    s->sa_c = a;
    s->sh_c = b;
    sel_mark_current(s);
}

/* ======================================================================
 * 粘贴
 * ====================================================================== */

void skt_paste(skterm *s, const char *text, int len)
{
    if (!text || len <= 0 || s->finished)
        return;
    char *buf = malloc((size_t)len + 16);
    if (!buf)
        return;
    size_t n = 0;
    if (s->t->bracketed_paste) {
        memcpy(buf, "\033[200~", 6);
        n = 6;
    }
    /* 换行一律变成 \r（就像在键盘上按回车）；括号粘贴模式下去掉 ESC，
     * 免得粘贴的内容里藏着一个 ESC[201~ 提前“结束粘贴” */
    for (int i = 0; i < len; i++) {
        char ch = text[i];
        if (ch == '\r' && i + 1 < len && text[i + 1] == '\n')
            continue;
        if (ch == '\n')
            ch = '\r';
        if (ch == 0x1B && s->t->bracketed_paste)
            continue;
        buf[n++] = ch;
    }
    if (s->t->bracketed_paste) {
        memcpy(buf + n, "\033[201~", 6);
        n += 6;
    }
    skt_clear_selection(s);
    skt_set_view(s, 0);
    skt_send(s, buf, n);
    free(buf);
}

/* ======================================================================
 * 键盘
 * ====================================================================== */

static int is_modifier_key(uint32_t kc)
{
    switch (kc) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
    case KEY_LEFTALT: case KEY_RIGHTALT: case KEY_LEFTMETA: case KEY_RIGHTMETA:
    case KEY_CAPSLOCK: case KEY_NUMLOCK: case KEY_SCROLLLOCK:
        return 1;
    default:
        return 0;
    }
}

/* Ctrl+字符 → 控制字符（与 xterm 相同）；没有对应的返回 -1 */
static int ctrl_code(uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z')
        return (int)(cp - 'a' + 1);
    if (cp >= 'A' && cp <= 'Z')
        return (int)(cp - 'A' + 1);
    switch (cp) {
    case ' ': case '@': case '2':            return 0x00;
    case '[': case '3':                      return 0x1B;
    case '\\': case '4':                     return 0x1C;
    case ']': case '5':                      return 0x1D;
    case '^': case '6': case '~': case '`':  return 0x1E;
    case '_': case '-': case '7': case '/':  return 0x1F;
    case '8': case '?':                      return 0x7F;
    default:                                 return -1;
    }
}

/* 功能键的 xterm 序列。mod 是 xterm 的修饰键编号（1 = 没有修饰键）。返回字节数 */
static int special_key(const skterm *s, uint32_t kc, int mod, char *out, size_t n)
{
    const char *ss3 = s->t->app_cursor ? "\033O" : "\033[";
    char fin = 0;
    int num = 0;
    switch (kc) {
    case KEY_UP:    fin = 'A'; break;
    case KEY_DOWN:  fin = 'B'; break;
    case KEY_RIGHT: fin = 'C'; break;
    case KEY_LEFT:  fin = 'D'; break;
    case KEY_HOME:  fin = 'H'; break;
    case KEY_END:   fin = 'F'; break;
    case KEY_F1:    fin = 'P'; ss3 = "\033O"; break;
    case KEY_F2:    fin = 'Q'; ss3 = "\033O"; break;
    case KEY_F3:    fin = 'R'; ss3 = "\033O"; break;
    case KEY_F4:    fin = 'S'; ss3 = "\033O"; break;
    case KEY_INSERT:   num = 2; break;
    case KEY_DELETE:   num = 3; break;
    case KEY_PAGEUP:   num = 5; break;
    case KEY_PAGEDOWN: num = 6; break;
    case KEY_F5:  num = 15; break;
    case KEY_F6:  num = 17; break;
    case KEY_F7:  num = 18; break;
    case KEY_F8:  num = 19; break;
    case KEY_F9:  num = 20; break;
    case KEY_F10: num = 21; break;
    case KEY_F11: num = 23; break;
    case KEY_F12: num = 24; break;
    default: return 0;
    }
    int k;
    if (fin) {
        if (mod > 1)
            k = snprintf(out, n, "\033[1;%d%c", mod, fin);   /* 例如 Ctrl+→ = ESC[1;5C */
        else
            k = snprintf(out, n, "%s%c", ss3, fin);
    } else {
        if (mod > 1)
            k = snprintf(out, n, "\033[%d;%d~", num, mod);
        else
            k = snprintf(out, n, "\033[%d~", num);
    }
    return k > 0 && (size_t)k < n ? k : 0;
}

int skt_key(skterm *s, const sk_event *e)
{
    uint32_t kc = e->key.keycode, mods = e->key.modifiers, cp = e->key.codepoint;
    if (e->key.state == 0 || is_modifier_key(kc))
        return 1;                                /* 松开、单独的修饰键：不管 */
    if (s->finished) {
        skt_close(s);                            /* “按任意键关闭窗口” */
        return 1;
    }
    int shift = (mods & SK_MOD_SHIFT) != 0, ctrl = (mods & SK_MOD_CTRL) != 0;
    int alt = (mods & SK_MOD_ALT) != 0;
    if (mods & SK_MOD_SUPER)
        return 0;

    /* ---- 终端自己的快捷键 ---- */
    if ((ctrl && shift && kc == KEY_C) || (ctrl && !shift && kc == KEY_INSERT)) {
        copy_selection(s);
        return 1;
    }
    if ((ctrl && shift && kc == KEY_V) || (shift && !ctrl && kc == KEY_INSERT)) {
        sk_clipboard_request(s->app, s->win);    /* 内容随后作为 SK_EVENT_CLIPBOARD 到达 */
        return 1;
    }
    if (shift && !ctrl && (kc == KEY_PAGEUP || kc == KEY_PAGEDOWN)) {
        int page = imax(s->t->rows - 1, 1);
        skt_scroll_view(s, kc == KEY_PAGEUP ? page : -page);
        return 1;
    }

    /* ---- 翻译成字节 ---- */
    char buf[32];
    int n = 0;
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    switch (kc) {
    case KEY_ENTER: case KEY_KPENTER:
        n = alt ? 2 : 1;
        memcpy(buf, alt ? "\033\r" : "\r", (size_t)n);
        break;
    case KEY_BACKSPACE:
        /* 退格发 DEL(0x7F)：这是 Linux 终端的约定（termios 的 VERASE） */
        if (ctrl)
            buf[n++] = 0x08;
        else {
            if (alt)
                buf[n++] = 0x1B;
            buf[n++] = 0x7F;
        }
        break;
    case KEY_TAB:
        if (shift)
            n = (int)strlen(strcpy(buf, "\033[Z"));
        else
            buf[n++] = '\t';
        break;
    case KEY_ESC:
        buf[n++] = 0x1B;
        break;
    default:
        n = special_key(s, kc, mod, buf, sizeof buf);
        if (n > 0)
            break;
        if (!cp)
            return 0;
        if (ctrl) {
            int c = ctrl_code(cp);
            if (c >= 0) {
                if (alt)
                    buf[n++] = 0x1B;
                buf[n++] = (char)c;
                break;
            }
        }
        if (cp < 0x20 || cp == 0x7F) {
            buf[n++] = (char)cp;
            break;
        }
        if (alt)
            buf[n++] = 0x1B;                     /* Alt+x = ESC x（“meta 键发 ESC 前缀”） */
        n += sk_utf8_encode(cp, buf + n);
        break;
    }
    if (n <= 0)
        return 0;
    /* 打字时回到最新的屏幕，选区取消 */
    skt_set_view(s, 0);
    skt_clear_selection(s);
    skt_send(s, buf, (size_t)n);
    return 1;
}

/* ======================================================================
 * 鼠标
 * ====================================================================== */

static int in_scrollbar(const skterm *s, int x, int y)
{
    return x >= s->sbar.x - 3 && y >= s->sbar.y && y < s->sbar.y + s->sbar.h;
}

/* 滚动条拖动：让滑块中心跟着鼠标（滑块尺寸的计算与 sk_draw_scrollbar 一致） */
static void sbar_drag_to(skterm *s, int y)
{
    const term *t = s->t;
    int hist = t->alt_on ? 0 : t->sb_count;
    if (hist <= 0)
        return;
    float len = (float)s->sbar.h - 4.0f;
    float tl = len * (float)t->rows / (float)(hist + t->rows);
    if (tl < 22.0f)
        tl = len < 22.0f ? len : 22.0f;
    float span = len - tl;
    float pos = (float)(y - s->sbar.y - 2) - tl * 0.5f;
    int off = span > 0.5f ? (int)(pos / span * (float)hist + 0.5f) : hist;
    off = imax(imin(off, hist), 0);
    skt_set_view(s, hist - off);
}

int skt_mouse(skterm *s, const sk_event *e)
{
    int x = e->mouse.x, y = e->mouse.y;
    int sbar = in_scrollbar(s, x, y);

    switch (e->mouse.kind) {
    case SK_MOUSE_LEAVE:
        s->sbar_hover = 0;
        return 1;
    case SK_MOUSE_WHEEL:
        if (!s->t->alt_on)
            skt_scroll_view(s, e->mouse.wheel * WHEEL_LINES);
        return 1;
    case SK_MOUSE_DOWN:
        if (e->mouse.button == SK_BUTTON_LEFT) {
            if (sbar) {
                s->sbar_drag = 1;
                sbar_drag_to(s, y);
                return 1;
            }
            long long L;
            int c;
            point_at(s, x, y, &L, &c);
            if (e->mouse.clicks >= 2) {
                int r = imax(imin((y - s->oy) / s->lh, s->t->rows - 1), 0);
                int cx = imax(imin((x - s->ox) / s->cw, s->t->cols - 1), 0);
                select_word(s, skt_view_line(s, r), cx);
                s->sel_drag = 0;
                return 1;
            }
            if ((e->mouse.modifiers & SK_MOD_SHIFT) && s->sel_on) {
                sel_mark_current(s);             /* Shift+单击：把选区延伸到这里 */
                s->sh_l = L;
                s->sh_c = c;
                sel_mark_current(s);
            } else {
                skt_clear_selection(s);
                s->sel_on = 1;
                s->sa_l = s->sh_l = L;
                s->sa_c = s->sh_c = c;
            }
            s->sel_drag = 1;
        } else if (e->mouse.button == SK_BUTTON_RIGHT) {
            /* 右键：有选区就复制（然后取消选区），没有就粘贴——和 Windows 的控制台一样 */
            long long l0 = s->sa_l, l1 = s->sh_l;
            if (s->sel_on && (l0 != l1 || s->sa_c != s->sh_c)) {
                copy_selection(s);
                skt_clear_selection(s);
            } else {
                sk_clipboard_request(s->app, s->win);
            }
        } else if (e->mouse.button == SK_BUTTON_MIDDLE) {
            sk_clipboard_request(s->app, s->win);
        }
        return 1;
    case SK_MOUSE_MOVE:
        sk_window_set_cursor(s->win, sbar && !s->sel_drag ? SK_CURSOR_ARROW : SK_CURSOR_TEXT);
        if (s->sbar_drag) {
            sbar_drag_to(s, y);
            return 1;
        }
        if (sbar != s->sbar_hover && !s->sel_drag)
            s->sbar_hover = sbar;
        if (s->sel_drag && (e->mouse.buttons & SK_BUTTON_LEFT)) {
            /* 拖到文字区上面/下面：自动往回/往后翻一行 */
            if (y < s->oy)
                skt_scroll_view(s, 1);
            else if (y >= s->oy + s->t->rows * s->lh)
                skt_scroll_view(s, -1);
            long long L;
            int c;
            point_at(s, x, y, &L, &c);
            if (L != s->sh_l || c != s->sh_c) {
                sel_mark_current(s);
                s->sh_l = L;
                s->sh_c = c;
                sel_mark_current(s);
            }
        }
        return 1;
    case SK_MOUSE_UP:
        if (e->mouse.button == SK_BUTTON_LEFT) {
            if (s->sbar_drag) {
                s->sbar_drag = 0;
                return 1;
            }
            if (s->sel_drag) {
                s->sel_drag = 0;
                if (s->sa_l == s->sh_l && s->sa_c == s->sh_c)
                    skt_clear_selection(s);      /* 只是单击，没有拖动 */
            }
        }
        return 1;
    default:
        return 0;
    }
}
