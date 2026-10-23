/*
 * skpaint — 画板
 *
 * 一个小小的画图程序：铅笔、刷子、橡皮、直线、矩形、椭圆、油漆桶，十二种颜色，
 * 撤销/重做，打开和保存 PPM 图片。命令行：skpaint [图片路径]
 * （apps.conf 里写的是 /bin/skpaint %f：在文件管理器里双击 .ppm/.pgm/.pbm 就会用它打开。）
 *
 * ┌──────────────────────── 画板是怎么工作的？ ────────────────────────┐
 *
 *  1. 两块像素内存
 *     窗口的内容是一块和显示服务器 sketchd 共享的内存（memfd，见 skhello 的说明），
 *     但“画”本身不能只存在窗口里：窗口会被缩小、画布会被滚动、拖动直线时要显示预览……
 *     所以画存在另一块只属于我们的“离屏画布”（offscreen canvas，pa->img）里，
 *     需要显示时再把可见的那部分一行一行 memcpy 到窗口内存里（sk_blit）。
 *
 *  2. 只重画变了的地方
 *     拖动鼠标画一笔时，每来一个鼠标移动事件，就从上一个点连一条线到这个点，
 *     然后只把这一小段线的包围盒标记为“脏”（sk_ui_invalidate）。本轮事件处理完，
 *     控件库只把这几十个像素见方的区域从离屏画布拷到窗口，再告诉 sketchd 这块变了。
 *     在网页版（v86 模拟器，CPU 慢几十倍）里，这就是“画起来跟手”和“一卡一卡”的区别。
 *
 *  3. 撤销：写时复制（见 undo.c）
 *     每次操作之前，只把即将被改动的 64×64 小块复制一份——和内核 fork() 的写时复制一个道理。
 *
 *  4. 保存：PPM 格式 + 原子替换（见 pnm.c）
 *     先写临时文件再 rename()，任何时候都不会留下半张图。
 *
 *  5. 没事做的时候睡觉
 *     事件循环睡在 poll() 里；没有任何定时器，所以不画画时 CPU 占用为 0。
 *
 * └────────────────────────────────────────────────────────────────────┘
 *
 * 快捷键：Ctrl+N 新建  Ctrl+O 打开  Ctrl+S 保存  Ctrl+Shift+S 另存为
 *         Ctrl+Z 撤销  Ctrl+Y / Ctrl+Shift+Z 重做
 *         P 铅笔 B 刷子 E 橡皮 L 直线 R 矩形 O 椭圆 F 填充   [ ] 调粗细   Esc 取消正在拖的图形
 *         画直线/矩形/椭圆时按住 Shift：水平/竖直/45° 线、正方形、正圆
 *         滚轮上下滚动画布，Shift+滚轮左右滚动
 *
 * 调试接口（自动化测试 tests/apps/test_skpaint.py 用）：
 *   环境变量 SKPAINT_DEBUG=1 时，程序往标准输出打印一行一个的状态：
 *     skpaint: ready {JSON}     启动完成
 *     skpaint: state {JSON}     按 F12 时（测试脚本用它和程序“对时”）
 *     skpaint: saved <路径>     保存成功        skpaint: opened <路径>   打开成功
 *     skpaint: error <原因>     弹出错误对话框之前
 *   JSON 里有：画布原点在窗口里的位置 ox/oy、图片尺寸 w/h、可见区域 vis、当前工具/颜色/粗细、
 *   是否已修改、撤销/重做步数、文件路径，以及各个控件的矩形（测试脚本据此计算点击位置）。
 */
#include <sketch/client.h>
#include <sketch/rough.h>
#include <sketch/theme.h>
#include <sketch/ui.h>

#include "skpaint.h"
#include "skutil.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------- 布局常量 */

#define DEFAULT_W    640     /* 新画布的尺寸 */
#define DEFAULT_H    420
#define TOP_H        52      /* 顶部工具栏（新建、打开、保存、撤销、重做） */
#define BOTTOM_H     52      /* 底部（调色盘、状态栏） */
#define LEFT_W       112     /* 左边一列（画图工具、粗细、手绘风格） */
#define MARGIN       20      /* 画布四周的留白，放手绘的边框和阴影 */
#define BAR          14      /* 滚动条的粗细 */
#define SHADOW       7       /* 画布阴影的偏移（和窗口阴影一样） */
#define PEN_MIN     1
#define PEN_MAX     40
#define FILL_TOL     32      /* 油漆桶的颜色容差（每个分量 0..255） */
#define WHEEL_STEP   48      /* 滚轮滚一格移动多少像素 */
#define NSWATCH      12
#define PAPER_SEED   0x5EEDA7u   /* 纸张颗粒的种子：所有地方用同一个，局部重画才看不出接缝 */

enum { DRAG_NONE = 0, DRAG_STROKE, DRAG_SHAPE, DRAG_VBAR, DRAG_HBAR };

static const struct {
    const char *id;         /* 调试输出里的名字 */
    const char *label;      /* 按钮上的字 */
    int         icon;
    int         key;        /* 快捷键 */
    int         uses_size;  /* 粗细滑块对它有没有用 */
    const char *hint;       /* 选中时状态栏的提示 */
} tool_info[TOOL_COUNT] = {
    [TOOL_PENCIL]  = { "pencil",  "铅笔", SK_ICON_PENCIL, KEY_P, 0, "铅笔：按住左键拖动，画 1 像素的细线" },
    [TOOL_BRUSH]   = { "brush",   "刷子", SK_ICON_BRUSH,  KEY_B, 1, "刷子：圆头笔刷，粗细用左边的滑块调" },
    [TOOL_ERASER]  = { "eraser",  "橡皮", SK_ICON_ERASER, KEY_E, 1, "橡皮：把画擦回白纸" },
    [TOOL_LINE]    = { "line",    "直线", SK_ICON_LINE,   KEY_L, 1, "直线：拖动画线，按住 Shift 画水平/竖直/斜 45° 线" },
    [TOOL_RECT]    = { "rect",    "矩形", SK_ICON_RECT,   KEY_R, 1, "矩形：拖动画矩形，按住 Shift 画正方形" },
    [TOOL_ELLIPSE] = { "ellipse", "椭圆", SK_ICON_CIRCLE, KEY_O, 1, "椭圆：拖动画椭圆，按住 Shift 画正圆" },
    [TOOL_FILL]    = { "fill",    "填充", SK_ICON_FILL,   KEY_F, 0, "填充：点一下，把连成一片的同色区域涂满" },
};

/* 调色盘：系统主题的蜡笔色，再加上纯黑和纯白 */
static const sk_color palette[NSWATCH] = {
    SK_RGB(0, 0, 0), SK_INK_SOFT, SK_SHADOW, PAINT_WHITE,
    SK_RED, SK_ORANGE, SK_YELLOW, SK_GREEN, SK_TEAL, SK_BLUE, SK_PURPLE, SK_PINK,
};

/* ---------------------------------------------------------------- 应用状态 */

typedef struct {
    sk_app    *app;
    sk_window *win;
    sk_ui     *ui;
    int        debug;               /* SKPAINT_DEBUG=1 */

    /* 控件 */
    sk_widget *btn_new, *btn_open, *btn_save, *btn_undo, *btn_redo;
    sk_widget *tool_btn[TOOL_COUNT];
    sk_widget *size_label, *size_slider, *rough_box;
    sk_widget *swatch[NSWATCH];
    sk_widget *status;
    sk_widget *canvas;              /* 工作区（自定义控件）：画布、边框、阴影、滚动条 */
    int        sep_x;               /* 工具栏里“文件”和“编辑”两组按钮之间的分隔线 */

    /* 文档 */
    sk_canvas *img;                 /* 离屏画布：画就存在这里 */
    char      *path;                /* 文件路径；NULL = 还没保存过（“未命名”） */
    undo_hist  hist;
    unsigned   saved_version;       /* 上次保存（或打开）时的图片版本 */
    char       title[SK_TITLE_MAX];

    /* 当前画笔 */
    int        tool;
    sk_color   color;
    int        size;
    int        rough;

    /* 正在进行的鼠标拖动 */
    int         drag;               /* DRAG_* */
    int         last_x, last_y;     /* 自由笔画的上一个点（图片坐标） */
    paint_shape shape;              /* 正在拖的图形（预览） */
    sk_rect     preview_dirty;      /* 预览上一次占用的窗口区域 */
    uint32_t    shape_count;
    int         grab;               /* 拖滚动条时，鼠标相对滑块起点的偏移 */

    /* 视图：图片在窗口里的位置 */
    sk_rect    view;                /* 工作区 */
    sk_rect    port;                /* 显示图片的区域 = 工作区去掉滚动条 */
    sk_rect    vis;                 /* 图片真正露出来的部分 = 图片矩形 ∩ port */
    int        ox, oy;              /* 图片 (0,0) 在窗口里的坐标 */
    int        scroll_x, scroll_y;
    int        hbar, vbar;          /* 是否需要横/竖滚动条 */
    int        hover;               /* 鼠标在工作区上 */
    int        mouse_x, mouse_y;
    int        in_dialog;           /* 对话框打开中（防止重复弹出） */
} paint_app;

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int v) { return v < 0 ? -v : v; }

static int rect_inside(sk_rect outer, sk_rect in)
{
    return in.x >= outer.x && in.y >= outer.y && in.x + in.w <= outer.x + outer.w && in.y + in.h <= outer.y + outer.h;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* 把 UTF-8 字符串截到最多 max 字节，不切断一个汉字（汉字的后续字节都是 10xxxxxx） */
static void utf8_cut(char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max)
        return;
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    s[n] = 0;
}

/* 文件名以 .ppm 结尾（不分大小写） */
static int is_ppm_name(const char *p)
{
    size_t n = strlen(p);
    return n >= 4 && strcasecmp(p + n - 4, ".ppm") == 0;
}

static int modified(const paint_app *pa) { return pa->hist.version != pa->saved_version; }

static const char *doc_name(const paint_app *pa) { return pa->path ? base_name(pa->path) : "未命名"; }

/* ================================================================ 调试输出 */

static void json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            printf("\\%c", ch);
        else if (ch < 0x20)
            printf("\\u%04x", ch);
        else
            putchar(ch);
    }
    putchar('"');
}

static void json_rect(const char *name, sk_rect r)
{
    printf("\"%s\":[%d,%d,%d,%d]", name, r.x, r.y, r.w, r.h);
}

static void debug_state(paint_app *pa, const char *tag)
{
    if (!pa->debug)
        return;
    printf("skpaint: %s {\"ox\":%d,\"oy\":%d,\"w\":%d,\"h\":%d,", tag, pa->ox, pa->oy, pa->img->w, pa->img->h);
    json_rect("vis", pa->vis);
    putchar(',');
    json_rect("port", pa->port);
    putchar(',');
    json_rect("view", pa->view);
    printf(",\"scroll\":[%d,%d],\"bars\":[%d,%d],\"tool\":\"%s\",\"color\":\"#%06X\",\"size\":%d,\"rough\":%d,",
           pa->scroll_x, pa->scroll_y, pa->hbar, pa->vbar, tool_info[pa->tool].id,
           (unsigned)(pa->color & 0xFFFFFFu), pa->size, pa->rough);
    printf("\"modified\":%d,\"undo\":%d,\"redo\":%d,\"drag\":%d,\"path\":", modified(pa), pa->hist.nundo,
           pa->hist.nredo, pa->drag);
    json_str(pa->path ? pa->path : "");
    printf(",\"title\":");
    json_str(pa->title);
    printf(",\"tools\":[");
    for (int i = 0; i < TOOL_COUNT; i++) {
        sk_rect r = pa->tool_btn[i]->r;
        printf("%s[%d,%d,%d,%d]", i ? "," : "", r.x, r.y, r.w, r.h);
    }
    printf("],\"swatches\":[");
    for (int i = 0; i < NSWATCH; i++) {
        sk_rect r = pa->swatch[i]->r;
        printf("%s[%d,%d,%d,%d]", i ? "," : "", r.x, r.y, r.w, r.h);
    }
    printf("],");
    json_rect("new", pa->btn_new->r);
    putchar(',');
    json_rect("open", pa->btn_open->r);
    putchar(',');
    json_rect("save", pa->btn_save->r);
    putchar(',');
    json_rect("undo_btn", pa->btn_undo->r);
    putchar(',');
    json_rect("redo_btn", pa->btn_redo->r);
    putchar(',');
    json_rect("slider", pa->size_slider->r);
    putchar(',');
    json_rect("rough_box", pa->rough_box->r);
    printf("}\n");
    fflush(stdout);
}

static void debug_line(paint_app *pa, const char *tag, const char *text)
{
    if (!pa->debug)
        return;
    printf("skpaint: %s ", tag);
    for (const char *s = text; *s; s++)
        putchar(*s == '\n' ? ' ' : *s);
    putchar('\n');
    fflush(stdout);
}

/* ================================================================ 状态栏、标题、按钮 */

static void status(paint_app *pa, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void status(paint_app *pa, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sk_widget_set_text(pa->status, buf);
}

/* 标题：“*文件名 - 画板”，* 表示改过还没保存。sk_window_set_title 会发报文，没变就不发 */
static void update_title(paint_app *pa)
{
    char name[SK_TITLE_MAX];
    sk_strlcpy(name, doc_name(pa), sizeof name);
    /* 标题最多 63 字节：“*” + 名字 + “ - 画板”(9 字节)；名字太长就截短并加省略号 */
    if (strlen(name) > 53) {
        utf8_cut(name, 50);
        sk_strlcat(name, "…", sizeof name);
    }
    char t[SK_TITLE_MAX + 16];
    snprintf(t, sizeof t, "%s%s - 画板", modified(pa) ? "*" : "", name);
    if (strcmp(t, pa->title) == 0)
        return;
    sk_strlcpy(pa->title, t, sizeof pa->title);
    if (pa->win)
        sk_window_set_title(pa->win, pa->title);
}

/* 文档状态变了（画了一笔、撤销、保存、打开……）：标题和撤销/重做按钮跟着变 */
static void update_doc_ui(paint_app *pa)
{
    update_title(pa);
    sk_widget_set_enabled(pa->btn_undo, pa->hist.nundo > 0);
    sk_widget_set_enabled(pa->btn_redo, pa->hist.nredo > 0);
}

/* ================================================================ 视图：图片在窗口里的位置 */

static int content_w(const paint_app *pa) { return pa->img->w + 2 * MARGIN; }
static int content_h(const paint_app *pa) { return pa->img->h + 2 * MARGIN; }

static sk_rect vbar_rect(const paint_app *pa)
{
    return sk_rect_make(pa->port.x + pa->port.w, pa->view.y, BAR, pa->port.h);
}

static sk_rect hbar_rect(const paint_app *pa)
{
    return sk_rect_make(pa->view.x, pa->port.y + pa->port.h, pa->port.w, BAR);
}

/* 根据工作区大小、图片大小和滚动位置算出 ox/oy、port、vis。
 * 图片比工作区小：居中摆放；比工作区大：出现滚动条，图片跟着滚动位置移动，
 * 露在 port 外面的部分不画（裁剪）。 */
static void compute_view(paint_app *pa)
{
    sk_rect v = pa->canvas->r;
    pa->view = v;
    int cw = content_w(pa), ch = content_h(pa);
    int hb = 0, vb = 0;
    for (int i = 0; i < 2; i++) {           /* 竖滚动条占掉的宽度可能让横向也放不下了，算两遍 */
        vb = ch > v.h - (hb ? BAR : 0);
        hb = cw > v.w - (vb ? BAR : 0);
    }
    pa->hbar = hb;
    pa->vbar = vb;
    int pw = imax(1, v.w - (vb ? BAR : 0)), ph = imax(1, v.h - (hb ? BAR : 0));
    pa->port = sk_rect_make(v.x, v.y, pw, ph);
    if (hb) {
        pa->scroll_x = imax(0, imin(pa->scroll_x, cw - pw));
        pa->ox = v.x + MARGIN - pa->scroll_x;
    } else {
        pa->scroll_x = 0;
        pa->ox = v.x + (pw - pa->img->w) / 2;
    }
    if (vb) {
        pa->scroll_y = imax(0, imin(pa->scroll_y, ch - ph));
        pa->oy = v.y + MARGIN - pa->scroll_y;
    } else {
        pa->scroll_y = 0;
        pa->oy = v.y + (ph - pa->img->h) / 2;
    }
    pa->vis = sk_rect_intersect(sk_rect_make(pa->ox, pa->oy, pa->img->w, pa->img->h), pa->port);
}

static void invalidate_view(paint_app *pa)
{
    sk_ui_invalidate(pa->ui, pa->view);
}

/* 图片的 r 区域（图片坐标）变了：只让它在窗口里露出来的那部分失效 */
static void invalidate_image(paint_app *pa, sk_rect r)
{
    r = sk_rect_intersect(sk_rect_offset(r, pa->ox, pa->oy), pa->vis);
    if (!sk_rect_empty(r))
        sk_ui_invalidate(pa->ui, r);
}

static void set_scroll(paint_app *pa, int sx, int sy)
{
    int ox = pa->ox, oy = pa->oy;
    pa->scroll_x = sx;
    pa->scroll_y = sy;
    compute_view(pa);                    /* 会把滚动位置限制在合法范围内 */
    if (pa->ox != ox || pa->oy != oy) {
        invalidate_view(pa);
        if (pa->drag == DRAG_SHAPE)
            pa->preview_dirty = sk_rect_make(0, 0, 0, 0);   /* 整个工作区都要重画了 */
    }
}

/* 滚动条滑块的位置和长度（和 sk_draw_scrollbar 的画法一致）。
 * track 是滚动条长度，返回滑块相对“轨道起点 + 2”的偏移和长度 */
static void thumb_geom(int track, int total, int visible, int offset, float *pos, float *len)
{
    float L = (float)track - 4.0f;
    float tl = L * (float)visible / (float)total;
    float minl = L < 22.0f ? L : 22.0f;
    if (tl < minl)
        tl = minl;
    int maxoff = total - visible;
    *pos = maxoff > 0 ? (L - tl) * (float)offset / (float)maxoff : 0.0f;
    *len = tl;
}

/* 拖动滚动条：鼠标位置 → 滚动位置 */
static void bar_drag(paint_app *pa, int vertical, int m)
{
    sk_rect r = vertical ? vbar_rect(pa) : hbar_rect(pa);
    int track = vertical ? r.h : r.w, start = vertical ? r.y : r.x;
    int total = vertical ? content_h(pa) : content_w(pa);
    int visible = vertical ? pa->port.h : pa->port.w;
    float pos, len;
    thumb_geom(track, total, visible, 0, &pos, &len);
    float range = (float)track - 4.0f - len;
    int maxoff = total - visible;
    float p = (float)(m - pa->grab - start - 2);
    int off = range > 0.0f ? (int)(p * (float)maxoff / range + 0.5f) : 0;
    if (vertical)
        set_scroll(pa, pa->scroll_x, off);
    else
        set_scroll(pa, off, pa->scroll_y);
}

static void bar_press(paint_app *pa, int vertical, int m)
{
    sk_rect r = vertical ? vbar_rect(pa) : hbar_rect(pa);
    int track = vertical ? r.h : r.w, start = vertical ? r.y : r.x;
    int total = vertical ? content_h(pa) : content_w(pa);
    int visible = vertical ? pa->port.h : pa->port.w;
    float pos, len;
    thumb_geom(track, total, visible, vertical ? pa->scroll_y : pa->scroll_x, &pos, &len);
    float t0 = (float)start + 2.0f + pos;
    /* 点在滑块上：按住拖；点在空白处：让滑块中心跳到鼠标这里 */
    pa->grab = ((float)m >= t0 && (float)m <= t0 + len) ? (int)((float)m - t0) : (int)(len / 2.0f);
    pa->drag = vertical ? DRAG_VBAR : DRAG_HBAR;
    bar_drag(pa, vertical, m);
    sk_ui_invalidate(pa->ui, r);
}

/* ================================================================ 绘制 */

/* 窗口背景：纸 + 几条手绘分隔线。图片完全盖住的区域不用铺纸（画布控件会整块覆盖） */
static void paint_background(sk_ui *ui, sk_canvas *c, sk_rect dirty, void *user)
{
    (void)ui;
    paint_app *pa = user;
    if (rect_inside(pa->vis, dirty))
        return;
    /* 只给图片“周围”铺纸：脏区域减去图片，最多剩上下左右四条（滚动时能省掉一大半像素） */
    sk_rect in = sk_rect_intersect(dirty, pa->vis);
    if (sk_rect_empty(in)) {
        sk_paper_fill(c, dirty, SK_PAPER, 3, PAPER_SEED);
    } else {
        int dx1 = dirty.x + dirty.w, dy1 = dirty.y + dirty.h, ix1 = in.x + in.w, iy1 = in.y + in.h;
        sk_rect band[4] = {
            sk_rect_make(dirty.x, dirty.y, dirty.w, in.y - dirty.y),       /* 上 */
            sk_rect_make(dirty.x, iy1, dirty.w, dy1 - iy1),                /* 下 */
            sk_rect_make(dirty.x, in.y, in.x - dirty.x, in.h),             /* 左 */
            sk_rect_make(ix1, in.y, dx1 - ix1, in.h),                      /* 右 */
        };
        for (int i = 0; i < 4; i++)
            if (!sk_rect_empty(band[i]))
                sk_paper_fill(c, band[i], SK_PAPER, 3, PAPER_SEED);
    }
    int W = c->w, H = c->h;
    /* 分隔线只在脏区域碰到它们时才画（手绘线也要算力） */
    if (!sk_rect_empty(sk_rect_intersect(dirty, sk_rect_make(0, TOP_H - 6, W, 12))))
        sk_draw_separator(c, 6, W - 6, TOP_H - 1, 0x51A7u);
    if (!sk_rect_empty(sk_rect_intersect(dirty, sk_rect_make(0, H - BOTTOM_H - 6, W, 12))))
        sk_draw_separator(c, 6, W - 6, H - BOTTOM_H, 0x51A8u);
    sk_rough_style s;
    sk_rough_default(&s);
    s.stroke = SK_INK_FAINT;
    s.stroke_width = 1.2f;
    s.roughness = 0.5f;
    s.double_stroke = 0;
    if (!sk_rect_empty(sk_rect_intersect(dirty, sk_rect_make(LEFT_W - 6, TOP_H, 12, H - TOP_H - BOTTOM_H)))) {
        s.seed = 0x51A9u;
        sk_rough_line(c, LEFT_W - 0.5f, TOP_H + 6.0f, LEFT_W - 0.5f, (float)(H - BOTTOM_H - 6), &s);
    }
    if (!sk_rect_empty(sk_rect_intersect(dirty, sk_rect_make(pa->sep_x - 4, 0, 8, TOP_H)))) {
        s.seed = 0x51AAu;
        sk_rough_line(c, pa->sep_x + 0.5f, 12.0f, pa->sep_x + 0.5f, (float)(TOP_H - 12), &s);
    }
}

/* 画布的“纸张”：一圈铅笔边框 + 右下方的排线阴影（和窗口阴影同样的画法） */
static void draw_sheet(paint_app *pa, sk_canvas *c)
{
    float x = (float)pa->ox, y = (float)pa->oy, w = (float)pa->img->w, h = (float)pa->img->h;
    const float S = SHADOW;
    sk_pointf L[6] = {
        { x + w + 2.0f, y + S }, { x + w + S, y + S }, { x + w + S, y + h + S },
        { x + S, y + h + S }, { x + S, y + h + 2.0f }, { x + w + 2.0f, y + h + 2.0f },
    };
    sk_rough_style sh;
    sk_rough_default(&sh);
    sh.seed = 0x5AD0u;
    sh.roughness = 0.5f;
    sh.stroke_width = 0;
    sh.fill_style = SK_FILL_HACHURE;
    sh.fill = SK_SHADOW;
    sh.hachure_gap = 3.6f;
    sh.fill_weight = 1.1f;
    sh.hachure_angle = -50.0f;
    sk_rough_fill_polygon(c, L, 6, &sh);

    sk_rough_style o;
    sk_rough_default(&o);
    o.seed = 0x5EE7u;
    o.stroke = SK_INK_SOFT;
    o.stroke_width = 1.6f;
    o.roughness = 0.7f;
    sk_rough_rect(c, x - 1.5f, y - 1.5f, w + 2.0f, h + 2.0f, &o);
}

/* 工作区控件的绘制。c->clip 已经是“脏区域 ∩ 控件”，只需要把它填满 */
static void canvas_draw(sk_widget *w, sk_canvas *c, void *user)
{
    (void)w;
    paint_app *pa = user;
    sk_rect old = sk_push_clip(c, pa->view);
    if (!rect_inside(pa->vis, c->clip)) {
        /* 脏区域碰到了图片外面：画纸张边框和阴影（背景的纸已经铺好了）、滚动条 */
        sk_rect o2 = sk_push_clip(c, pa->port);
        draw_sheet(pa, c);
        sk_pop_clip(c, o2);
        if (pa->vbar)
            sk_draw_scrollbar(c, vbar_rect(pa), content_h(pa), pa->port.h, pa->scroll_y,
                              pa->drag == DRAG_VBAR ? SK_STATE_PRESSED : 0, 0x5C01u);
        if (pa->hbar)
            sk_draw_scrollbar(c, hbar_rect(pa), content_w(pa), pa->port.w, pa->scroll_x,
                              pa->drag == DRAG_HBAR ? SK_STATE_PRESSED : 0, 0x5C02u);
    }
    /* 图片：只把脏区域里露出来的那部分从离屏画布拷过来（sk_blit 内部是一行一次 memmove） */
    sk_rect o3 = sk_push_clip(c, pa->vis);
    sk_rect cl = c->clip;
    if (!sk_rect_empty(cl)) {
        sk_blit(c, cl.x, cl.y, pa->img, sk_rect_offset(cl, -pa->ox, -pa->oy));
        /* 拖动中的图形只画在窗口上（预览），松开鼠标时才画进图片。
         * 预览和定稿裁剪到同一个范围（shape_bounds），保证两者一个像素都不差 */
        if (pa->drag == DRAG_SHAPE) {
            sk_rect o4 = sk_push_clip(c, sk_rect_offset(shape_bounds(&pa->shape), pa->ox, pa->oy));
            shape_draw(c, &pa->shape, pa->ox, pa->oy);
            sk_pop_clip(c, o4);
        }
    }
    sk_pop_clip(c, o3);
    sk_pop_clip(c, old);
}

/* ================================================================ 画图操作 */

/* 一次操作（一笔、一个图形、一次填充）结束：交给撤销记录，刷新标题和按钮 */
static void finish_op(paint_app *pa)
{
    switch (undo_end(&pa->hist)) {
    case UNDO_TOO_BIG:
        status(pa, "这一步改动太大，超过了撤销记录的内存上限，没法撤销");
        break;
    case UNDO_NO_MEMORY:
        status(pa, "内存不足：撤销记录已经清空");
        break;
    default:
        break;
    }
    update_doc_ui(pa);
}

/* 自由笔画的一段：从 (x0,y0) 连到 (x1,y1) */
static void stroke_segment(paint_app *pa, int x0, int y0, int x1, int y1)
{
    sk_canvas *img = pa->img;
    int pencil = pa->tool == TOOL_PENCIL;
    int r = pencil ? 1 : pa->size / 2 + 2;
    sk_rect box = sk_rect_make(imin(x0, x1) - r, imin(y0, y1) - r, iabs(x1 - x0) + 2 * r + 1, iabs(y1 - y0) + 2 * r + 1);
    box = sk_rect_intersect(box, sk_rect_make(0, 0, img->w, img->h));
    if (sk_rect_empty(box))
        return;
    /* 先让撤销记录保存这块“改之前”的样子，再把画笔限制在这块里画 */
    undo_touch(&pa->hist, box);
    sk_rect old = sk_push_clip(img, box);
    if (pencil)
        draw_pencil_line(img, x0, y0, x1, y1, pa->color);
    else
        draw_brush_line(img, (float)x0 + 0.5f, (float)y0 + 0.5f, (float)x1 + 0.5f, (float)y1 + 0.5f,
                        (float)pa->size, pa->tool == TOOL_ERASER ? PAINT_WHITE : pa->color);
    sk_pop_clip(img, old);
    invalidate_image(pa, box);           /* 只有这一小段需要重画 */
}

static void stroke_begin(paint_app *pa, int ix, int iy)
{
    undo_begin(&pa->hist);
    pa->drag = DRAG_STROKE;
    pa->last_x = ix;
    pa->last_y = iy;
    stroke_segment(pa, ix, iy, ix, iy);  /* 只点一下也留下一个点 */
}

static void stroke_move(paint_app *pa, int ix, int iy)
{
    if (ix == pa->last_x && iy == pa->last_y)
        return;
    stroke_segment(pa, pa->last_x, pa->last_y, ix, iy);
    pa->last_x = ix;
    pa->last_y = iy;
}

static void stroke_end(paint_app *pa)
{
    pa->drag = DRAG_NONE;
    finish_op(pa);
}

/* 预览图形占用的窗口区域 */
static sk_rect shape_screen_rect(const paint_app *pa)
{
    return sk_rect_intersect(sk_rect_offset(shape_bounds(&pa->shape), pa->ox, pa->oy), pa->vis);
}

/* 预览变了：旧位置和新位置都要重画（旧的擦掉，新的画上） */
static void preview_changed(paint_app *pa)
{
    if (!sk_rect_empty(pa->preview_dirty))
        sk_ui_invalidate(pa->ui, pa->preview_dirty);
    pa->preview_dirty = pa->drag == DRAG_SHAPE ? shape_screen_rect(pa) : sk_rect_make(0, 0, 0, 0);
    if (!sk_rect_empty(pa->preview_dirty))
        sk_ui_invalidate(pa->ui, pa->preview_dirty);
}

static void shape_begin(paint_app *pa, int ix, int iy)
{
    paint_shape *s = &pa->shape;
    s->tool = pa->tool;
    s->x0 = s->x1 = ix;
    s->y0 = s->y1 = iy;
    s->size = pa->size;
    s->color = pa->color;
    s->rough = pa->rough;
    s->seed = sk_hash_u32(0x5AE7u, ++pa->shape_count);   /* 每个图形抖得不一样，但拖动中保持不变 */
    pa->drag = DRAG_SHAPE;
    pa->preview_dirty = sk_rect_make(0, 0, 0, 0);
    preview_changed(pa);
}

static void shape_move(paint_app *pa, int ix, int iy, int constrain)
{
    paint_shape *s = &pa->shape;
    int dx = ix - s->x0, dy = iy - s->y0;
    if (constrain) {
        if (s->tool == TOOL_LINE) {
            /* 吸附到水平、竖直或 45° */
            if (iabs(dx) > 2 * iabs(dy))
                dy = 0;
            else if (iabs(dy) > 2 * iabs(dx))
                dx = 0;
            else {
                int m = imax(iabs(dx), iabs(dy));
                dx = dx < 0 ? -m : m;
                dy = dy < 0 ? -m : m;
            }
        } else {
            int m = imax(iabs(dx), iabs(dy));    /* 正方形 / 正圆 */
            dx = dx < 0 ? -m : m;
            dy = dy < 0 ? -m : m;
        }
    }
    if (s->x1 == s->x0 + dx && s->y1 == s->y0 + dy)
        return;
    s->x1 = s->x0 + dx;
    s->y1 = s->y0 + dy;
    preview_changed(pa);
}

static void shape_cancel(paint_app *pa)
{
    pa->drag = DRAG_NONE;
    preview_changed(pa);                 /* 擦掉预览 */
}

static void shape_commit(paint_app *pa)
{
    pa->drag = DRAG_NONE;
    preview_changed(pa);
    paint_shape *s = &pa->shape;
    if (s->x0 == s->x1 && s->y0 == s->y1)
        return;                          /* 只是点了一下，没有拖 */
    sk_rect box = sk_rect_intersect(shape_bounds(s), sk_rect_make(0, 0, pa->img->w, pa->img->h));
    if (sk_rect_empty(box))
        return;
    undo_begin(&pa->hist);
    undo_touch(&pa->hist, box);
    sk_rect old = sk_push_clip(pa->img, box);
    shape_draw(pa->img, s, 0, 0);        /* 和预览用的是同一个函数、同一个种子：所见即所得 */
    sk_pop_clip(pa->img, old);
    finish_op(pa);
    invalidate_image(pa, box);
}

static void do_fill(paint_app *pa, int ix, int iy)
{
    uint8_t *mask = NULL;
    sk_rect box;
    int r = fill_compute(pa->img, ix, iy, pa->color, FILL_TOL, &mask, &box);
    if (r < 0) {
        status(pa, "内存不足，填不了这么大的区域");
        return;
    }
    if (r == 0)
        return;
    undo_begin(&pa->hist);
    undo_touch(&pa->hist, box);
    fill_apply(pa->img, mask, box, pa->color);
    free(mask);
    finish_op(pa);
    invalidate_image(pa, box);
}

/* 结束正在进行的拖动（松开鼠标、窗口失去焦点、按了快捷键……）。commit=0 时放弃图形预览 */
static void end_drag(paint_app *pa, int commit)
{
    switch (pa->drag) {
    case DRAG_STROKE:
        stroke_end(pa);
        break;
    case DRAG_SHAPE:
        if (commit)
            shape_commit(pa);
        else
            shape_cancel(pa);
        break;
    case DRAG_VBAR:
    case DRAG_HBAR:
        pa->drag = DRAG_NONE;            /* 只有滚动条的样子变了（按下 → 松开） */
        sk_ui_invalidate(pa->ui, vbar_rect(pa));
        sk_ui_invalidate(pa->ui, hbar_rect(pa));
        break;
    default:
        break;
    }
}

/* ================================================================ 鼠标光标 */

/*
 * 鼠标在画布上时显示铅笔（画图形时显示十字），在滚动条上显示箭头。
 * 控件库每次鼠标按下/松开都会按“控件的光标”（自定义控件默认是箭头）重设一次光标，
 * 所以除了在事件里设置，还在每轮事件循环末尾（空闲回调）再确认一次。
 * sk_window_set_cursor 自己会判断“和上次一样就不发报文”，反复调用没有开销。
 */
static int wanted_cursor(const paint_app *pa)
{
    int tool_cursor = (pa->tool == TOOL_LINE || pa->tool == TOOL_RECT || pa->tool == TOOL_ELLIPSE)
                      ? SK_CURSOR_CROSS : SK_CURSOR_PENCIL;
    if (pa->drag == DRAG_STROKE || pa->drag == DRAG_SHAPE)
        return tool_cursor;
    if (pa->drag == DRAG_VBAR || pa->drag == DRAG_HBAR)
        return SK_CURSOR_ARROW;
    if (pa->hover)
        return sk_rect_contains(pa->port, pa->mouse_x, pa->mouse_y) ? tool_cursor : SK_CURSOR_ARROW;
    return -1;                           /* 不在画布上：交给控件库 */
}

static void apply_cursor(paint_app *pa)
{
    int c = wanted_cursor(pa);
    if (c >= 0 && pa->win)
        sk_window_set_cursor(pa->win, c);
}

static void cursor_idle(sk_app *app, void *user)
{
    (void)app;
    apply_cursor(user);
}

/* ================================================================ 工作区的鼠标事件 */

static int canvas_event(sk_widget *w, const sk_event *e, void *user)
{
    (void)w;
    paint_app *pa = user;
    if (e->type != SK_EVENT_MOUSE)
        return 0;
    int mx = e->mouse.x, my = e->mouse.y;
    pa->mouse_x = mx;
    pa->mouse_y = my;
    int ix = mx - pa->ox, iy = my - pa->oy;          /* 窗口坐标 → 图片坐标 */

    switch (e->mouse.kind) {
    case SK_MOUSE_ENTER:
        pa->hover = 1;
        break;
    case SK_MOUSE_LEAVE:
        pa->hover = 0;
        break;
    case SK_MOUSE_WHEEL: {
        int step = e->mouse.wheel * WHEEL_STEP;
        if ((e->mouse.modifiers & SK_MOD_SHIFT) || (!pa->vbar && pa->hbar))
            set_scroll(pa, pa->scroll_x - step, pa->scroll_y);
        else
            set_scroll(pa, pa->scroll_x, pa->scroll_y - step);
        break;
    }
    case SK_MOUSE_DOWN:
        if (e->mouse.button == SK_BUTTON_RIGHT && pa->drag == DRAG_SHAPE) {
            shape_cancel(pa);                        /* 拖图形时按右键 = 不画了 */
            break;
        }
        if (e->mouse.button != SK_BUTTON_LEFT || pa->drag != DRAG_NONE)
            break;
        if (pa->vbar && sk_rect_contains(vbar_rect(pa), mx, my)) {
            bar_press(pa, 1, my);
            break;
        }
        if (pa->hbar && sk_rect_contains(hbar_rect(pa), mx, my)) {
            bar_press(pa, 0, mx);
            break;
        }
        if (!sk_rect_contains(pa->port, mx, my))
            break;
        /* 在图片外面的留白处按下也可以开始画（画出界的部分被裁掉），这样从边上起笔很自然 */
        switch (pa->tool) {
        case TOOL_PENCIL:
        case TOOL_BRUSH:
        case TOOL_ERASER:
            stroke_begin(pa, ix, iy);
            break;
        case TOOL_LINE:
        case TOOL_RECT:
        case TOOL_ELLIPSE:
            shape_begin(pa, ix, iy);
            break;
        case TOOL_FILL:
            do_fill(pa, ix, iy);
            break;
        default:
            break;
        }
        break;
    case SK_MOUSE_MOVE:
        if (pa->drag != DRAG_NONE && !(e->mouse.buttons & SK_BUTTON_LEFT)) {
            end_drag(pa, 1);                         /* 松开事件丢了（例如窗口失去焦点）：就当松开了 */
            break;
        }
        if (pa->drag == DRAG_STROKE)
            stroke_move(pa, ix, iy);
        else if (pa->drag == DRAG_SHAPE)
            shape_move(pa, ix, iy, (e->mouse.modifiers & SK_MOD_SHIFT) != 0);
        else if (pa->drag == DRAG_VBAR)
            bar_drag(pa, 1, my);
        else if (pa->drag == DRAG_HBAR)
            bar_drag(pa, 0, mx);
        break;
    case SK_MOUSE_UP:
        if (e->mouse.button == SK_BUTTON_LEFT) {
            /* 松开前的最后一个位置也要画上（快速拖动时，它和上一个移动事件可能隔得很远） */
            if (pa->drag == DRAG_STROKE)
                stroke_move(pa, ix, iy);
            else if (pa->drag == DRAG_SHAPE)
                shape_move(pa, ix, iy, (e->mouse.modifiers & SK_MOD_SHIFT) != 0);
            end_drag(pa, 1);
        }
        break;
    default:
        break;
    }
    apply_cursor(pa);
    return 1;
}

/* ================================================================ 工具、颜色、粗细 */

static void set_tool(paint_app *pa, int t)
{
    if (t < 0 || t >= TOOL_COUNT)
        return;
    end_drag(pa, 1);
    pa->tool = t;
    for (int i = 0; i < TOOL_COUNT; i++)
        sk_widget_set_checked(pa->tool_btn[i], i == t);
    /* 铅笔永远 1 像素、油漆桶没有粗细：滑块变灰，提示它此刻不起作用 */
    sk_widget_set_enabled(pa->size_slider, tool_info[t].uses_size);
    sk_widget_set_enabled(pa->size_label, tool_info[t].uses_size);
    sk_widget_set_enabled(pa->rough_box, t == TOOL_LINE || t == TOOL_RECT || t == TOOL_ELLIPSE);
    status(pa, "%s", tool_info[t].hint);
    apply_cursor(pa);
}

static void on_tool(sk_widget *w, void *user)
{
    set_tool(user, w->id);
}

static void set_color(paint_app *pa, int i)
{
    pa->color = palette[i];
    for (int k = 0; k < NSWATCH; k++)
        sk_widget_set_checked(pa->swatch[k], k == i);
}

static void on_swatch(sk_widget *w, void *user)
{
    set_color(user, w->id);
}

static void set_size(paint_app *pa, int v)
{
    v = imax(PEN_MIN, imin(PEN_MAX, v));
    pa->size = v;
    if (sk_slider_value(pa->size_slider) != v)
        sk_slider_set_value(pa->size_slider, v);
    char buf[32];
    snprintf(buf, sizeof buf, "粗细：%d", v);
    sk_widget_set_text(pa->size_label, buf);
}

static void on_size(sk_widget *w, void *user)
{
    set_size(user, sk_slider_value(w));
}

static void on_rough(sk_widget *w, void *user)
{
    paint_app *pa = user;
    pa->rough = sk_widget_checked(w);
}

/* ================================================================ 撤销 / 重做 */

static void do_undo(paint_app *pa)
{
    end_drag(pa, 1);
    sk_rect r;
    if (!undo_undo(&pa->hist, &r)) {
        status(pa, "没有可以撤销的了");
        return;
    }
    invalidate_image(pa, r);
    update_doc_ui(pa);
    status(pa, "撤销（还可以撤销 %d 步）", pa->hist.nundo);
}

static void do_redo(paint_app *pa)
{
    end_drag(pa, 1);
    sk_rect r;
    if (!undo_redo(&pa->hist, &r)) {
        status(pa, "没有可以重做的了");
        return;
    }
    invalidate_image(pa, r);
    update_doc_ui(pa);
    status(pa, "重做（还可以重做 %d 步）", pa->hist.nredo);
}

static void on_undo(sk_widget *w, void *user)
{
    (void)w;
    do_undo(user);
}

static void on_redo(sk_widget *w, void *user)
{
    (void)w;
    do_redo(user);
}

/* ================================================================ 文件：新建、打开、保存 */

static void error_box(paint_app *pa, const char *title, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void error_box(paint_app *pa, const char *title, const char *fmt, ...)
{
    char msg[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    debug_line(pa, "error", msg);
    pa->in_dialog++;
    sk_dialog_message(pa->app, title, msg, "确定");
    pa->in_dialog--;
}

static sk_canvas *blank_image(int w, int h)
{
    sk_canvas *c = sk_canvas_new(w, h);
    if (c)
        sk_clear(c, PAINT_WHITE);
    return c;
}

/* 相对路径 → 绝对路径（标题、保存位置都用绝对路径，不依赖进程的当前目录） */
static char *absolute_path(const char *p)
{
    char buf[4096];
    if (p[0] == '/') {
        sk_strlcpy(buf, p, sizeof buf);
    } else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof cwd))
            return strdup(p);
        sk_path_join(buf, sizeof buf, cwd, p);
    }
    sk_path_normalize(buf);
    return strdup(buf);
}

/* 默认的存图位置：~/图片（存在的话），否则 ~ */
static void default_dir(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/root";
    sk_path_join(buf, n, home, "图片");
    struct stat st;
    if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode))
        sk_strlcpy(buf, home, n);
}

/* 对话框的起始目录：当前文件所在的目录，没有就用默认位置 */
static void start_dir(const paint_app *pa, char *buf, size_t n)
{
    if (pa->path) {
        sk_strlcpy(buf, pa->path, n);
        char *slash = strrchr(buf, '/');
        if (slash) {
            *(slash == buf ? slash + 1 : slash) = 0;
            struct stat st;
            if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
                return;
        }
    }
    default_dir(buf, n);
}

/* 换上一张新图片（新建、打开）。img 的所有权交给 pa；path 会被复制 */
static void set_document(paint_app *pa, sk_canvas *img, const char *path)
{
    end_drag(pa, 0);
    sk_canvas *old = pa->img;
    pa->img = img;
    if (undo_reset(&pa->hist, img) != 0)
        status(pa, "内存不足：这张图不能撤销");
    pa->saved_version = pa->hist.version;
    sk_canvas_free(old);
    char *np = path ? strdup(path) : NULL;
    free(pa->path);
    pa->path = np;
    pa->scroll_x = pa->scroll_y = 0;
    compute_view(pa);
    invalidate_view(pa);
    update_doc_ui(pa);
}

/* 有没保存的改动时问一句。返回 1 = 可以继续（已保存或不要了），0 = 用户取消 */
static int do_save(paint_app *pa, int save_as);
static int confirm_discard(paint_app *pa)
{
    if (!modified(pa))
        return 1;
    char q[600];
    snprintf(q, sizeof q, "“%s”还没有保存，要保存吗？", doc_name(pa));
    pa->in_dialog++;
    int r = sk_dialog_message(pa->app, "画板", q, "保存|不保存|取消");
    pa->in_dialog--;
    if (r == 0)
        return do_save(pa, 0) == 0;
    return r == 1;
}

/* 保存。save_as=1 或者还没有 .ppm 路径时先弹出“保存”对话框。返回 0 成功，-1 失败或取消 */
static int do_save(paint_app *pa, int save_as)
{
    end_drag(pa, 1);
    char *target = NULL;
    if (!save_as && pa->path && is_ppm_name(pa->path)) {
        target = strdup(pa->path);
    } else {
        char dir[2048], name[256];
        start_dir(pa, dir, sizeof dir);
        if (pa->path) {
            /* 打开的是 .pgm/.pbm：保存时换成 .ppm（我们写出来的是彩色 P6），不覆盖原文件 */
            sk_strlcpy(name, base_name(pa->path), sizeof name);
            char *dot = strrchr(name, '.');
            if (dot && dot != name)
                *dot = 0;
            sk_strlcat(name, ".ppm", sizeof name);
        } else {
            sk_strlcpy(name, "画.ppm", sizeof name);
        }
        pa->in_dialog++;
        target = sk_dialog_file(pa->app, "保存图片", dir, 1, name);
        pa->in_dialog--;
        if (!target)
            return -1;                   /* 取消了 */
        /* 文件名没写扩展名：自动加上 .ppm（加完如果撞上已有的文件，再问一次） */
        if (!strchr(base_name(target), '.')) {
            char *with_ext = sk_strdup_printf("%s.ppm", target);
            free(target);
            target = with_ext;
            struct stat st;
            if (target && stat(target, &st) == 0) {
                char q[600];
                snprintf(q, sizeof q, "“%s”已经存在。\n要替换它吗？", base_name(target));
                pa->in_dialog++;
                int r = sk_dialog_message(pa->app, "确认替换", q, "替换|取消");
                pa->in_dialog--;
                if (r != 0) {
                    free(target);
                    return -1;
                }
            }
        }
    }
    if (!target) {
        status(pa, "内存不足");
        return -1;
    }
    char err[512];
    if (pnm_save(pa->img, target, err, sizeof err) != 0) {
        error_box(pa, "保存失败", "无法保存“%s”：\n%s", base_name(target), err);
        free(target);
        return -1;
    }
    free(pa->path);
    pa->path = target;
    pa->saved_version = pa->hist.version;
    update_doc_ui(pa);
    status(pa, "已保存：%s", base_name(target));
    debug_line(pa, "saved", target);
    return 0;
}

/* 读图片并换上。失败时弹出错误对话框，原来的画保持不变 */
static int open_path(paint_app *pa, const char *path)
{
    char err[512];
    sk_canvas *img = pnm_load(path, err, sizeof err);
    if (!img) {
        error_box(pa, "打开失败", "无法打开“%s”：\n%s", base_name(path), err);
        return -1;
    }
    char *abs = absolute_path(path);
    set_document(pa, img, abs ? abs : path);
    free(abs);
    status(pa, "已打开：%s（%d × %d）", doc_name(pa), img->w, img->h);
    debug_line(pa, "opened", pa->path);
    return 0;
}

static void do_open(paint_app *pa)
{
    end_drag(pa, 1);
    if (!confirm_discard(pa))
        return;
    char dir[2048];
    start_dir(pa, dir, sizeof dir);
    pa->in_dialog++;
    char *path = sk_dialog_file(pa->app, "打开图片", dir, 0, NULL);
    pa->in_dialog--;
    if (!path)
        return;
    open_path(pa, path);
    free(path);
}

static void do_new(paint_app *pa)
{
    end_drag(pa, 1);
    if (!confirm_discard(pa))
        return;
    sk_canvas *img = blank_image(DEFAULT_W, DEFAULT_H);
    if (!img) {
        error_box(pa, "新建失败", "内存不足，无法新建画布。");
        return;
    }
    set_document(pa, img, NULL);
    status(pa, "新画布：%d × %d 像素", DEFAULT_W, DEFAULT_H);
}

static void on_new(sk_widget *w, void *user)
{
    (void)w;
    do_new(user);
}

static void on_open(sk_widget *w, void *user)
{
    (void)w;
    do_open(user);
}

static void on_save(sk_widget *w, void *user)
{
    (void)w;
    do_save(user, 0);
}

/* ================================================================ 键盘、关闭、焦点 */

static int on_key(paint_app *pa, const sk_event *e)
{
    uint32_t kc = e->key.keycode, mods = e->key.modifiers;
    if (mods & SK_MOD_CTRL) {
        int shift = (mods & SK_MOD_SHIFT) != 0;
        switch (kc) {
        case KEY_Z: shift ? do_redo(pa) : do_undo(pa); return 1;
        case KEY_Y: do_redo(pa); return 1;
        case KEY_S: do_save(pa, shift); return 1;
        case KEY_O: do_open(pa); return 1;
        case KEY_N: do_new(pa); return 1;
        default: return 0;
        }
    }
    if (mods & (SK_MOD_ALT | SK_MOD_SUPER))
        return 0;
    switch (kc) {
    case KEY_ESC:
        if (pa->drag == DRAG_SHAPE)
            shape_cancel(pa);
        return 1;
    case KEY_LEFTBRACE:
        set_size(pa, pa->size - 1);
        return 1;
    case KEY_RIGHTBRACE:
        set_size(pa, pa->size + 1);
        return 1;
    case KEY_F12:
        if (!pa->debug)
            return 0;
        debug_state(pa, "state");
        return 1;
    default:
        break;
    }
    for (int t = 0; t < TOOL_COUNT; t++) {
        if ((int)kc == tool_info[t].key) {
            set_tool(pa, t);
            return 1;
        }
    }
    return 0;
}

/* 控件没处理的事件（键盘、关闭、焦点）都到这里 */
static int app_event(sk_window *w, const sk_event *e, void *user)
{
    (void)w;
    paint_app *pa = user;
    switch (e->type) {
    case SK_EVENT_CLOSE:
        if (pa->in_dialog)
            return 1;                    /* 正在问别的事情，先不关 */
        end_drag(pa, 1);
        if (!confirm_discard(pa))
            return 1;                    /* 用户取消了：窗口留着 */
        /* 自己销毁窗口，最后一个窗口没了 sk_app_run 就返回。
         * （没有用“返回 0 让库执行默认动作”：libsketch 的默认关闭路径在释放窗口之后
         *  还会读一次窗口结构体。在回调里销毁则会被库推迟到回调返回后再释放，是安全的。） */
        sk_window *win = pa->win;
        pa->win = NULL;
        sk_window_destroy(win);
        return 1;
    case SK_EVENT_FOCUS:
        if (!e->focused && pa->drag != DRAG_NONE)
            end_drag(pa, pa->drag == DRAG_STROKE);   /* 切到别的窗口：笔画收尾，图形预览放弃 */
        return 0;
    case SK_EVENT_KEY:
        if (e->key.state == 0)
            return 0;
        return on_key(pa, e);
    default:
        return 0;
    }
}

/* ================================================================ 布局 */

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    paint_app *pa = user;
    /* 顶部：文件一组、编辑一组 */
    int x = 10, y = 8, bh = TOP_H - 16, bw = 80;
    sk_widget_set_rect(pa->btn_new, sk_rect_make(x, y, bw, bh));
    x += bw + 4;
    sk_widget_set_rect(pa->btn_open, sk_rect_make(x, y, bw, bh));
    x += bw + 4;
    sk_widget_set_rect(pa->btn_save, sk_rect_make(x, y, bw, bh));
    x += bw + 10;
    pa->sep_x = x;
    x += 10;
    sk_widget_set_rect(pa->btn_undo, sk_rect_make(x, y, bw, bh));
    x += bw + 4;
    sk_widget_set_rect(pa->btn_redo, sk_rect_make(x, y, bw, bh));

    /* 左边一列：工具、粗细、手绘风格 */
    y = TOP_H + 8;
    for (int i = 0; i < TOOL_COUNT; i++) {
        sk_widget_set_rect(pa->tool_btn[i], sk_rect_make(8, y, LEFT_W - 18, 36));
        y += 40;
    }
    y += 6;
    sk_widget_set_rect(pa->size_label, sk_rect_make(14, y, LEFT_W - 24, 24));
    y += 24;
    sk_widget_set_rect(pa->size_slider, sk_rect_make(8, y, LEFT_W - 18, 28));
    y += 34;
    sk_widget_set_rect(pa->rough_box, sk_rect_make(8, y, LEFT_W - 12, 28));

    /* 底部：调色盘、状态栏 */
    int sy = H - BOTTOM_H + (BOTTOM_H - 34) / 2 + 1;
    x = 12;
    for (int i = 0; i < NSWATCH; i++) {
        sk_widget_set_rect(pa->swatch[i], sk_rect_make(x, sy, 34, 34));
        x += 36;
    }
    sk_widget_set_rect(pa->status, sk_rect_make(x + 16, sy, imax(10, W - x - 16 - 14), 34));

    /* 剩下的都是工作区 */
    sk_widget_set_rect(pa->canvas, sk_rect_make(LEFT_W, TOP_H, imax(1, W - LEFT_W), imax(1, H - TOP_H - BOTTOM_H)));
    compute_view(pa);
}

/* ================================================================ main */

static int build_ui(paint_app *pa)
{
    sk_rect r0 = sk_rect_make(0, 0, 0, 0);
    sk_ui *ui = pa->ui;
    /* 工作区放在最下层：它不和别的控件重叠，先创建只是为了让工具栏的“出血”线条画在它上面 */
    pa->canvas = sk_custom_new(ui, r0, canvas_draw, canvas_event, 0, pa);
    pa->btn_new = sk_icon_button_new(ui, r0, SK_ICON_FILE, "新建", on_new, pa);
    pa->btn_open = sk_icon_button_new(ui, r0, SK_ICON_OPEN, "打开", on_open, pa);
    pa->btn_save = sk_icon_button_new(ui, r0, SK_ICON_SAVE, "保存", on_save, pa);
    pa->btn_undo = sk_icon_button_new(ui, r0, SK_ICON_BACK, "撤销", on_undo, pa);
    pa->btn_redo = sk_icon_button_new(ui, r0, SK_ICON_REFRESH, "重做", on_redo, pa);
    if (!pa->canvas || !pa->btn_new || !pa->btn_open || !pa->btn_save || !pa->btn_undo || !pa->btn_redo)
        return -1;
    for (int i = 0; i < TOOL_COUNT; i++) {
        pa->tool_btn[i] = sk_icon_button_new(ui, r0, tool_info[i].icon, tool_info[i].label, on_tool, pa);
        if (!pa->tool_btn[i])
            return -1;
        pa->tool_btn[i]->id = i;
        sk_widget_set_accent(pa->tool_btn[i], SK_RED);   /* 选中的工具用画板的主题色排线 */
    }
    pa->size_label = sk_label_new(ui, r0, "", SK_FONT_UI, SK_ALIGN_LEFT);
    pa->size_slider = sk_slider_new(ui, r0, PEN_MIN, PEN_MAX, pa->size, on_size, pa);
    pa->rough_box = sk_checkbox_new(ui, r0, "手绘风格", pa->rough, on_rough, pa);
    if (!pa->size_label || !pa->size_slider || !pa->rough_box)
        return -1;
    sk_widget_set_accent(pa->size_slider, SK_RED);
    for (int i = 0; i < NSWATCH; i++) {
        pa->swatch[i] = sk_swatch_new(ui, r0, palette[i], on_swatch, pa);
        if (!pa->swatch[i])
            return -1;
        pa->swatch[i]->id = i;
    }
    pa->status = sk_label_new(ui, r0, "", SK_FONT_UI, SK_ALIGN_RIGHT);
    if (!pa->status)
        return -1;
    sk_label_set_color(pa->status, SK_INK_SOFT);
    return 0;
}

int main(int argc, char **argv)
{
    paint_app pa;
    memset(&pa, 0, sizeof pa);
    sk_log_init("skpaint");
    const char *dbg = getenv("SKPAINT_DEBUG");
    pa.debug = dbg && *dbg && strcmp(dbg, "0") != 0;
    pa.tool = TOOL_PENCIL;
    pa.color = palette[0];
    pa.size = 4;
    pa.rough = 1;

    /* 先读命令行给的图片（窗口大小要跟着图片走）；失败的原因等窗口出来以后再用对话框告诉用户 */
    char load_err[600] = "";
    const char *arg = argc > 1 && argv[1][0] ? argv[1] : NULL;
    char *arg_path = NULL;
    sk_canvas *img = NULL;
    int new_file = 0;
    if (arg) {
        arg_path = absolute_path(arg);
        struct stat st;
        if (arg_path && stat(arg_path, &st) != 0 && errno == ENOENT) {
            /* 文件还不存在：开一张新画布，保存时就写到这个路径（和记事本打开新文件一样） */
            new_file = 1;
        } else {
            char err[512];
            img = pnm_load(arg, err, sizeof err);
            if (!img) {
                snprintf(load_err, sizeof load_err, "无法打开“%s”：\n%s", base_name(arg), err);
                free(arg_path);
                arg_path = NULL;
            }
        }
    }
    if (!img)
        img = blank_image(DEFAULT_W, DEFAULT_H);
    if (!img) {
        fprintf(stderr, "skpaint: 内存不足\n");
        free(arg_path);
        return 1;
    }

    pa.app = sk_app_connect("skpaint");
    if (!pa.app) {
        sk_canvas_free(img);
        free(arg_path);
        return 1;
    }
    /* 窗口刚好装下图片（再加工具栏），但不超过屏幕 */
    int sw = sk_app_screen_w(pa.app), sh = sk_app_screen_h(pa.app);
    int W = LEFT_W + img->w + 2 * MARGIN, H = TOP_H + img->h + 2 * MARGIN + BOTTOM_H;
    if (sw > 0)
        W = imin(W, sw - 60);
    if (sh > 0)
        H = imin(H, sh - 100);
    W = imax(W, 600);
    H = imax(H, 490);
    /* 先用固定的标题“画板”创建窗口：控件库按标题生成手绘种子，这样无论打开哪个文件，
     * 界面的手绘线条都一模一样；真正的标题马上再设置 */
    pa.win = sk_window_create(pa.app, "画板", W, H, SK_WIN_CENTER | SK_WIN_RESIZABLE, SK_RED);
    if (!pa.win) {
        sk_canvas_free(img);
        free(arg_path);
        sk_app_disconnect(pa.app);
        return 1;
    }
    sk_window_set_min_size(pa.win, 600, 490);
    pa.ui = sk_ui_new(pa.win);
    pa.img = img;
    if (!pa.ui || undo_reset(&pa.hist, img) != 0 || build_ui(&pa) != 0) {
        fprintf(stderr, "skpaint: 内存不足\n");
        if (pa.ui)
            sk_ui_free(pa.ui);
        sk_window_destroy(pa.win);
        sk_app_disconnect(pa.app);
        undo_free(&pa.hist);
        sk_canvas_free(img);
        free(arg_path);
        return 1;
    }
    pa.saved_version = pa.hist.version;
    pa.path = arg_path;
    sk_ui_set_background(pa.ui, paint_background, &pa);
    sk_ui_set_handler(pa.ui, app_event, &pa);
    sk_app_add_idle(pa.app, cursor_idle, &pa);
    set_size(&pa, pa.size);
    set_color(&pa, 0);
    set_tool(&pa, pa.tool);
    sk_ui_set_layout(pa.ui, layout, &pa);
    update_doc_ui(&pa);
    if (new_file && pa.path)
        status(&pa, "新图片：保存时会写到 %s", doc_name(&pa));
    else if (arg && pa.path)
        status(&pa, "%s：%d × %d 像素", doc_name(&pa), img->w, img->h);
    debug_state(&pa, "ready");

    if (load_err[0])
        error_box(&pa, "打开失败", "%s", load_err);

    int rc = sk_app_run(pa.app);
    sk_ui_free(pa.ui);
    sk_app_disconnect(pa.app);
    undo_free(&pa.hist);
    sk_canvas_free(pa.img);
    free(pa.path);
    return rc;
}
