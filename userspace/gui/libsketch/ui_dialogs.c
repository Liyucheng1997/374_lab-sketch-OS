/*
 * ui_dialogs.c — 对话框：消息框、输入框、文件选择框（sk_dialog_*）
 *
 * “阻塞式”对话框是怎么实现的？
 *   应用写 int r = sk_dialog_message(app, "提示", "确定要删除吗？", "删除|取消");
 *   这一行要等用户点了按钮才返回——可整个程序是单线程、事件驱动的，“等”的时候谁来处理事件？
 *   答案是“嵌套事件循环”：对话框创建自己的窗口后调用 sk_app_run_until(app, &done)，
 *   它和主循环一样 poll() 套接字、分发事件、调用空闲回调重画，只是在 done 变成 1 时返回。
 *   在这期间，应用的其它窗口也照常重画（它们的空闲回调还在），但通过 sk_app_set_modal
 *   把对话框设为“模态窗口”后，其它窗口收不到鼠标和键盘——用户只能先处理对话框。
 *   对话框可以嵌套：文件对话框里点“保存”，如果文件已存在，再弹一个“要替换吗？”的消息框。
 *
 * 每个对话框是一个 SK_WIN_DIALOG | SK_WIN_CENTER 窗口，有自己的 sk_ui 和控件。
 * 键盘约定：Esc = 取消（返回 -1 / NULL），Enter = 默认按钮（第一个按钮 / 确定 / 打开 / 保存）。
 * 用户点窗口的关闭按钮也等于取消。
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#include "sketch/ui.h"
#include "sketch/rough.h"
#include "ui_internal.h"
#include "skutil.h"

#define DLG_PAD      20      /* 对话框内容到窗口边缘的距离 */
#define DLG_BTN_H    34      /* 按钮高度（>= 34 时按钮用 20px 字体） */
#define DLG_BTN_GAP  12
#define DLG_LINE_GAP 4       /* 与 sk_paragraph 的行距一致 */
#define DLG_MAX_BTNS 6

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ================================================================ 公共部分 */

typedef struct dlg {
    sk_app        *app;
    sk_window     *win;
    sk_ui         *ui;
    sk_window     *prev_modal;
    volatile int   done;
    int            result;        /* 按钮序号；-1 = 取消 */
    int            default_btn;   /* 回车对应的按钮；-1 = 没有 */
    void         (*on_enter)(struct dlg *d);   /* 自定义回车（文件对话框） */
    int          (*on_key)(struct dlg *d, const sk_event *e);   /* 其它没人处理的按键，返回 1 = 已处理 */
    void          *ctx;           /* 具体对话框的数据 */
} dlg;

static void dlg_finish(dlg *d, int result)
{
    d->result = result;
    d->done = 1;
}

/* 对话框窗口的事件：控件没处理的键盘事件和窗口关闭都在这里 */
static int dlg_handler(sk_window *win, const sk_event *e, void *user)
{
    (void)win;
    dlg *d = user;
    if (e->type == SK_EVENT_CLOSE) {
        dlg_finish(d, -1);
        return 1;               /* 不要默认销毁：窗口由 dlg_close 统一销毁 */
    }
    if (e->type == SK_EVENT_KEY && e->key.state != 0) {
        if (e->key.modifiers & (SK_MOD_CTRL | SK_MOD_ALT | SK_MOD_SUPER))
            return 0;
        uint32_t kc = e->key.keycode, cp = e->key.codepoint;
        if (kc == KEY_ESC || (kc == 0 && cp == 0x1B)) {
            dlg_finish(d, -1);
            return 1;
        }
        if (kc == KEY_ENTER || kc == KEY_KPENTER || (kc == 0 && cp == '\r')) {
            if (d->on_enter)
                d->on_enter(d);
            else if (d->default_btn >= 0)
                dlg_finish(d, d->default_btn);
            return 1;
        }
        if (d->on_key)
            return d->on_key(d, e);
    }
    return 0;
}

static void dlg_button_click(sk_widget *b, void *user)
{
    dlg_finish(user, b->id);
}

/* 创建对话框窗口（内容区 w x h） */
static int dlg_open(dlg *d, sk_app *app, const char *title, int w, int h, unsigned extra_flags, sk_color accent)
{
    memset(d, 0, sizeof(*d));
    d->app = app;
    d->default_btn = -1;
    d->result = -1;
    if (!app)
        return -1;
    int sw = sk_app_screen_w(app), sh = sk_app_screen_h(app);
    if (sw > 0)
        w = imin(w, sw - 40);
    if (sh > 0)
        h = imin(h, sh - 80);
    d->win = sk_window_create(app, title && *title ? title : "提示", imax(w, 120), imax(h, 80),
                              SK_WIN_DIALOG | SK_WIN_CENTER | extra_flags, accent);
    if (!d->win)
        return -1;
    d->ui = sk_ui_new(d->win);
    if (!d->ui) {
        sk_window_destroy(d->win);
        d->win = NULL;
        return -1;
    }
    sk_ui_set_handler(d->ui, dlg_handler, d);
    return 0;
}

/* 运行嵌套事件循环直到对话框结束；返回结果（按钮序号或 -1） */
static int dlg_run(dlg *d)
{
    d->prev_modal = sk_app_set_modal(d->app, d->win);
    sk_ui_invalidate_all(d->ui);
    sk_window_show(d->win);
    if (sk_app_run_until(d->app, &d->done) < 0)
        d->result = -1;         /* 连接断开或应用要退出：当作取消 */
    return d->result;
}

static void dlg_close(dlg *d)
{
    if (!d->win)
        return;
    sk_app_set_modal(d->app, d->prev_modal);
    sk_ui_free(d->ui);
    sk_window_destroy(d->win);
    d->ui = NULL;
    d->win = NULL;
}

/* 把 "确定|取消" 拆成按钮文字，返回个数 */
static int dlg_split_buttons(const char *spec, char *buf, size_t bufsz, char **labels, int max)
{
    sk_strlcpy(buf, spec && *spec ? spec : "确定", bufsz);
    int n = 0;
    char *s = buf;
    while (n < max) {
        char *bar = strchr(s, '|');
        if (bar)
            *bar = 0;
        char *t = sk_trim(s);
        if (*t)
            labels[n++] = t;
        if (!bar)
            break;
        s = bar + 1;
    }
    if (n == 0)
        labels[n++] = "确定";
    return n;
}

static int dlg_button_width(const char *label)
{
    return imax(88, sk_text_width(sk_font_get(SK_FONT_TITLE), label) + 36);
}

/* 在底部从右往左排一行按钮（第一个按钮在最左边，和 Windows/中文软件的习惯一致：确定 在 取消 左边） */
static void dlg_layout_buttons(sk_widget **btns, int n, int right, int y)
{
    int total = 0;
    for (int i = 0; i < n; i++)
        total += btns[i]->r.w + (i ? DLG_BTN_GAP : 0);
    int x = right - total;
    for (int i = 0; i < n; i++) {
        sk_widget_set_rect(btns[i], sk_rect_make(x, y, btns[i]->r.w, DLG_BTN_H));
        x += btns[i]->r.w + DLG_BTN_GAP;
    }
}

/* ================================================================ 消息框 */

enum { ICON_NONE = 0, ICON_INFO, ICON_QUESTION, ICON_WARN, ICON_ERROR };

static int contains_any(const char *s, const char *const *words)
{
    if (!s)
        return 0;
    for (; *words; words++)
        if (strstr(s, *words))
            return 1;
    return 0;
}

/* 按关键词选图标：先看标题，再看正文 */
static int pick_icon(const char *title, const char *text)
{
    static const char *const err[] = { "错误", "失败", "无法", "不能", "出错", "Error", "error", NULL };
    static const char *const warn[] = { "警告", "注意", "小心", "危险", "Warning", NULL };
    static const char *const ask[] = { "确定要", "是否", "要不要", "确认", "吗？", "？", "?", NULL };
    static const char *const info[] = { "提示", "信息", "关于", "完成", "成功", "说明", "欢迎", NULL };
    const char *src[2] = { title, text };
    for (int i = 0; i < 2; i++) {
        if (contains_any(src[i], err)) return ICON_ERROR;
        if (contains_any(src[i], warn)) return ICON_WARN;
        if (contains_any(src[i], ask)) return ICON_QUESTION;
        if (contains_any(src[i], info)) return ICON_INFO;
    }
    return ICON_NONE;
}

static sk_color icon_color(int kind)
{
    switch (kind) {
    case ICON_ERROR: return SK_RED;
    case ICON_WARN: return SK_YELLOW;
    case ICON_QUESTION: return SK_PURPLE;
    default: return SK_BLUE;
    }
}

/* 手绘的提示图标：蜡笔排线涂色的圆（警告是三角形），中间一个大大的手写符号 */
static void msg_icon_draw(sk_widget *w, sk_canvas *c, void *user)
{
    int kind = (int)(intptr_t)user;
    sk_rect r = sk_rect_inset(w->r, 4, 4);   /* 手绘线会“出头”，留一点边 */
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, d = (float)imin(r.w, r.h) - 2.0f;
    sk_rough_style s;
    sk_style_hatched(&s, icon_color(kind), w->seed);
    s.stroke = SK_INK;
    s.stroke_width = 1.8f;
    s.hachure_gap = 4.0f;
    s.fill_weight = 1.6f;
    s.roughness = 0.7f;          /* 图标小，线条收敛一点，角上“出头”不会太长 */
    const char *glyph = "i";
    if (kind == ICON_WARN) {
        sk_pointf pts[3] = { { cx, (float)r.y }, { (float)(r.x + r.w), (float)(r.y + r.h - 3) },
                             { (float)r.x, (float)(r.y + r.h - 3) } };
        sk_rough_polygon(c, pts, 3, &s);
        glyph = "!";
        cy += 6;
    } else {
        sk_rough_circle(c, cx, cy, d, &s);
        glyph = kind == ICON_ERROR ? "×" : kind == ICON_QUESTION ? "?" : "i";
    }
    const sk_font *f = sk_font_get(SK_FONT_LARGE);
    int tw = sk_text_width(f, glyph);
    int base = (int)cy + (sk_font_ascent(f) - sk_font_descent(f)) / 2;
    /* 纸色描一圈，让符号从排线里“跳”出来 */
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy)
                sk_draw_text(c, f, (int)cx - tw / 2 + dx, base + dy, glyph, SK_WITH_ALPHA(SK_PAPER, 0xE0));
    sk_draw_text(c, f, (int)cx - tw / 2, base, glyph, SK_INK);
}

int sk_dialog_message(sk_app *app, const char *title, const char *text, const char *buttons)
{
    char bbuf[256];
    char *labels[DLG_MAX_BTNS];
    int nb = dlg_split_buttons(buttons, bbuf, sizeof(bbuf), labels, DLG_MAX_BTNS);
    if (!text)
        text = "";
    int icon = pick_icon(title, text);

    /* 先量尺寸：文字最宽 420 像素，自动换行 */
    const sk_font *f = sk_font_get(SK_FONT_UI);
    int icon_w = icon ? 56 : 0;
    int text_w = imin(420, imax(180, sk_text_width(f, text) + 8));
    int text_h = sk_draw_text_wrapped(NULL, f, sk_rect_make(0, 0, text_w, 10000), text, SK_INK, DLG_LINE_GAP);
    int btn_total = 0;
    for (int i = 0; i < nb; i++)
        btn_total += dlg_button_width(labels[i]) + (i ? DLG_BTN_GAP : 0);
    int content_w = imax(icon_w + text_w, btn_total);
    int body_h = imax(icon ? 48 : 0, text_h);
    int W = DLG_PAD * 2 + content_w;
    int H = DLG_PAD + body_h + 22 + DLG_BTN_H + DLG_PAD - 4;

    dlg d;
    if (dlg_open(&d, app, title, W, H, 0, icon ? icon_color(icon) : SK_BLUE) < 0)
        return -1;
    if (icon)
        sk_custom_new(d.ui, sk_rect_make(DLG_PAD - 6, DLG_PAD - 6, 58, 58), msg_icon_draw, NULL, 0,
                      (void *)(intptr_t)icon);
    /* 文字比图标矮时垂直居中对齐图标 */
    int ty = DLG_PAD + (icon && text_h < 48 ? (48 - text_h) / 2 : 0);
    sk_paragraph_new(d.ui, sk_rect_make(DLG_PAD + icon_w, ty, text_w, text_h + 4), text, SK_FONT_UI);
    sk_widget *btns[DLG_MAX_BTNS];
    for (int i = 0; i < nb; i++) {
        btns[i] = sk_button_new(d.ui, sk_rect_make(0, 0, dlg_button_width(labels[i]), DLG_BTN_H), labels[i],
                                dlg_button_click, &d);
        if (btns[i])
            btns[i]->id = i;
        else
            nb = i;
    }
    if (nb > 0)
        btns[0]->accent = icon ? icon_color(icon) : SK_BLUE;
    dlg_layout_buttons(btns, nb, sk_window_width(d.win) - DLG_PAD, sk_window_height(d.win) - DLG_PAD - DLG_BTN_H + 4);
    d.default_btn = 0;
    int r = dlg_run(&d);
    dlg_close(&d);
    return r;
}

/* ================================================================ 输入框 */

typedef struct {
    sk_widget *box;
    char      *answer;
} input_ctx;

static void input_ok(dlg *d)
{
    input_ctx *ic = d->ctx;
    free(ic->answer);
    ic->answer = strdup(sk_textbox_text(ic->box));
    dlg_finish(d, 0);
}

static void input_enter(sk_widget *box, void *user)
{
    (void)box;
    input_ok(user);
}

static void input_button(sk_widget *b, void *user)
{
    if (b->id == 0)
        input_ok(user);
    else
        dlg_finish(user, -1);
}

char *sk_dialog_input(sk_app *app, const char *title, const char *prompt, const char *initial)
{
    const sk_font *f = sk_font_get(SK_FONT_UI);
    if (!prompt)
        prompt = "";
    int content_w = 380;
    int text_h = *prompt ? sk_draw_text_wrapped(NULL, f, sk_rect_make(0, 0, content_w, 10000), prompt, SK_INK,
                                                DLG_LINE_GAP)
                         : 0;
    int box_h = 34;
    int W = DLG_PAD * 2 + content_w;
    int H = DLG_PAD + text_h + (text_h ? 10 : 0) + box_h + 22 + DLG_BTN_H + DLG_PAD - 4;

    dlg d;
    input_ctx ic = { 0 };
    if (dlg_open(&d, app, title && *title ? title : "请输入", W, H, 0, SK_BLUE) < 0)
        return NULL;
    d.ctx = &ic;
    int y = DLG_PAD;
    if (*prompt) {
        sk_paragraph_new(d.ui, sk_rect_make(DLG_PAD, y, content_w, text_h + 4), prompt, SK_FONT_UI);
        y += text_h + 10;
    }
    ic.box = sk_textbox_new(d.ui, sk_rect_make(DLG_PAD, y, content_w, box_h), initial ? initial : "", NULL, &d);
    if (ic.box) {
        ic.box->on_click = input_enter;      /* 在输入框里按回车 = 确定 */
        ic.box->user = &d;
        sk_ui_focus(d.ui, ic.box);
    }
    sk_widget *btns[2];
    btns[0] = sk_button_new(d.ui, sk_rect_make(0, 0, dlg_button_width("确定"), DLG_BTN_H), "确定", input_button, &d);
    btns[1] = sk_button_new(d.ui, sk_rect_make(0, 0, dlg_button_width("取消"), DLG_BTN_H), "取消", input_button, &d);
    int nb = 0;
    for (int i = 0; i < 2; i++)
        if (btns[i]) {
            btns[i]->id = i;
            btns[nb++] = btns[i];
        }
    dlg_layout_buttons(btns, nb, sk_window_width(d.win) - DLG_PAD, sk_window_height(d.win) - DLG_PAD - DLG_BTN_H + 4);
    d.on_enter = input_ok;
    int r = dlg_run(&d);
    dlg_close(&d);
    if (r != 0) {
        free(ic.answer);
        return NULL;
    }
    return ic.answer;
}

/* ================================================================ 文件选择框 */
/*
 * 文件系统小知识（这个对话框用到的系统调用）：
 *   opendir/readdir —— 读目录：目录在 Linux 里也是一种文件，内容是“名字 → inode 号”的列表；
 *   stat            —— 按名字取 inode 的信息：类型（目录/普通文件/符号链接……）、大小、修改时间、权限位；
 *   getcwd          —— 当前工作目录（每个进程都有一个，由内核记在进程描述符里）。
 * 符号链接用 stat（跟随链接）判断它指向的是目录还是文件；链接坏了再用 lstat 退一步。
 */

typedef struct {
    char    *name;
    int      is_dir;
    int      is_exec;
    long long size;
    time_t   mtime;
} fd_entry;

typedef struct {
    dlg        *d;
    int         save;
    char        dir[1024];
    fd_entry   *ents;
    int         n, cap;
    int         sort_col, sort_desc;
    sk_widget  *up, *home, *path, *list, *name_label, *name, *status, *ok, *cancel;
    char       *result;
} fd_ctx;

static fd_ctx *g_sort_ctx;      /* qsort 的比较函数拿不到 user 参数，用一个静态变量（单线程，安全） */

static int fd_name_cmp(const char *a, const char *b)
{
    /* 不区分 ASCII 大小写；中文按码点顺序（简单、确定） */
    for (;; a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca)
            return (int)ca - (int)cb;
    }
}

static int fd_cmp(const void *pa, const void *pb)
{
    const fd_entry *a = pa, *b = pb;
    if (a->is_dir != b->is_dir)
        return b->is_dir - a->is_dir;           /* 文件夹永远排在前面 */
    int r = 0;
    switch (g_sort_ctx->sort_col) {
    case 1: r = (a->size > b->size) - (a->size < b->size); break;
    case 2: r = (a->mtime > b->mtime) - (a->mtime < b->mtime); break;
    default: break;
    }
    if (r == 0)
        r = fd_name_cmp(a->name, b->name);
    return g_sort_ctx->sort_desc ? -r : r;
}

static int has_ext(const char *name, const char *const *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return 0;
    for (; *exts; exts++)
        if (fd_name_cmp(dot + 1, *exts) == 0)
            return 1;
    return 0;
}

static int fd_icon(const fd_entry *e)
{
    static const char *const text_ext[] = { "txt", "md", "c", "h", "conf", "cfg", "ini", "sh", "log", "json",
                                            "py", "js", "html", "css", "csv", "xml", "mk", "s", NULL };
    static const char *const img_ext[] = { "ppm", "pgm", "pbm", "png", "bmp", "jpg", "jpeg", "gif", NULL };
    if (e->is_dir)
        return SK_ICON_FOLDER;
    if (has_ext(e->name, img_ext))
        return SK_ICON_IMAGE;
    if (has_ext(e->name, text_ext) || strcmp(e->name, "Makefile") == 0)
        return SK_ICON_TEXT;
    if (e->is_exec)
        return SK_ICON_PROGRAM;
    return SK_ICON_FILE;
}

static void fd_free_entries(fd_ctx *fc)
{
    for (int i = 0; i < fc->n; i++)
        free(fc->ents[i].name);
    free(fc->ents);
    fc->ents = NULL;
    fc->n = fc->cap = 0;
}

static void fd_status(fd_ctx *fc, const char *msg)
{
    if (fc->status)
        sk_widget_set_text(fc->status, msg ? msg : "");
}

/* 把条目填进列表 */
static void fd_fill_list(fd_ctx *fc)
{
    g_sort_ctx = fc;
    if (fc->n > 1)
        qsort(fc->ents, fc->n, sizeof(fd_entry), fd_cmp);
    sk_list_clear(fc->list);
    for (int i = 0; i < fc->n; i++) {
        fd_entry *e = &fc->ents[i];
        char size[32] = "", when[32] = "";
        if (!e->is_dir)
            sk_fmt_size(size, sizeof(size), (uint64_t)e->size);
        struct tm tm;
        if (localtime_r(&e->mtime, &tm))
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        const char *cells[3] = { e->name, size, when };
        sk_list_add(fc->list, fd_icon(e), cells, i);
    }
}

/* 读目录 path；失败时保持原来的目录并在状态栏说明原因。返回 0 成功 */
static int fd_load(fd_ctx *fc, const char *path)
{
    char dir[1024];
    if (!path || !*path)
        path = "/";
    if (path[0] == '/')
        sk_strlcpy(dir, path, sizeof(dir));
    else
        sk_path_join(dir, sizeof(dir), fc->dir, path);
    sk_path_normalize(dir);
    DIR *dp = opendir(dir);
    if (!dp) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "无法打开文件夹 %.900s：%s", dir, sk_strerror(errno));
        fd_status(fc, msg);
        return -1;
    }
    fd_free_entries(fc);
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char full[2048];
        sk_path_join(full, sizeof(full), dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 && lstat(full, &st) != 0)
            continue;               /* 读目录和 stat 之间文件被删掉了 */
        if (fc->n + 1 > fc->cap) {
            int cap = fc->cap ? fc->cap * 2 : 64;
            fd_entry *ne = realloc(fc->ents, cap * sizeof(fd_entry));
            if (!ne)
                break;
            fc->ents = ne;
            fc->cap = cap;
        }
        fd_entry *e = &fc->ents[fc->n];
        e->name = strdup(de->d_name);
        if (!e->name)
            break;
        e->is_dir = S_ISDIR(st.st_mode);
        e->is_exec = S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH));
        e->size = (long long)st.st_size;
        e->mtime = st.st_mtime;
        fc->n++;
    }
    closedir(dp);
    sk_strlcpy(fc->dir, dir, sizeof(fc->dir));
    sk_textbox_set_text(fc->path, fc->dir);
    fd_fill_list(fc);
    sk_list_set_scroll(fc->list, 0);
    char msg[64];
    int nd = 0;
    for (int i = 0; i < fc->n; i++)
        nd += fc->ents[i].is_dir;
    if (fc->n == 0)
        msg[0] = 0;
    else
        snprintf(msg, sizeof(msg), "%d 个文件夹，%d 个文件", nd, fc->n - nd);
    fd_status(fc, msg);
    return 0;
}

static const fd_entry *fd_selected(fd_ctx *fc)
{
    int row = sk_list_selected(fc->list);
    if (row < 0)
        return NULL;
    intptr_t i = sk_list_data(fc->list, row);
    return (i >= 0 && i < fc->n) ? &fc->ents[i] : NULL;
}

static void fd_accept(fd_ctx *fc, const char *full)
{
    free(fc->result);
    fc->result = strdup(full);
    dlg_finish(fc->d, 0);
}

/* 保存：检查文件名，已存在时问一句 */
static void fd_try_save(fd_ctx *fc)
{
    char name[512];
    sk_strlcpy(name, sk_textbox_text(fc->name), sizeof(name));
    char *t = sk_trim(name);
    if (!*t) {
        fd_status(fc, "请输入文件名");
        sk_ui_focus(fc->d->ui, fc->name);
        return;
    }
    char full[2048];
    if (t[0] == '/')
        sk_strlcpy(full, t, sizeof(full));
    else
        sk_path_join(full, sizeof(full), fc->dir, t);
    sk_path_normalize(full);
    struct stat st;
    if (stat(full, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            /* 输入的是一个文件夹：进去 */
            if (fd_load(fc, full) == 0)
                sk_textbox_set_text(fc->name, "");
            return;
        }
        const char *base = strrchr(full, '/');
        char q[1200];
        snprintf(q, sizeof(q), "“%.900s”已经存在。\n要替换它吗？", base ? base + 1 : full);
        if (sk_dialog_message(fc->d->app, "确认替换", q, "替换|取消") != 0)
            return;
        fd_accept(fc, full);
        return;
    }
    /* 新文件：所在的文件夹必须存在 */
    char parent[2048];
    sk_strlcpy(parent, full, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash)
        *(slash == parent ? slash + 1 : slash) = 0;
    if (stat(parent, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "文件夹 %.900s 不存在", parent);
        fd_status(fc, msg);
        return;
    }
    fd_accept(fc, full);
}

/* “打开/保存”按钮或回车 */
static void fd_ok(dlg *d)
{
    fd_ctx *fc = d->ctx;
    const fd_entry *e = fd_selected(fc);
    if (fc->save) {
        /* 列表焦点在文件夹上时回车 = 进入文件夹 */
        if (e && e->is_dir && sk_ui_focused(d->ui) == fc->list) {
            char full[2048];
            sk_path_join(full, sizeof(full), fc->dir, e->name);
            fd_load(fc, full);
            return;
        }
        fd_try_save(fc);
        return;
    }
    if (!e) {
        fd_status(fc, "请先选择一个文件");
        return;
    }
    char full[2048];
    sk_path_join(full, sizeof(full), fc->dir, e->name);
    if (e->is_dir)
        fd_load(fc, full);
    else
        fd_accept(fc, full);
}

static void fd_ok_click(sk_widget *b, void *user)
{
    (void)b;
    fd_ok(user);
}

/* 在文件名框里按回车 = 保存 */
static void fd_name_enter(sk_widget *box, void *user)
{
    (void)box;
    fd_try_save(((dlg *)user)->ctx);
}

static void fd_cancel_click(sk_widget *b, void *user)
{
    (void)b;
    dlg_finish(user, -1);
}

static void fd_up_click(sk_widget *b, void *user)
{
    (void)b;
    fd_ctx *fc = ((dlg *)user)->ctx;
    if (strcmp(fc->dir, "/") == 0)
        return;
    /* 回到上一级后，选中刚才所在的那个文件夹（方便接着往下找） */
    char child[256];
    const char *slash = strrchr(fc->dir, '/');
    sk_strlcpy(child, slash ? slash + 1 : "", sizeof(child));
    char parent[1024];
    sk_strlcpy(parent, fc->dir, sizeof(parent));
    char *ps = strrchr(parent, '/');
    if (ps)
        *(ps == parent ? ps + 1 : ps) = 0;
    if (fd_load(fc, parent) == 0) {
        for (int i = 0; i < sk_list_count(fc->list); i++)
            if (strcmp(sk_list_cell(fc->list, i, 0), child) == 0) {
                sk_list_select(fc->list, i);
                break;
            }
    }
}

/* 列表有焦点时按退格 = 回到上一级（输入框自己会处理退格，不会走到这里） */
static int fd_key(dlg *d, const sk_event *e)
{
    if (e->key.keycode == KEY_BACKSPACE || (e->key.keycode == 0 && e->key.codepoint == 0x08)) {
        fd_up_click(NULL, d);
        return 1;
    }
    return 0;
}

static void fd_home_click(sk_widget *b, void *user)
{
    (void)b;
    fd_ctx *fc = ((dlg *)user)->ctx;
    const char *home = getenv("HOME");
    fd_load(fc, home && *home ? home : "/root");
}

/* 在路径框里按回车 */
static void fd_path_enter(sk_widget *box, void *user)
{
    fd_ctx *fc = ((dlg *)user)->ctx;
    char path[1024];
    sk_strlcpy(path, sk_textbox_text(box), sizeof(path));
    char *t = sk_trim(path);
    char full[2048];
    if (t[0] == '/')
        sk_strlcpy(full, t, sizeof(full));
    else
        sk_path_join(full, sizeof(full), fc->dir, t);
    sk_path_normalize(full);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        fd_load(fc, full);
        sk_ui_focus(fc->d->ui, fc->list);
    } else if (stat(full, &st) == 0 && !fc->save) {
        fd_accept(fc, full);
    } else if (fc->save) {
        /* 保存模式下输入了一个完整的新文件路径：拆成目录 + 文件名 */
        char *slash = strrchr(full, '/');
        if (slash) {
            *slash = 0;
            if (fd_load(fc, *full ? full : "/") == 0)
                sk_textbox_set_text(fc->name, slash + 1);
        }
    } else {
        char msg[1200];
        snprintf(msg, sizeof(msg), "找不到 %.900s", full);
        fd_status(fc, msg);
        sk_textbox_set_text(box, fc->dir);
    }
}

/* 列表选中变化：保存模式下把文件名填进文件名框 */
static void fd_sel_change(sk_widget *list, void *user)
{
    (void)list;
    fd_ctx *fc = ((dlg *)user)->ctx;
    const fd_entry *e = fd_selected(fc);
    if (fc->save && e && !e->is_dir)
        sk_textbox_set_text(fc->name, e->name);
}

/* 双击 / 回车：文件夹 → 进入；文件 → 选定 */
static void fd_activate(sk_widget *list, void *user)
{
    (void)list;
    dlg *d = user;
    fd_ctx *fc = d->ctx;
    const fd_entry *e = fd_selected(fc);
    if (!e)
        return;
    char full[2048];
    sk_path_join(full, sizeof(full), fc->dir, e->name);
    if (e->is_dir)
        fd_load(fc, full);
    else if (fc->save) {
        sk_textbox_set_text(fc->name, e->name);
        fd_try_save(fc);
    } else
        fd_accept(fc, full);
}

static void fd_sort(sk_widget *list, int col, void *user)
{
    (void)list;
    fd_ctx *fc = ((dlg *)user)->ctx;
    /* 与列表表头小三角的规则一致：同一列再点一次切换升降序 */
    if (fc->sort_col == col)
        fc->sort_desc = !fc->sort_desc;
    else {
        fc->sort_col = col;
        fc->sort_desc = 0;
    }
    /* 排序后保持选中同一个文件 */
    const fd_entry *e = fd_selected(fc);
    char keep[256] = "";
    if (e)
        sk_strlcpy(keep, e->name, sizeof(keep));
    fd_fill_list(fc);
    for (int i = 0; keep[0] && i < sk_list_count(fc->list); i++)
        if (strcmp(sk_list_cell(fc->list, i, 0), keep) == 0) {
            sk_list_select(fc->list, i);
            break;
        }
}

static void fd_layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    fd_ctx *fc = ((dlg *)user)->ctx;
    int pad = 16, y = pad;
    sk_widget_set_rect(fc->up, sk_rect_make(pad, y, 36, 34));
    sk_widget_set_rect(fc->home, sk_rect_make(pad + 42, y, 36, 34));
    sk_widget_set_rect(fc->path, sk_rect_make(pad + 88, y, W - pad * 2 - 88, 34));
    y += 34 + 10;
    int bottom = H - pad - DLG_BTN_H;               /* 按钮行的 y */
    int name_h = fc->save ? 34 + 10 : 0;
    int list_h = bottom - 10 - name_h - y;
    sk_widget_set_rect(fc->list, sk_rect_make(pad, y, W - pad * 2, imax(60, list_h)));
    if (fc->save) {
        int ny = bottom - 10 - 34;
        sk_widget_set_rect(fc->name_label, sk_rect_make(pad, ny, 70, 34));
        sk_widget_set_rect(fc->name, sk_rect_make(pad + 72, ny, W - pad * 2 - 72, 34));
    }
    int bw_ok = fc->ok->r.w, bw_cancel = fc->cancel->r.w;
    sk_widget_set_rect(fc->cancel, sk_rect_make(W - pad - bw_cancel, bottom, bw_cancel, DLG_BTN_H));
    sk_widget_set_rect(fc->ok, sk_rect_make(W - pad - bw_cancel - DLG_BTN_GAP - bw_ok, bottom, bw_ok, DLG_BTN_H));
    sk_widget_set_rect(fc->status, sk_rect_make(pad, bottom, W - pad * 2 - bw_ok - bw_cancel - DLG_BTN_GAP - 10,
                                                DLG_BTN_H));
}

char *sk_dialog_file(sk_app *app, const char *title, const char *start_dir, int save, const char *default_name)
{
    /* 起始目录：参数 → 当前目录 → HOME；如果给的是文件路径，拆成目录 + 文件名 */
    char start[1024] = "", name0[256] = "";
    struct stat st;
    if (start_dir && *start_dir) {
        sk_strlcpy(start, start_dir, sizeof(start));
        if (stat(start, &st) == 0 && !S_ISDIR(st.st_mode)) {
            char *slash = strrchr(start, '/');
            if (slash) {
                sk_strlcpy(name0, slash + 1, sizeof(name0));
                *(slash == start ? slash + 1 : slash) = 0;
            }
        }
    }
    if (start[0] && start[0] != '/') {
        /* 相对路径：相对于进程的当前目录 */
        char cwd[1024], rel[1024];
        sk_strlcpy(rel, start, sizeof(rel));
        if (getcwd(cwd, sizeof(cwd)))
            sk_path_join(start, sizeof(start), cwd, rel);
        else
            start[0] = 0;
    }
    if (!start[0] && !getcwd(start, sizeof(start)))
        start[0] = 0;
    if (!start[0]) {
        const char *home = getenv("HOME");
        sk_strlcpy(start, home && *home ? home : "/", sizeof(start));
    }
    if (default_name && *default_name)
        sk_strlcpy(name0, default_name, sizeof(name0));

    dlg d;
    fd_ctx fc;
    memset(&fc, 0, sizeof(fc));
    fc.save = save != 0;
    fc.d = &d;
    if (!title || !*title)
        title = save ? "保存文件" : "打开文件";
    if (dlg_open(&d, app, title, 600, save ? 470 : 430, SK_WIN_RESIZABLE, save ? SK_GREEN : SK_ORANGE) < 0)
        return NULL;
    sk_window_set_min_size(d.win, 420, 300);
    d.ctx = &fc;
    d.on_enter = fd_ok;
    d.on_key = fd_key;

    fc.up = sk_icon_button_new(d.ui, sk_rect_make(0, 0, 36, 34), SK_ICON_UP, NULL, fd_up_click, &d);
    fc.home = sk_icon_button_new(d.ui, sk_rect_make(0, 0, 36, 34), SK_ICON_HOME, NULL, fd_home_click, &d);
    fc.path = sk_textbox_new(d.ui, sk_rect_make(0, 0, 100, 34), start, NULL, &d);
    if (fc.path) {
        fc.path->on_click = fd_path_enter;
        fc.path->user = &d;
    }
    fc.list = sk_list_new(d.ui, sk_rect_make(0, 0, 100, 100), fd_sel_change, fd_activate, &d);
    static const char *const cols[] = { "名称", "大小", "修改时间" };
    static const int widths[] = { 0, -90, 172 };
    sk_list_set_columns(fc.list, cols, widths, 3);
    sk_list_set_sort_handler(fc.list, fd_sort);
    sk_widget_set_text(fc.list, "（空文件夹）");
    if (fc.save) {
        fc.name_label = sk_label_new(d.ui, sk_rect_make(0, 0, 70, 34), "文件名：", SK_FONT_UI, SK_ALIGN_LEFT);
        fc.name = sk_textbox_new(d.ui, sk_rect_make(0, 0, 100, 34), name0, NULL, &d);
        if (fc.name) {
            fc.name->on_click = fd_name_enter;   /* 输入框自己吃掉回车，所以要把回车接到“保存”上 */
            fc.name->user = &d;
        }
    }
    fc.status = sk_label_new(d.ui, sk_rect_make(0, 0, 100, 34), "", SK_FONT_UI, SK_ALIGN_LEFT);
    if (fc.status)
        sk_label_set_color(fc.status, SK_INK_SOFT);
    const char *ok_label = save ? "保存" : "打开";
    fc.ok = sk_button_new(d.ui, sk_rect_make(0, 0, dlg_button_width(ok_label), DLG_BTN_H), ok_label, fd_ok_click, &d);
    fc.cancel = sk_button_new(d.ui, sk_rect_make(0, 0, dlg_button_width("取消"), DLG_BTN_H), "取消",
                              fd_cancel_click, &d);
    if (!fc.up || !fc.home || !fc.path || !fc.list || !fc.status || !fc.ok || !fc.cancel ||
        (fc.save && (!fc.name_label || !fc.name))) {
        dlg_close(&d);
        return NULL;
    }
    fc.ok->accent = save ? SK_GREEN : SK_ORANGE;
    fc.sort_col = 0;
    sk_ui_set_layout(d.ui, fd_layout, &d);
    if (fd_load(&fc, start) != 0) {
        const char *home = getenv("HOME");
        if (fd_load(&fc, home && *home ? home : "/") != 0)
            fd_load(&fc, "/");
    }
    if (fc.save) {
        sk_ui_focus(d.ui, fc.name);
    } else {
        sk_ui_focus(d.ui, fc.list);
        for (int i = 0; name0[0] && i < sk_list_count(fc.list); i++)
            if (strcmp(sk_list_cell(fc.list, i, 0), name0) == 0) {
                sk_list_select(fc.list, i);
                break;
            }
    }
    int r = dlg_run(&d);
    dlg_close(&d);
    fd_free_entries(&fc);
    if (r != 0) {
        free(fc.result);
        return NULL;
    }
    return fc.result;
}
