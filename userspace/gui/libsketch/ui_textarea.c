/*
 * ui_textarea.c — 多行文本编辑器控件 sk_textarea_*（记事本 sknote 的核心）
 *
 * 一个文本编辑器可以分成四层，这个文件从下到上依次实现：
 *
 *   1. 文本模型（ta_doc）：“行数组”——每个逻辑行是一块独立的 UTF-8 缓冲区（不含换行符）。
 *      为什么不用一整块大缓冲区或间隙缓冲区（gap buffer）？
 *        - 编辑器的几乎所有操作都是“按行”的：画第 N 行、折行、行号、上下移动光标；
 *        - 在一行里插入一个字只需要移动这一行的字节（通常几十字节），与文件大小无关；
 *        - 插入/删除一整行只是 memmove 一个指针数组（1 MB 的文件约 2 万行，几十 KB）。
 *      对 1 MB 以内的文本，这是最简单而又足够快的结构。
 *
 *   2. 折行缓存：开启“自动换行”时，一个逻辑行会显示成好几个“显示行”。
 *      每行缓存自己的折行位置 brk[]，再用一个前缀和数组 pre[] 记录“第 i 行之前一共有多少显示行”，
 *      于是“第 g 个显示行属于哪一行”可以二分查找。打字时只重新折行被修改的那一行
 *      （而且只从被修改的“词”所在显示行的上一行开始），前缀和从这一行往后懒惰地重算。
 *
 *   3. 撤销/重做（ta_undo）：每次编辑记录成“组”，一组最多两个基本操作（删除 + 插入，
 *      例如“用新文字替换选中的文字”）。撤销 = 反向执行；重做 = 正向再执行一次。
 *      连续打字、连续退格会合并成一组（按“词”和停顿时间分组），这样 Ctrl+Z 一次撤销一个词，
 *      而不是一个字母。
 *
 *   4. 控件：笔记本纸外观（淡蓝横线、红色边线）、铅笔光标、荧光笔选区，键盘、鼠标、剪贴板。
 *      性能（网页版 v86 很慢）：打字只让受影响的“显示行”失效；只有行数变化
 *      （下面的行整体移动）或滚动时才重画更大的区域。
 *
 * 坐标与位置：
 *   - 文本位置 ta_pos = (逻辑行号, 行内字节偏移)，偏移总是落在 UTF-8 字符边界上。
 *   - 光标位于折行处时有歧义：既可以显示在上一显示行的末尾，也可以显示在下一显示行的开头。
 *     caret_eol 标志（“行尾亲和”）决定显示在哪里：按 End 键、点击某显示行的最右边时置 1。
 *
 * 无界面模式：w->ui == NULL 时（只在宿主机单元测试 gui/tests/test_ui_widgets.c 里出现）
 * 所有与窗口有关的调用（失效、光标闪烁、剪贴板、定时器）都被跳过，编辑逻辑可以单独测试。
 */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <linux/input-event-codes.h>

#include "sketch/ui.h"
#include "sketch/rough.h"
#include "ui_internal.h"

/* ---------------------------------------------------------------- 常量 */
#define TA_FRAME        3       /* 外框线到内容区的距离 */
#define TA_SB_W         12      /* 滚动条宽度 */
#define TA_TEXT_PAD     7       /* 红色边线到文字的距离 */
#define TA_MARGIN0      12      /* 没有行号时红色边线离左边框的距离 */
#define TA_TAB_SPACES   4       /* Tab 键插入的空格数；文件里的 \t 也按 4 个空格宽的制表位显示 */
#define TA_MERGE_MS     1000    /* 两次按键间隔小于它才合并进同一个撤销组 */
#define TA_UNDO_GROUPS  1000    /* 撤销栈最多保留的组数 */
#define TA_UNDO_BYTES   (8u << 20)  /* 撤销栈最多占用的文字字节数 */
#define TA_WHEEL_ROWS   3       /* 滚轮一格滚动的行数 */
#define TA_AUTOSCROLL_MS 60     /* 拖选到控件外时自动滚动的间隔 */

typedef struct { int line, off; } ta_pos;

static int ta_pos_cmp(ta_pos a, ta_pos b)
{
    if (a.line != b.line)
        return a.line < b.line ? -1 : 1;
    return a.off < b.off ? -1 : a.off > b.off;
}
static ta_pos ta_pos_make(int line, int off) { ta_pos p = { line, off }; return p; }
static ta_pos ta_pos_min(ta_pos a, ta_pos b) { return ta_pos_cmp(a, b) <= 0 ? a : b; }
static ta_pos ta_pos_max(ta_pos a, ta_pos b) { return ta_pos_cmp(a, b) >= 0 ? a : b; }

/* 向下取整的除法（C 的 / 对负数是向零取整；鼠标拖到控件上方时 y 差值是负的） */
static int ta_floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* ================================================================ UTF-8 与字符分类 */

/* 解码 s 开头（最多 n 字节）的一个字符，*clen 返回它占的字节数（至少 1）。
 * 非法序列（孤立的续字节、过长编码、代理区、被截断的序列）一律当作 1 字节的 U+FFFD，
 * 这样任何字节串都能被一个字符一个字符地走完，光标永远不会卡住。 */
static uint32_t ta_decode(const char *s, int n, int *clen)
{
    const unsigned char *u = (const unsigned char *)s;
    uint32_t c = u[0], min;
    int need;
    *clen = 1;
    if (c < 0x80)
        return c;
    if ((c & 0xE0) == 0xC0)      { need = 1; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { need = 2; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { need = 3; c &= 0x07; min = 0x10000; }
    else return 0xFFFD;
    if (need >= n)
        return 0xFFFD;
    for (int i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80)
            return 0xFFFD;
        c = (c << 6) | (u[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        return 0xFFFD;
    *clen = need + 1;
    return c;
}

/* off 之前一个字符的起点（与 ta_decode 的切分方式一致） */
static int ta_prev_off(const char *s, int off)
{
    if (off <= 0)
        return 0;
    int p = off - 1, k = 0, clen;
    while (p > 0 && k < 3 && ((unsigned char)s[p] & 0xC0) == 0x80) {
        p--;
        k++;
    }
    ta_decode(s + p, off - p, &clen);
    return p + clen == off ? p : off - 1;
}

static int ta_next_off(const char *s, int len, int off)
{
    int clen;
    if (off >= len)
        return len;
    ta_decode(s + off, len - off, &clen);
    return off + clen;
}

static uint32_t ta_char_at(const char *s, int len, int off)
{
    int clen;
    return off < len ? ta_decode(s + off, len - off, &clen) : 0;
}

static int ta_is_space(uint32_t c) { return c == ' ' || c == '\t' || c == 0x3000; }

/* “宽字符”：中日韩文字、全角符号。它们之间（和它们前后）可以随处折行。 */
static int ta_is_wide(uint32_t c)
{
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF);
}

static int ta_in_list(uint32_t c, const uint32_t *list)
{
    for (; *list; list++)
        if (*list == c)
            return 1;
    return 0;
}

/* 避头尾规则（中文排版）：句号、逗号、右括号等不能出现在行首；左括号、左引号不能出现在行尾 */
static int ta_is_closing(uint32_t c)
{
    static const uint32_t list[] = {
        0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF01, 0xFF1F, 0xFF09, 0x300D, 0x300F,
        0x3011, 0x300B, 0x3009, 0x3015, 0x3017, 0x201D, 0x2019, 0x2026, 0xFF5E, 0x00B7, 0x30FC,
        ',', '.', ';', ':', '!', '?', ')', ']', '}', 0 };
    return ta_in_list(c, list);
}
static int ta_is_opening(uint32_t c)
{
    static const uint32_t list[] = {
        0xFF08, 0x300C, 0x300E, 0x3010, 0x300A, 0x3008, 0x3014, 0x3016, 0x201C, 0x2018,
        '(', '[', '{', 0 };
    return ta_in_list(c, list);
}

/* 可以在 prev 和 cp 之间折行吗？（cp 将成为新显示行的第一个字符） */
static int ta_break_before(uint32_t prev, uint32_t cp)
{
    if (ta_is_space(cp))
        return 0;               /* 空格“挂”在行尾，不让它出现在下一行开头 */
    if (ta_is_space(prev))
        return 1;               /* 英文在空格后折行（按词换行） */
    if (ta_is_closing(cp) || ta_is_opening(prev))
        return 0;
    return ta_is_wide(cp) || ta_is_wide(prev);   /* 中文按字折行 */
}

/* 选词/按词移动用的字符类别：0 空白，1 字母数字（含拉丁扩展），2 中日韩文字，3 标点符号 */
static int ta_word_class(uint32_t c)
{
    if (ta_is_space(c))
        return 0;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
        return 1;
    if (c < 0x80)
        return 3;
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x3FFFF))
        return 2;
    if ((c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65))
        return 3;
    return 1;   /* 其它字母（希腊文、西里尔文、带重音的拉丁字母、全角字母……） */
}

/* ================================================================ 字宽缓存 */
/*
 * 折行要测量每一个字的宽度。sk_glyph_advance 每次都要在字体的字形表里二分查找，
 * 1 MB 的文件就是上百万次。这里加两层缓存：ASCII 直接查表；其它字符用一个
 * 256 项的“直接映射”缓存（用码点的哈希选槽位，冲突就覆盖）——中文文章里常用字反复出现，
 * 命中率很高。
 */
typedef struct {
    const sk_font *font;
    int (*test_adv)(uint32_t cp);   /* 单元测试用：不加载字体，直接给出字宽 */
    int16_t  ascii[128];
    uint32_t ck[256];
    int16_t  cv[256];
    int      tab_w;                 /* 制表位宽度（像素） */
} ta_metrics;

static int ta_raw_adv(const ta_metrics *m, uint32_t cp)
{
    int a = m->test_adv ? m->test_adv(cp) : sk_glyph_advance(m->font, cp);
    return a < 0 ? 0 : a > 255 ? 255 : a;
}

static int ta_adv(ta_metrics *m, uint32_t cp)
{
    if (cp < 128) {
        if (m->ascii[cp] < 0)
            m->ascii[cp] = (int16_t)ta_raw_adv(m, cp);
        return m->ascii[cp];
    }
    unsigned h = (cp * 2654435761u) >> 24;
    if (m->ck[h] != cp) {
        m->ck[h] = cp;
        m->cv[h] = (int16_t)ta_raw_adv(m, cp);
    }
    return m->cv[h];
}

/* 字宽；x 是它在显示行里的起始位置（制表符要对齐到下一个制表位） */
static int ta_adv_at(ta_metrics *m, uint32_t cp, int x)
{
    if (cp == '\t')
        return m->tab_w - x % m->tab_w;
    return ta_adv(m, cp);
}

static void ta_metrics_init(ta_metrics *m, const sk_font *f, int (*test_adv)(uint32_t))
{
    m->font = f;
    m->test_adv = test_adv;
    for (int i = 0; i < 128; i++)
        m->ascii[i] = -1;
    memset(m->ck, 0, sizeof(m->ck));      /* 码点 0 永远走 ASCII 表，所以 0 可以表示“空槽” */
    m->tab_w = 1;
    int sp = ta_adv(m, ' ');
    m->tab_w = TA_TAB_SPACES * (sp > 0 ? sp : 8);
}

/* ================================================================ 文本模型 + 折行缓存 */

typedef struct {
    char    *s;         /* UTF-8 字节（不含换行）；有内容时总以 NUL 结尾，方便调试 */
    int      len, cap;
    int      nrows;     /* 显示行数（>= 1） */
    int     *brk;       /* nrows-1 个折行位置：第 k+1 个显示行从字节 brk[k] 开始 */
    int      brk_cap;
    int      width;     /* 不换行模式下整行的像素宽度 */
    unsigned gen;       /* 折行结果对应的布局代数；与 doc.gen 不同 = 需要重新折行 */
} ta_line;

typedef struct {
    ta_line   *lines;
    int        n, cap;      /* 至少 1 行（空文档 = 一个空行） */
    int       *pre;         /* pre[i] = 第 i 行之前的显示行总数，pre[0..pre_valid] 有效 */
    int        pre_cap, pre_valid;
    unsigned   gen;         /* 换行宽度/字体变化时加 1，所有行的折行缓存随之作废 */
    int        wrap_w;      /* 折行宽度（像素），<= 0 表示不自动换行 */
    int        max_w;       /* 不换行模式：最长一行的宽度 */
    int        max_w_dirty;
    ta_metrics m;
} ta_doc;

static int ta_line_reserve(ta_line *L, int need)
{
    if (need + 1 <= L->cap)
        return 0;
    int cap = L->cap ? L->cap : 16;
    while (cap < need + 1)
        cap *= 2;
    char *s = realloc(L->s, cap);
    if (!s)
        return -1;
    L->s = s;
    L->cap = cap;
    return 0;
}

static void ta_line_free(ta_line *L)
{
    free(L->s);
    free(L->brk);
    memset(L, 0, sizeof(*L));
}

/* 把 [t, t+n) 设为行内容（失败时保持原样并返回 -1） */
static int ta_line_set(ta_line *L, const char *t, int n)
{
    if (ta_line_reserve(L, n) < 0)
        return -1;
    if (n)
        memcpy(L->s, t, n);
    L->s[n] = 0;
    L->len = n;
    L->gen = 0;
    return 0;
}

static int ta_doc_reserve(ta_doc *d, int nlines)
{
    if (nlines > d->cap) {
        int cap = d->cap ? d->cap : 64;
        while (cap < nlines)
            cap *= 2;
        ta_line *l = realloc(d->lines, cap * sizeof(ta_line));
        if (!l)
            return -1;
        d->lines = l;
        d->cap = cap;
    }
    if (nlines + 1 > d->pre_cap) {
        int cap = d->pre_cap ? d->pre_cap : 64;
        while (cap < nlines + 1)
            cap *= 2;
        int *p = realloc(d->pre, cap * sizeof(int));
        if (!p)
            return -1;
        d->pre = p;
        d->pre_cap = cap;
    }
    return 0;
}

static void ta_doc_invalidate_pre(ta_doc *d, int line)
{
    if (d->pre_valid > line)
        d->pre_valid = line;
}

/* 在 at 处插入 count 个空行 */
static int ta_doc_insert_lines(ta_doc *d, int at, int count)
{
    if (ta_doc_reserve(d, d->n + count) < 0)
        return -1;
    memmove(&d->lines[at + count], &d->lines[at], (d->n - at) * sizeof(ta_line));
    memset(&d->lines[at], 0, count * sizeof(ta_line));
    d->n += count;
    ta_doc_invalidate_pre(d, at);
    return 0;
}

static void ta_doc_remove_lines(ta_doc *d, int at, int count)
{
    for (int i = at; i < at + count; i++)
        ta_line_free(&d->lines[i]);
    memmove(&d->lines[at], &d->lines[at + count], (d->n - at - count) * sizeof(ta_line));
    d->n -= count;
    ta_doc_invalidate_pre(d, at);
    d->max_w_dirty = 1;
}

static void ta_doc_init(ta_doc *d)
{
    memset(d, 0, sizeof(*d));
    d->gen = 1;
    if (ta_doc_reserve(d, 1) == 0) {
        memset(&d->lines[0], 0, sizeof(ta_line));
        d->n = 1;
    }
    d->pre_valid = 0;
}

static void ta_doc_free(ta_doc *d)
{
    for (int i = 0; i < d->n; i++)
        ta_line_free(&d->lines[i]);
    free(d->lines);
    free(d->pre);
    memset(d, 0, sizeof(*d));
}

/* 清理外来文字：去掉 NUL 字节，把 \r\n 和单独的 \r 统一成 \n（Windows/老 Mac 的换行）。
 * 不需要清理时返回 NULL（调用者直接用原文），否则返回 malloc 的新串。 */
static char *ta_clean_text(const char *t, int len, int *out_len)
{
    if (!memchr(t, '\r', len) && !memchr(t, '\0', len))
        return NULL;
    char *o = malloc(len + 1);
    int n = 0;
    if (!o) {
        *out_len = 0;
        return NULL;
    }
    for (int i = 0; i < len; i++) {
        char ch = t[i];
        if (ch == '\0')
            continue;
        if (ch == '\r') {
            if (i + 1 < len && t[i + 1] == '\n')
                continue;
            ch = '\n';
        }
        o[n++] = ch;
    }
    o[n] = 0;
    *out_len = n;
    return o;
}

/* 用整段文字（已清理）替换文档内容 */
static int ta_doc_set_text(ta_doc *d, const char *t, int len)
{
    int nl = 1;
    for (const char *q = t; (q = memchr(q, '\n', len - (q - t))) != NULL; q++)
        nl++;
    ta_doc nd;
    memset(&nd, 0, sizeof(nd));
    if (ta_doc_reserve(&nd, nl) < 0) {
        ta_doc_free(&nd);
        return -1;
    }
    memset(nd.lines, 0, nl * sizeof(ta_line));
    const char *seg = t, *end = t + len;
    for (int i = 0; i < nl; i++) {
        const char *e = memchr(seg, '\n', end - seg);
        if (!e)
            e = end;
        if (ta_line_set(&nd.lines[i], seg, (int)(e - seg)) < 0) {
            nd.n = i;
            ta_doc_free(&nd);
            return -1;
        }
        seg = e + 1;
    }
    nd.n = nl;
    /* 保留布局参数（换行宽度、字体），只换内容 */
    nd.gen = d->gen + 1;
    nd.wrap_w = d->wrap_w;
    nd.m = d->m;
    nd.max_w_dirty = 1;
    ta_doc_free(d);
    *d = nd;
    return 0;
}

/* 取出 [a, b) 之间的文字（行之间用 \n 连接），返回 malloc 的串 */
static char *ta_doc_get_range(const ta_doc *d, ta_pos a, ta_pos b, int *out_len)
{
    size_t total = 0;
    for (int i = a.line; i <= b.line; i++) {
        int s = i == a.line ? a.off : 0;
        int e = i == b.line ? b.off : d->lines[i].len;
        total += e - s + (i < b.line);
    }
    char *o = malloc(total + 1);
    if (!o) {
        *out_len = 0;
        return NULL;
    }
    size_t n = 0;
    for (int i = a.line; i <= b.line; i++) {
        int s = i == a.line ? a.off : 0;
        int e = i == b.line ? b.off : d->lines[i].len;
        if (e > s) {
            memcpy(o + n, d->lines[i].s + s, e - s);
            n += e - s;
        }
        if (i < b.line)
            o[n++] = '\n';
    }
    o[n] = 0;
    *out_len = (int)n;
    return o;
}

/* 在 p 处插入文字（可含 \n），返回插入内容末尾的位置。内存不足时返回 p（什么也不做） */
static ta_pos ta_doc_insert(ta_doc *d, ta_pos p, const char *t, int len)
{
    int nl = 0;
    for (int i = 0; i < len; i++)
        nl += t[i] == '\n';
    ta_line *L = &d->lines[p.line];
    if (nl == 0) {
        if (ta_line_reserve(L, L->len + len) < 0)
            return p;
        memmove(L->s + p.off + len, L->s + p.off, L->len - p.off);
        memcpy(L->s + p.off, t, len);
        L->len += len;
        L->s[L->len] = 0;
        L->gen = 0;
        return ta_pos_make(p.line, p.off + len);
    }
    /* 多行：原来这一行在 p 之后的部分（tail）接到最后一段文字后面 */
    int tail_len = L->len - p.off;
    const char *last = t + len;
    while (last > t && last[-1] != '\n')
        last--;
    int last_len = (int)(t + len - last);
    char *tail = malloc(tail_len + last_len + 1);
    if (!tail || ta_doc_insert_lines(d, p.line + 1, nl) < 0) {
        free(tail);
        return p;
    }
    memcpy(tail, last, last_len);
    if (tail_len)
        memcpy(tail + last_len, d->lines[p.line].s + p.off, tail_len);
    /* 第一段接在 p 之前的内容后面 */
    L = &d->lines[p.line];
    const char *e = memchr(t, '\n', len);
    int first_len = (int)(e - t);
    if (ta_line_reserve(L, p.off + first_len) == 0) {
        memcpy(L->s + p.off, t, first_len);
        L->len = p.off + first_len;
        L->s[L->len] = 0;
    }
    L->gen = 0;
    /* 中间各段各成一行 */
    const char *seg = e + 1;
    for (int i = 1; i < nl; i++) {
        const char *se = memchr(seg, '\n', t + len - seg);
        ta_line_set(&d->lines[p.line + i], seg, (int)(se - seg));
        seg = se + 1;
    }
    ta_line_set(&d->lines[p.line + nl], tail, last_len + tail_len);
    free(tail);
    return ta_pos_make(p.line + nl, last_len);
}

/* 删除 [a, b) */
static void ta_doc_delete(ta_doc *d, ta_pos a, ta_pos b)
{
    ta_line *La = &d->lines[a.line];
    if (a.line == b.line) {
        memmove(La->s + a.off, La->s + b.off, La->len - b.off);
        La->len -= b.off - a.off;
        La->s[La->len] = 0;
        La->gen = 0;
        return;
    }
    ta_line *Lb = &d->lines[b.line];
    int tail = Lb->len - b.off;
    if (ta_line_reserve(La, a.off + tail) < 0)
        return;
    Lb = &d->lines[b.line];
    if (tail)
        memcpy(La->s + a.off, Lb->s + b.off, tail);
    La->len = a.off + tail;
    La->s[La->len] = 0;
    La->gen = 0;
    ta_doc_remove_lines(d, a.line + 1, b.line - a.line);
}

/* 文字插入到 a 之后末尾的位置（不修改文档，撤销/重做时用） */
static ta_pos ta_pos_after(ta_pos a, const char *t, int len)
{
    const char *last = NULL;
    int nl = 0;
    for (int i = 0; i < len; i++)
        if (t[i] == '\n') {
            nl++;
            last = t + i;
        }
    if (!nl)
        return ta_pos_make(a.line, a.off + len);
    return ta_pos_make(a.line + nl, (int)(t + len - last - 1));
}

/* ---- 测量 ---- */

/* 从显示行起点 from 量到 to 的宽度（制表位相对 from 对齐） */
static int ta_measure(ta_doc *d, const ta_line *L, int from, int to)
{
    int x = 0, clen;
    for (int off = from; off < to; off += clen) {
        uint32_t cp = ta_decode(L->s + off, L->len - off, &clen);
        x += ta_adv_at(&d->m, cp, x);
    }
    return x;
}

/* 显示行 [from, to) 里，离像素位置 px 最近的字符边界 */
static int ta_hit(ta_doc *d, const ta_line *L, int from, int to, int px)
{
    int x = 0, clen;
    for (int off = from; off < to; off += clen) {
        uint32_t cp = ta_decode(L->s + off, L->len - off, &clen);
        int adv = ta_adv_at(&d->m, cp, x);
        if (px < x + (adv + 1) / 2)
            return off;
        x += adv;
    }
    return to;
}

/* ---- 折行 ---- */

static int ta_push_brk(ta_line *L, int nb, int off)
{
    if (nb + 1 > L->brk_cap) {
        int cap = L->brk_cap ? L->brk_cap * 2 : 4;
        int *b = realloc(L->brk, cap * sizeof(int));
        if (!b)
            return nb;          /* 内存不足：这一行就少折一次，显示宽一点而已 */
        L->brk = b;
        L->brk_cap = cap;
    }
    L->brk[nb] = off;
    return nb + 1;
}

/*
 * 给第 li 行折行。贪心算法：一个字一个字往右排，排不下时在“最近的可折行位置”断开；
 * 如果这一显示行里根本没有可折行的位置（一个超长的英文单词），就在当前字前面硬断。
 *
 * from_row > 0 表示只从第 from_row 个显示行开始重新计算，前面的折行结果保留——
 * 调用者保证修改发生在这些行之后，且不会影响它们的折行决定（见 ta_rewrap_from）。
 */
static void ta_wrap_line(ta_doc *d, int li, int from_row)
{
    ta_line *L = &d->lines[li];
    int W = d->wrap_w;
    if (W <= 0 || L->gen != d->gen || from_row < 0 || from_row > L->nrows - 1)
        from_row = 0;
    int nb = from_row;
    int row_start = from_row == 0 ? 0 : L->brk[from_row - 1];
    int x = 0, maxw = 0, last_break = -1, clen;
    uint32_t prev = 0;
    for (int off = row_start; off < L->len; off += clen) {
        uint32_t cp = ta_decode(L->s + off, L->len - off, &clen);
        int adv = ta_adv_at(&d->m, cp, x);
        if (W > 0 && off > row_start) {
            if (ta_break_before(prev, cp))
                last_break = off;
            if (x + adv > W && !ta_is_space(cp)) {
                int b = last_break > row_start ? last_break : off;
                nb = ta_push_brk(L, nb, b);
                row_start = b;
                x = ta_measure(d, L, b, off);
                last_break = -1;
                adv = ta_adv_at(&d->m, cp, x);
                if (x + adv > W && off > row_start) {     /* 剩下的半个词自己就超宽：硬断 */
                    nb = ta_push_brk(L, nb, off);
                    row_start = off;
                    x = 0;
                    adv = ta_adv_at(&d->m, cp, 0);
                }
            }
        }
        x += adv;
        if (x > maxw)
            maxw = x;
        prev = cp;
    }
    L->nrows = nb + 1;
    L->width = maxw;
    L->gen = d->gen;
}

/* 取第 i 行（保证折行缓存是最新的） */
static ta_line *ta_L(ta_doc *d, int i)
{
    ta_line *L = &d->lines[i];
    if (L->gen != d->gen)
        ta_wrap_line(d, i, 0);
    return L;
}

/* 保证 pre[0..upto] 有效 */
static void ta_ensure_pre(ta_doc *d, int upto)
{
    d->pre[0] = 0;
    if (d->pre_valid >= upto)
        return;
    for (int i = d->pre_valid; i < upto; i++)
        d->pre[i + 1] = d->pre[i] + ta_L(d, i)->nrows;
    d->pre_valid = upto;
}

static int ta_total_rows(ta_doc *d)
{
    ta_ensure_pre(d, d->n);
    return d->pre[d->n];
}

static int ta_row_start(const ta_line *L, int k) { return k <= 0 ? 0 : L->brk[k - 1]; }
static int ta_row_end(const ta_line *L, int k) { return k >= L->nrows - 1 ? L->len : L->brk[k]; }

/* 位置 off 在第几个显示行（行内）。eol=1 且正好落在折行处时算作上一显示行的末尾 */
static int ta_row_in_line(const ta_line *L, int off, int eol)
{
    int lo = 0, hi = L->nrows - 1;          /* 统计 brk 中 <= off 的个数 */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (L->brk[mid] <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (eol && lo > 0 && L->brk[lo - 1] == off)
        lo--;
    return lo;
}

/* 位置 → 全局显示行号 */
static int ta_global_row(ta_doc *d, ta_pos p, int eol)
{
    ta_ensure_pre(d, p.line);
    return d->pre[p.line] + ta_row_in_line(ta_L(d, p.line), p.off, eol);
}

/* 全局显示行号 → (逻辑行, 行内第几个显示行)。g 会被限制在有效范围内 */
static void ta_row_lookup(ta_doc *d, int g, int *li, int *k)
{
    int total = ta_total_rows(d);
    if (g >= total)
        g = total - 1;
    if (g < 0)
        g = 0;
    int lo = 0, hi = d->n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (d->pre[mid] <= g)
            lo = mid;
        else
            hi = mid - 1;
    }
    *li = lo;
    *k = g - d->pre[lo];
    if (*k > ta_L(d, lo)->nrows - 1)
        *k = d->lines[lo].nrows - 1;
}

/* 位置 → 它所在显示行内的 x 像素 */
static int ta_pos_x(ta_doc *d, ta_pos p, int eol)
{
    ta_line *L = ta_L(d, p.line);
    int k = ta_row_in_line(L, p.off, eol);
    return ta_measure(d, L, ta_row_start(L, k), p.off);
}

/* 不换行模式下最长一行的宽度（懒惰重算） */
static int ta_doc_max_width(ta_doc *d)
{
    if (d->max_w_dirty) {
        int m = 0;
        for (int i = 0; i < d->n; i++)
            if (ta_L(d, i)->width > m)
                m = d->lines[i].width;
        d->max_w = m;
        d->max_w_dirty = 0;
    }
    return d->max_w;
}

/* ================================================================ 撤销 / 重做 */

enum { TA_K_OTHER = 0, TA_K_TYPE, TA_K_BACK, TA_K_DEL };

typedef struct {
    int    ins;         /* 1 = 在 a 处插入了 text；0 = 从 a 处删除了 text */
    ta_pos a;
    char  *text;
    int    len;
} ta_op;

typedef struct {
    int      kind;      /* TA_K_*：决定能否与下一次按键合并 */
    unsigned seq;       /* 唯一编号（用于“是否已修改”的判断） */
    uint64_t time;
    ta_pos   caret0, anchor0;   /* 编辑前的光标/选区（撤销后恢复） */
    ta_pos   caret1, anchor1;   /* 编辑后的（重做后恢复） */
    int      nops;
    ta_op    ops[2];
} ta_group;

typedef struct {
    ta_group *g;
    int       n, cap;
    int       top;          /* g[0..top) 可撤销，g[top..n) 可重做 */
    unsigned  next_seq;
    unsigned  base_seq;     /* 栈底状态的编号（最老的组被丢弃后就是它的 seq） */
    unsigned  saved_seq;    /* 保存时的状态编号；UINT_MAX = 无法回到的状态（永远“已修改”） */
    size_t    bytes;
} ta_undo;

static void ta_group_free(ta_undo *u, ta_group *g)
{
    for (int i = 0; i < g->nops; i++) {
        u->bytes -= g->ops[i].len;
        free(g->ops[i].text);
    }
    g->nops = 0;
}

static void ta_undo_clear(ta_undo *u)
{
    for (int i = 0; i < u->n; i++)
        ta_group_free(u, &u->g[i]);
    free(u->g);
    memset(u, 0, sizeof(*u));
    u->next_seq = 1;
}

static unsigned ta_undo_state(const ta_undo *u)
{
    return u->top ? u->g[u->top - 1].seq : u->base_seq;
}

/* 丢弃最老的组，直到满足数量/内存限制 */
static void ta_undo_trim(ta_undo *u)
{
    int drop = 0;
    while (drop < u->top - 1 && (u->top - drop > TA_UNDO_GROUPS || u->bytes > TA_UNDO_BYTES)) {
        u->base_seq = u->g[drop].seq;
        ta_group_free(u, &u->g[drop]);
        drop++;
    }
    if (drop) {
        memmove(u->g, u->g + drop, (u->n - drop) * sizeof(ta_group));
        u->n -= drop;
        u->top -= drop;
    }
}

/* 新开一个组（会清掉所有“可重做”的组）。失败返回 NULL */
static ta_group *ta_undo_push(ta_undo *u, int kind)
{
    for (int i = u->top; i < u->n; i++)
        ta_group_free(u, &u->g[i]);
    u->n = u->top;
    if (u->n + 1 > u->cap) {
        int cap = u->cap ? u->cap * 2 : 32;
        ta_group *g = realloc(u->g, cap * sizeof(ta_group));
        if (!g)
            return NULL;
        u->g = g;
        u->cap = cap;
    }
    ta_group *g = &u->g[u->n++];
    memset(g, 0, sizeof(*g));
    g->kind = kind;
    g->seq = u->next_seq++;
    u->top = u->n;
    return g;
}

/* 往组里加一个操作；text 的所有权转移给撤销栈 */
static void ta_group_add(ta_undo *u, ta_group *g, int ins, ta_pos a, char *text, int len)
{
    if (g->nops >= 2) {
        free(text);
        return;
    }
    ta_op *op = &g->ops[g->nops++];
    op->ins = ins;
    op->a = a;
    op->text = text;
    op->len = len;
    u->bytes += len;
}

/* ================================================================ 控件私有数据 */

enum { TA_DRAG_NONE = 0, TA_DRAG_TEXT, TA_DRAG_VBAR, TA_DRAG_HBAR };

typedef struct {
    ta_doc   doc;
    ta_undo  undo;
    ta_pos   caret, anchor;
    int      caret_eol;
    int      pref_x;            /* 上下移动时想保持的 x（像素，相对显示行起点）；-1 = 未设置 */
    int      scroll_row;        /* 第一个可见显示行 */
    int      scroll_x;          /* 不换行模式的水平滚动（像素） */
    int      wrap, numbers, readonly;
    /* 布局（窗口坐标） */
    const sk_font *font;
    sk_rect  inner;             /* 外框以内 */
    sk_rect  text_r;            /* 文字区 */
    sk_rect  vbar, hbar;        /* 竖/横滚动条（hbar 只在不换行时有） */
    int      margin_x;          /* 红色边线的 x */
    int      row_h, ascent, descent;
    int      vis_rows;          /* 完整可见的显示行数（>= 1） */
    int      digits;            /* 行号位数 */
    /* 交互 */
    int      drag;
    int      drag_grab;         /* 拖滚动条：鼠标相对滑块起点的偏移 */
    int      word_drag;         /* 双击后拖动：按词扩展选区 */
    ta_pos   word_a, word_b;
    int      mouse_x, mouse_y;
    int      autoscroll_timer;
    int      paste_pending;
    int      notify;            /* 本次事件里内容或光标变了：事件处理完后调用 on_change */
} ta_priv;

#define TA(w) ((ta_priv *)(w)->priv)

/* 毫秒时钟（撤销分组用）。无界面模式下用一个可由单元测试设置的假时钟 */
static uint64_t ta_test_clock;
static uint64_t ta_now(const sk_widget *w) { return w->ui ? sk_now_ms() : ta_test_clock; }

/* ---- 与窗口交互的小包装（无界面模式下全部跳过） ----
 * 单元测试可以设置 ta_test_inval 来记录失效区域，检查“打一个字只重画一行”。 */
static void (*ta_test_inval)(sk_rect r);

static void ta_inval(sk_widget *w, sk_rect r)
{
    if (r.w <= 0 || r.h <= 0)
        return;
    if (w->ui)
        sk_widget_invalidate_rect(w, r);
    else if (ta_test_inval)
        ta_test_inval(r);
}
static void ta_inval_all(sk_widget *w)
{
    if (w->ui)
        sk_widget_invalidate(w);
    else if (ta_test_inval)
        ta_test_inval(sk_rect_inset(w->r, -SK_WIDGET_BLEED, -SK_WIDGET_BLEED));
}

/* ================================================================ 布局 */

static int ta_digits(int n)
{
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d < 2 ? 2 : d;
}

/* 顶部可见显示行的起点位置：布局变化（重新折行）前后用它保持同一段文字在顶部 */
static ta_pos ta_top_anchor(sk_widget *w)
{
    ta_priv *p = TA(w);
    int li, k;
    ta_row_lookup(&p->doc, p->scroll_row, &li, &k);
    return ta_pos_make(li, ta_row_start(ta_L(&p->doc, li), k));
}

static int ta_max_scroll(sk_widget *w)
{
    ta_priv *p = TA(w);
    int m = ta_total_rows(&p->doc) - p->vis_rows;
    return m > 0 ? m : 0;
}

static int ta_max_scroll_x(sk_widget *w)
{
    ta_priv *p = TA(w);
    if (p->wrap)
        return 0;
    int m = ta_doc_max_width(&p->doc) + p->doc.m.tab_w - p->text_r.w;
    return m > 0 ? m : 0;
}

/*
 * 计算各部分的矩形。font_px_h / 行号位数 / 是否换行 / 控件大小 任一变化都要重新计算；
 * 如果文字区宽度变了（会影响折行），整篇重新折行，并保持顶部那一行不跳走。
 */
static void ta_layout(sk_widget *w)
{
    ta_priv *p = TA(w);
    unsigned gen0 = p->doc.gen;
    /* 记下顶部显示的是哪段文字：重新折行后滚动到同一处，阅读位置不跳 */
    int have_top = p->row_h > 0 && p->scroll_row > 0;
    ta_pos top = have_top ? ta_top_anchor(w) : ta_pos_make(0, 0);
    if (w->ui) {
        const sk_font *f = sk_widget_get_font(w);
        if (f != p->font) {
            p->font = f;
            ta_metrics_init(&p->doc.m, f, NULL);
            p->ascent = sk_font_ascent(f);
            p->descent = sk_font_descent(f);
            int lh = sk_font_line_height(f);
            if (lh < p->ascent + p->descent)
                lh = p->ascent + p->descent;
            p->row_h = lh + 6;          /* 笔记本的横线间距比字高一点，留出呼吸感 */
            p->doc.gen++;
        }
    }
    if (p->row_h <= 0)
        p->row_h = 20;
    p->digits = ta_digits(p->doc.n);
    sk_rect r = w->r;
    p->inner = sk_rect_inset(r, TA_FRAME, TA_FRAME);
    int sb_h = p->wrap ? 0 : TA_SB_W;
    p->vbar = sk_rect_make(p->inner.x + p->inner.w - TA_SB_W - 1, p->inner.y + 3,
                           TA_SB_W, p->inner.h - 6 - sb_h);
    int gutter = TA_MARGIN0;
    if (p->numbers)
        gutter = p->digits * ta_adv(&p->doc.m, '0') + 14;
    p->margin_x = p->inner.x + gutter;
    int tx = p->margin_x + TA_TEXT_PAD;
    p->text_r = sk_rect_make(tx, p->inner.y + 3, p->vbar.x - 4 - tx, p->inner.h - 6 - sb_h);
    if (p->text_r.w < 8)
        p->text_r.w = 8;
    if (p->text_r.h < p->row_h)
        p->text_r.h = p->row_h;
    p->hbar = p->wrap ? sk_rect_make(0, 0, 0, 0)
                      : sk_rect_make(p->inner.x + 4, p->inner.y + p->inner.h - TA_SB_W - 2,
                                     p->vbar.x - p->inner.x - 8, TA_SB_W);
    p->vis_rows = p->text_r.h / p->row_h;
    if (p->vis_rows < 1)
        p->vis_rows = 1;

    int ww = p->wrap ? p->text_r.w : 0;
    if (ww != p->doc.wrap_w) {
        p->doc.wrap_w = ww;
        p->doc.gen++;
    }
    if (p->doc.gen != gen0) {
        /* 折行宽度或字体变了：所有行的折行缓存作废（之后用到时懒惰重算） */
        p->doc.pre_valid = 0;
        p->doc.max_w_dirty = 1;
        if (have_top)
            p->scroll_row = ta_global_row(&p->doc, top, 0);
    }
    if (p->scroll_row > ta_max_scroll(w))
        p->scroll_row = ta_max_scroll(w);
    if (p->scroll_row < 0)
        p->scroll_row = 0;
    if (p->scroll_x > ta_max_scroll_x(w))
        p->scroll_x = ta_max_scroll_x(w);
}

/* 全局显示行 g 的顶边 y（窗口坐标） */
static int ta_row_y(ta_priv *p, int g) { return p->text_r.y + (g - p->scroll_row) * p->row_h; }

/* 显示行 [g0, g1] 失效（宽度覆盖行号栏和文字区，不含滚动条） */
static void ta_inval_rows(sk_widget *w, int g0, int g1)
{
    ta_priv *p = TA(w);
    if (g1 < g0)
        return;
    int first = p->scroll_row, last = p->scroll_row + p->vis_rows;   /* 含最下面半行 */
    if (g1 < first || g0 > last)
        return;
    if (g0 < first)
        g0 = first;
    if (g1 > last)
        g1 = last;
    int y0 = ta_row_y(p, g0), y1 = ta_row_y(p, g1 + 1);
    int bottom = p->inner.y + p->inner.h;
    if (y1 > bottom)
        y1 = bottom;
    ta_inval(w, sk_rect_make(p->inner.x, y0, p->vbar.x - p->inner.x, y1 - y0));
}

/* 从显示行 g0 一直到文字区底部都失效（下面的行整体移动了） */
static void ta_inval_from(sk_widget *w, int g0)
{
    ta_priv *p = TA(w);
    if (g0 < p->scroll_row)
        g0 = p->scroll_row;
    int y0 = ta_row_y(p, g0);
    int bottom = p->inner.y + p->inner.h;
    if (y0 < bottom)
        ta_inval(w, sk_rect_make(p->inner.x, y0, p->vbar.x - p->inner.x, bottom - y0));
}

static void ta_inval_bars(sk_widget *w)
{
    ta_priv *p = TA(w);
    ta_inval(w, sk_rect_inset(p->vbar, -2, -2));
    if (!p->wrap)
        ta_inval(w, sk_rect_inset(p->hbar, -2, -2));
}

/* ================================================================ 光标显示 */

/* 光标矩形（窗口坐标）；光标不在可见范围内时返回 0 */
static int ta_caret_rect(sk_widget *w, sk_rect *out)
{
    ta_priv *p = TA(w);
    int g = ta_global_row(&p->doc, p->caret, p->caret_eol);
    if (g < p->scroll_row || g > p->scroll_row + p->vis_rows)
        return 0;
    int x = p->text_r.x - p->scroll_x + ta_pos_x(&p->doc, p->caret, p->caret_eol);
    if (x < p->text_r.x - 2 || x > p->text_r.x + p->text_r.w + 2)
        return 0;
    int y = ta_row_y(p, g);
    *out = sk_rect_make(x - 2, y + 2, 6, p->row_h - 3);
    return 1;
}

/* 把光标的新位置告诉 sk_ui 的闪烁服务（它会让旧位置和新位置失效） */
static void ta_update_caret(sk_widget *w)
{
    if (!w->ui || !sk_widget_has_focus(w))
        return;
    sk_rect r;
    if (ta_caret_rect(w, &r))
        sk_ui_caret_set(w, r);
    else
        sk_ui_caret_hide(w);
}

/* 滚动到 (row, x)；变化时整个控件重画 */
static void ta_scroll_to(sk_widget *w, int row, int sx)
{
    ta_priv *p = TA(w);
    int mr = ta_max_scroll(w), mx = ta_max_scroll_x(w);
    if (row > mr) row = mr;
    if (row < 0) row = 0;
    if (sx > mx) sx = mx;
    if (sx < 0) sx = 0;
    if (row == p->scroll_row && sx == p->scroll_x)
        return;
    p->scroll_row = row;
    p->scroll_x = sx;
    ta_inval_all(w);
    ta_update_caret(w);
}

/* 让光标可见：必要时滚动（水平方向一次跳四分之一宽，减少重画次数） */
static void ta_ensure_visible(sk_widget *w)
{
    ta_priv *p = TA(w);
    int g = ta_global_row(&p->doc, p->caret, p->caret_eol);
    int row = p->scroll_row, sx = p->scroll_x;
    if (g < row)
        row = g;
    else if (g >= row + p->vis_rows)
        row = g - p->vis_rows + 1;
    if (!p->wrap) {
        int cx = ta_pos_x(&p->doc, p->caret, p->caret_eol);
        int tw = p->text_r.w;
        if (cx < sx)
            sx = cx - tw / 4;
        else if (cx > sx + tw - 4)
            sx = cx - tw + tw / 4;
    } else {
        sx = 0;
    }
    ta_scroll_to(w, row, sx);
}

/* ---- 选区 ---- */

static int ta_has_sel(const ta_priv *p) { return ta_pos_cmp(p->caret, p->anchor) != 0; }

/* 选区从 (a0,b0) 变成 (a1,b1) 后，只让变化的那几行失效 */
static void ta_inval_sel_change(sk_widget *w, ta_pos lo0, ta_pos hi0, ta_pos lo1, ta_pos hi1)
{
    ta_priv *p = TA(w);
    int e0 = ta_pos_cmp(lo0, hi0) == 0, e1 = ta_pos_cmp(lo1, hi1) == 0;
    if (e0 && e1)
        return;
    if (e0 || e1) {
        ta_pos a = e0 ? lo1 : lo0, b = e0 ? hi1 : hi0;
        ta_inval_rows(w, ta_global_row(&p->doc, a, 0), ta_global_row(&p->doc, b, 1));
        return;
    }
    if (ta_pos_cmp(lo0, lo1) != 0) {
        ta_pos a = ta_pos_min(lo0, lo1), b = ta_pos_max(lo0, lo1);
        ta_inval_rows(w, ta_global_row(&p->doc, a, 0), ta_global_row(&p->doc, b, 1));
    }
    if (ta_pos_cmp(hi0, hi1) != 0) {
        ta_pos a = ta_pos_min(hi0, hi1), b = ta_pos_max(hi0, hi1);
        ta_inval_rows(w, ta_global_row(&p->doc, a, 0), ta_global_row(&p->doc, b, 1));
    }
}

/* 移动光标（extend=1：Shift 选择，锚点不动）。keep_pref=1：上下移动，保留期望 x */
static void ta_move(sk_widget *w, ta_pos np, int eol, int extend, int keep_pref)
{
    ta_priv *p = TA(w);
    ta_pos lo0 = ta_pos_min(p->caret, p->anchor), hi0 = ta_pos_max(p->caret, p->anchor);
    p->caret = np;
    p->caret_eol = eol;
    if (!extend)
        p->anchor = np;
    if (!keep_pref)
        p->pref_x = -1;
    ta_inval_sel_change(w, lo0, hi0, ta_pos_min(p->caret, p->anchor), ta_pos_max(p->caret, p->anchor));
    ta_ensure_visible(w);
    ta_update_caret(w);
    p->notify = 1;
}

/* ================================================================ 编辑原语（带局部失效） */

/*
 * 一次“行内”修改后要重画哪些显示行？
 * 重新折行之后，把新旧折行位置比较一下：完全在修改点之前、且起止位置都没变的显示行不用重画；
 * 完全在修改点之后、起止位置正好整体平移了（插入/删除的字节数）的显示行内容也没变，不用重画。
 * 这样在长段落的最后一行打字，只重画这一行；在段落中间打字导致后面重新排版，才重画后面几行。
 */
typedef struct {
    int  line, from_row, g0;
    int  old_nrows, old_n, old_digits;
    int *old_brk;
    int  old_width;
} ta_change;

/* 从修改点 off 往前找“所在词”的起点，再退一个显示行：从那里开始重新折行就够了 */
static int ta_rewrap_from(ta_doc *d, int li, int off)
{
    ta_line *L = ta_L(d, li);
    int p = off;
    while (p > 0) {
        int q = ta_prev_off(L->s, p);
        uint32_t cq = ta_char_at(L->s, L->len, q);
        uint32_t cb = q > 0 ? ta_char_at(L->s, L->len, ta_prev_off(L->s, q)) : 0;
        p = q;
        if (q > 0 && ta_break_before(cb, cq))
            break;
        if (off - p > 4096)        /* 超长的“词”：干脆整行重排 */
            return 0;
    }
    int r = ta_row_in_line(L, p, 0) - 1;
    return r < 0 ? 0 : r;
}

static void ta_change_begin(sk_widget *w, ta_change *ch, int line, int off)
{
    ta_priv *p = TA(w);
    ta_doc *d = &p->doc;
    ta_line *L = ta_L(d, line);
    ch->line = line;
    ch->from_row = ta_rewrap_from(d, line, off);
    ch->g0 = ta_global_row(d, ta_pos_make(line, 0), 0);
    ch->old_nrows = L->nrows;
    ch->old_n = d->n;
    ch->old_digits = p->digits;
    ch->old_width = L->width;
    ch->old_brk = NULL;
    if (L->nrows > 1) {
        ch->old_brk = malloc((L->nrows - 1) * sizeof(int));
        if (ch->old_brk)
            memcpy(ch->old_brk, L->brk, (L->nrows - 1) * sizeof(int));
    }
}

/* 修改完成后：重新折行、决定失效范围。
 * edit_off: 修改点（行内字节）；ins_len: 修改点之后新插入的字节数；delta: 本行字节数的变化 */
static void ta_change_end(sk_widget *w, ta_change *ch, int edit_off, int ins_len, int delta)
{
    ta_priv *p = TA(w);
    ta_doc *d = &p->doc;
    int li = ch->line;
    int old_total_n = ch->old_n;
    ta_wrap_line(d, li, ch->from_row);
    ta_line *L = &d->lines[li];
    if (d->n != old_total_n || L->nrows != ch->old_nrows)
        ta_doc_invalidate_pre(d, li);
    if (!p->wrap) {
        if (L->width > d->max_w)
            d->max_w = L->width;
        else if (ch->old_width >= d->max_w && L->width < ch->old_width)
            d->max_w_dirty = 1;
    }
    if (ta_digits(d->n) != ch->old_digits && p->numbers) {
        /* 行号多了一位：文字区变窄，整篇重新折行 */
        ta_layout(w);
        ta_inval_all(w);
        free(ch->old_brk);
        return;
    }
    if (d->n != old_total_n || L->nrows != ch->old_nrows) {
        ta_inval_from(w, ch->g0 + ch->from_row);
        ta_inval_bars(w);
    } else {
        /* 比较新旧折行，找出真正变化的显示行 */
        int first = -1, last = -1;
        int ins_end = edit_off + ins_len;
        for (int k = 0; k < L->nrows; k++) {
            int ns = ta_row_start(L, k), ne = ta_row_end(L, k);
            int os = k == 0 ? 0 : ch->old_brk ? ch->old_brk[k - 1] : 0;
            int oe = k == L->nrows - 1 ? L->len - delta : ch->old_brk ? ch->old_brk[k] : 0;
            int clean;
            if (ne < edit_off || (ne == edit_off && k < L->nrows - 1))
                clean = ns == os && ne == oe;
            else if (ns >= ins_end && ns > edit_off)
                clean = ns == os + delta && ne == oe + delta;
            else
                clean = 0;
            if (!clean) {
                if (first < 0)
                    first = k;
                last = k;
            }
        }
        if (first >= 0)
            ta_inval_rows(w, ch->g0 + first, ch->g0 + last);
    }
    free(ch->old_brk);
}

/* 插入（不记录撤销），返回末尾位置 */
static ta_pos ta_do_insert(sk_widget *w, ta_pos a, const char *t, int len)
{
    ta_priv *p = TA(w);
    if (len <= 0)
        return a;
    ta_change ch;
    ta_change_begin(w, &ch, a.line, a.off);
    unsigned g = p->doc.lines[a.line].gen;
    ta_pos e = ta_doc_insert(&p->doc, a, t, len);
    /* ta_doc_insert 把这一行标记成“需要折行”(gen=0)。但 a 之前的内容没变，
     * from_row 之前的折行结果仍然有效：恢复 gen，让 ta_change_end 只做部分重排。
     * （多行插入时新加的行保持 gen=0，等用到时再折行。） */
    p->doc.lines[a.line].gen = g;
    if (e.line != a.line)
        ta_change_end(w, &ch, a.off, p->doc.lines[a.line].len - a.off, 0);
    else
        ta_change_end(w, &ch, a.off, len, len);
    return e;
}

/* 删除 [a, b)（不记录撤销） */
static void ta_do_delete(sk_widget *w, ta_pos a, ta_pos b)
{
    ta_priv *p = TA(w);
    if (ta_pos_cmp(a, b) >= 0)
        return;
    ta_change ch;
    ta_change_begin(w, &ch, a.line, a.off);
    unsigned g = p->doc.lines[a.line].gen;
    int delta = a.line == b.line ? -(b.off - a.off) : 0;
    ta_doc_delete(&p->doc, a, b);
    p->doc.lines[a.line].gen = g;      /* 同上：a 之前的折行仍有效 */
    if (a.line != b.line)
        ta_change_end(w, &ch, a.off, p->doc.lines[a.line].len - a.off, 0);
    else
        ta_change_end(w, &ch, a.off, 0, delta);
}

/* 内容变了之后的收尾：滚动范围、光标、通知 */
static void ta_after_edit(sk_widget *w)
{
    ta_priv *p = TA(w);
    int mr = ta_max_scroll(w);
    if (p->scroll_row > mr) {
        p->scroll_row = mr;
        ta_inval_all(w);
    }
    if (p->scroll_x > ta_max_scroll_x(w)) {
        p->scroll_x = ta_max_scroll_x(w);
        ta_inval_all(w);
    }
    ta_ensure_visible(w);
    ta_update_caret(w);
    if (!p->wrap)
        ta_inval_bars(w);
    p->notify = 1;
}

/*
 * 用户的一次编辑：删除 [a, b)，再在 a 处插入 text，光标放到插入内容之后。
 * kind 决定能否与上一组撤销记录合并（连续打字/连续退格/连续 Delete）。
 */
static void ta_edit(sk_widget *w, ta_pos a, ta_pos b, const char *text, int len, int kind)
{
    ta_priv *p = TA(w);
    ta_undo *u = &p->undo;
    ta_pos c0 = p->caret, a0 = p->anchor;
    int has_del = ta_pos_cmp(a, b) < 0;
    if (!has_del && len <= 0)
        return;
    char *deleted = NULL;
    int dlen = 0;
    if (has_del) {
        deleted = ta_doc_get_range(&p->doc, a, b, &dlen);
        if (!deleted)
            return;
    }
    uint64_t now = ta_now(w);

    /* ---- 尝试并入上一组 ---- */
    ta_group *top = (u->top == u->n && u->top > 0) ? &u->g[u->top - 1] : NULL;
    int merged = 0;
    if (top && top->kind == kind && kind != TA_K_OTHER && top->seq != u->saved_seq &&
        now - top->time < TA_MERGE_MS && top->nops >= 1) {
        ta_op *op = &top->ops[top->nops - 1];
        if (kind == TA_K_TYPE && !has_del && op->ins &&
            ta_pos_cmp(ta_pos_after(op->a, op->text, op->len), a) == 0) {
            /* 按词分组：空格之后开始打一个新词时另起一组 */
            int new_word = op->len > 0 && (op->text[op->len - 1] == ' ') && text[0] != ' ';
            if (!new_word) {
                char *t = realloc(op->text, op->len + len + 1);
                if (t) {
                    memcpy(t + op->len, text, len);
                    op->len += len;
                    t[op->len] = 0;
                    op->text = t;
                    u->bytes += len;
                    merged = 1;
                }
            }
        } else if (kind == TA_K_BACK && has_del && len == 0 && top->nops == 1 && !op->ins &&
                   ta_pos_cmp(op->a, b) == 0) {
            char *t = malloc(op->len + dlen + 1);
            if (t) {
                memcpy(t, deleted, dlen);
                memcpy(t + dlen, op->text, op->len);
                t[op->len + dlen] = 0;
                free(op->text);
                op->text = t;
                op->len += dlen;
                op->a = a;
                u->bytes += dlen;
                merged = 1;
            }
        } else if (kind == TA_K_DEL && has_del && len == 0 && top->nops == 1 && !op->ins &&
                   ta_pos_cmp(op->a, a) == 0) {
            char *t = realloc(op->text, op->len + dlen + 1);
            if (t) {
                memcpy(t + op->len, deleted, dlen);
                op->len += dlen;
                t[op->len] = 0;
                op->text = t;
                u->bytes += dlen;
                merged = 1;
            }
        }
    }

    /* ---- 真正修改文档 ---- */
    if (has_del)
        ta_do_delete(w, a, b);
    ta_pos end = len > 0 ? ta_do_insert(w, a, text, len) : a;

    ta_group *g = NULL;
    if (merged) {
        g = top;
        free(deleted);
    } else {
        g = ta_undo_push(u, kind);
        if (g) {
            g->caret0 = c0;
            g->anchor0 = a0;
            if (has_del)
                ta_group_add(u, g, 0, a, deleted, dlen);
            else
                free(deleted);
            if (len > 0) {
                char *t = malloc(len + 1);
                if (t) {
                    memcpy(t, text, len);
                    t[len] = 0;
                    ta_group_add(u, g, 1, a, t, len);
                }
            }
        } else {
            free(deleted);
        }
    }
    if (g) {
        g->time = now;
        g->caret1 = g->anchor1 = end;
        ta_undo_trim(u);
    }

    /* 被删除的选区所在的行已经由 ta_do_delete 失效了，这里只需要放好光标 */
    p->caret = p->anchor = end;
    p->caret_eol = 0;
    p->pref_x = -1;
    ta_after_edit(w);
}

/* 撤销 / 重做 */
static int ta_undo_apply(sk_widget *w, int redo)
{
    ta_priv *p = TA(w);
    ta_undo *u = &p->undo;
    ta_group *g;
    if (redo ? u->top >= u->n : u->top <= 0)
        return 0;
    /* 先擦掉当前选区的荧光笔（文档改动之后这些位置就没有意义了） */
    ta_pos lo0 = ta_pos_min(p->caret, p->anchor), hi0 = ta_pos_max(p->caret, p->anchor);
    if (ta_pos_cmp(lo0, hi0) != 0)
        ta_inval_rows(w, ta_global_row(&p->doc, lo0, 0), ta_global_row(&p->doc, hi0, 1));
    if (redo) {
        g = &u->g[u->top];
        for (int i = 0; i < g->nops; i++) {
            ta_op *op = &g->ops[i];
            if (op->ins)
                ta_do_insert(w, op->a, op->text, op->len);
            else
                ta_do_delete(w, op->a, ta_pos_after(op->a, op->text, op->len));
        }
        u->top++;
    } else {
        g = &u->g[u->top - 1];
        for (int i = g->nops - 1; i >= 0; i--) {
            ta_op *op = &g->ops[i];
            if (op->ins)
                ta_do_delete(w, op->a, ta_pos_after(op->a, op->text, op->len));
            else
                ta_do_insert(w, op->a, op->text, op->len);
        }
        u->top--;
    }
    p->caret = redo ? g->caret1 : g->caret0;
    p->anchor = redo ? g->anchor1 : g->anchor0;
    /* 防御：位置必须在文档范围内 */
    if (p->caret.line >= p->doc.n) p->caret = ta_pos_make(p->doc.n - 1, p->doc.lines[p->doc.n - 1].len);
    if (p->anchor.line >= p->doc.n) p->anchor = p->caret;
    if (p->caret.off > p->doc.lines[p->caret.line].len) p->caret.off = p->doc.lines[p->caret.line].len;
    if (p->anchor.off > p->doc.lines[p->anchor.line].len) p->anchor.off = p->doc.lines[p->anchor.line].len;
    p->caret_eol = 0;
    p->pref_x = -1;
    /* 撤销后恢复的选区（例如撤销“替换选中文字”会重新选中原来的文字） */
    lo0 = ta_pos_min(p->caret, p->anchor);
    hi0 = ta_pos_max(p->caret, p->anchor);
    if (ta_pos_cmp(lo0, hi0) != 0)
        ta_inval_rows(w, ta_global_row(&p->doc, lo0, 0), ta_global_row(&p->doc, hi0, 1));
    ta_after_edit(w);
    return 1;
}

/* 用 text 替换选区（没有选区就是在光标处插入） */
static void ta_replace_sel(sk_widget *w, const char *text, int len, int kind)
{
    ta_priv *p = TA(w);
    ta_pos a = ta_pos_min(p->caret, p->anchor), b = ta_pos_max(p->caret, p->anchor);
    ta_edit(w, a, b, text, len, kind);
}

/* ================================================================ 光标移动的计算 */

static ta_pos ta_doc_end(ta_doc *d) { return ta_pos_make(d->n - 1, d->lines[d->n - 1].len); }

static ta_pos ta_left_of(ta_doc *d, ta_pos c)
{
    if (c.off > 0)
        return ta_pos_make(c.line, ta_prev_off(d->lines[c.line].s, c.off));
    if (c.line > 0)
        return ta_pos_make(c.line - 1, d->lines[c.line - 1].len);
    return c;
}

static ta_pos ta_right_of(ta_doc *d, ta_pos c)
{
    ta_line *L = &d->lines[c.line];
    if (c.off < L->len)
        return ta_pos_make(c.line, ta_next_off(L->s, L->len, c.off));
    if (c.line < d->n - 1)
        return ta_pos_make(c.line + 1, 0);
    return c;
}

/* 按词向左/右（Ctrl+←/→）：先跳过空白，再跳过一串同类字符。到行首/行尾时跨行 */
static ta_pos ta_word_left(ta_doc *d, ta_pos c)
{
    ta_line *L = &d->lines[c.line];
    if (c.off == 0)
        return ta_left_of(d, c);
    int p = c.off;
    while (p > 0 && ta_word_class(ta_char_at(L->s, L->len, ta_prev_off(L->s, p))) == 0)
        p = ta_prev_off(L->s, p);
    if (p > 0) {
        int cls = ta_word_class(ta_char_at(L->s, L->len, ta_prev_off(L->s, p)));
        while (p > 0 && ta_word_class(ta_char_at(L->s, L->len, ta_prev_off(L->s, p))) == cls)
            p = ta_prev_off(L->s, p);
    }
    return ta_pos_make(c.line, p);
}

static ta_pos ta_word_right(ta_doc *d, ta_pos c)
{
    ta_line *L = &d->lines[c.line];
    if (c.off >= L->len)
        return ta_right_of(d, c);
    int p = c.off;
    int cls = ta_word_class(ta_char_at(L->s, L->len, p));
    if (cls != 0)
        while (p < L->len && ta_word_class(ta_char_at(L->s, L->len, p)) == cls)
            p = ta_next_off(L->s, L->len, p);
    while (p < L->len && ta_word_class(ta_char_at(L->s, L->len, p)) == 0)
        p = ta_next_off(L->s, L->len, p);
    return ta_pos_make(c.line, p);
}

/* 位置所在的“词”（一串同类字符；中文就是一串连续的汉字） */
static void ta_word_at(ta_doc *d, ta_pos c, ta_pos *a, ta_pos *b)
{
    ta_line *L = &d->lines[c.line];
    *a = *b = c;
    if (L->len == 0)
        return;
    int at = c.off;
    if (at >= L->len)
        at = ta_prev_off(L->s, L->len);
    int cls = ta_word_class(ta_char_at(L->s, L->len, at));
    int s = at, e = ta_next_off(L->s, L->len, at);
    while (s > 0) {
        int q = ta_prev_off(L->s, s);
        if (ta_word_class(ta_char_at(L->s, L->len, q)) != cls)
            break;
        s = q;
    }
    while (e < L->len && ta_word_class(ta_char_at(L->s, L->len, e)) == cls)
        e = ta_next_off(L->s, L->len, e);
    a->off = s;
    b->off = e;
}

/* 竖直移动 drow 个显示行，保持 pref_x */
static void ta_move_vertical(sk_widget *w, int drow, int extend)
{
    ta_priv *p = TA(w);
    ta_doc *d = &p->doc;
    if (p->pref_x < 0)
        p->pref_x = ta_pos_x(d, p->caret, p->caret_eol);
    int g = ta_global_row(d, p->caret, p->caret_eol) + drow;
    int total = ta_total_rows(d);
    if (g < 0) {
        ta_move(w, ta_pos_make(0, 0), 0, extend, 0);
        return;
    }
    if (g >= total) {
        ta_move(w, ta_doc_end(d), 0, extend, 0);
        return;
    }
    int li, k;
    ta_row_lookup(d, g, &li, &k);
    ta_line *L = ta_L(d, li);
    int s = ta_row_start(L, k), e = ta_row_end(L, k);
    int off = ta_hit(d, L, s, e, p->pref_x);
    int eol = off == e && k < L->nrows - 1;
    ta_move(w, ta_pos_make(li, off), eol, extend, 1);
}

/* ================================================================ 剪贴板 */

static void ta_copy(sk_widget *w)
{
    ta_priv *p = TA(w);
    if (!ta_has_sel(p) || !w->ui)
        return;
    int len;
    char *t = ta_doc_get_range(&p->doc, ta_pos_min(p->caret, p->anchor), ta_pos_max(p->caret, p->anchor), &len);
    if (!t)
        return;
    /* 系统剪贴板最多 SK_CLIP_MAX 字节：在字符边界上截断，避免半个汉字 */
    if (len > SK_CLIP_MAX) {
        len = SK_CLIP_MAX;
        while (len > 0 && ((unsigned char)t[len] & 0xC0) == 0x80)
            len--;
    }
    sk_widget_set_clipboard(w, t, len);
    free(t);
}

static void ta_paste_text(sk_widget *w, const char *t, int len)
{
    ta_priv *p = TA(w);
    if (p->readonly || len <= 0)
        return;
    int clen = len;
    char *clean = ta_clean_text(t, len, &clen);
    ta_replace_sel(w, clean ? clean : t, clean ? clen : len, TA_K_OTHER);
    free(clean);
}

/* ================================================================ 绘制 */

static void ta_rough_hline(sk_canvas *c, float x0, float x1, float y, sk_color col, uint32_t seed)
{
    sk_rough_style s;
    sk_rough_default(&s);
    s.roughness = 0.35f;
    s.bowing = 0.4f;
    s.stroke_width = 1.0f;
    s.double_stroke = 0;
    s.stroke = col;
    s.seed = seed;
    sk_rough_line(c, x0, y, x1, y, &s);
}

/* 一个显示行的文字：逐字画（为了制表符和水平裁剪），不在裁剪范围内的字只测量不画 */
static void ta_draw_row_text(sk_canvas *c, ta_priv *p, const ta_line *L, int s, int e,
                             int x0, int baseline, sk_color ink)
{
    int x = 0, clen;
    int cl = c->clip.x - 6, cr = c->clip.x + c->clip.w + 6;
    for (int off = s; off < e; off += clen) {
        uint32_t cp = ta_decode(L->s + off, L->len - off, &clen);
        int adv = ta_adv_at(&p->doc.m, cp, x);
        int gx = x0 + x;
        if (gx > cr)
            break;
        if (gx + adv >= cl && cp != '\t' && cp != ' ')
            sk_draw_glyph(c, p->font, gx, baseline, cp, ink);
        x += adv;
    }
}

/* 铅笔光标：一道略微倾斜、上细下粗的短笔画 */
static void ta_draw_caret(sk_canvas *c, sk_rect r)
{
    float x = r.x + 2.5f;
    float y0 = r.y + 1.0f, y1 = (float)(r.y + r.h - 1);
    sk_line_aa(c, x + 0.9f, y0, x - 0.1f, y1, 1.6f, SK_INK);
    sk_line_aa(c, x - 0.1f, y1 - 1.0f, x + 0.3f, y1, 2.2f, SK_INK);
}

/*
 * 滚动条滑块的位置和长度（相对滚动条起点，像素）。必须和 theme.c 的 sk_draw_scrollbar
 * 用同一套公式，鼠标才能“抓”在画出来的滑块上：轨道长 = 长度 - 4，滑块最短 22 像素，
 * 从起点 +2 开始。total/visible/offset 用同一单位（这里是显示行或像素）。
 */
static void ta_thumb(sk_rect bar, int vertical, int total, int visible, int offset, int *pos, int *len)
{
    float track = (float)(vertical ? bar.h : bar.w) - 4.0f;
    if (track < 1.0f)
        track = 1.0f;
    if (total <= visible || visible <= 0) {
        *pos = 2;
        *len = (int)track;
        return;
    }
    float tl = track * (float)visible / (float)total;
    float minl = track < 22.0f ? track : 22.0f;
    if (tl < minl)
        tl = minl;
    int maxoff = total - visible;
    int off = offset < 0 ? 0 : offset > maxoff ? maxoff : offset;
    *pos = 2 + (int)((track - tl) * (float)off / (float)maxoff);
    *len = (int)tl;
}

/* 拖动滑块：滑块起点（相对滚动条起点）→ 滚动偏移 */
static int ta_thumb_to_offset(sk_rect bar, int vertical, int total, int visible, int thumb_pos)
{
    int pos, len;
    ta_thumb(bar, vertical, total, visible, 0, &pos, &len);
    float range = (float)(vertical ? bar.h : bar.w) - 4.0f - (float)len;
    if (range <= 0 || total <= visible)
        return 0;
    float off = (float)(thumb_pos - 2) * (float)(total - visible) / range;
    return (int)(off + 0.5f);
}

static void ta_draw(sk_widget *w, sk_canvas *c)
{
    ta_priv *p = TA(w);
    if (!p)
        return;
    if (sk_widget_get_font(w) != p->font) {
        ta_layout(w);
        ta_inval_all(w);
    }
    ta_doc *d = &p->doc;
    sk_rect clip0 = c->clip;
    sk_rect area = sk_rect_intersect(clip0, p->inner);

    if (!sk_rect_empty(area)) {
        sk_rect old = sk_push_clip(c, p->inner);
        /* 纸：控件内部总是白纸，不管窗口背景是什么颜色（颗粒强度与 sk_ui 的默认背景一致） */
        sk_paper_fill(c, area, SK_PAPER, 3, w->seed);

        /* 与脏区域相交的显示行（多算上下各一行：手绘横线会抖出自己那一行一两个像素） */
        int g_first = p->scroll_row + ta_floor_div(area.y - p->text_r.y, p->row_h) - 1;
        int g_last = p->scroll_row + ta_floor_div(area.y + area.h - p->text_r.y, p->row_h) + 1;
        if (g_first < p->scroll_row - 1)
            g_first = p->scroll_row - 1;
        int total = ta_total_rows(d);
        /* 文字（含最下面露出一半的那一行）能画到哪里：不换行时下面是横向滚动条 */
        int text_bottom = p->wrap ? p->inner.y + p->inner.h : p->hbar.y - 1;
        sk_rect rows_clip = sk_rect_make(p->inner.x, p->inner.y, p->inner.w, text_bottom - p->inner.y);

        /* 1. 淡蓝色横线（印在纸上的，跟着文字一起滚动：种子由显示行号决定） */
        for (int g = g_first; g <= g_last; g++) {
            int y = ta_row_y(p, g) + p->row_h - 1;
            if (y < p->text_r.y || y > text_bottom - 2)
                continue;
            ta_rough_hline(c, (float)p->inner.x + 1, (float)p->vbar.x - 3, (float)y, SK_DESK_GRID,
                           sk_hash_u32(w->seed, (uint32_t)g));
        }
        /* 2. 红色边线 */
        {
            sk_rough_style s;
            sk_rough_default(&s);
            s.roughness = 0.3f;
            s.bowing = 0.3f;
            s.stroke_width = 1.1f;
            s.double_stroke = 0;
            s.stroke = SK_WITH_ALPHA(SK_RED, 0x80);
            s.seed = w->seed ^ 0xA5A5u;
            sk_rough_line(c, (float)p->margin_x, (float)p->inner.y + 1, (float)p->margin_x,
                          (float)(text_bottom - 1), &s);
        }
        sk_rect orows = sk_push_clip(c, rows_clip);

        /* 3. 文字、选区、行号 */
        ta_pos lo = ta_pos_min(p->caret, p->anchor), hi = ta_pos_max(p->caret, p->anchor);
        int has_sel = ta_pos_cmp(lo, hi) != 0;
        sk_color ink = sk_widget_ink(w, SK_INK);
        /* 没有焦点时选区颜色淡一些（和单行输入框一致） */
        sk_color hl = (w->state & SK_STATE_FOCUSED) ? SK_HIGHLIGHT : SK_WITH_ALPHA(SK_HIGHLIGHT, 0x48);
        int text_x0 = p->text_r.x - p->scroll_x;
        int g0 = g_first < p->scroll_row ? p->scroll_row : g_first;
        if (g0 < total) {
            int li, k;
            ta_row_lookup(d, g0, &li, &k);
            sk_rect text_clip = sk_rect_make(p->margin_x + 2, p->inner.y, p->vbar.x - 2 - (p->margin_x + 2), p->inner.h);
            for (int g = g0; g <= g_last && g < total; g++) {
                ta_line *L = ta_L(d, li);
                int y = ta_row_y(p, g);
                int baseline = y + (p->row_h - (p->ascent + p->descent)) / 2 + p->ascent;
                int s = ta_row_start(L, k), e = ta_row_end(L, k);
                sk_rect oc = sk_push_clip(c, text_clip);
                /* 选区：黄色荧光笔，画在文字下面 */
                if (has_sel) {
                    ta_pos rs = ta_pos_make(li, s), re = ta_pos_make(li, e);
                    if (ta_pos_cmp(hi, rs) > 0 && ta_pos_cmp(lo, re) <= 0) {
                        int a = ta_pos_cmp(lo, rs) > 0 ? lo.off : s;
                        int b = ta_pos_cmp(hi, re) < 0 ? hi.off : e;
                        int xa = ta_measure(d, L, s, a), xb = ta_measure(d, L, s, b);
                        /* 选区跨过了行尾的换行符：多涂一小段表示“换行也选中了” */
                        if (k == L->nrows - 1 && hi.line > li)
                            xb += 6;
                        if (xb > xa)
                            sk_marker_highlight(c, sk_rect_make(text_x0 + xa - 1, y + 3, xb - xa + 2, p->row_h - 6),
                                                hl, sk_hash_u32(w->seed ^ 0x51u, (uint32_t)g));
                    }
                }
                ta_draw_row_text(c, p, L, s, e, text_x0, baseline, ink);
                sk_pop_clip(c, oc);
                /* 行号（只在逻辑行的第一个显示行） */
                if (p->numbers && k == 0) {
                    char num[16];
                    int n = li + 1, len = 0;
                    char tmp[16];
                    while (n > 0) {
                        tmp[len++] = (char)('0' + n % 10);
                        n /= 10;
                    }
                    for (int i = 0; i < len; i++)
                        num[i] = tmp[len - 1 - i];
                    num[len] = 0;
                    int tw = sk_text_width(p->font, num);
                    sk_draw_text(c, p->font, p->margin_x - 6 - tw, baseline, num,
                                 li == p->caret.line ? SK_INK_SOFT : SK_INK_FAINT);
                }
                if (++k >= L->nrows) {
                    k = 0;
                    if (++li >= d->n)
                        break;
                }
            }
        }

        /* 4. 光标 */
        sk_rect cr;
        if (w->ui && sk_ui_caret_visible(w) && ta_caret_rect(w, &cr)) {
            sk_rect oc = sk_push_clip(c, sk_rect_make(p->margin_x + 1, p->inner.y, p->vbar.x - p->margin_x - 1, p->inner.h));
            ta_draw_caret(c, cr);
            sk_pop_clip(c, oc);
        }
        sk_pop_clip(c, orows);
        sk_pop_clip(c, old);
    }

    /* 5. 滚动条 */
    {
        int total = ta_total_rows(d);
        int vis = p->vis_rows;
        if (sk_rect_intersect(clip0, sk_rect_inset(p->vbar, -3, -3)).w > 0)
            sk_draw_scrollbar(c, p->vbar, total > vis ? total : vis, vis, p->scroll_row,
                              p->drag == TA_DRAG_VBAR ? SK_STATE_PRESSED : 0, w->seed ^ 0x77u);
        if (!p->wrap && sk_rect_intersect(clip0, sk_rect_inset(p->hbar, -3, -3)).w > 0) {
            int tw = ta_doc_max_width(d) + d->m.tab_w;
            sk_draw_scrollbar(c, p->hbar, tw > p->text_r.w ? tw : p->text_r.w, p->text_r.w, p->scroll_x,
                              p->drag == TA_DRAG_HBAR ? SK_STATE_PRESSED : 0, w->seed ^ 0x99u);
        }
    }
    /* 6. 外框 */
    sk_draw_textbox_frame(c, w->r, (int)w->state, w->seed);
}

/* ================================================================ 鼠标 */

static ta_pos ta_pos_from_xy(sk_widget *w, int x, int y, int *eol)
{
    ta_priv *p = TA(w);
    ta_doc *d = &p->doc;
    int g = p->scroll_row + ta_floor_div(y - p->text_r.y, p->row_h);
    int total = ta_total_rows(d);
    *eol = 0;
    if (g < 0)
        return ta_pos_make(0, 0);
    if (g >= total)
        g = total - 1;
    int li, k;
    ta_row_lookup(d, g, &li, &k);
    ta_line *L = ta_L(d, li);
    int s = ta_row_start(L, k), e = ta_row_end(L, k);
    int off = ta_hit(d, L, s, e, x - p->text_r.x + p->scroll_x);
    *eol = off == e && k < L->nrows - 1;
    return ta_pos_make(li, off);
}

/* 拖动选择到 (x, y) */
static void ta_drag_to(sk_widget *w, int x, int y)
{
    ta_priv *p = TA(w);
    int eol;
    ta_pos np = ta_pos_from_xy(w, x, y, &eol);
    if (p->word_drag) {
        ta_pos a, b;
        ta_word_at(&p->doc, np, &a, &b);
        ta_pos lo0 = ta_pos_min(p->caret, p->anchor), hi0 = ta_pos_max(p->caret, p->anchor);
        if (ta_pos_cmp(np, p->word_a) < 0) {
            p->anchor = p->word_b;
            p->caret = a;
        } else if (ta_pos_cmp(np, p->word_b) > 0) {
            p->anchor = p->word_a;
            p->caret = b;
        } else {
            p->anchor = p->word_a;
            p->caret = p->word_b;
        }
        p->caret_eol = 0;
        p->pref_x = -1;
        ta_inval_sel_change(w, lo0, hi0, ta_pos_min(p->caret, p->anchor), ta_pos_max(p->caret, p->anchor));
        ta_ensure_visible(w);
        ta_update_caret(w);
        p->notify = 1;
        return;
    }
    if (ta_pos_cmp(np, p->caret) != 0 || eol != p->caret_eol)
        ta_move(w, np, eol, 1, 0);
}

/* 拖选时鼠标在文字区上方/下方：定时器自动滚动 */
static void ta_autoscroll_tick(sk_widget *w)
{
    ta_priv *p = TA(w);
    if (p->drag != TA_DRAG_TEXT) {
        sk_widget_remove_timer(w, p->autoscroll_timer);
        p->autoscroll_timer = 0;
        return;
    }
    int dy = 0;
    if (p->mouse_y < p->text_r.y)
        dy = -1 - (p->text_r.y - p->mouse_y) / p->row_h;
    else if (p->mouse_y >= p->text_r.y + p->text_r.h)
        dy = 1 + (p->mouse_y - p->text_r.y - p->text_r.h) / p->row_h;
    int dx = 0;
    if (!p->wrap) {
        if (p->mouse_x < p->text_r.x)
            dx = -p->doc.m.tab_w;
        else if (p->mouse_x > p->text_r.x + p->text_r.w)
            dx = p->doc.m.tab_w;
    }
    if (!dy && !dx) {
        sk_widget_remove_timer(w, p->autoscroll_timer);
        p->autoscroll_timer = 0;
        return;
    }
    ta_scroll_to(w, p->scroll_row + dy, p->scroll_x + dx);
    ta_drag_to(w, p->mouse_x, p->mouse_y);
    if (p->notify && w->on_change) {
        p->notify = 0;
        w->on_change(w, w->user);
    }
}

static int ta_vbar_hit(ta_priv *p, int x, int y)
{
    return x >= p->vbar.x - 2 && x < p->vbar.x + p->vbar.w + 3 && y >= p->inner.y && y < p->vbar.y + p->vbar.h + 2;
}
static int ta_hbar_hit(ta_priv *p, int x, int y)
{
    return !p->wrap && y >= p->hbar.y - 1 && y < p->hbar.y + p->hbar.h + 2 && x >= p->hbar.x && x < p->hbar.x + p->hbar.w;
}

static int ta_mouse(sk_widget *w, const sk_event *e)
{
    ta_priv *p = TA(w);
    int x = e->mouse.x, y = e->mouse.y;
    p->mouse_x = x;
    p->mouse_y = y;
    switch (e->mouse.kind) {
    case SK_MOUSE_DOWN: {
        if (e->mouse.button != SK_BUTTON_LEFT)
            return e->mouse.button == SK_BUTTON_RIGHT ? 0 : 1;
        int total = ta_total_rows(&p->doc);
        if (ta_vbar_hit(p, x, y)) {
            int pos, len;
            int t = total > p->vis_rows ? total : p->vis_rows;
            ta_thumb(p->vbar, 1, t, p->vis_rows, p->scroll_row, &pos, &len);
            int ty = y - p->vbar.y;
            if (ty >= pos && ty < pos + len) {
                p->drag = TA_DRAG_VBAR;
                p->drag_grab = ty - pos;
                ta_inval_bars(w);
            } else {
                ta_scroll_to(w, p->scroll_row + (ty < pos ? -(p->vis_rows - 1) : p->vis_rows - 1), p->scroll_x);
            }
            return 1;
        }
        if (ta_hbar_hit(p, x, y)) {
            int pos, len;
            int tw = ta_doc_max_width(&p->doc) + p->doc.m.tab_w;
            if (tw < p->text_r.w)
                tw = p->text_r.w;
            ta_thumb(p->hbar, 0, tw, p->text_r.w, p->scroll_x, &pos, &len);
            int tx = x - p->hbar.x;
            if (tx >= pos && tx < pos + len) {
                p->drag = TA_DRAG_HBAR;
                p->drag_grab = tx - pos;
                ta_inval_bars(w);
            } else {
                ta_scroll_to(w, p->scroll_row, p->scroll_x + (tx < pos ? -p->text_r.w * 3 / 4 : p->text_r.w * 3 / 4));
            }
            return 1;
        }
        int eol;
        ta_pos np = ta_pos_from_xy(w, x, y, &eol);
        p->drag = TA_DRAG_TEXT;
        p->word_drag = 0;
        if (e->mouse.clicks >= 2) {
            ta_word_at(&p->doc, np, &p->word_a, &p->word_b);
            p->word_drag = 1;
            ta_pos lo0 = ta_pos_min(p->caret, p->anchor), hi0 = ta_pos_max(p->caret, p->anchor);
            p->anchor = p->word_a;
            p->caret = p->word_b;
            p->caret_eol = 0;
            p->pref_x = -1;
            ta_inval_sel_change(w, lo0, hi0, p->word_a, p->word_b);
            ta_ensure_visible(w);
            ta_update_caret(w);
            p->notify = 1;
        } else {
            ta_move(w, np, eol, (e->mouse.modifiers & SK_MOD_SHIFT) != 0, 0);
        }
        return 1;
    }
    case SK_MOUSE_MOVE:
        if (p->drag == TA_DRAG_TEXT && (e->mouse.buttons & SK_BUTTON_LEFT)) {
            ta_drag_to(w, x, y);
            int outside = y < p->text_r.y || y >= p->text_r.y + p->text_r.h ||
                          (!p->wrap && (x < p->text_r.x || x > p->text_r.x + p->text_r.w));
            if (outside && !p->autoscroll_timer && w->ui)
                p->autoscroll_timer = sk_widget_add_timer(w, TA_AUTOSCROLL_MS, 1, ta_autoscroll_tick);
            return 1;
        }
        if (p->drag == TA_DRAG_VBAR) {
            int total = ta_total_rows(&p->doc);
            int t = total > p->vis_rows ? total : p->vis_rows;
            int row = ta_thumb_to_offset(p->vbar, 1, t, p->vis_rows, y - p->vbar.y - p->drag_grab);
            ta_scroll_to(w, row, p->scroll_x);
            return 1;
        }
        if (p->drag == TA_DRAG_HBAR) {
            int tw = ta_doc_max_width(&p->doc) + p->doc.m.tab_w;
            if (tw < p->text_r.w)
                tw = p->text_r.w;
            int sx = ta_thumb_to_offset(p->hbar, 0, tw, p->text_r.w, x - p->hbar.x - p->drag_grab);
            ta_scroll_to(w, p->scroll_row, sx);
            return 1;
        }
        /* 悬停：滚动条上显示箭头，文字上显示 I 形光标 */
        if (w->ui)
            sk_widget_set_cursor(w, (ta_vbar_hit(p, x, y) || ta_hbar_hit(p, x, y)) ? SK_CURSOR_ARROW : SK_CURSOR_TEXT);
        return 1;
    case SK_MOUSE_UP:
        if (p->drag == TA_DRAG_VBAR || p->drag == TA_DRAG_HBAR)
            ta_inval_bars(w);
        p->drag = TA_DRAG_NONE;
        p->word_drag = 0;
        if (p->autoscroll_timer) {
            sk_widget_remove_timer(w, p->autoscroll_timer);
            p->autoscroll_timer = 0;
        }
        return 1;
    case SK_MOUSE_WHEEL: {
        int amount = e->mouse.wheel;
        if ((e->mouse.modifiers & SK_MOD_SHIFT) && !p->wrap)
            ta_scroll_to(w, p->scroll_row, p->scroll_x - amount * 3 * p->doc.m.tab_w);
        else
            ta_scroll_to(w, p->scroll_row - amount * TA_WHEEL_ROWS, p->scroll_x);
        return 1;
    }
    case SK_MOUSE_ENTER:
    case SK_MOUSE_LEAVE:
        return 1;
    }
    return 0;
}

/* ================================================================ 键盘 */

static int ta_ctrl_letter(const sk_event *e)
{
    switch (e->key.keycode) {
    case KEY_A: return 'a';
    case KEY_C: return 'c';
    case KEY_X: return 'x';
    case KEY_V: return 'v';
    case KEY_Z: return 'z';
    case KEY_Y: return 'y';
    }
    uint32_t cp = e->key.codepoint;
    if (cp >= 'A' && cp <= 'Z')
        cp += 'a' - 'A';
    return (e->key.keycode == 0 && cp >= 'a' && cp <= 'z') ? (int)cp : 0;
}

static void ta_select_all(sk_widget *w)
{
    ta_priv *p = TA(w);
    ta_pos lo0 = ta_pos_min(p->caret, p->anchor), hi0 = ta_pos_max(p->caret, p->anchor);
    p->anchor = ta_pos_make(0, 0);
    p->caret = ta_doc_end(&p->doc);
    p->caret_eol = 0;
    p->pref_x = -1;
    ta_inval_sel_change(w, lo0, hi0, p->anchor, p->caret);
    ta_ensure_visible(w);
    ta_update_caret(w);
    p->notify = 1;
}

static void ta_insert_char(sk_widget *w, uint32_t cp)
{
    char buf[4];
    int n = sk_utf8_encode(cp, buf);
    ta_replace_sel(w, buf, n, TA_K_TYPE);
}

static void ta_backspace(sk_widget *w, int word)
{
    ta_priv *p = TA(w);
    if (ta_has_sel(p)) {
        ta_replace_sel(w, "", 0, TA_K_OTHER);
        return;
    }
    ta_pos a = word ? ta_word_left(&p->doc, p->caret) : ta_left_of(&p->doc, p->caret);
    ta_edit(w, a, p->caret, "", 0, word ? TA_K_OTHER : TA_K_BACK);
}

static void ta_delete_fwd(sk_widget *w, int word)
{
    ta_priv *p = TA(w);
    if (ta_has_sel(p)) {
        ta_replace_sel(w, "", 0, TA_K_OTHER);
        return;
    }
    ta_pos b = word ? ta_word_right(&p->doc, p->caret) : ta_right_of(&p->doc, p->caret);
    ta_edit(w, p->caret, b, "", 0, word ? TA_K_OTHER : TA_K_DEL);
}

static int ta_key(sk_widget *w, const sk_event *e)
{
    ta_priv *p = TA(w);
    ta_doc *d = &p->doc;
    unsigned mods = e->key.modifiers;
    int shift = (mods & SK_MOD_SHIFT) != 0, ctrl = (mods & SK_MOD_CTRL) != 0;
    uint32_t kc = e->key.keycode, cp = e->key.codepoint;
    if (mods & (SK_MOD_ALT | SK_MOD_SUPER))
        return 0;
    if (e->key.state == 0) {
        /* 松开：普通字符的松开也算我们的（和单行输入框一样，免得应用收到一串莫名其妙的松开事件） */
        return !ctrl && !p->readonly && cp >= 0x20 && cp != 0x7F;
    }

    if (ctrl) {
        switch (ta_ctrl_letter(e)) {
        case 'a': ta_select_all(w); return 1;
        case 'c': ta_copy(w); return 1;
        case 'x':
            if (p->readonly) return 0;
            ta_copy(w);
            if (ta_has_sel(p))
                ta_replace_sel(w, "", 0, TA_K_OTHER);
            return 1;
        case 'v':
            if (p->readonly || !w->ui) return 0;
            p->paste_pending++;
            sk_widget_request_clipboard(w);
            return 1;
        case 'z':
            if (p->readonly) return 0;
            ta_undo_apply(w, shift);
            return 1;
        case 'y':
            if (p->readonly) return 0;
            ta_undo_apply(w, 1);
            return 1;
        }
        switch (kc) {
        case KEY_HOME: ta_move(w, ta_pos_make(0, 0), 0, shift, 0); return 1;
        case KEY_END:  ta_move(w, ta_doc_end(d), 0, shift, 0); return 1;
        case KEY_LEFT: ta_move(w, ta_word_left(d, p->caret), 0, shift, 0); return 1;
        case KEY_RIGHT: ta_move(w, ta_word_right(d, p->caret), 0, shift, 0); return 1;
        case KEY_UP:   ta_scroll_to(w, p->scroll_row - 1, p->scroll_x); return 1;
        case KEY_DOWN: ta_scroll_to(w, p->scroll_row + 1, p->scroll_x); return 1;
        case KEY_INSERT: ta_copy(w); return 1;
        case KEY_BACKSPACE: if (p->readonly) return 0; ta_backspace(w, 1); return 1;
        case KEY_DELETE: if (p->readonly) return 0; ta_delete_fwd(w, 1); return 1;
        }
        return 0;   /* 其它 Ctrl 组合键（Ctrl+S 保存……）交给应用 */
    }

    switch (kc) {
    case KEY_LEFT:
        if (ta_has_sel(p) && !shift)
            ta_move(w, ta_pos_min(p->caret, p->anchor), 0, 0, 0);
        else
            ta_move(w, ta_left_of(d, p->caret), 0, shift, 0);
        return 1;
    case KEY_RIGHT:
        if (ta_has_sel(p) && !shift)
            ta_move(w, ta_pos_max(p->caret, p->anchor), 0, 0, 0);
        else
            ta_move(w, ta_right_of(d, p->caret), 0, shift, 0);
        return 1;
    case KEY_UP:       ta_move_vertical(w, -1, shift); return 1;
    case KEY_DOWN:     ta_move_vertical(w, 1, shift); return 1;
    case KEY_PAGEUP:
    case KEY_PAGEDOWN: {
        int step = p->vis_rows > 1 ? p->vis_rows - 1 : 1;
        if (kc == KEY_PAGEUP)
            step = -step;
        /* 先滚动一页，再把光标移动同样的行数：光标在屏幕上的位置保持不变 */
        ta_scroll_to(w, p->scroll_row + step, p->scroll_x);
        ta_move_vertical(w, step, shift);
        return 1;
    }
    case KEY_HOME: {
        ta_line *L = ta_L(d, p->caret.line);
        int k = ta_row_in_line(L, p->caret.off, p->caret_eol);
        ta_move(w, ta_pos_make(p->caret.line, ta_row_start(L, k)), 0, shift, 0);
        return 1;
    }
    case KEY_END: {
        ta_line *L = ta_L(d, p->caret.line);
        int k = ta_row_in_line(L, p->caret.off, p->caret_eol);
        ta_move(w, ta_pos_make(p->caret.line, ta_row_end(L, k)), k < L->nrows - 1, shift, 0);
        return 1;
    }
    case KEY_INSERT:
        if (shift && !p->readonly && w->ui) {
            p->paste_pending++;
            sk_widget_request_clipboard(w);
            return 1;
        }
        return 0;
    case KEY_ESC:
        return 0;
    }

    /* 以下是编辑键：只读时不处理（交给应用，例如对话框里回车 = 确定） */
    if (p->readonly)
        return 0;
    if (kc == KEY_BACKSPACE || (kc == 0 && cp == 0x08)) {
        ta_backspace(w, 0);
        return 1;
    }
    if (kc == KEY_DELETE) {
        if (shift) {
            ta_copy(w);
            if (ta_has_sel(p))
                ta_replace_sel(w, "", 0, TA_K_OTHER);
        } else {
            ta_delete_fwd(w, 0);
        }
        return 1;
    }
    if (kc == KEY_ENTER || kc == KEY_KPENTER || cp == '\r' || cp == '\n') {
        ta_replace_sel(w, "\n", 1, TA_K_OTHER);
        return 1;
    }
    if (kc == KEY_TAB || cp == '\t') {
        if (shift)
            return 0;
        ta_replace_sel(w, "    ", TA_TAB_SPACES, TA_K_OTHER);
        return 1;
    }
    /* 可打印字符（输入法提交的中文是 keycode=0 + codepoint） */
    if (cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp < 0xA0) && cp <= 0x10FFFF &&
        !(cp >= 0xD800 && cp <= 0xDFFF)) {
        ta_insert_char(w, cp);
        return 1;
    }
    return 0;
}

/* ================================================================ 控件类 */

static int ta_event(sk_widget *w, const sk_event *e)
{
    ta_priv *p = TA(w);
    if (!p)
        return 0;
    int handled = 0;
    switch (e->type) {
    case SK_EVENT_MOUSE:
        handled = ta_mouse(w, e);
        break;
    case SK_EVENT_KEY:
        handled = ta_key(w, e);
        break;
    case SK_EVENT_FOCUS:
        /* 有效焦点变化：外框换颜色（只重画四条边），光标开始/停止闪烁 */
        {
            sk_rect r = w->r;
            int b = TA_FRAME + 3;
            ta_inval(w, sk_rect_make(r.x - 2, r.y - 2, r.w + 4, b + 2));
            ta_inval(w, sk_rect_make(r.x - 2, r.y + r.h - b, r.w + 4, b + 2));
            ta_inval(w, sk_rect_make(r.x - 2, r.y, b + 2, r.h));
            ta_inval(w, sk_rect_make(r.x + r.w - b, r.y, b + 2, r.h));
            /* 选区的荧光笔深浅也跟着焦点变 */
            if (ta_has_sel(p)) {
                ta_pos lo = ta_pos_min(p->caret, p->anchor), hi = ta_pos_max(p->caret, p->anchor);
                ta_inval_rows(w, ta_global_row(&p->doc, lo, 0), ta_global_row(&p->doc, hi, 1));
            }
            if (e->focused)
                ta_update_caret(w);
        }
        return 1;
    case SK_EVENT_CLIPBOARD:
        if (p->paste_pending <= 0)
            return 0;
        p->paste_pending--;
        ta_paste_text(w, e->clip.text, e->clip.text_len);
        handled = 1;
        break;
    default:
        return 0;
    }
    if (p->notify) {
        p->notify = 0;
        if (w->on_change)
            w->on_change(w, w->user);   /* 回调里可能销毁控件，之后不要再碰 p */
    }
    return handled;
}

static void ta_destroy(sk_widget *w)
{
    ta_priv *p = TA(w);
    if (!p)
        return;
    ta_doc_free(&p->doc);
    ta_undo_clear(&p->undo);
    free(p);
    w->priv = NULL;
}

static void ta_resized(sk_widget *w)
{
    if (!TA(w))
        return;
    ta_layout(w);
    ta_inval_all(w);
    ta_update_caret(w);
}

static const sk_widget_class ta_class = {
    .name = "textarea",
    .draw = ta_draw,
    .event = ta_event,
    .destroy = ta_destroy,
    .resized = ta_resized,
    .focusable = 1,
};

static int ta_is(const sk_widget *w) { return w && w->cls == &ta_class && w->priv; }

/* 替换全部内容：光标回到开头，清空撤销历史，“未修改” */
static void ta_set_text_internal(sk_widget *w, const char *text)
{
    ta_priv *p = TA(w);
    if (!text)
        text = "";
    int len = (int)strlen(text), clen = len;
    char *clean = ta_clean_text(text, len, &clen);
    if (ta_doc_set_text(&p->doc, clean ? clean : text, clean ? clen : len) < 0) {
        free(clean);
        return;
    }
    free(clean);
    ta_undo_clear(&p->undo);
    p->caret = p->anchor = ta_pos_make(0, 0);
    p->caret_eol = 0;
    p->pref_x = -1;
    p->scroll_row = 0;
    p->scroll_x = 0;
    ta_layout(w);
    ta_inval_all(w);
    ta_update_caret(w);
}

static void ta_text_hook(sk_widget *w, const char *text)
{
    if (ta_is(w))
        ta_set_text_internal(w, text);
}

/* ================================================================ 公共接口 */

sk_widget *sk_textarea_new(sk_ui *ui, sk_rect r, const char *initial)
{
    sk_widget *w = sk_widget_new(ui, &ta_class, r);
    if (!w)
        return NULL;
    ta_priv *p = calloc(1, sizeof(*p));
    if (!p) {
        sk_widget_destroy(w);
        return NULL;
    }
    ta_doc_init(&p->doc);
    if (p->doc.n != 1) {
        free(p);
        sk_widget_destroy(w);
        return NULL;
    }
    ta_undo_clear(&p->undo);
    p->wrap = 1;
    p->pref_x = -1;
    w->priv = p;
    sk_widget_set_flags(w, SK_WF_NO_AUTO_REDRAW | SK_WF_NO_FOCUS_RING, 0);
    sk_widget_set_cursor(w, SK_CURSOR_TEXT);
    sk_widget_set_text_hook(w, ta_text_hook);
    ta_layout(w);
    if (initial && *initial)
        ta_set_text_internal(w, initial);
    sk_widget_invalidate(w);
    return w;
}

char *sk_textarea_get_text(const sk_widget *w)
{
    if (!ta_is(w))
        return NULL;
    ta_priv *p = TA(w);
    int len;
    return ta_doc_get_range(&p->doc, ta_pos_make(0, 0), ta_doc_end(&p->doc), &len);
}

void sk_textarea_set_text(sk_widget *w, const char *text)
{
    if (ta_is(w))
        ta_set_text_internal(w, text);
}

int sk_textarea_modified(const sk_widget *w)
{
    if (!ta_is(w))
        return 0;
    const ta_undo *u = &TA(w)->undo;
    return ta_undo_state(u) != u->saved_seq;
}

void sk_textarea_set_modified(sk_widget *w, int modified)
{
    if (!ta_is(w))
        return;
    ta_undo *u = &TA(w)->undo;
    u->saved_seq = modified ? UINT_MAX : ta_undo_state(u);
}

void sk_textarea_cursor_pos(const sk_widget *w, int *line, int *col)
{
    int l = 1, c = 1;
    if (ta_is(w)) {
        ta_priv *p = TA(w);
        const ta_line *L = &p->doc.lines[p->caret.line];
        l = p->caret.line + 1;
        for (int off = 0, clen; off < p->caret.off && off < L->len; off += clen) {
            ta_decode(L->s + off, L->len - off, &clen);
            c++;
        }
    }
    if (line)
        *line = l;
    if (col)
        *col = c;
}

void sk_textarea_set_readonly(sk_widget *w, int readonly)
{
    if (ta_is(w))
        TA(w)->readonly = readonly != 0;
}

void sk_textarea_set_wrap(sk_widget *w, int wrap)
{
    if (!ta_is(w))
        return;
    ta_priv *p = TA(w);
    wrap = wrap != 0;
    if (p->wrap == wrap)
        return;
    p->wrap = wrap;
    p->scroll_x = 0;
    ta_layout(w);
    ta_ensure_visible(w);
    ta_inval_all(w);
    ta_update_caret(w);
}

void sk_textarea_set_line_numbers(sk_widget *w, int on)
{
    if (!ta_is(w))
        return;
    ta_priv *p = TA(w);
    on = on != 0;
    if (p->numbers == on)
        return;
    p->numbers = on;
    ta_layout(w);
    ta_ensure_visible(w);
    ta_inval_all(w);
    ta_update_caret(w);
}
