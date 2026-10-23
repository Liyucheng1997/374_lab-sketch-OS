/*
 * twidgets.c — 控件自动化测试用的小程序（由 tests/widgets/test_widgets.py 驱动）
 *
 * 窗口里放：左边一个多行编辑器 A，右边上方编辑器 B、单行输入框 C、下方一个带表头的列表 L。
 * 测试脚本通过模拟显示服务器（tools/mock_display.py）发鼠标键盘事件，
 * 再按功能键让程序把内部状态打印到标准输出（一行一个 JSON），据此检查控件行为：
 *
 *   F2   在列表末尾 add 一行（不 clear）
 *   F3   清空列表并显示提示文字（sk_widget_set_text）
 *   F4   列表 clear 后只 add 一行
 *   F5   打印状态：{"a": 编辑器A文字, "a_mod":, "a_pos": [行, 列], "b":, "c":, "sel":, "count":, "scroll":,
 *                   "changes": 编辑器A的 on_change 次数, "cell": 列表第 2 行第 1 列}
 *   F6   sk_list_set_cell 改列表第 2 行第 1 列（测“只重画一个单元格”）
 *   F7   列表 clear + 原样重新 add（测“重建优化”：内容没变就不重画）
 *   F8   编辑器 A 切换自动换行
 *   F9   编辑器 A 切换行号
 *   F10  编辑器 A 载入 300 行文字
 *   F11  编辑器 A 切换只读
 *   F12  打印 {"mark": n}（测试脚本用来同步）
 * 列表回调打印 {"event": "list-change"/"list-activate"/"sort", ...}。
 */
#include <sketch/client.h>
#include <sketch/ui.h>

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sk_app    *app;
    sk_window *win;
    sk_ui     *ui;
    sk_widget *a, *b, *c, *list;
    int        wrap, numbers, readonly, changes, marks, cell_rev;
    int        sort_col, sort_desc;
    int        order[50];
} T;

/* 把字符串按 JSON 转义打印 */
static void json_str(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        if (*p == '"' || *p == '\\')
            printf("\\%c", *p);
        else if (*p == '\n')
            printf("\\n");
        else if (*p == '\t')
            printf("\\t");
        else if (*p < 0x20)
            printf("\\u%04x", *p);
        else
            putchar(*p);
    }
    putchar('"');
}

static void dump(T *t)
{
    char *a = sk_textarea_get_text(t->a), *b = sk_textarea_get_text(t->b);
    int line, col;
    sk_textarea_cursor_pos(t->a, &line, &col);
    printf("{\"a\": ");
    json_str(a);
    printf(", \"a_mod\": %d, \"a_pos\": [%d, %d], \"b\": ", sk_textarea_modified(t->a), line, col);
    json_str(b);
    printf(", \"b_mod\": %d, \"c\": ", sk_textarea_modified(t->b));
    json_str(sk_textbox_text(t->c));
    printf(", \"sel\": %d, \"count\": %d, \"scroll\": %d, \"changes\": %d, \"cell\": ", sk_list_selected(t->list),
           sk_list_count(t->list), sk_list_scroll(t->list), t->changes);
    json_str(sk_list_cell(t->list, 2, 1));
    printf(", \"sel_name\": ");
    int sel = sk_list_selected(t->list);
    json_str(sel >= 0 ? sk_list_cell(t->list, sel, 0) : "");
    printf("}\n");
    fflush(stdout);
    free(a);
    free(b);
}

static void fill_list(T *t)
{
    sk_list_clear(t->list);
    for (int k = 0; k < 50; k++) {
        int i = t->order[k];
        char name[32], num[16], desc[64];
        snprintf(name, sizeof name, "项目 %02d", i);
        snprintf(num, sizeof num, "%d", (i * 37) % 101);
        snprintf(desc, sizeof desc, i == 2 ? "第二行 rev%d" : "说明文字 %d", i == 2 ? t->cell_rev : i);
        const char *cells[3] = { name, num, desc };
        sk_list_add(t->list, i % 3 == 0 ? SK_ICON_FOLDER : SK_ICON_TEXT, cells, i);
    }
}

static int cmp_key(T *t, int i)
{
    return t->sort_col == 1 ? (i * 37) % 101 : i;
}

static void on_sort(sk_widget *w, int col, void *user)
{
    (void)w;
    T *t = user;
    if (t->sort_col == col)
        t->sort_desc = !t->sort_desc;
    else {
        t->sort_col = col;
        t->sort_desc = 0;
    }
    /* 简单的插入排序（50 行） */
    for (int i = 1; i < 50; i++) {
        int v = t->order[i], j = i - 1;
        while (j >= 0 && (t->sort_desc ? cmp_key(t, t->order[j]) < cmp_key(t, v) : cmp_key(t, t->order[j]) > cmp_key(t, v))) {
            t->order[j + 1] = t->order[j];
            j--;
        }
        t->order[j + 1] = v;
    }
    fill_list(t);
    printf("{\"event\": \"sort\", \"col\": %d, \"desc\": %d, \"first\": ", col, t->sort_desc);
    json_str(sk_list_cell(t->list, 0, 0));
    printf("}\n");
    fflush(stdout);
}

static void on_list_change(sk_widget *w, void *user)
{
    (void)user;
    printf("{\"event\": \"list-change\", \"sel\": %d}\n", sk_list_selected(w));
    fflush(stdout);
}

static void on_list_activate(sk_widget *w, void *user)
{
    (void)user;
    printf("{\"event\": \"list-activate\", \"sel\": %d, \"name\": ", sk_list_selected(w));
    json_str(sk_list_cell(w, sk_list_selected(w), 0));
    printf("}\n");
    fflush(stdout);
}

static void on_a_change(sk_widget *w, void *user)
{
    (void)w;
    ((T *)user)->changes++;
}

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    T *t = user;
    int rx = W - 355;
    sk_widget_set_rect(t->a, sk_rect_make(10, 10, rx - 25, H - 20));
    sk_widget_set_rect(t->b, sk_rect_make(rx, 10, 345, 180));
    sk_widget_set_rect(t->c, sk_rect_make(rx, 200, 345, 34));
    sk_widget_set_rect(t->list, sk_rect_make(rx, 245, 345, H - 255));
}

static int on_event(sk_window *win, const sk_event *e, void *user)
{
    (void)win;
    T *t = user;
    if (e->type != SK_EVENT_KEY || e->key.state != 1)
        return 0;
    switch (e->key.keycode) {
    case KEY_F2:
    case KEY_F4: {
        /* 测“从空列表变成非空”时提示文字被擦干净（F2 走普通 add，F4 走 clear 后的重建） */
        if (e->key.keycode == KEY_F4)
            sk_list_clear(t->list);
        const char *cells[3] = { "新的一行", "1", "说明" };
        sk_list_add(t->list, -1, cells, 100);   /* 不带图标：带图标会让整个列表重画，掩盖问题 */
        return 1;
    }
    case KEY_F3:
        sk_list_clear(t->list);
        sk_widget_set_text(t->list, "列表是空的，按 F2 或 F4 添加一行");
        return 1;
    case KEY_F5:
        dump(t);
        return 1;
    case KEY_F6: {
        char buf[32];
        snprintf(buf, sizeof buf, "%d", 1000 + ++t->cell_rev);
        sk_list_set_cell(t->list, 2, 1, buf);
        return 1;
    }
    case KEY_F7: {
        int sel = sk_list_selected(t->list);
        fill_list(t);
        sk_list_select(t->list, sel);
        return 1;
    }
    case KEY_F8:
        t->wrap = !t->wrap;
        sk_textarea_set_wrap(t->a, t->wrap);
        return 1;
    case KEY_F9:
        t->numbers = !t->numbers;
        sk_textarea_set_line_numbers(t->a, t->numbers);
        return 1;
    case KEY_F10: {
        size_t cap = 300 * 80, n = 0;
        char *s = malloc(cap);
        if (!s)
            return 1;
        for (int i = 1; i <= 300; i++)
            n += (size_t)snprintf(s + n, cap - n, "第 %d 行：用来测试滚动的文字 line %d\n", i, i);
        sk_textarea_set_text(t->a, s);
        free(s);
        return 1;
    }
    case KEY_F11:
        t->readonly = !t->readonly;
        sk_textarea_set_readonly(t->a, t->readonly);
        return 1;
    case KEY_F12:
        printf("{\"mark\": %d}\n", ++t->marks);
        fflush(stdout);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    T t;
    memset(&t, 0, sizeof t);
    t.wrap = 1;
    t.sort_col = 0;
    for (int i = 0; i < 50; i++)
        t.order[i] = i;
    const char *initial = argc > 1 ? argv[1] : "";
    t.app = sk_app_connect("twidgets");
    if (!t.app)
        return 1;
    t.win = sk_window_create(t.app, "控件测试", 900, 600, SK_WIN_RESIZABLE, 0);
    if (!t.win)
        return 1;
    t.ui = sk_ui_new(t.win);
    t.a = sk_textarea_new(t.ui, sk_rect_make(0, 0, 10, 10), initial);
    t.a->on_change = on_a_change;
    t.a->user = &t;
    t.b = sk_textarea_new(t.ui, sk_rect_make(0, 0, 10, 10), "");
    t.c = sk_textbox_new(t.ui, sk_rect_make(0, 0, 10, 10), "", NULL, NULL);
    t.list = sk_list_new(t.ui, sk_rect_make(0, 0, 10, 10), on_list_change, on_list_activate, &t);
    static const char *const cols[] = { "名称", "编号", "说明" };
    static const int widths[] = { 110, -60, 0 };
    sk_list_set_columns(t.list, cols, widths, 3);
    sk_list_set_sort_handler(t.list, on_sort);
    fill_list(&t);
    sk_ui_set_layout(t.ui, layout, &t);
    sk_ui_set_handler(t.ui, on_event, &t);
    sk_ui_focus(t.ui, t.a);
    printf("{\"ready\": 1}\n");
    fflush(stdout);
    return sk_app_run(t.app);
}
