/*
 * test_ui_widgets.c — 多行编辑器 (ui_textarea.c) 核心逻辑的宿主机单元测试
 *
 * 编译运行：
 *   make -C userspace BUILD=<输出目录> <输出目录>/host/test_ui_widgets
 *   <输出目录>/host/test_ui_widgets            # 全部通过时退出码 0
 *
 * 这里用的是“包含 .c 文件”的单元测试技巧：直接 #include 被测源文件，
 * 就能调用它里面的 static 函数（文本模型、折行、撤销），不需要为测试专门导出接口。
 * 控件以“无界面模式”运行（w->ui == NULL）：不连接显示服务器、不加载字体，
 * 字宽由下面的 fake_adv 给出（ASCII 8 像素，中文 16 像素），所以结果是确定的。
 *
 * 除了逐项检查，还有两个“性质测试”：
 *   1. 随机编辑 2000 次，每次之后检查“部分重排”的折行结果与“整行从头重排”完全一致；
 *   2. 每次编辑后检查：所有内容发生变化的显示行都被失效区域覆盖（局部重画不会漏画）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../libsketch/ui_textarea.c"

static int g_fail, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "%s:%d: 检查失败: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); g_checks++; \
    if (strcmp(_a, _b) != 0) { g_fail++; \
    fprintf(stderr, "%s:%d: 字符串不同:\n   实际 \"%s\"\n   期望 \"%s\"\n", __FILE__, __LINE__, _a, _b); } } while (0)

/* ---------------------------------------------------------------- 测试脚手架 */

static int fake_adv(uint32_t cp)
{
    return ta_is_wide(cp) ? 16 : 8;
}

/* 建一个无界面的编辑器：文字区宽 text_w 像素、可见 rows 行 */
static sk_widget *mk(int text_w, int rows, const char *text)
{
    sk_widget *w = calloc(1, sizeof(*w));
    ta_priv *p = calloc(1, sizeof(*p));
    w->cls = &ta_class;
    w->priv = p;
    w->seed = 1;
    ta_doc_init(&p->doc);
    ta_undo_clear(&p->undo);
    ta_metrics_init(&p->doc.m, NULL, fake_adv);
    p->wrap = 1;
    p->pref_x = -1;
    p->row_h = 20;
    w->r = sk_rect_make(0, 0, 200, rows * 20 + 12);
    ta_layout(w);
    w->r.w += text_w - p->text_r.w;   /* 调整控件宽度，让文字区正好 text_w 宽 */
    ta_layout(w);
    ta_set_text_internal(w, text);
    return w;
}

static void rm(sk_widget *w)
{
    ta_destroy(w);
    free(w);
}

static char *text_of(sk_widget *w) { return sk_textarea_get_text(w); }

#define CHECK_TEXT(w, s) do { char *_t = text_of(w); CHECK_STR(_t, s); free(_t); } while (0)

static void key(sk_widget *w, uint32_t keycode, uint32_t mods, uint32_t cp)
{
    sk_event e;
    memset(&e, 0, sizeof(e));
    e.type = SK_EVENT_KEY;
    e.key.keycode = keycode;
    e.key.state = 1;
    e.key.modifiers = mods;
    e.key.codepoint = cp;
    ta_event(w, &e);
}

/* 像 mock_display.type_text 那样逐字输入：ASCII 带键码，其它字符只给 codepoint */
static void type(sk_widget *w, const char *s)
{
    while (*s) {
        uint32_t cp = sk_utf8_next(&s);
        if (cp == '\n')
            key(w, KEY_ENTER, 0, 0x0D);
        else
            key(w, cp < 0x80 ? KEY_A : 0, 0, cp);    /* 键码只要非 0 即可，具体是哪个键不重要 */
    }
}

static ta_priv *P(sk_widget *w) { return TA(w); }

/* 某一行的折行位置串，例如 "6,12" */
static void brk_str(sk_widget *w, int li, char *out, size_t n)
{
    ta_line *L = ta_L(&P(w)->doc, li);
    out[0] = 0;
    for (int k = 0; k < L->nrows - 1; k++) {
        char b[16];
        snprintf(b, sizeof(b), k ? ",%d" : "%d", L->brk[k]);
        strncat(out, b, n - strlen(out) - 1);
    }
}

/* ---------------------------------------------------------------- 1. UTF-8 */
static void test_utf8(void)
{
    int clen;
    CHECK(ta_decode("A", 1, &clen) == 'A' && clen == 1);
    CHECK(ta_decode("\xE4\xBD\xA0", 3, &clen) == 0x4F60 && clen == 3);        /* 你 */
    CHECK(ta_decode("\xF0\x9F\x98\x80", 4, &clen) == 0x1F600 && clen == 4);   /* 😀 */
    CHECK(ta_decode("\xE4\xBD", 2, &clen) == 0xFFFD && clen == 1);            /* 被截断 */
    CHECK(ta_decode("\x80", 1, &clen) == 0xFFFD && clen == 1);                /* 孤立续字节 */
    CHECK(ta_decode("\xC0\x80", 2, &clen) == 0xFFFD && clen == 1);            /* 过长编码 */
    CHECK(ta_decode("\xED\xA0\x80", 3, &clen) == 0xFFFD && clen == 1);        /* 代理区 */
    const char *s = "a你\x80" "b";
    CHECK(ta_next_off(s, 6, 0) == 1);
    CHECK(ta_next_off(s, 6, 1) == 4);
    CHECK(ta_next_off(s, 6, 4) == 5);
    CHECK(ta_prev_off(s, 6) == 5);
    CHECK(ta_prev_off(s, 5) == 4);
    CHECK(ta_prev_off(s, 4) == 1);
    CHECK(ta_prev_off(s, 1) == 0);
}

/* ---------------------------------------------------------------- 2. 文本模型 */
static void test_doc(void)
{
    sk_widget *w = mk(400, 10, "第一行\r\nsecond\rthird\n");
    CHECK(P(w)->doc.n == 4);
    CHECK_TEXT(w, "第一行\nsecond\nthird\n");
    ta_doc *d = &P(w)->doc;
    ta_pos e = ta_doc_insert(d, ta_pos_make(1, 3), "XX\nYY\nZ", 7);
    CHECK(e.line == 3 && e.off == 1);
    CHECK_TEXT(w, "第一行\nsecXX\nYY\nZond\nthird\n");
    ta_doc_delete(d, ta_pos_make(1, 3), ta_pos_make(3, 1));
    CHECK_TEXT(w, "第一行\nsecond\nthird\n");
    ta_doc_delete(d, ta_pos_make(0, 3), ta_pos_make(2, 0));
    CHECK_TEXT(w, "第third\n");
    int len;
    char *r = ta_doc_get_range(d, ta_pos_make(0, 0), ta_pos_make(1, 0), &len);
    CHECK_STR(r, "第third\n");
    CHECK(len == 9);
    free(r);
    ta_pos a = ta_pos_after(ta_pos_make(2, 5), "ab\ncde", 6);
    CHECK(a.line == 3 && a.off == 3);
    rm(w);
}

/* ---------------------------------------------------------------- 3. 折行 */
static void test_wrap(void)
{
    char b[128];
    sk_widget *w = mk(80, 10, "hello world foo");
    brk_str(w, 0, b, sizeof(b));
    CHECK_STR(b, "6");                                 /* "hello " | "world foo" */

    sk_textarea_set_text(w, "中文排版测试一下");      /* 每字 16px，一行 5 个字 */
    brk_str(w, 0, b, sizeof(b));
    CHECK_STR(b, "15");

    sk_textarea_set_text(w, "一二三四五，六");        /* 避头：逗号不能放在行首，把“五”一起带下去 */
    brk_str(w, 0, b, sizeof(b));
    CHECK_STR(b, "12");

    sk_textarea_set_text(w, "abcdefghijklmnop");       /* 超长单词硬断 */
    brk_str(w, 0, b, sizeof(b));
    CHECK_STR(b, "10");

    sk_textarea_set_text(w, "aaaaaaaaaa     b");       /* 行尾空格“挂”在行尾 */
    brk_str(w, 0, b, sizeof(b));
    CHECK_STR(b, "15");

    sk_textarea_set_text(w, "ab中文cd");               /* 中英混排：可以在中文前后折行 */
    brk_str(w, 0, b, sizeof(b));
    CHECK_STR(b, "");
    sk_textarea_set_text(w, "abcdef中文cd");           /* 48+32=80 正好放下，cd 折到下一行 */
    brk_str(w, 0, b, sizeof(b));
    CHECK_STR(b, "12");

    sk_textarea_set_text(w, "\tab");                   /* 制表符对齐到 32px */
    CHECK(ta_L(&P(w)->doc, 0)->width == 48);
    CHECK(ta_measure(&P(w)->doc, ta_L(&P(w)->doc, 0), 0, 2) == 40);

    /* 关掉自动换行：只有一个显示行，记录整行宽度 */
    sk_textarea_set_text(w, "hello world foo");
    sk_textarea_set_wrap(w, 0);
    CHECK(ta_L(&P(w)->doc, 0)->nrows == 1);
    CHECK(ta_doc_max_width(&P(w)->doc) == 15 * 8);
    sk_textarea_set_wrap(w, 1);
    CHECK(ta_L(&P(w)->doc, 0)->nrows == 2);

    /* 显示行查找 */
    sk_textarea_set_text(w, "aaaa bbbb cccc\nx\nyyyy zzzz");   /* 2 + 1 + 1 显示行（"yyyy zzzz" 正好 72px） */
    CHECK(ta_total_rows(&P(w)->doc) == 4);
    int li, k;
    ta_row_lookup(&P(w)->doc, 1, &li, &k);
    CHECK(li == 0 && k == 1);
    ta_row_lookup(&P(w)->doc, 2, &li, &k);
    CHECK(li == 1 && k == 0);
    ta_row_lookup(&P(w)->doc, 99, &li, &k);
    CHECK(li == 2 && k == 0);
    CHECK(ta_global_row(&P(w)->doc, ta_pos_make(0, 10), 0) == 1);
    CHECK(ta_global_row(&P(w)->doc, ta_pos_make(0, 10), 1) == 0);   /* 行尾亲和 */
    rm(w);
}

/* ---------------------------------------------------------------- 4. 性质测试：部分重排 = 整行重排，失效区域覆盖所有变化 */

static sk_rect g_inval[4096];
static int g_ninval;
static void rec_inval(sk_rect r)
{
    if (g_ninval < (int)(sizeof(g_inval) / sizeof(g_inval[0])))
        g_inval[g_ninval++] = r;
}

static int covered(sk_rect band)
{
    /* 逐像素行检查：band 的每一行都必须落在某个失效矩形里（宽度方向检查文字区） */
    for (int y = band.y; y < band.y + band.h; y++) {
        int ok = 0;
        for (int i = 0; i < g_ninval && !ok; i++) {
            sk_rect r = g_inval[i];
            if (y >= r.y && y < r.y + r.h && r.x <= band.x && r.x + r.w >= band.x + band.w)
                ok = 1;
        }
        if (!ok)
            return 0;
    }
    return 1;
}

/* 可见显示行的“画面内容”：行号 + 行内文字 */
static char **snapshot(sk_widget *w)
{
    ta_priv *p = P(w);
    int n = p->vis_rows + 1, total = ta_total_rows(&p->doc);
    char **rows = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) {
        int g = p->scroll_row + i;
        if (g >= total) {
            rows[i] = strdup("<空>");
            continue;
        }
        int li, k;
        ta_row_lookup(&p->doc, g, &li, &k);
        ta_line *L = ta_L(&p->doc, li);
        int s = ta_row_start(L, k), e = ta_row_end(L, k);
        int hdr = snprintf(NULL, 0, "%d:%d|", li, k);
        rows[i] = malloc(hdr + (e - s) + 1);
        sprintf(rows[i], "%d:%d|", li, k);
        memcpy(rows[i] + hdr, L->s + s, e - s);
        rows[i][hdr + (e - s)] = 0;
    }
    return rows;
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static const char *pieces[] = { "a", "bb", "ccc ", " ", "  ", "词", "中文", "，", "。", "(", ")", "hello ",
                                "longwordwithoutspaces", "\n", "x\ny", "\t", "“引号”", "e\xCC\x81",
                                "\n\n", "第一\n第二\n" };

static void test_property(void)
{
    sk_widget *w = mk(120, 30, "");
    ta_priv *p = P(w);
    /* 初始：随机拼一篇 */
    char buf[4096] = "";
    for (int i = 0; i < 120; i++)
        strncat(buf, pieces[rnd() % (sizeof(pieces) / sizeof(pieces[0]))], sizeof(buf) - strlen(buf) - 1);
    sk_textarea_set_text(w, buf);
    ta_test_inval = rec_inval;
    int mismatches = 0, uncovered = 0;
    for (int iter = 0; iter < 2000; iter++) {
        char **before = snapshot(w);
        g_ninval = 0;
        ta_doc *d = &p->doc;
        int li = rnd() % d->n;
        ta_line *L = ta_L(d, li);
        /* 随机找一个字符边界 */
        int off = 0, steps = L->len ? rnd() % (L->len + 1) : 0;
        for (int s = 0; s < steps && off < L->len; s++)
            off = ta_next_off(L->s, L->len, off);
        ta_pos a = ta_pos_make(li, off);
        if (rnd() % 4 == 0 && d->n > 1) {
            /* 删除一段（可能跨行） */
            ta_pos b = a;
            int n = 1 + rnd() % 8;
            for (int s = 0; s < n; s++)
                b = ta_right_of(d, b);
            ta_do_delete(w, a, b);
        } else {
            const char *t = pieces[rnd() % (sizeof(pieces) / sizeof(pieces[0]))];
            ta_do_insert(w, a, t, (int)strlen(t));
        }
        /* 随机滚动一下，让检查覆盖“修改发生在屏幕中间/外面”的情况 */
        if (iter % 50 == 0) {
            int mr = ta_max_scroll(w);
            p->scroll_row = mr ? (int)(rnd() % (mr + 1)) : 0;
            for (int i = 0; i < p->vis_rows + 1; i++)
                free(before[i]);
            free(before);
            continue;
        }
        /* 检查 1：每一行的折行与整行重排一致 */
        for (int i = 0; i < d->n; i++) {
            ta_line *X = ta_L(d, i);
            ta_line copy = *X;
            copy.brk = malloc((X->brk_cap ? X->brk_cap : 1) * sizeof(int));
            copy.gen = 0;
            ta_line saved = d->lines[i];
            d->lines[i] = copy;
            ta_wrap_line(d, i, 0);
            int same = d->lines[i].nrows == saved.nrows &&
                       (saved.nrows <= 1 || memcmp(d->lines[i].brk, saved.brk, (saved.nrows - 1) * sizeof(int)) == 0);
            free(d->lines[i].brk);
            d->lines[i] = saved;
            if (!same)
                mismatches++;
        }
        /* 检查 2：前缀和正确 */
        int sum = 0;
        for (int i = 0; i < d->n; i++)
            sum += ta_L(d, i)->nrows;
        CHECK(ta_total_rows(d) == sum);
        /* 检查 3：变化的显示行都被失效覆盖。
         * （文档变短导致滚动位置被夹紧时，真实代码会整个重画——这里直接跳过检查） */
        if (p->scroll_row > ta_max_scroll(w)) {
            p->scroll_row = ta_max_scroll(w);
            for (int i = 0; i < p->vis_rows + 1; i++)
                free(before[i]);
            free(before);
            continue;
        }
        char **after = snapshot(w);
        for (int i = 0; i < p->vis_rows + 1; i++) {
            if (strcmp(before[i], after[i]) != 0) {
                sk_rect band = sk_rect_make(p->text_r.x, ta_row_y(p, p->scroll_row + i), p->text_r.w, p->row_h);
                if (band.y + band.h > p->inner.y + p->inner.h)
                    band.h = p->inner.y + p->inner.h - band.y;
                if (band.h > 0 && !covered(band)) {
                    if (uncovered < 5)
                        fprintf(stderr, "  第 %d 次编辑：显示行 %d 变了但没失效: \"%s\" -> \"%s\"\n",
                                iter, i, before[i], after[i]);
                    uncovered++;
                }
            }
            free(before[i]);
            free(after[i]);
        }
        free(before);
        free(after);
    }
    ta_test_inval = NULL;
    CHECK(mismatches == 0);
    CHECK(uncovered == 0);
    printf("  性质测试：2000 次随机编辑，最终 %d 行 / %d 显示行，折行不一致 %d 次，漏失效 %d 次\n",
           p->doc.n, ta_total_rows(&p->doc), mismatches, uncovered);
    rm(w);
}

/* ---------------------------------------------------------------- 5. 打字只让一行失效 */
static void test_damage(void)
{
    sk_widget *w = mk(200, 20, "第一行\n第二行是一段比较长的文字，需要折成好几行才能显示完整的样子\n第三行\n");
    ta_priv *p = P(w);
    ta_test_inval = rec_inval;
    /* 光标放到第一行末尾，打一个字 */
    p->caret = p->anchor = ta_pos_make(0, P(w)->doc.lines[0].len);
    g_ninval = 0;
    type(w, "啊");
    int area = 0, maxh = 0;
    for (int i = 0; i < g_ninval; i++) {
        area += g_inval[i].w * g_inval[i].h;
        if (g_inval[i].h > maxh)
            maxh = g_inval[i].h;
    }
    printf("  在第一行末尾打一个字：%d 个失效矩形，最高 %d 像素（行高 %d）\n", g_ninval, maxh, p->row_h);
    CHECK(g_ninval >= 1 && maxh <= p->row_h);

    /* 在长段落最后一个显示行打字：只重画那一行 */
    ta_line *L = ta_L(&p->doc, 1);
    p->caret = p->anchor = ta_pos_make(1, L->len);
    g_ninval = 0;
    type(w, "x");
    maxh = 0;
    for (int i = 0; i < g_ninval; i++)
        if (g_inval[i].h > maxh)
            maxh = g_inval[i].h;
    CHECK(maxh <= p->row_h);

    /* 回车：下面的行整体下移，从当前行到底部都要重画 */
    g_ninval = 0;
    type(w, "\n");
    maxh = 0;
    for (int i = 0; i < g_ninval; i++)
        if (g_inval[i].h > maxh)
            maxh = g_inval[i].h;
    CHECK(maxh > p->row_h * 3);
    ta_test_inval = NULL;
    rm(w);
}

/* ---------------------------------------------------------------- 6. 键盘编辑 + 撤销 */
static void test_editing(void)
{
    sk_widget *w = mk(400, 10, "");
    type(w, "hello world");
    CHECK_TEXT(w, "hello world");
    CHECK(sk_textarea_modified(w));
    key(w, KEY_Z, SK_MOD_CTRL, 'z');
    CHECK_TEXT(w, "hello ");                  /* 按词撤销 */
    key(w, KEY_Z, SK_MOD_CTRL, 'z');
    CHECK_TEXT(w, "");
    CHECK(!sk_textarea_modified(w));
    key(w, KEY_Y, SK_MOD_CTRL, 'y');
    CHECK_TEXT(w, "hello ");
    key(w, KEY_Z, SK_MOD_CTRL | SK_MOD_SHIFT, 'Z');   /* Ctrl+Shift+Z 也是重做 */
    CHECK_TEXT(w, "hello world");
    key(w, KEY_Y, SK_MOD_CTRL, 'y');                   /* 没有可重做的了 */
    CHECK_TEXT(w, "hello world");

    /* 停顿超过 1 秒：分成两组 */
    sk_textarea_set_text(w, "");
    ta_test_clock = 10000;
    type(w, "ab");
    ta_test_clock += 1500;
    type(w, "cd");
    key(w, KEY_Z, SK_MOD_CTRL, 'z');
    CHECK_TEXT(w, "ab");
    ta_test_clock = 0;

    /* 退格连续合并成一组 */
    sk_textarea_set_text(w, "abcdef");
    key(w, KEY_END, SK_MOD_CTRL, 0);
    key(w, KEY_BACKSPACE, 0, 0x08);
    key(w, KEY_BACKSPACE, 0, 0x08);
    key(w, KEY_BACKSPACE, 0, 0x08);
    CHECK_TEXT(w, "abc");
    key(w, KEY_Z, SK_MOD_CTRL, 'z');
    CHECK_TEXT(w, "abcdef");

    /* Delete 键连续合并 */
    key(w, KEY_HOME, SK_MOD_CTRL, 0);
    key(w, KEY_DELETE, 0, 0);
    key(w, KEY_DELETE, 0, 0);
    CHECK_TEXT(w, "cdef");
    key(w, KEY_Z, SK_MOD_CTRL, 'z');
    CHECK_TEXT(w, "abcdef");

    /* 回车、跨行退格/Delete、Tab */
    sk_textarea_set_text(w, "ab");
    key(w, KEY_END, 0, 0);
    key(w, KEY_LEFT, 0, 0);
    key(w, KEY_ENTER, 0, 0x0D);
    CHECK_TEXT(w, "a\nb");
    key(w, KEY_BACKSPACE, 0, 0x08);
    CHECK_TEXT(w, "ab");
    key(w, KEY_ENTER, 0, 0x0D);
    key(w, KEY_LEFT, 0, 0);
    key(w, KEY_DELETE, 0, 0);
    CHECK_TEXT(w, "ab");
    key(w, KEY_TAB, 0, 0x09);
    CHECK_TEXT(w, "a    b");
    int l, c;
    sk_textarea_cursor_pos(w, &l, &c);
    CHECK(l == 1 && c == 6);

    /* 中文（keycode=0，只有 codepoint） */
    sk_textarea_set_text(w, "");
    key(w, 0, 0, 0x4F60);
    key(w, 0, 0, 0x597D);
    type(w, "，世界");
    CHECK_TEXT(w, "你好，世界");
    sk_textarea_cursor_pos(w, &l, &c);
    CHECK(l == 1 && c == 6);
    key(w, KEY_LEFT, SK_MOD_SHIFT, 0);
    key(w, KEY_LEFT, SK_MOD_SHIFT, 0);
    type(w, "朋友");                           /* 替换选中的“世界” */
    CHECK_TEXT(w, "你好，朋友");
    key(w, KEY_Z, SK_MOD_CTRL, 'z');
    CHECK_TEXT(w, "你好，世界");
    CHECK(ta_has_sel(P(w)));                   /* 撤销后原来的选区回来了 */

    /* 全选 + 删除；Ctrl+A 后打字替换全部 */
    key(w, KEY_A, SK_MOD_CTRL, 'a');
    key(w, KEY_DELETE, 0, 0);
    CHECK_TEXT(w, "");
    key(w, KEY_Z, SK_MOD_CTRL, 'z');
    CHECK_TEXT(w, "你好，世界");

    /* “已修改”标志：保存点之后继续打字另起一组，撤销回保存点 = 未修改 */
    sk_textarea_set_text(w, "abc");
    CHECK(!sk_textarea_modified(w));
    key(w, KEY_END, 0, 0);
    type(w, "d");
    CHECK(sk_textarea_modified(w));
    sk_textarea_set_modified(w, 0);
    CHECK(!sk_textarea_modified(w));
    type(w, "e");
    CHECK(sk_textarea_modified(w));
    key(w, KEY_Z, SK_MOD_CTRL, 'z');
    CHECK_TEXT(w, "abcd");
    CHECK(!sk_textarea_modified(w));
    key(w, KEY_Z, SK_MOD_CTRL, 'z');
    CHECK_TEXT(w, "abc");
    CHECK(sk_textarea_modified(w));
    sk_textarea_set_modified(w, 1);
    CHECK(sk_textarea_modified(w));

    /* 只读：编辑键不处理，光标仍然能动 */
    sk_textarea_set_text(w, "readonly");
    sk_textarea_set_readonly(w, 1);
    type(w, "x");
    key(w, KEY_BACKSPACE, 0, 0x08);
    key(w, KEY_END, 0, 0);
    CHECK_TEXT(w, "readonly");
    sk_textarea_cursor_pos(w, &l, &c);
    CHECK(c == 9);
    sk_textarea_set_readonly(w, 0);

    /* 按词移动 */
    sk_textarea_set_text(w, "hello  世界你好，ok");
    key(w, KEY_RIGHT, SK_MOD_CTRL, 0);
    CHECK(P(w)->caret.off == 7);
    key(w, KEY_RIGHT, SK_MOD_CTRL, 0);
    CHECK(P(w)->caret.off == 19);              /* 跳过一串汉字 */
    key(w, KEY_LEFT, SK_MOD_CTRL, 0);
    CHECK(P(w)->caret.off == 7);
    ta_pos a, b;
    ta_word_at(&P(w)->doc, ta_pos_make(0, 10), &a, &b);   /* 双击“界” → 选中“世界你好” */
    CHECK(a.off == 7 && b.off == 19);
    ta_word_at(&P(w)->doc, ta_pos_make(0, 2), &a, &b);
    CHECK(a.off == 0 && b.off == 5);
    rm(w);
}

/* ---------------------------------------------------------------- 6b. 撤销栈上限 */
static void test_undo_limit(void)
{
    sk_widget *w = mk(400, 10, "");
    for (int i = 0; i < TA_UNDO_GROUPS + 100; i++) {
        ta_test_clock += 2000;                 /* 每次都停顿：不合并，各成一组 */
        type(w, "x");
    }
    ta_test_clock = 0;
    CHECK(P(w)->undo.n == TA_UNDO_GROUPS);
    int undos = 0;
    while (ta_undo_apply(w, 0))
        undos++;
    CHECK(undos == TA_UNDO_GROUPS);
    char *t = text_of(w);
    CHECK(strlen(t) == 100);                   /* 最早的 100 步已经被丢弃，撤销不回去了 */
    free(t);
    CHECK(sk_textarea_modified(w));            /* 打开时的“空文档”状态再也回不去 → 仍是已修改 */
    while (ta_undo_apply(w, 1))
        ;
    t = text_of(w);
    CHECK(strlen(t) == TA_UNDO_GROUPS + 100);
    free(t);
    rm(w);
}

/* ---------------------------------------------------------------- 7. 光标在折行间上下移动 */
static void test_vertical(void)
{
    /* 宽 80：每显示行 10 个 ASCII 字符 */
    sk_widget *w = mk(80, 5, "aaaaaaaaaabbbbbbbbbbcccc\nshort\n0123456789012345");
    ta_priv *p = P(w);
    CHECK(ta_total_rows(&p->doc) == 6);
    p->caret = p->anchor = ta_pos_make(0, 7);
    key(w, KEY_DOWN, 0, 0);
    CHECK(p->caret.line == 0 && p->caret.off == 17);   /* 同一逻辑行的下一个显示行，x 保持 */
    key(w, KEY_DOWN, 0, 0);
    CHECK(p->caret.line == 0 && p->caret.off == 24);   /* 最后一显示行只有 4 个字：到行尾 */
    key(w, KEY_DOWN, 0, 0);
    CHECK(p->caret.line == 1 && p->caret.off == 5);
    key(w, KEY_DOWN, 0, 0);
    CHECK(p->caret.line == 2 && p->caret.off == 7);    /* 期望 x 一直保持在 56px */
    key(w, KEY_UP, 0, 0);
    key(w, KEY_UP, 0, 0);
    key(w, KEY_UP, 0, 0);
    CHECK(p->caret.line == 0 && p->caret.off == 17);
    /* End：显示行末尾，行尾亲和 */
    key(w, KEY_END, 0, 0);
    CHECK(p->caret.off == 20 && p->caret_eol == 1);
    CHECK(ta_global_row(&p->doc, p->caret, p->caret_eol) == 1);
    key(w, KEY_HOME, 0, 0);
    CHECK(p->caret.off == 10);
    /* 滚动：光标移到最后会滚动，PageUp 回去 */
    key(w, KEY_END, SK_MOD_CTRL, 0);
    CHECK(p->scroll_row == ta_total_rows(&p->doc) - p->vis_rows);
    key(w, KEY_PAGEUP, 0, 0);
    key(w, KEY_PAGEUP, 0, 0);
    CHECK(p->scroll_row == 0);
    CHECK(p->caret.line == 0);
    /* 鼠标命中：点击第二显示行中间 */
    int eol;
    ta_pos hit = ta_pos_from_xy(w, p->text_r.x + 8 * 3 + 2, p->text_r.y + p->row_h + 5, &eol);
    CHECK(hit.line == 0 && hit.off == 13);
    hit = ta_pos_from_xy(w, p->text_r.x + 500, p->text_r.y + 5, &eol);
    CHECK(hit.off == 10 && eol == 1);
    rm(w);
}

/* ---------------------------------------------------------------- 8. 大文件性能 */
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void test_perf(void)
{
    /* 约 1 MB：两万行中英混排 */
    size_t cap = 1200000, n = 0;
    char *big = malloc(cap);
    for (int i = 0; n < 1000000; i++) {
        n += snprintf(big + n, cap - n, "第 %d 行：The quick brown fox 跳过了懒狗。这是一段用来测试折行速度的文字 %d\n", i, i * 7);
    }
    sk_widget *w = mk(600, 30, "");
    double t0 = now_s();
    sk_textarea_set_text(w, big);
    int rows = ta_total_rows(&P(w)->doc);
    double t1 = now_s();
    printf("  1 MB 文本（%d 行，%d 显示行）载入 + 全部折行：%.1f ms\n", P(w)->doc.n, rows, (t1 - t0) * 1000);
    CHECK(t1 - t0 < 1.0);
    /* 在中间打 1000 个字 */
    P(w)->caret = P(w)->anchor = ta_pos_make(P(w)->doc.n / 2, 10);
    t0 = now_s();
    for (int i = 0; i < 1000; i++)
        key(w, 0, 0, 0x4E00 + (i % 100));
    t1 = now_s();
    printf("  在 1 MB 文档中间打 1000 个字：平均每个 %.1f µs\n", (t1 - t0) * 1e6 / 1000);
    CHECK((t1 - t0) / 1000 < 0.002);
    /* 回车（整个前缀和要从这里重算） */
    t0 = now_s();
    for (int i = 0; i < 100; i++)
        key(w, KEY_ENTER, 0, 0x0D);
    t1 = now_s();
    printf("  在 1 MB 文档中间按 100 次回车：平均每次 %.1f µs\n", (t1 - t0) * 1e6 / 100);
    char *t = sk_textarea_get_text(w);
    CHECK(strlen(t) == n + 1000 * 3 + 100);
    free(t);
    rm(w);

    /* 用真正的字体（SKETCH_FONT_DIR）测量：字宽缓存的效果 */
    const sk_font *f = sk_font_get(SK_FONT_UI);
    w = mk(600, 30, "");
    ta_metrics_init(&P(w)->doc.m, f, NULL);
    P(w)->doc.gen++;
    t0 = now_s();
    sk_textarea_set_text(w, big);
    rows = ta_total_rows(&P(w)->doc);
    t1 = now_s();
    printf("  同样的 1 MB 文本用真实字体折行（宽 600px）：%.1f ms，%d 显示行\n", (t1 - t0) * 1000, rows);
    CHECK(t1 - t0 < 2.0);
    rm(w);
    free(big);
}

int main(void)
{
    printf("test_ui_widgets: 多行编辑器核心逻辑\n");
    test_utf8();
    test_doc();
    test_wrap();
    test_vertical();
    test_editing();
    test_undo_limit();
    test_damage();
    test_property();
    test_perf();
    printf("%d 项检查，%d 项失败\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
