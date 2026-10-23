/*
 * skwidgets — 控件展示（libsketch 控件库的“样品册”）
 *
 * 把 sketch/ui.h 里的每一种控件都摆出来，分成五页：
 *   基础控件   标签、段落、按钮、图标按钮、复选框、色块、单行输入框、滑块、占用率条
 *   文本编辑   多行编辑器（自动换行、行号、只读、撤销、两个编辑器之间复制粘贴）
 *   列表       带表头的表格（点表头排序、“实时刷新”模拟系统监视器）、带大图标的列表
 *   对话框     消息框（按关键词自动配图标）、输入框、打开/保存文件
 *   绘图与菜单 rough.h 手绘图元的样张（自定义控件）、弹出菜单
 * 它既是测试工具，也是写图形应用的参考：每种控件怎么创建、回调怎么写、
 * 窗口改变大小时怎么重新摆放（layout 回调）、怎么用标签页切换一组控件的显示。
 *
 * 命令行（自动化测试用）：
 *   skwidgets [--tab N] [--log] [--demo 对话框类型 [参数]]
 *     --tab N     启动时显示第 N 页（0..4）
 *     --log       把状态栏的每条消息也打印到标准输出（测试脚本据此检查回调）
 *     --demo X    启动后立刻打开一个对话框：message ask warn error input open save，
 *                 open/save 可以再跟一个起始目录；结果打印到标准输出后程序退出
 */
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>
#include <sketch/rough.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NPAGES     5
#define PAGE_MAX   48
#define MARGIN     16
#define TABS_Y     46
#define CONTENT_Y  (TABS_Y + 44)
#define STATUS_H   30

/* 假的进程表（列表页用，模拟系统监视器） */
typedef struct {
    char name[32];
    int  pid;
    int  cpu;       /* 千分比 */
    int  mem_kb;
    int  icon;
} fake_proc;

typedef struct {
    sk_app    *app;
    sk_window *win;
    sk_ui     *ui;
    int        log;
    sk_widget *title, *tabs, *status;
    sk_widget *page[NPAGES][PAGE_MAX];
    int        npage[NPAGES];
    int        active;

    /* 第 0 页：基础控件 */
    sk_widget *lbl_left, *lbl_center, *lbl_right, *lbl_color, *para;
    sk_widget *btn_normal, *btn_green, *btn_red, *btn_disabled;
    sk_widget *tool[6];
    sk_widget *chk_a, *chk_b, *chk_c;
    sk_widget *swatch[8];
    sk_widget *tb_name, *tb_echo;
    sk_widget *slider, *meter, *meter2, *meter3, *lbl_slider;
    sk_widget *sec0[4];

    /* 第 1 页：文本编辑 */
    sk_widget *ta_main, *ta_side, *chk_wrap, *chk_numbers, *chk_readonly;
    sk_widget *btn_clear, *btn_sample, *btn_count, *lbl_pos, *tb_paste, *lbl_side;

    /* 第 2 页：列表 */
    sk_widget *list, *icons_list, *chk_live, *btn_add, *btn_rebuild, *btn_clearlist, *lbl_list;
    fake_proc  procs[64];
    int        nprocs;
    int        sort_col, sort_desc;
    int        live_timer;
    unsigned   rnd;

    /* 第 3 页：对话框 */
    sk_widget *dlg_btn[7], *dlg_result, *dlg_intro;

    /* 第 4 页：绘图与菜单 */
    sk_widget *sampler, *btn_resketch, *btn_menu, *lbl_menu;
    uint32_t   sampler_seed;
} gallery;

static const char *const tab_labels[NPAGES] = { "基础控件", "文本编辑", "列表", "对话框", "绘图与菜单" };

static const char *sample_text =
    "欢迎使用 SketchOS 的多行编辑器控件！\n"
    "\n"
    "这里的一切都是“手绘”的：淡蓝色的横线、红色的页边线、铅笔一样的光标，"
    "选中的文字会被黄色荧光笔涂亮。\n"
    "\n"
    "试试这些操作：\n"
    "    · 打字（中文、English 都行），回车换行，Tab 插入 4 个空格\n"
    "    · 方向键、Home/End、PageUp/PageDown 移动，按住 Shift 选择\n"
    "    · 双击选中一个词（中文会选中一串连续的汉字）\n"
    "    · Ctrl+A 全选，Ctrl+C / Ctrl+X / Ctrl+V 复制、剪切、粘贴\n"
    "    · Ctrl+Z 撤销，Ctrl+Y 重做（连续输入按词合并成一步）\n"
    "    · 改变窗口宽度，长段落会自动重新折行\n"
    "\n"
    "The quick brown fox jumps over the lazy dog. 这一段很长很长，用来演示自动换行："
    "一行写不下的时候，英文按单词折行，中文按字折行，而且句号、逗号这样的标点不会跑到行首。"
    "编辑器内部把文字存成“行数组”，每一行再缓存自己的折行位置，所以打字的时候只需要重新排版这一行。\n";

/* ================================================================ 小工具 */

static void status(gallery *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void status(gallery *g, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sk_widget_set_text(g->status, buf);
    if (g->log) {
        printf("skwidgets: %s\n", buf);
        fflush(stdout);
    }
}

/* 把控件登记到第 p 页（切换标签页时整页显示/隐藏） */
static sk_widget *add(gallery *g, int p, sk_widget *w)
{
    if (w && g->npage[p] < PAGE_MAX)
        g->page[p][g->npage[p]++] = w;
    if (w && p != g->active)
        sk_widget_set_visible(w, 0);
    return w;
}

static void show_page(gallery *g, int p)
{
    for (int i = 0; i < NPAGES; i++)
        for (int k = 0; k < g->npage[i]; k++)
            sk_widget_set_visible(g->page[i][k], i == p);
    g->active = p;
}

static unsigned next_rand(gallery *g)
{
    g->rnd = g->rnd * 1103515245u + 12345u;
    return (g->rnd >> 8) & 0xFFFFFF;
}

/* ================================================================ 第 0 页：基础控件 */

static void on_button(sk_widget *w, void *user)
{
    status(user, "按钮「%s」被点击", w->text ? w->text : "");
}

static void on_tool(sk_widget *w, void *user)
{
    static const char *const names[] = { "新建", "打开", "保存", "铅笔", "橡皮", "刷新" };
    gallery *g = user;
    /* 铅笔/橡皮像工具栏里的“开关按钮”：同一时间只按下一个 */
    if (w->id == 3 || w->id == 4) {
        sk_widget_set_checked(g->tool[3], w->id == 3);
        sk_widget_set_checked(g->tool[4], w->id == 4);
    }
    status(g, "工具栏：%s", names[w->id]);
}

static void on_check(sk_widget *w, void *user)
{
    status(user, "复选框「%s」%s", w->text ? w->text : "", sk_widget_checked(w) ? "已勾选" : "已取消");
}

static void on_swatch(sk_widget *w, void *user)
{
    gallery *g = user;
    for (int i = 0; i < 8; i++)
        sk_widget_set_checked(g->swatch[i], g->swatch[i] == w);
    sk_color c = sk_swatch_color(w);
    sk_label_set_color(g->lbl_color, c);
    status(g, "选中颜色 #%06X", (unsigned)(c & 0xFFFFFF));
}

static void on_name_change(sk_widget *w, void *user)
{
    gallery *g = user;
    char buf[200];
    const char *t = sk_textbox_text(w);
    snprintf(buf, sizeof buf, "%s%s", *t ? "你输入了：" : "", t);
    sk_widget_set_text(g->tb_echo, buf);
}

static void on_name_enter(sk_widget *w, void *user)
{
    status(user, "输入框回车：%s", sk_textbox_text(w));
}

static void on_slider(sk_widget *w, void *user)
{
    gallery *g = user;
    int v = sk_slider_value(w);
    char cap[32];
    snprintf(cap, sizeof cap, "%d%%", v);
    sk_meter_set(g->meter, v * 10, cap);
    sk_meter_set(g->meter2, 1000 - v * 10, NULL);
    char lbl[48];
    snprintf(lbl, sizeof lbl, "滑块：%d", v);
    sk_widget_set_text(g->lbl_slider, lbl);
}

static void build_page0(gallery *g)
{
    sk_ui *ui = g->ui;
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    g->sec0[0] = add(g, 0, sk_label_new(ui, z, "文字", SK_FONT_TITLE, SK_ALIGN_LEFT));
    g->lbl_left = add(g, 0, sk_label_new(ui, z, "左对齐的标签", SK_FONT_UI, SK_ALIGN_LEFT));
    g->lbl_center = add(g, 0, sk_label_new(ui, z, "居中的标签", SK_FONT_UI, SK_ALIGN_CENTER));
    g->lbl_right = add(g, 0, sk_label_new(ui, z, "右对齐的标签", SK_FONT_UI, SK_ALIGN_RIGHT));
    g->lbl_color = add(g, 0, sk_label_new(ui, z, "彩色的字（点下面的色块换颜色）", SK_FONT_UI, SK_ALIGN_LEFT));
    sk_label_set_color(g->lbl_color, SK_BLUE);
    g->para = add(g, 0, sk_paragraph_new(ui, z,
        "段落控件会自动换行：中文按字折行，English words wrap at spaces. "
        "所有控件都由 rough.h 的手绘图元画出，同一个控件每次重画都一模一样，"
        "所以局部刷新时不会“抖”。", SK_FONT_UI));

    g->sec0[1] = add(g, 0, sk_label_new(ui, z, "按钮", SK_FONT_TITLE, SK_ALIGN_LEFT));
    g->btn_normal = add(g, 0, sk_button_new(ui, z, "普通按钮", on_button, g));
    g->btn_green = add(g, 0, sk_button_new(ui, z, "绿色按钮", on_button, g));
    sk_widget_set_accent(g->btn_green, SK_GREEN);
    g->btn_red = add(g, 0, sk_button_new(ui, z, "危险操作", on_button, g));
    sk_widget_set_accent(g->btn_red, SK_RED);
    g->btn_disabled = add(g, 0, sk_button_new(ui, z, "禁用的按钮", on_button, g));
    sk_widget_set_enabled(g->btn_disabled, 0);
    static const int tool_icons[6] = { SK_ICON_FILE, SK_ICON_OPEN, SK_ICON_SAVE, SK_ICON_PENCIL, SK_ICON_ERASER,
                                       SK_ICON_REFRESH };
    for (int i = 0; i < 6; i++) {
        g->tool[i] = add(g, 0, sk_icon_button_new(ui, z, tool_icons[i], i == 2 ? "保存" : NULL, on_tool, g));
        if (g->tool[i])
            g->tool[i]->id = i;
    }
    sk_widget_set_checked(g->tool[3], 1);

    g->sec0[2] = add(g, 0, sk_label_new(ui, z, "选择", SK_FONT_TITLE, SK_ALIGN_LEFT));
    g->chk_a = add(g, 0, sk_checkbox_new(ui, z, "自动保存", 1, on_check, g));
    g->chk_b = add(g, 0, sk_checkbox_new(ui, z, "显示网格", 0, on_check, g));
    g->chk_c = add(g, 0, sk_checkbox_new(ui, z, "禁用的选项", 1, on_check, g));
    sk_widget_set_enabled(g->chk_c, 0);
    static const sk_color colors[8] = { SK_INK, SK_BLUE, SK_RED, SK_GREEN, SK_YELLOW, SK_ORANGE, SK_PURPLE, SK_TEAL };
    for (int i = 0; i < 8; i++)
        g->swatch[i] = add(g, 0, sk_swatch_new(ui, z, colors[i], on_swatch, g));
    sk_widget_set_checked(g->swatch[1], 1);

    g->sec0[3] = add(g, 0, sk_label_new(ui, z, "输入与数值", SK_FONT_TITLE, SK_ALIGN_LEFT));
    g->tb_name = add(g, 0, sk_textbox_new(ui, z, "", on_name_change, g));
    sk_textbox_set_placeholder(g->tb_name, "在这里输入你的名字…");
    if (g->tb_name)
        g->tb_name->on_click = on_name_enter;
    g->tb_echo = add(g, 0, sk_label_new(ui, z, "", SK_FONT_UI, SK_ALIGN_LEFT));
    sk_label_set_color(g->tb_echo, SK_INK_SOFT);
    g->lbl_slider = add(g, 0, sk_label_new(ui, z, "滑块：40", SK_FONT_UI, SK_ALIGN_LEFT));
    g->slider = add(g, 0, sk_slider_new(ui, z, 0, 100, 40, on_slider, g));
    g->meter = add(g, 0, sk_meter_new(ui, z, SK_GREEN));
    g->meter2 = add(g, 0, sk_meter_new(ui, z, SK_ORANGE));
    g->meter3 = add(g, 0, sk_meter_new(ui, z, SK_PURPLE));
    sk_meter_set(g->meter3, 730, "内存 73%");
    on_slider(g->slider, g);
}

static void layout_page0(gallery *g, int W, int H)
{
    (void)H;
    int y = CONTENT_Y, x = MARGIN, colw = (W - MARGIN * 3) / 2;
    int x2 = MARGIN * 2 + colw;
    /* 左栏：文字、按钮 */
    sk_widget_set_rect(g->sec0[0], sk_rect_make(x, y, colw, 28));
    sk_widget_set_rect(g->lbl_left, sk_rect_make(x, y + 32, colw, 24));
    sk_widget_set_rect(g->lbl_center, sk_rect_make(x, y + 56, colw, 24));
    sk_widget_set_rect(g->lbl_right, sk_rect_make(x, y + 80, colw, 24));
    sk_widget_set_rect(g->lbl_color, sk_rect_make(x, y + 104, colw, 24));
    sk_widget_set_rect(g->para, sk_rect_make(x, y + 132, colw, 90));
    int by = y + 228;
    sk_widget_set_rect(g->sec0[1], sk_rect_make(x, by, colw, 28));
    int bw = (colw - 10) / 2;
    sk_widget_set_rect(g->btn_normal, sk_rect_make(x, by + 34, bw, 36));
    sk_widget_set_rect(g->btn_green, sk_rect_make(x + bw + 10, by + 34, bw, 36));
    sk_widget_set_rect(g->btn_red, sk_rect_make(x, by + 78, bw, 36));
    sk_widget_set_rect(g->btn_disabled, sk_rect_make(x + bw + 10, by + 78, bw, 36));
    int tx = x;
    for (int i = 0; i < 6; i++) {
        int tw = i == 2 ? 84 : 40;
        sk_widget_set_rect(g->tool[i], sk_rect_make(tx, by + 124, tw, 38));
        tx += tw + 6;
    }
    /* 右栏：选择、输入与数值 */
    sk_widget_set_rect(g->sec0[2], sk_rect_make(x2, y, colw, 28));
    sk_widget_set_rect(g->chk_a, sk_rect_make(x2, y + 34, colw / 2, 28));
    sk_widget_set_rect(g->chk_b, sk_rect_make(x2 + colw / 2, y + 34, colw / 2, 28));
    sk_widget_set_rect(g->chk_c, sk_rect_make(x2, y + 66, colw, 28));
    int sw = (colw - 7 * 6) / 8;
    if (sw > 34)
        sw = 34;
    for (int i = 0; i < 8; i++)
        sk_widget_set_rect(g->swatch[i], sk_rect_make(x2 + i * (sw + 6), y + 102, sw, sw));
    int iy = y + 150;
    sk_widget_set_rect(g->sec0[3], sk_rect_make(x2, iy, colw, 28));
    sk_widget_set_rect(g->tb_name, sk_rect_make(x2, iy + 34, colw, 34));
    sk_widget_set_rect(g->tb_echo, sk_rect_make(x2, iy + 72, colw, 24));
    sk_widget_set_rect(g->lbl_slider, sk_rect_make(x2, iy + 104, 90, 28));
    sk_widget_set_rect(g->slider, sk_rect_make(x2 + 92, iy + 104, colw - 92, 28));
    sk_widget_set_rect(g->meter, sk_rect_make(x2, iy + 142, colw, 26));
    sk_widget_set_rect(g->meter2, sk_rect_make(x2, iy + 176, colw, 26));
    sk_widget_set_rect(g->meter3, sk_rect_make(x2, iy + 210, colw, 26));
}

/* ================================================================ 第 1 页：文本编辑 */

static void update_pos(gallery *g)
{
    int line, col;
    sk_textarea_cursor_pos(g->ta_main, &line, &col);
    char buf[96];
    snprintf(buf, sizeof buf, "第 %d 行，第 %d 列%s", line, col, sk_textarea_modified(g->ta_main) ? " · 已修改" : "");
    sk_widget_set_text(g->lbl_pos, buf);
}

static void on_text_change(sk_widget *w, void *user)
{
    (void)w;
    update_pos(user);
}

static void on_wrap(sk_widget *w, void *user)
{
    gallery *g = user;
    sk_textarea_set_wrap(g->ta_main, sk_widget_checked(w));
    status(g, "自动换行：%s", sk_widget_checked(w) ? "开" : "关（可以水平滚动）");
}

static void on_numbers(sk_widget *w, void *user)
{
    gallery *g = user;
    sk_textarea_set_line_numbers(g->ta_main, sk_widget_checked(w));
    status(g, "行号：%s", sk_widget_checked(w) ? "显示" : "隐藏");
}

static void on_readonly(sk_widget *w, void *user)
{
    gallery *g = user;
    sk_textarea_set_readonly(g->ta_main, sk_widget_checked(w));
    status(g, "只读：%s", sk_widget_checked(w) ? "是" : "否");
}

static void on_clear_text(sk_widget *w, void *user)
{
    (void)w;
    gallery *g = user;
    sk_textarea_set_text(g->ta_main, "");
    update_pos(g);
    status(g, "已清空编辑器");
}

static void on_sample_text(sk_widget *w, void *user)
{
    (void)w;
    gallery *g = user;
    sk_textarea_set_text(g->ta_main, sample_text);
    update_pos(g);
    status(g, "已载入示例文字");
}

static void on_count(sk_widget *w, void *user)
{
    (void)w;
    gallery *g = user;
    char *t = sk_textarea_get_text(g->ta_main);
    if (!t)
        return;
    int chars = 0, cjk = 0, lines = 1, bytes = (int)strlen(t);
    for (const char *s = t; *s;) {
        uint32_t cp = sk_utf8_next(&s);
        chars++;
        if (cp == '\n')
            lines++;
        if (cp >= 0x4E00 && cp <= 0x9FFF)
            cjk++;
    }
    free(t);
    char msg[256];
    snprintf(msg, sizeof msg, "一共 %d 行，%d 个字符（其中汉字 %d 个），UTF-8 编码占 %d 字节。", lines, chars, cjk, bytes);
    status(g, "字数统计：%d 行 %d 字符", lines, chars);
    sk_dialog_message(g->app, "字数统计", msg, "好的");
}

static void on_paste_box(sk_widget *w, void *user)
{
    status(user, "单行输入框：%s", sk_textbox_text(w));
}

static void build_page1(gallery *g)
{
    sk_ui *ui = g->ui;
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    g->chk_wrap = add(g, 1, sk_checkbox_new(ui, z, "自动换行", 1, on_wrap, g));
    g->chk_numbers = add(g, 1, sk_checkbox_new(ui, z, "行号", 0, on_numbers, g));
    g->chk_readonly = add(g, 1, sk_checkbox_new(ui, z, "只读", 0, on_readonly, g));
    g->btn_clear = add(g, 1, sk_button_new(ui, z, "清空", on_clear_text, g));
    g->btn_sample = add(g, 1, sk_button_new(ui, z, "示例文字", on_sample_text, g));
    g->btn_count = add(g, 1, sk_button_new(ui, z, "字数统计", on_count, g));
    g->ta_main = add(g, 1, sk_textarea_new(ui, z, sample_text));
    if (g->ta_main) {
        g->ta_main->on_change = on_text_change;
        g->ta_main->user = g;
    }
    g->lbl_pos = add(g, 1, sk_label_new(ui, z, "", SK_FONT_UI, SK_ALIGN_LEFT));
    sk_label_set_color(g->lbl_pos, SK_INK_SOFT);
    g->lbl_side = add(g, 1, sk_label_new(ui, z, "另一个编辑器（试试互相复制粘贴）：", SK_FONT_UI, SK_ALIGN_LEFT));
    g->ta_side = add(g, 1, sk_textarea_new(ui, z, "把左边的文字选中，Ctrl+C，\n再点这里 Ctrl+V。\n"));
    g->tb_paste = add(g, 1, sk_textbox_new(ui, z, "", NULL, g));
    sk_textbox_set_placeholder(g->tb_paste, "单行输入框也能粘贴");
    if (g->tb_paste)
        g->tb_paste->on_click = on_paste_box;
    update_pos(g);
}

static void layout_page1(gallery *g, int W, int H)
{
    int y = CONTENT_Y, x = MARGIN;
    sk_widget_set_rect(g->chk_wrap, sk_rect_make(x, y + 4, 110, 28));
    sk_widget_set_rect(g->chk_numbers, sk_rect_make(x + 116, y + 4, 80, 28));
    sk_widget_set_rect(g->chk_readonly, sk_rect_make(x + 200, y + 4, 80, 28));
    int bx = W - MARGIN - 3 * 100 - 2 * 8;
    sk_widget_set_rect(g->btn_clear, sk_rect_make(bx, y, 100, 34));
    sk_widget_set_rect(g->btn_sample, sk_rect_make(bx + 108, y, 100, 34));
    sk_widget_set_rect(g->btn_count, sk_rect_make(bx + 216, y, 100, 34));
    int top = y + 46, bottom = H - STATUS_H - 8;
    int side_w = (W - MARGIN * 3) * 32 / 100;
    if (side_w < 180)
        side_w = 180;
    int main_w = W - MARGIN * 3 - side_w;
    sk_widget_set_rect(g->ta_main, sk_rect_make(x, top, main_w, bottom - top - 30));
    sk_widget_set_rect(g->lbl_pos, sk_rect_make(x + 4, bottom - 26, main_w, 24));
    int sx = x + main_w + MARGIN;
    sk_widget_set_rect(g->lbl_side, sk_rect_make(sx, top, side_w, 24));
    sk_widget_set_rect(g->ta_side, sk_rect_make(sx, top + 28, side_w, bottom - top - 28 - 76));
    sk_widget_set_rect(g->tb_paste, sk_rect_make(sx, bottom - 64, side_w, 34));
}

/* ================================================================ 第 2 页：列表 */

static const char *const proc_names[] = {
    "sinit", "sketchd", "sksh", "skterm", "skfiles", "sknote", "skpaint", "skmon", "skclock", "skabout",
    "kthreadd", "kworker/0:1", "ksoftirqd/0", "rcu_sched", "migration/0", "kswapd0", "skwidgets", "cat",
    "sleep", "top", "ls", "grep", "sort", "uniq", "make", "cc1", "as", "ld", "python3", "vim", "httpd", "sshd",
};

static void proc_cells(const fake_proc *p, char cells[4][32])
{
    snprintf(cells[0], 32, "%s", p->name);
    snprintf(cells[1], 32, "%d", p->pid);
    snprintf(cells[2], 32, "%d.%d%%", p->cpu / 10, p->cpu % 10);
    if (p->mem_kb >= 1024)
        snprintf(cells[3], 32, "%.1f MB", p->mem_kb / 1024.0);
    else
        snprintf(cells[3], 32, "%d KB", p->mem_kb);
}

static gallery *g_sort;   /* qsort 比较函数拿不到 user 参数（单线程，安全） */

static int proc_cmp(const void *pa, const void *pb)
{
    const fake_proc *a = pa, *b = pb;
    int r;
    switch (g_sort->sort_col) {
    case 1: r = a->pid - b->pid; break;
    case 2: r = a->cpu - b->cpu; break;
    case 3: r = a->mem_kb - b->mem_kb; break;
    default: r = strcmp(a->name, b->name); break;
    }
    if (r == 0)
        r = a->pid - b->pid;
    return g_sort->sort_desc ? -r : r;
}

/* 排序后整个列表重建：clear + add。列表会和旧内容逐行比较，只重画变了的行 */
static void refill_list(gallery *g)
{
    int sel = sk_list_selected(g->list);
    int keep_pid = sel >= 0 ? (int)sk_list_data(g->list, sel) : -1;
    g_sort = g;
    qsort(g->procs, g->nprocs, sizeof(fake_proc), proc_cmp);
    sk_list_clear(g->list);
    for (int i = 0; i < g->nprocs; i++) {
        char c[4][32];
        proc_cells(&g->procs[i], c);
        const char *cells[4] = { c[0], c[1], c[2], c[3] };
        int row = sk_list_add(g->list, g->procs[i].icon, cells, g->procs[i].pid);
        if (g->procs[i].pid == keep_pid)
            sk_list_select(g->list, row);
    }
}

static void on_sort(sk_widget *w, int col, void *user)
{
    (void)w;
    gallery *g = user;
    /* 和列表表头小三角的规则一致：同一列再点一次切换升降序 */
    if (g->sort_col == col)
        g->sort_desc = !g->sort_desc;
    else {
        g->sort_col = col;
        g->sort_desc = 0;
    }
    refill_list(g);
    static const char *const names[] = { "进程", "PID", "CPU", "内存" };
    status(g, "列表排序：%s %s", names[col], g->sort_desc ? "降序" : "升序");
}

static void on_list_change(sk_widget *w, void *user)
{
    gallery *g = user;
    int row = sk_list_selected(w);
    if (row < 0)
        return;
    status(g, "选中：第 %d 行 %s（PID %s）", row + 1, sk_list_cell(w, row, 0), sk_list_cell(w, row, 1));
}

static void on_list_activate(sk_widget *w, void *user)
{
    gallery *g = user;
    int row = sk_list_selected(w);
    if (row < 0)
        return;
    char msg[256];
    snprintf(msg, sizeof msg, "进程名：%s\nPID：%s\nCPU：%s\n内存：%s", sk_list_cell(w, row, 0),
             sk_list_cell(w, row, 1), sk_list_cell(w, row, 2), sk_list_cell(w, row, 3));
    status(g, "打开：%s", sk_list_cell(w, row, 0));
    sk_dialog_message(g->app, "进程信息", msg, "关闭");
}

/* “实时刷新”：每秒随机改几个进程的 CPU/内存，只用 sk_list_set_cell 更新变了的单元格 */
static void live_tick(sk_app *app, void *user)
{
    (void)app;
    gallery *g = user;
    for (int row = 0; row < sk_list_count(g->list); row++) {
        intptr_t pid = sk_list_data(g->list, row);
        for (int i = 0; i < g->nprocs; i++) {
            fake_proc *p = &g->procs[i];
            if (p->pid != pid)
                continue;
            if (next_rand(g) % 4 == 0) {
                p->cpu = (int)(next_rand(g) % 300);
                p->mem_kb += (int)(next_rand(g) % 200) - 100;
                if (p->mem_kb < 100)
                    p->mem_kb = 100;
            }
            char c[4][32];
            proc_cells(p, c);
            sk_list_set_cell(g->list, row, 2, c[2]);
            sk_list_set_cell(g->list, row, 3, c[3]);
        }
    }
}

static void on_live(sk_widget *w, void *user)
{
    gallery *g = user;
    if (sk_widget_checked(w) && !g->live_timer)
        g->live_timer = sk_app_add_timer(g->app, 1000, 1, live_tick, g);
    else if (!sk_widget_checked(w) && g->live_timer) {
        sk_app_remove_timer(g->app, g->live_timer);
        g->live_timer = 0;
    }
    status(g, "实时刷新：%s", g->live_timer ? "开（每秒）" : "关");
}

static void add_proc(gallery *g)
{
    if (g->nprocs >= (int)(sizeof g->procs / sizeof g->procs[0]))
        return;
    fake_proc *p = &g->procs[g->nprocs];
    int k = g->nprocs % (int)(sizeof proc_names / sizeof proc_names[0]);
    snprintf(p->name, sizeof p->name, "%s", proc_names[k]);
    p->pid = g->nprocs < 10 ? 1 + g->nprocs * 7 : 100 + g->nprocs * 13;
    p->cpu = (int)(next_rand(g) % 250);
    p->mem_kb = 300 + (int)(next_rand(g) % 30000);
    p->icon = k < 10 ? SK_ICON_PROGRAM : k < 16 ? SK_ICON_KERNEL : SK_ICON_PROCESS;
    g->nprocs++;
}

static void on_add_row(sk_widget *w, void *user)
{
    (void)w;
    gallery *g = user;
    add_proc(g);
    refill_list(g);
    status(g, "添加了一行，共 %d 行", sk_list_count(g->list));
}

static void on_rebuild(sk_widget *w, void *user)
{
    (void)w;
    gallery *g = user;
    refill_list(g);
    status(g, "重建列表（内容没变：只重画选中行）");
}

static void on_clear_list(sk_widget *w, void *user)
{
    (void)w;
    gallery *g = user;
    g->nprocs = 0;
    sk_list_clear(g->list);
    status(g, "列表已清空");
}

static void on_icon_list(sk_widget *w, void *user)
{
    gallery *g = user;
    int row = sk_list_selected(w);
    if (row >= 0)
        status(g, "应用：%s", sk_list_cell(w, row, 0));
}

static void build_page2(gallery *g)
{
    sk_ui *ui = g->ui;
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    g->list = add(g, 2, sk_list_new(ui, z, on_list_change, on_list_activate, g));
    static const char *const cols[] = { "进程", "PID", "CPU", "内存" };
    static const int widths[] = { 0, -70, -80, -100 };
    sk_list_set_columns(g->list, cols, widths, 4);
    sk_list_set_sort_handler(g->list, on_sort);
    sk_widget_set_text(g->list, "（没有进程）");
    g->rnd = 2024;
    for (int i = 0; i < 26; i++)
        add_proc(g);
    g->sort_col = 0;
    refill_list(g);

    g->icons_list = add(g, 2, sk_list_new(ui, z, on_icon_list, NULL, g));
    sk_list_set_row_height(g->icons_list, 38);
    static const struct { int icon; const char *name; } apps[] = {
        { SK_ICON_TERMINAL, "终端" }, { SK_ICON_FOLDER, "文件" }, { SK_ICON_NOTEPAD, "记事本" },
        { SK_ICON_PAINT, "画板" }, { SK_ICON_MONITOR, "系统监视器" }, { SK_ICON_CLOCK, "时钟" },
        { SK_ICON_ABOUT, "关于本机" }, { SK_ICON_SETTINGS, "设置" }, { SK_ICON_BOOK, "文档" },
    };
    for (size_t i = 0; i < sizeof apps / sizeof apps[0]; i++) {
        const char *cells[1] = { apps[i].name };
        sk_list_add(g->icons_list, apps[i].icon, cells, (intptr_t)i);
    }
    g->chk_live = add(g, 2, sk_checkbox_new(ui, z, "实时刷新", 0, on_live, g));
    g->btn_add = add(g, 2, sk_button_new(ui, z, "添加一行", on_add_row, g));
    g->btn_rebuild = add(g, 2, sk_button_new(ui, z, "重建", on_rebuild, g));
    g->btn_clearlist = add(g, 2, sk_button_new(ui, z, "清空", on_clear_list, g));
    g->lbl_list = add(g, 2, sk_paragraph_new(ui, z,
        "点表头排序（再点一次反过来），双击或回车打开一行，上下键 / PageUp / PageDown / Home / End 移动，"
        "输入首字母快速定位。", SK_FONT_UI));
}

static void layout_page2(gallery *g, int W, int H)
{
    int y = CONTENT_Y, x = MARGIN;
    int bottom = H - STATUS_H - 8;
    int side = 200;
    int lw = W - MARGIN * 3 - side;
    sk_widget_set_rect(g->chk_live, sk_rect_make(x, y + 4, 110, 28));
    sk_widget_set_rect(g->btn_add, sk_rect_make(x + 120, y, 100, 34));
    sk_widget_set_rect(g->btn_rebuild, sk_rect_make(x + 228, y, 80, 34));
    sk_widget_set_rect(g->btn_clearlist, sk_rect_make(x + 316, y, 80, 34));
    sk_widget_set_rect(g->list, sk_rect_make(x, y + 44, lw, bottom - y - 44 - 52));
    sk_widget_set_rect(g->lbl_list, sk_rect_make(x, bottom - 48, lw, 48));
    sk_widget_set_rect(g->icons_list, sk_rect_make(x + lw + MARGIN, y, side, bottom - y));
}

/* ================================================================ 第 3 页：对话框 */

static void on_dialog(sk_widget *w, void *user)
{
    gallery *g = user;
    char res[1200];
    switch (w->id) {
    case 0: {
        int r = sk_dialog_message(g->app, "提示", "文件已经保存好了。", "确定");
        snprintf(res, sizeof res, "消息框返回 %d", r);
        break;
    }
    case 1: {
        static const char *const names[] = { "保存", "不保存", "取消" };
        int r = sk_dialog_message(g->app, "记事本", "文档“未命名.txt”还没有保存。\n要保存更改吗？", "保存|不保存|取消");
        snprintf(res, sizeof res, "询问框返回 %d（%s）", r, r >= 0 ? names[r] : "关闭/Esc");
        break;
    }
    case 2: {
        int r = sk_dialog_message(g->app, "警告", "磁盘空间只剩下 3%。\n请删除一些不需要的文件，否则可能无法保存新的文档。",
                                  "我知道了");
        snprintf(res, sizeof res, "警告框返回 %d", r);
        break;
    }
    case 3: {
        int r = sk_dialog_message(g->app, "错误", "无法打开 /root/不存在.txt：没有那个文件或目录", "确定|重试");
        snprintf(res, sizeof res, "错误框返回 %d", r);
        break;
    }
    case 4: {
        char *s = sk_dialog_input(g->app, "重命名", "请输入新的文件名：", "新建文本文档.txt");
        if (s)
            snprintf(res, sizeof res, "输入框返回：“%s”", s);
        else
            snprintf(res, sizeof res, "输入框：取消了");
        free(s);
        break;
    }
    case 5:
    case 6: {
        char *path = sk_dialog_file(g->app, w->id == 5 ? "打开文件" : "另存为", NULL, w->id == 6,
                                    w->id == 6 ? "我的笔记.txt" : NULL);
        if (path)
            snprintf(res, sizeof res, "%s：%s", w->id == 5 ? "打开" : "保存到", path);
        else
            snprintf(res, sizeof res, "文件对话框：取消了");
        free(path);
        break;
    }
    default:
        return;
    }
    /* 对话框关闭后，按钮回调才继续执行：这就是“阻塞式”对话框 */
    sk_widget_set_text(g->dlg_result, res);
    status(g, "%s", res);
}

static void build_page3(gallery *g)
{
    sk_ui *ui = g->ui;
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    static const char *const labels[7] = { "提示消息", "询问（三个按钮）", "警告", "错误", "输入框",
                                           "打开文件…", "保存文件…" };
    static const sk_color accents[7] = { SK_BLUE, SK_PURPLE, SK_YELLOW, SK_RED, SK_TEAL, SK_ORANGE, SK_GREEN };
    g->dlg_intro = add(g, 3, sk_paragraph_new(ui, z,
        "对话框是独立的小窗口，调用会“阻塞”直到用户做出选择——内部是一个嵌套的事件循环，"
        "并且把对话框设为模态窗口（主窗口暂时收不到鼠标和键盘）。回车 = 第一个按钮，Esc = 取消。"
        "消息框会根据标题和内容里的关键词（提示/警告/错误/是否…）自动配上手绘图标。", SK_FONT_UI));
    for (int i = 0; i < 7; i++) {
        g->dlg_btn[i] = add(g, 3, sk_button_new(ui, z, labels[i], on_dialog, g));
        if (g->dlg_btn[i]) {
            g->dlg_btn[i]->id = i;
            sk_widget_set_accent(g->dlg_btn[i], accents[i]);
        }
    }
    g->dlg_result = add(g, 3, sk_paragraph_new(ui, z, "（结果显示在这里）", SK_FONT_UI));
}

static void layout_page3(gallery *g, int W, int H)
{
    (void)H;
    int y = CONTENT_Y + 8, x = MARGIN, w = W - MARGIN * 2;
    sk_widget_set_rect(g->dlg_intro, sk_rect_make(x, y, w, 80));
    int bw = (w - 3 * 12) / 4;
    for (int i = 0; i < 7; i++)
        sk_widget_set_rect(g->dlg_btn[i], sk_rect_make(x + (i % 4) * (bw + 12), y + 96 + (i / 4) * 50, bw, 38));
    sk_widget_set_rect(g->dlg_result, sk_rect_make(x, y + 210, w, 60));
}

/* ================================================================ 第 4 页：绘图与菜单 */

/* rough.h 的样张：各种填充方式、线条、曲线、圆弧、自由笔触 */
static void sampler_draw(sk_widget *w, sk_canvas *c, void *user)
{
    gallery *g = user;
    sk_rect r = w->r;
    const sk_font *f = sk_font_get(SK_FONT_UI);
    static const char *const fill_names[] = { "排线", "交叉排线", "锯齿", "圆点", "实心", "荧光笔" };
    static const int fills[] = { SK_FILL_HACHURE, SK_FILL_CROSS_HATCH, SK_FILL_ZIGZAG, SK_FILL_DOTS, SK_FILL_SOLID,
                                 SK_FILL_MARKER };
    static const sk_color cols[] = { SK_BLUE, SK_RED, SK_GREEN, SK_PURPLE, SK_ORANGE, SK_YELLOW };
    int cw = (r.w - 20) / 6;
    for (int i = 0; i < 6; i++) {
        sk_rough_style s;
        sk_style_hatched(&s, cols[i], sk_hash_u32(g->sampler_seed, (uint32_t)i));
        s.fill_style = fills[i];
        if (fills[i] == SK_FILL_MARKER)
            s.fill = SK_WITH_ALPHA(cols[i], 0xA0);
        float bx = (float)(r.x + 10 + i * cw), by = (float)r.y + 12;
        if (i % 2 == 0)
            sk_rough_rect(c, bx + 6, by, (float)cw - 16, 70, &s);
        else
            sk_rough_ellipse(c, bx + cw / 2.0f - 2, by + 35, (float)cw - 16, 70, &s);
        sk_draw_text_in_rect(c, f, sk_rect_make((int)bx, (int)by + 76, cw - 4, 24), fill_names[i], SK_INK_SOFT,
                             SK_ALIGN_CENTER);
    }
    int y2 = r.y + 130;
    /* 线条：不同粗糙度 */
    for (int i = 0; i < 3; i++) {
        sk_rough_style s;
        sk_style_pencil(&s, sk_hash_u32(g->sampler_seed, 100u + i));
        s.roughness = 0.4f + i * 0.9f;
        s.stroke_width = 1.4f + i * 0.4f;
        sk_rough_line(c, (float)r.x + 20, (float)(y2 + i * 22), (float)r.x + r.w * 0.42f, (float)(y2 + i * 22 + 6), &s);
    }
    sk_draw_text(c, f, r.x + 20, y2 + 82, "直线：粗糙度 0.4 / 1.3 / 2.2", SK_INK_SOFT);
    /* 曲线 + 圆弧 */
    sk_pointf pts[6];
    for (int i = 0; i < 6; i++)
        pts[i] = (sk_pointf){ (float)r.x + r.w * 0.48f + i * (r.w * 0.06f), (float)y2 + 30 + ((i % 2) ? -26 : 26) };
    sk_rough_style s;
    sk_style_pencil(&s, sk_hash_u32(g->sampler_seed, 200u));
    s.stroke = SK_BLUE;
    s.stroke_width = 2.0f;
    sk_rough_curve(c, pts, 6, &s);
    sk_style_hatched(&s, SK_TEAL, sk_hash_u32(g->sampler_seed, 201u));
    sk_rough_arc(c, (float)r.x + r.w * 0.9f, (float)y2 + 34, 76, 76, -0.6f, 4.2f, 1, &s);
    sk_draw_text(c, f, (int)(r.x + r.w * 0.5f), y2 + 82, "曲线与扇形", SK_INK_SOFT);
    /* 自由笔触 + 下划线 */
    int y3 = y2 + 116;
    sk_pointf stroke[8];
    for (int i = 0; i < 8; i++)
        stroke[i] = (sk_pointf){ (float)r.x + 24 + i * 34.0f, (float)y3 + 18 + ((i * 37 + (int)g->sampler_seed) % 21) - 10 };
    sk_brush_stroke(c, stroke, 8, 1.0f, 5.0f, SK_RED, 1, g->sampler_seed);
    sk_draw_text(c, f, r.x + 330, y3 + 24, "铅笔/毛笔笔触（起笔细、收笔粗）", SK_INK_SOFT);
    sk_rough_underline(c, (float)r.x + 330, (float)r.x + 580, (float)y3 + 32, SK_ORANGE, 2.0f, g->sampler_seed);
    sk_draw_text(c, f, r.x + 20, r.y + r.h - 14, "（点一下样张换一种笔迹：手绘的随机数来自种子，同一种子永远画得一模一样；右键弹出菜单）",
                 SK_INK_FAINT);
}

static int sampler_event(sk_widget *w, const sk_event *e, void *user)
{
    gallery *g = user;
    if (e->type != SK_EVENT_MOUSE || e->mouse.kind != SK_MOUSE_DOWN)
        return 0;
    if (e->mouse.button == SK_BUTTON_LEFT) {
        g->sampler_seed = g->sampler_seed * 2654435761u + 1;
        sk_widget_invalidate(w);
        status(g, "换了一种笔迹（种子 %u）", g->sampler_seed);
        return 1;
    }
    return 0;   /* 右键交给窗口的事件回调：弹出菜单 */
}

static const char *const menu_items[] = { "撤销", "-", "剪切", "复制", "粘贴", NULL, "全选", "关于控件展示…" };

static void on_menu_pick(sk_ui *ui, int index, void *user)
{
    (void)ui;
    gallery *g = user;
    char buf[96];
    snprintf(buf, sizeof buf, "菜单：选了第 %d 项「%s」", index, menu_items[index] ? menu_items[index] : "-");
    sk_widget_set_text(g->lbl_menu, buf);
    status(g, "%s", buf);
    if (index == 7)
        sk_dialog_message(g->app, "关于控件展示",
                          "skwidgets 展示了 libsketch 控件库里的所有控件。\n源代码：userspace/gui/apps/skwidgets/main.c",
                          "确定");
}

static void on_menu_button(sk_widget *w, void *user)
{
    gallery *g = user;
    sk_menu_popup(g->ui, w->r.x, w->r.y + w->r.h + 2, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]),
                  on_menu_pick, g);
}

static void on_resketch(sk_widget *w, void *user)
{
    (void)w;
    gallery *g = user;
    g->sampler_seed = g->sampler_seed * 2654435761u + 7;
    sk_widget_invalidate(g->sampler);
    status(g, "换了一种笔迹（种子 %u）", g->sampler_seed);
}

static void build_page4(gallery *g)
{
    sk_ui *ui = g->ui;
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    g->sampler_seed = 7;
    g->sampler = add(g, 4, sk_custom_new(ui, z, sampler_draw, sampler_event, 0, g));
    g->btn_resketch = add(g, 4, sk_button_new(ui, z, "换一种笔迹", on_resketch, g));
    g->btn_menu = add(g, 4, sk_button_new(ui, z, "弹出菜单…", on_menu_button, g));
    g->lbl_menu = add(g, 4, sk_label_new(ui, z, "", SK_FONT_UI, SK_ALIGN_LEFT));
    sk_label_set_color(g->lbl_menu, SK_INK_SOFT);
}

static void layout_page4(gallery *g, int W, int H)
{
    int y = CONTENT_Y, x = MARGIN;
    sk_widget_set_rect(g->btn_resketch, sk_rect_make(x, y, 120, 34));
    sk_widget_set_rect(g->btn_menu, sk_rect_make(x + 130, y, 120, 34));
    sk_widget_set_rect(g->lbl_menu, sk_rect_make(x + 262, y, W - x - 262 - MARGIN, 34));
    sk_widget_set_rect(g->sampler, sk_rect_make(x, y + 44, W - MARGIN * 2, H - STATUS_H - 8 - (y + 44)));
}

/* ================================================================ 窗口 */

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    gallery *g = user;
    sk_widget_set_rect(g->title, sk_rect_make(MARGIN, 8, W - MARGIN * 2, 32));
    sk_widget_set_rect(g->tabs, sk_rect_make(MARGIN, TABS_Y, W - MARGIN * 2, 40));
    sk_widget_set_rect(g->status, sk_rect_make(MARGIN, H - STATUS_H, W - MARGIN * 2, STATUS_H - 4));
    layout_page0(g, W, H);
    layout_page1(g, W, H);
    layout_page2(g, W, H);
    layout_page3(g, W, H);
    layout_page4(g, W, H);
}

/* 底部状态栏上方画一条手绘分隔线 */
static void background(sk_ui *ui, sk_canvas *c, sk_rect dirty, void *user)
{
    (void)ui;
    gallery *g = user;
    sk_paper_fill(c, dirty, SK_PAPER, 3, 0x5EEDu);
    int W = sk_window_width(g->win), H = sk_window_height(g->win);
    int y = H - STATUS_H - 4;
    if (dirty.y <= y + 3 && dirty.y + dirty.h >= y - 3)
        sk_draw_separator(c, MARGIN, W - MARGIN, y, 0xBADu);
}

static void on_tab(sk_widget *w, void *user)
{
    gallery *g = user;
    int p = sk_tabs_active(w);
    show_page(g, p);
    status(g, "切换到「%s」", tab_labels[p]);
}

/* 窗口级事件：右键菜单（样张上）、Ctrl+Q 退出 */
static int on_event(sk_window *win, const sk_event *e, void *user)
{
    (void)win;
    gallery *g = user;
    if (e->type == SK_EVENT_MOUSE && e->mouse.kind == SK_MOUSE_DOWN && e->mouse.button == SK_BUTTON_RIGHT) {
        sk_menu_popup(g->ui, e->mouse.x, e->mouse.y, menu_items, (int)(sizeof menu_items / sizeof menu_items[0]),
                      on_menu_pick, g);
        return 1;
    }
    if (e->type == SK_EVENT_KEY && e->key.state == 1 && (e->key.modifiers & SK_MOD_CTRL) &&
        (e->key.codepoint == 'q' || e->key.codepoint == 'Q')) {
        sk_app_quit(g->app, 0);
        return 1;
    }
    return 0;
}

/* --demo：直接打开一个对话框，把结果打印出来（自动化测试用） */
static int run_demo(gallery *g, const char *kind, const char *arg)
{
    int r = -2;
    char *s = NULL;
    if (strcmp(kind, "message") == 0)
        r = sk_dialog_message(g->app, "提示", "文件已经保存好了。", "确定");
    else if (strcmp(kind, "ask") == 0)
        r = sk_dialog_message(g->app, "记事本", "文档“未命名.txt”还没有保存。\n要保存更改吗？", "保存|不保存|取消");
    else if (strcmp(kind, "warn") == 0)
        r = sk_dialog_message(g->app, "警告", "磁盘空间只剩下 3%。\n请删除一些不需要的文件，否则可能无法保存新的文档。",
                              "我知道了");
    else if (strcmp(kind, "error") == 0)
        r = sk_dialog_message(g->app, "错误", "无法打开 /root/不存在.txt：没有那个文件或目录", "确定|重试");
    else if (strcmp(kind, "input") == 0)
        s = sk_dialog_input(g->app, "重命名", "请输入新的文件名：", arg ? arg : "新建文本文档.txt");
    else if (strcmp(kind, "open") == 0)
        s = sk_dialog_file(g->app, NULL, arg, 0, NULL);
    else if (strcmp(kind, "save") == 0)
        s = sk_dialog_file(g->app, NULL, arg, 1, "我的笔记.txt");
    else {
        fprintf(stderr, "skwidgets: 未知的对话框类型 %s\n", kind);
        return 2;
    }
    if (r != -2)
        printf("demo result: %d\n", r);
    else
        printf("demo result: %s\n", s ? s : "(NULL)");
    fflush(stdout);
    free(s);
    return 0;
}

int main(int argc, char **argv)
{
    gallery g;
    memset(&g, 0, sizeof g);
    int tab = 0;
    const char *demo = NULL, *demo_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tab") == 0 && i + 1 < argc)
            tab = atoi(argv[++i]);
        else if (strcmp(argv[i], "--log") == 0)
            g.log = 1;
        else if (strcmp(argv[i], "--demo") == 0 && i + 1 < argc) {
            demo = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-')
                demo_arg = argv[++i];
        } else {
            fprintf(stderr, "用法: skwidgets [--tab 0-4] [--log] [--demo message|ask|warn|error|input|open|save [参数]]\n");
            return 2;
        }
    }
    if (tab < 0 || tab >= NPAGES)
        tab = 0;

    g.app = sk_app_connect("skwidgets");
    if (!g.app)
        return 1;
    g.win = sk_window_create(g.app, "控件展示", 860, 620, SK_WIN_RESIZABLE | SK_WIN_CENTER, SK_PURPLE);
    if (!g.win)
        return 1;
    sk_window_set_min_size(g.win, 700, 520);
    g.ui = sk_ui_new(g.win);
    if (!g.ui)
        return 1;

    g.active = tab;
    g.title = sk_label_new(g.ui, sk_rect_make(0, 0, 10, 10), "控件展示 · libsketch 手绘控件样品册", SK_FONT_TITLE,
                           SK_ALIGN_LEFT);
    g.tabs = sk_tabs_new(g.ui, sk_rect_make(0, 0, 10, 10), tab_labels, NPAGES, on_tab, &g);
    g.status = sk_label_new(g.ui, sk_rect_make(0, 0, 10, 10), "", SK_FONT_UI, SK_ALIGN_LEFT);
    sk_label_set_color(g.status, SK_INK_SOFT);
    build_page0(&g);
    build_page1(&g);
    build_page2(&g);
    build_page3(&g);
    build_page4(&g);
    sk_tabs_set_active(g.tabs, tab);
    show_page(&g, tab);
    sk_ui_set_background(g.ui, background, &g);
    sk_ui_set_layout(g.ui, layout, &g);
    sk_ui_set_handler(g.ui, on_event, &g);
    status(&g, "就绪：%d 页控件，切换标签页看看", NPAGES);

    if (demo) {
        /* 先把主窗口画出来（对话框在它上面），再打开对话框 */
        sk_ui_redraw_now(g.ui);
        int rc = run_demo(&g, demo, demo_arg);
        sk_app_disconnect(g.app);
        return rc;
    }
    int rc = sk_app_run(g.app);
    sk_ui_free(g.ui);
    sk_app_disconnect(g.app);
    return rc;
}
