/*
 * skhello — SketchOS 图形应用入门教程（“你好，世界”）
 *
 * 这是一个尽量小、但五脏俱全的图形应用：一个窗口，里面有标题、说明文字、
 * 一个输入框和一个按钮；输入名字、点按钮（或按回车），下面就会跟你打招呼。
 * 建议从头到尾读一遍注释，读完就知道 SketchOS 上的图形程序是怎么工作的。
 *
 * ┌─────────────── 这个程序运行时，底下发生了什么？ ───────────────┐
 *
 *  1. 连接显示服务器（sk_app_connect）
 *     图形界面不是内核画的，而是一个普通的用户进程 sketchd 负责：它独占显卡
 *     （/dev/dri/card0）、键盘和鼠标（/dev/input/event*）。别的程序想显示东西，
 *     就用 Unix 域套接字连接到 /run/sketch/display.sock，说一声 HELLO，
 *     服务器回答 WELCOME（顺便告诉我们屏幕多大）。
 *
 *  2. 创建窗口（sk_window_create）
 *     库用 memfd_create() 向内核要一块“匿名共享内存”，大小 = 宽 × 高 × 4 字节，
 *     每 4 个字节是一个像素（0xAARRGGBB）。然后把这块内存的文件描述符**通过套接字
 *     传给 sketchd**（SCM_RIGHTS）。从此我们和服务器映射的是同一批物理内存页：
 *     我们往里画，服务器直接读，一个像素都不用拷贝。
 *
 *  3. 摆控件（sk_ui_new、sk_label_new、sk_button_new ……）
 *     控件库是“保留模式”的：我们只告诉它“这里有个按钮”，它记住所有控件，
 *     负责画它们、把鼠标键盘事件分给它们。我们只需要写“按钮被点了要做什么”。
 *
 *  4. 事件循环（sk_app_run）
 *     程序大部分时间睡在 poll() 系统调用里，CPU 占用为 0。
 *     你按下一个键时：
 *       键盘硬件中断 → 内核 i8042/evdev 驱动 → sketchd 从 /dev/input/event0 读到按键
 *       → sketchd 把按键发给拥有焦点的窗口（就是我们）→ poll() 返回，库读出事件
 *       → 交给输入框控件 → 输入框把新字符存进去，并把自己标记为“脏”
 *       → 本轮循环结束前，库只重画脏的那一小块（不是整个窗口！）
 *       → 发 DAMAGE 报文告诉 sketchd “这块变了” → sketchd 只把这一块合成到屏幕上。
 *     在网页版（v86 模拟器，CPU 慢几十倍）里，正是“只重画变了的部分”让界面依然流畅。
 *
 * └────────────────────────────────────────────────────────────────┘
 *
 * 想自己写一个应用？复制这个目录，改个名字（目录名 = 程序名），
 * 在 /etc/sketch/apps.conf 里加一行，它就会出现在桌面上。
 */
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>

#include <stdio.h>
#include <string.h>

/* 应用的所有状态放在一个结构体里，回调函数通过 user 指针拿到它（不用全局变量满天飞） */
typedef struct {
    sk_app    *app;
    sk_ui     *ui;
    sk_widget *logo, *title, *intro, *name_label, *name, *button, *greeting, *footer;
    int        greetings;           /* 打了几次招呼 */
} hello_app;

/* 每次打招呼换一种蜡笔颜色 */
static const sk_color crayons[] = { SK_BLUE, SK_RED, SK_GREEN, SK_PURPLE, SK_ORANGE, SK_TEAL };

/* ---------------------------------------------------------------- 回调 */

/* 按钮被点击（或者在输入框里按了回车）时，控件库调用这个函数 */
static void on_greet(sk_widget *w, void *user)
{
    (void)w;
    hello_app *h = user;
    const char *name = sk_textbox_text(h->name);    /* 输入框的内容（UTF-8 字符串） */
    char text[160];
    if (*name)
        snprintf(text, sizeof text, "你好，%s！欢迎来到 SketchOS。", name);
    else
        snprintf(text, sizeof text, "你还没告诉我你的名字呢～");

    /* 改文字、改颜色：这两个函数内部都只会把标签那一块标记为“脏”，
     * 等这一轮事件处理完，库再统一重画——我们从来不需要自己调用“重画”。 */
    sk_widget_set_text(h->greeting, text);
    sk_label_set_color(h->greeting, crayons[h->greetings % (int)(sizeof crayons / sizeof crayons[0])]);

    h->greetings++;
    char footer[64];
    snprintf(footer, sizeof footer, "已经打了 %d 次招呼", h->greetings);
    sk_widget_set_text(h->footer, footer);

    /* 打完招呼，把键盘焦点放回输入框，方便接着输入下一个名字 */
    sk_ui_focus(h->ui, h->name);
}

/* 自定义控件：画一个手绘的 SketchOS 标志。sk_custom_new 让我们自己决定怎么画 */
static void draw_logo(sk_widget *w, sk_canvas *c, void *user)
{
    (void)user;
    /* 画在控件矩形里；seed 让手绘抖动每次重画都一模一样 */
    sk_draw_icon(c, SK_ICON_SKETCH_LOGO, w->r.x, w->r.y, w->r.w < w->r.h ? w->r.w : w->r.h, w->seed);
}

/* 窗口大小改变时（以及刚设置时）调用：在这里重新摆放控件。
 * 窗口可以被用户拖大拖小，所以位置都按窗口宽度 width 来算，而不是写死。 */
static void layout(sk_ui *ui, int width, int height, void *user)
{
    (void)ui;
    hello_app *h = user;
    int pad = 24;
    int inner = width - 2 * pad;

    sk_widget_set_rect(h->logo, sk_rect_make(pad, 20, 44, 44));
    sk_widget_set_rect(h->title, sk_rect_make(pad + 56, 26, inner - 56, 32));
    sk_widget_set_rect(h->intro, sk_rect_make(pad, 78, inner, 48));

    int y = 140;
    int button_w = 100;
    sk_widget_set_rect(h->name_label, sk_rect_make(pad, y + 5, 56, 26));
    sk_widget_set_rect(h->name, sk_rect_make(pad + 56, y, inner - 56 - button_w - 12, 36));
    sk_widget_set_rect(h->button, sk_rect_make(width - pad - button_w, y, button_w, 36));

    sk_widget_set_rect(h->greeting, sk_rect_make(pad, y + 60, inner, 34));
    sk_widget_set_rect(h->footer, sk_rect_make(pad, height - 36, inner, 24));
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    hello_app h;
    memset(&h, 0, sizeof h);

    /* 第 1 步：连接显示服务器。失败（例如没有图形界面）时库会在 stderr 说明原因 */
    h.app = sk_app_connect("skhello");
    if (!h.app)
        return 1;

    /* 第 2 步：创建一个 480x330 的窗口。
     * SK_WIN_CENTER = 放在屏幕中间；SK_WIN_RESIZABLE = 允许用户拖动右下角改变大小；
     * 最后一个参数是标题栏的主题色（黄色蜡笔）。 */
    sk_window *win = sk_window_create(h.app, "你好", 480, 330, SK_WIN_CENTER | SK_WIN_RESIZABLE, SK_YELLOW);
    if (!win) {
        sk_app_disconnect(h.app);
        return 1;
    }
    sk_window_set_min_size(win, 380, 300);

    /* 第 3 步：在窗口上建一个控件库，然后放控件。
     * 先随便给个位置（0,0,0,0），真正的位置由下面的 layout() 统一计算。 */
    sk_rect r0 = sk_rect_make(0, 0, 0, 0);
    h.ui = sk_ui_new(win);
    h.logo = sk_custom_new(h.ui, r0, draw_logo, NULL, 0, NULL);
    h.title = sk_label_new(h.ui, r0, "你好，SketchOS！", SK_FONT_TITLE, SK_ALIGN_LEFT);
    h.intro = sk_paragraph_new(h.ui, r0,
                               "这是一个最小的图形应用。在下面写下你的名字，"
                               "然后点「打招呼」或者直接按回车。", SK_FONT_UI);
    h.name_label = sk_label_new(h.ui, r0, "名字：", SK_FONT_UI, SK_ALIGN_LEFT);
    h.name = sk_textbox_new(h.ui, r0, "", NULL, NULL);
    sk_textbox_set_placeholder(h.name, "比如：小明");
    h.button = sk_button_new(h.ui, r0, "打招呼", on_greet, &h);
    h.greeting = sk_label_new(h.ui, r0, "", SK_FONT_TITLE, SK_ALIGN_LEFT);
    h.footer = sk_label_new(h.ui, r0, "还没有打过招呼", SK_FONT_UI, SK_ALIGN_RIGHT);
    sk_label_set_color(h.footer, SK_INK_SOFT);

    /* 输入框里按回车 = 点按钮（单行输入框的 on_click 就是“回车”） */
    sk_widget_set_callbacks(h.name, on_greet, NULL, &h);
    /* 一打开窗口就可以直接打字 */
    sk_ui_focus(h.ui, h.name);

    /* 设置布局函数：它会立刻被调用一次，以后每次窗口大小改变都会再调用 */
    sk_ui_set_layout(h.ui, layout, &h);

    /* 第 4 步：进入事件循环。用户关掉窗口后（最后一个窗口没了）它会返回 0，
     * 我们把它当作进程的退出码。 */
    int rc = sk_app_run(h.app);
    sk_ui_free(h.ui);
    sk_app_disconnect(h.app);
    return rc;
}
