/*
 * skfiles — SketchOS 文件管理器
 *
 * 窗口布局：
 *   ┌──────────────────────────────────────────────────────────────────────┐
 *   │ [←后退][↑上一级][⌂主目录][⟳刷新] │ [+新建文件夹][删除]   □显示隐藏文件 │ 工具栏
 *   │ [/root/文档                                                        ] │ 地址栏（可编辑）
 *   ├──────────┬───────────────────────────────────────────────────────────┤
 *   │ ⌂ 主目录  │ 名称              大小     类型         修改时间            │
 *   │ 📖 文档   │ 📁 照片                    文件夹       2026-10-23 13:21    │
 *   │ 🖼 图片   │ 📄 笔记.txt       1.2 KB   文本文件     2026-10-23 13:21    │
 *   │ …        │ …                                                         │
 *   ├──────────┴───────────────────────────────────────────────────────────┤
 *   │ 5 个项目　已选中“笔记.txt”，1.2 KB                                    │ 状态栏
 *   └──────────────────────────────────────────────────────────────────────┘
 *
 * 操作：双击/回车打开（文件夹 → 进入；图片 → 画板；可执行文件 → 终端里运行；文本 → 记事本），
 * 右键菜单（打开、在终端中打开、复制、粘贴、重命名、删除、新建、属性），点表头排序。
 * 快捷键：Delete 删除  F2 重命名  F5 刷新  Backspace / Alt+← 后退  Alt+↑ 上一级  Alt+Home 主目录
 *         Ctrl+C / Ctrl+V 复制粘贴  Ctrl+H 显示/隐藏隐藏文件  Ctrl+L 编辑地址  Alt+Enter 属性
 *         菜单键 / Shift+F10 弹出右键菜单
 *
 * 文件系统操作（读目录、复制、删除……用到的系统调用）都在 ops.c，这个文件只管界面。
 *
 * 命令行：skfiles [文件夹或文件]
 *   参数是文件夹：打开它；是文件：打开它所在的文件夹并选中它；没有参数：打开 $HOME。
 *   （apps.conf 里是 “/bin/skfiles %f”，桌面上双击文件夹时 %f 就是那个文件夹。）
 *
 * 调试钩子（自动化测试 tests/apps/test_skfiles.py 依赖它）：
 *   环境变量 SKFILES_DEBUG=1 时，程序把内部状态逐行打印到标准输出，每行以 "skfiles: " 开头：
 *     geom <控件名> x y w h          每次布局之后：控件的位置（测试据此计算点击坐标）。
 *                                     控件名：back up home refresh newdir delete hidden path places list status
 *     list <路径>                     每次刷新 / 进入文件夹 / 排序 / 切换隐藏文件之后：当前文件夹，
 *     item <名字>\t<类型>\t<大小>      接着按显示顺序，每个可见的项目一行，
 *     end <项目数> sel=<选中的名字>    最后一行（sel= 后面为空表示没有选中）
 *     sel <名字>                      选中项改变（没有选中时名字为空）
 *     dialog <标题>: <正文>            弹出对话框之前（正文里的换行变成 " | "）
 *     launch <应用 id> <参数>          请显示服务器启动别的应用
 *     status <文字>                   状态栏上的操作提示
 */
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>
#include <sketch/rough.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#include "skutil.h"
#include "skfiles.h"

#define PAD          10
#define TOOL_Y       8
#define TOOL_H       38
#define TOOL_GAP     4
#define PATH_Y       (TOOL_Y + TOOL_H + 8)
#define PATH_H       34
#define BAND_H       (PATH_Y + PATH_H + 6)      /* 上方“工具栏纸条”的高度 */
#define BODY_Y       (BAND_H + 6)
#define STATUS_H     30
#define PLACES_W     150
#define PLACES_MIN_W 740                        /* 窗口比这窄时不显示左边的“常用位置” */
#define ROW_H        28                         /* 文件列表的行高 */
#define PLACE_ROW_H  34                         /* 常用位置的行高（>= 32 时列表画 24 像素的大图标） */
#define HISTORY_MAX  64
#define MENU_MAX     16
#define PLACES_MAX   8

enum { T_BACK, T_UP, T_HOME, T_REFRESH, T_NEWDIR, T_DELETE, T_COUNT };

static const struct {
    int         icon;
    const char *label;
    const char *debug_name;
} tools[T_COUNT] = {
    { SK_ICON_BACK, "后退", "back" },        { SK_ICON_UP, "上一级", "up" },
    { SK_ICON_HOME, "主目录", "home" },      { SK_ICON_REFRESH, "刷新", "refresh" },
    { SK_ICON_NEW_FOLDER, "新建文件夹", "newdir" }, { SK_ICON_TRASH, "删除", "delete" },
};

/* 右键菜单里的动作 */
enum {
    ACT_NONE = -1, ACT_OPEN, ACT_TERMINAL, ACT_TERMINAL_HERE, ACT_COPY, ACT_PASTE, ACT_RENAME,
    ACT_DELETE, ACT_NEW_FOLDER, ACT_NEW_FILE, ACT_PROPS, ACT_PROPS_DIR, ACT_REFRESH, ACT_HIDDEN,
};

/* “大小”列右对齐，后面补两个空格，免得数字和右边的“类型”挤在一起 */
static const char *const col_titles[4] = { "名称", "大小  ", "类型", "修改时间" };
static const int col_widths[4] = { 0, -84, 140, 156 };   /* 0 = 占满剩余宽度；负数 = 右对齐 */

typedef struct {
    const char *label;
    int         icon;
    char        path[FM_PATH_MAX];
} place;

/* 应用的全部状态：回调函数通过 user 指针拿到它 */
typedef struct {
    sk_app    *app;
    sk_window *win;
    sk_ui     *ui;
    sk_widget *tool[T_COUNT], *hidden_chk, *path, *places, *list, *status;
    int        labels;                  /* 工具栏按钮当前带不带文字（窗口窄时只显示图标） */
    int        sep_x;                   /* 工具栏两组按钮之间那条竖线的 x */

    char       cwd[FM_PATH_MAX];        /* 当前文件夹（规范化的绝对路径） */
    char       home[FM_PATH_MAX];
    fm_entry  *ents;                    /* 当前文件夹的全部项目（含隐藏的），已排序 */
    int        n;
    int        nvisible, nhidden;
    int        show_hidden;
    int        sort_col, sort_desc;     /* -1 = 默认（按名称升序） */

    char      *history[HISTORY_MAX];    /* “后退”用的历史（栈） */
    int        nhist;
    char       clip[FM_PATH_MAX];       /* 应用内的“文件剪贴板”：复制的是哪个文件 */
    int        has_clip;

    place      places_tab[PLACES_MAX];
    int        nplaces;

    int        menu_act[MENU_MAX];      /* 当前弹出菜单每一项对应的动作 */
    char       menu_target[FM_NAME_MAX];/* 右键点中的项目名（空 = 点在空白处） */

    int        dialog_depth;            /* >0：正在显示对话框（嵌套事件循环里） */
    int        refresh_pending, refresh_timer;
    int        debug;
} fm_app;

/* ================================================================ 调试输出 */

static void dbg(fm_app *fm, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dbg(fm_app *fm, const char *fmt, ...)
{
    if (!fm->debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("skfiles: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static void dbg_geom(fm_app *fm, const char *name, const sk_widget *w)
{
    if (w)
        dbg(fm, "geom %s %d %d %d %d", name, w->r.x, w->r.y, w->r.w, w->r.h);
}

/* ================================================================ 小工具 */

/* cwd 里名为 name 的项目的完整路径；放不下返回 -1 */
static int full_path(const fm_app *fm, const char *name, char *out, size_t n)
{
    int len = snprintf(out, n, "%s%s%s", fm->cwd, strcmp(fm->cwd, "/") == 0 ? "" : "/", name);
    return (len < 0 || (size_t)len >= n) ? -1 : 0;
}

/* 路径的上一级（只按字面处理，不解析符号链接——和 shell 里 cd .. 的习惯一致） */
static void parent_of(const char *path, char *out, size_t n)
{
    char tmp[FM_DEEP_PATH];
    sk_strlcpy(tmp, path, sizeof tmp);
    char *s = strrchr(tmp, '/');
    if (!s || s == tmp)
        sk_strlcpy(out, "/", n);
    else {
        *s = 0;
        sk_strlcpy(out, tmp, n);
    }
}

static const char *base_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return (s && s[1]) ? s + 1 : path;
}

/* 去掉首尾空白，结果拷进 out */
static void trimmed(const char *s, char *out, size_t n)
{
    char buf[FM_DEEP_PATH];
    sk_strlcpy(buf, s ? s : "", sizeof buf);
    sk_strlcpy(out, sk_trim(buf), n);
}

/* ================================================================ 对话框 */

static void refresh(fm_app *fm, const char *select);

static void deferred_refresh(sk_app *app, void *user)
{
    (void)app;
    fm_app *fm = user;
    fm->refresh_timer = 0;
    if (fm->dialog_depth > 0) {
        fm->refresh_pending = 1;     /* 又弹出了一个对话框：等它也关掉 */
        return;
    }
    refresh(fm, NULL);
}

/*
 * 对话框是“阻塞”的：sk_dialog_* 在嵌套的事件循环里等用户点按钮。等待期间我们的主窗口
 * 仍然会收到事件（例如重新获得焦点 → 刷新列表），所以：
 *   1. 调用对话框之前，要把需要的名字都拷贝出来——对话框返回后 fm->ents 可能已经换了一批；
 *   2. 对话框打开期间不刷新，只记下“待刷新”，等对话框关掉后在下一轮事件循环里刷新。
 */
static void after_dialog(fm_app *fm)
{
    if (fm->dialog_depth == 0 && fm->refresh_pending && !fm->refresh_timer) {
        fm->refresh_pending = 0;
        fm->refresh_timer = sk_app_add_timer(fm->app, 0, 0, deferred_refresh, fm);
    }
}

static int dialog_v(fm_app *fm, const char *title, const char *buttons, const char *fmt, va_list ap)
{
    char text[3000];
    vsnprintf(text, sizeof text, fmt, ap);
    if (fm->debug) {
        char flat[4000];
        size_t o = 0;
        for (const char *s = text; *s && o + 4 < sizeof flat; s++) {
            if (*s == '\n') {
                memcpy(flat + o, " | ", 3);
                o += 3;
            } else
                flat[o++] = *s;
        }
        flat[o] = 0;
        dbg(fm, "dialog %s: %s", title, flat);
    }
    fm->dialog_depth++;
    int r = sk_dialog_message(fm->app, title, text, buttons);
    fm->dialog_depth--;
    after_dialog(fm);
    return r;
}

/* 带几个按钮的询问框，返回按钮序号（-1 = 关掉/Esc） */
static int ask(fm_app *fm, const char *title, const char *buttons, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int ask(fm_app *fm, const char *title, const char *buttons, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = dialog_v(fm, title, buttons, fmt, ap);
    va_end(ap);
    return r;
}

/* 出错提示（标题里有“出错”，消息框会自动配上红色的 × 图标） */
static void error_box(fm_app *fm, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void error_box(fm_app *fm, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dialog_v(fm, "出错了", "确定", fmt, ap);
    va_end(ap);
}

static void info_box(fm_app *fm, const char *title, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void info_box(fm_app *fm, const char *title, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dialog_v(fm, title, "确定", fmt, ap);
    va_end(ap);
}

/* 输入框：返回 malloc 的字符串，取消返回 NULL */
static char *input_box(fm_app *fm, const char *title, const char *prompt, const char *initial)
{
    dbg(fm, "dialog %s: %s", title, prompt);
    fm->dialog_depth++;
    char *s = sk_dialog_input(fm->app, title, prompt, initial);
    fm->dialog_depth--;
    after_dialog(fm);
    return s;
}

/* ================================================================ 列表和状态栏 */

static fm_entry *entry_at(fm_app *fm, int row)
{
    if (row < 0 || row >= sk_list_count(fm->list))
        return NULL;
    intptr_t i = sk_list_data(fm->list, row);
    return (i >= 0 && i < fm->n) ? &fm->ents[i] : NULL;
}

static fm_entry *selected(fm_app *fm)
{
    return entry_at(fm, sk_list_selected(fm->list));
}

static int row_of(fm_app *fm, const char *name)
{
    int count = sk_list_count(fm->list);
    for (int row = 0; row < count; row++) {
        const fm_entry *e = entry_at(fm, row);
        if (e && strcmp(e->name, name) == 0)
            return row;
    }
    return -1;
}

/* 状态栏上的操作提示（下一次选中项改变时会被换回“N 个项目”） */
static void set_status(fm_app *fm, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(fm_app *fm, const char *fmt, ...)
{
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sk_widget_set_text(fm->status, buf);
    dbg(fm, "status %s", buf);
}

static void update_toolbar(fm_app *fm)
{
    sk_widget_set_enabled(fm->tool[T_BACK], fm->nhist > 0);
    sk_widget_set_enabled(fm->tool[T_UP], strcmp(fm->cwd, "/") != 0);
    sk_widget_set_enabled(fm->tool[T_DELETE], selected(fm) != NULL);
}

/* 状态栏：项目数 + 选中项。这些字符串都是读目录时算好的，选中项改变时不做任何系统调用 */
static void update_info(fm_app *fm)
{
    char buf[800];
    int len = snprintf(buf, sizeof buf, "%d 个项目", fm->nvisible);
    if (!fm->show_hidden && fm->nhidden > 0 && len > 0 && (size_t)len < sizeof buf)
        len += snprintf(buf + len, sizeof buf - (size_t)len, "（另有 %d 个隐藏）", fm->nhidden);
    const fm_entry *e = selected(fm);
    if (e && len > 0 && (size_t)len < sizeof buf) {
        if (e->is_dir)
            snprintf(buf + len, sizeof buf - (size_t)len, "　已选中文件夹“%s”", e->name);
        else if (e->size_str[0])
            snprintf(buf + len, sizeof buf - (size_t)len, "　已选中“%s”，%s", e->name, e->size_str);
        else
            snprintf(buf + len, sizeof buf - (size_t)len, "　已选中“%s”（%s）", e->name, e->type_str);
    }
    sk_widget_set_text(fm->status, buf);
    update_toolbar(fm);
}

static void debug_dump(fm_app *fm)
{
    if (!fm->debug)
        return;
    dbg(fm, "list %s", fm->cwd);
    int count = sk_list_count(fm->list);
    for (int row = 0; row < count; row++) {
        const fm_entry *e = entry_at(fm, row);
        if (e)
            dbg(fm, "item %s\t%s\t%s", e->name, e->type_str, e->size_str);
    }
    const fm_entry *s = selected(fm);
    dbg(fm, "end %d sel=%s", count, s ? s->name : "");
}

/*
 * 把 fm->ents 填进列表。排序在读目录时已经做好了，这里只是按顺序 add。
 * 列表控件很聪明：sk_list_clear 之后再 add 的每一行都会和屏幕上原来那一行比较，
 * 内容一样就不重画——所以“刷新”一个没变化的文件夹几乎不花时间，也不闪。
 * select：要选中的项目名（NULL = 不选）；scroll：第一可见行。
 */
static void fill_list(fm_app *fm, const char *select, int scroll)
{
    sk_list_clear(fm->list);
    int sel_row = -1;
    fm->nvisible = fm->nhidden = 0;
    for (int i = 0; i < fm->n; i++) {
        const fm_entry *e = &fm->ents[i];
        if (e->hidden)
            fm->nhidden++;
        if (e->hidden && !fm->show_hidden)
            continue;
        char size[32];
        snprintf(size, sizeof size, "%s  ", e->size_str);
        const char *cells[4] = { e->name, size, e->type_str, e->time_str };
        int row = sk_list_add(fm->list, e->icon, cells, i);
        if (row < 0)
            break;                      /* 内存不够了 */
        if (select && strcmp(e->name, select) == 0)
            sel_row = row;
        fm->nvisible++;
    }
    /* 空列表时列表中间淡淡地写一行提示 */
    if (fm->n == 0)
        sk_widget_set_text(fm->list, "（这个文件夹是空的）");
    else if (fm->nvisible == 0)
        sk_widget_set_text(fm->list, "（只有隐藏文件，按 Ctrl+H 显示）");
    else
        sk_widget_set_text(fm->list, "");
    sk_list_set_scroll(fm->list, scroll);
    if (sel_row >= 0)
        sk_list_select(fm->list, sel_row);     /* 并滚动到能看见它 */
    update_info(fm);
    debug_dump(fm);
}

/* 选中某一行（-1 = 不选），并同步状态栏（sk_list_select 本身不会触发 on_change） */
static void select_row(fm_app *fm, int row)
{
    sk_list_select(fm->list, row);
    update_info(fm);
    const fm_entry *e = selected(fm);
    dbg(fm, "sel %s", e ? e->name : "");
}

/* ================================================================ 进入文件夹、刷新 */

static void update_title(fm_app *fm)
{
    char title[FM_PATH_MAX + 32];
    const char *name = strcmp(fm->cwd, "/") == 0 ? "根目录"
                     : strcmp(fm->cwd, fm->home) == 0 ? "主目录" : base_of(fm->cwd);
    snprintf(title, sizeof title, "%s - 文件", name);
    sk_window_set_title(fm->win, title);   /* 太长时库会按完整的字截断 */
}

static void update_places(fm_app *fm)
{
    int idx = -1;
    for (int i = 0; i < fm->nplaces; i++)
        if (strcmp(fm->places_tab[i].path, fm->cwd) == 0)
            idx = i;
    sk_list_select(fm->places, idx);
}

/* 用新读到的内容替换当前文件夹。ents 的所有权交给 fm */
static void set_entries(fm_app *fm, const char *dir, fm_entry *ents, int n, const char *select, int scroll)
{
    /* select 可能指向旧的 fm->ents 里的名字，释放之前先拷贝一份 */
    char keep[FM_NAME_MAX] = "";
    if (select)
        sk_strlcpy(keep, select, sizeof keep);
    int moved = strcmp(fm->cwd, dir) != 0;
    fm_free_entries(fm->ents, fm->n);
    fm->ents = ents;
    fm->n = n;
    if (moved)
        sk_strlcpy(fm->cwd, dir, sizeof fm->cwd);
    fm_sort(fm->ents, fm->n, fm->sort_col, fm->sort_desc);
    if (moved) {
        update_title(fm);
        update_places(fm);
    }
    fill_list(fm, keep[0] ? keep : NULL, scroll);
}

static void history_push(fm_app *fm, const char *dir)
{
    if (fm->nhist == HISTORY_MAX) {
        free(fm->history[0]);
        memmove(fm->history, fm->history + 1, sizeof fm->history[0] * (HISTORY_MAX - 1));
        fm->nhist--;
    }
    char *copy = strdup(dir);
    if (copy)
        fm->history[fm->nhist++] = copy;
}

/* 进入文件夹 path。push=1 时把当前文件夹记进“后退”历史。
 * 失败时什么都不改，返回错误号（不弹对话框，由调用者决定怎么提示） */
static int navigate_quiet(fm_app *fm, const char *path, int push, const char *select)
{
    char dir[FM_PATH_MAX];
    if (strlen(path) >= sizeof dir)
        return ENAMETOOLONG;
    sk_strlcpy(dir, path, sizeof dir);
    sk_path_normalize(dir);
    char sel[FM_NAME_MAX] = "";
    if (select)
        sk_strlcpy(sel, select, sizeof sel);
    fm_entry *ents;
    int n;
    int err = fm_list_dir(dir, &ents, &n);
    if (err)
        return err;
    if (push && fm->cwd[0] && strcmp(dir, fm->cwd) != 0)
        history_push(fm, fm->cwd);
    set_entries(fm, dir, ents, n, sel[0] ? sel : NULL, 0);
    sk_textbox_set_text(fm->path, fm->cwd);
    update_toolbar(fm);
    return 0;
}

static int navigate(fm_app *fm, const char *path, int push, const char *select)
{
    int err = navigate_quiet(fm, path, push, select);
    if (err) {
        char shown[FM_PATH_MAX];
        sk_strlcpy(shown, path, sizeof shown);
        error_box(fm, "无法打开文件夹“%s”：%s", shown, fm_strerror(err));
    }
    return err;
}

/* 重新读当前文件夹，保持选中项和滚动位置（select 非 NULL 时改为选中它） */
static void refresh(fm_app *fm, const char *select)
{
    if (!fm->cwd[0])
        return;
    char keep[FM_NAME_MAX] = "";
    const fm_entry *cur = selected(fm);
    if (select)
        sk_strlcpy(keep, select, sizeof keep);
    else if (cur)
        sk_strlcpy(keep, cur->name, sizeof keep);
    int scroll = sk_list_scroll(fm->list);
    fm_entry *ents;
    int n;
    int err = fm_list_dir(fm->cwd, &ents, &n);
    if (err == 0) {
        set_entries(fm, fm->cwd, ents, n, keep[0] ? keep : NULL, scroll);
        return;
    }
    /* 当前文件夹没了（被别的程序删掉了）或者不让读了：退到最近的还能打开的上级 */
    char gone[FM_PATH_MAX], dir[FM_PATH_MAX];
    sk_strlcpy(gone, fm->cwd, sizeof gone);
    sk_strlcpy(dir, fm->cwd, sizeof dir);
    while (strcmp(dir, "/") != 0) {
        char up[FM_PATH_MAX];
        parent_of(dir, up, sizeof up);
        sk_strlcpy(dir, up, sizeof dir);
        if (navigate_quiet(fm, dir, 0, NULL) == 0) {
            set_status(fm, "文件夹“%s”打不开了（%s），回到了“%s”", gone, fm_strerror(err), fm->cwd);
            return;
        }
    }
}

static void go_back(fm_app *fm)
{
    if (fm->nhist == 0)
        return;
    char *prev = fm->history[--fm->nhist];
    /* 如果是从子文件夹退回来的，选中刚才那个子文件夹，方便接着找 */
    char parent[FM_PATH_MAX], child[FM_NAME_MAX] = "";
    parent_of(fm->cwd, parent, sizeof parent);
    if (strcmp(parent, prev) == 0 && strcmp(fm->cwd, "/") != 0)
        sk_strlcpy(child, base_of(fm->cwd), sizeof child);
    navigate(fm, prev, 0, child[0] ? child : NULL);
    free(prev);
    update_toolbar(fm);
}

static void go_up(fm_app *fm)
{
    if (strcmp(fm->cwd, "/") == 0)
        return;
    char parent[FM_PATH_MAX], child[FM_NAME_MAX];
    parent_of(fm->cwd, parent, sizeof parent);
    sk_strlcpy(child, base_of(fm->cwd), sizeof child);
    navigate(fm, parent, 1, child);
}

static void go_home(fm_app *fm)
{
    if (strcmp(fm->cwd, fm->home) == 0)
        refresh(fm, NULL);
    else
        navigate(fm, fm->home, 1, NULL);
}

/* ================================================================ 打开文件 */

/*
 * 为什么不自己 fork + exec /bin/sknote？
 * 当然可以，但交给显示服务器 sketchd 去启动有几个好处：新应用是 sketchd 的子进程，
 * 文件管理器关掉了它也不受影响；sketchd 统一按 /etc/sketch/apps.conf 找到命令行、
 * 设置好 SKETCH_SOCKET 等环境变量、记录日志——和在桌面上双击图标完全一样。
 * 我们只需要发一个 SK_REQ_LAUNCH 报文：“请用 notepad 打开这个路径”。
 */
static void launch(fm_app *fm, const char *app_id, const char *arg, const char *app_label, const char *what)
{
    if (strlen(arg) > 255) {
        error_box(fm, "路径太长（超过 255 字节），没法交给%s打开：\n%s", app_label, arg);
        return;
    }
    sk_app_launch(fm->app, app_id, arg);
    dbg(fm, "launch %s %s", app_id, arg);
    set_status(fm, "已用%s打开“%s”", app_label, what);
}

/* skterm 把“不是文件夹的参数”当作一整行命令交给 sksh -c 执行。路径里有空格、引号、$ 之类
 * shell 会解释的字符时，要用单引号括起来（单引号里的字符 shell 一概不解释；
 * 路径本身含有的单引号写成 '\''：先结束引号，再写一个转义的 '，再开始新的引号）。 */
static void shell_quote(const char *s, char *out, size_t n)
{
    int safe = 1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = *p;
        if (!(c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              strchr("/._-+,:@%=", c)))
            safe = 0;
    }
    if (safe) {
        sk_strlcpy(out, s, n);
        return;
    }
    size_t o = 0;
    if (n < 3) {
        if (n)
            out[0] = 0;
        return;
    }
    out[o++] = '\'';
    for (const char *p = s; *p && o + 6 < n; p++) {
        if (*p == '\'') {
            memcpy(out + o, "'\\''", 4);
            o += 4;
        } else
            out[o++] = *p;
    }
    out[o++] = '\'';
    out[o] = 0;
}

static int looks_like_image(const char *name, const char *full, int is_link)
{
    if (fm_is_image_name(name))
        return 1;
    /* 符号链接按它指向的那个文件的名字再判断一次（link.ppm → 图片.ppm 都算） */
    char real[PATH_MAX];
    return is_link && realpath(full, real) && fm_is_image_name(base_of(real));
}

static void open_entry(fm_app *fm, const char *name_in)
{
    char name[FM_NAME_MAX], full[FM_PATH_MAX];
    sk_strlcpy(name, name_in, sizeof name);
    if (full_path(fm, name, full, sizeof full) != 0) {
        error_box(fm, "无法打开“%s”：%s", name, sk_strerror(ENAMETOOLONG));
        return;
    }
    /* 显示时用 lstat（看见“这是个链接”），决定怎么打开时用 stat（看它指向的东西） */
    struct stat st, lst;
    int is_link = lstat(full, &lst) == 0 && S_ISLNK(lst.st_mode);
    if (stat(full, &st) != 0) {
        int err = errno;
        char target[FM_PATH_MAX];
        ssize_t tl = is_link ? readlink(full, target, sizeof target - 1) : -1;
        if (tl >= 0) {
            target[tl] = 0;
            error_box(fm, "“%s”是一个失效的符号链接。\n它指向“%s”，可是%s。", name, target, fm_strerror(err));
        } else {
            error_box(fm, "无法打开“%s”：%s", name, fm_strerror(err));
        }
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        navigate(fm, full, 1, NULL);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        info_box(fm, "提示",
                 "“%s”是%s，不是普通文件。\n设备文件、管道和套接字是程序和内核、程序和程序之间"
                 "传递数据用的“特殊文件”，没有可以打开它的应用。",
                 name, fm_kind_name(st.st_mode));
        return;
    }
    if (looks_like_image(name, full, is_link)) {
        launch(fm, "paint", full, "画板", name);
        return;
    }
    if (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
        /* 可执行文件（任何一个 x 位）：在终端里运行它，这样能看见它的输出 */
        char cmd[FM_PATH_MAX * 2];
        shell_quote(full, cmd, sizeof cmd);
        if (strlen(cmd) > 255) {
            error_box(fm, "路径太长（超过 255 字节），没法交给终端运行：\n%s", full);
            return;
        }
        sk_app_launch(fm->app, "terminal", cmd);
        dbg(fm, "launch terminal %s", cmd);
        set_status(fm, "已在终端里运行“%s”", name);
        return;
    }
    int is_text;
    int err = fm_sniff_text(full, &is_text);
    if (err) {
        error_box(fm, "无法读取“%s”：%s", name, fm_strerror(err));
        return;
    }
    if (is_text)
        launch(fm, "notepad", full, "记事本", name);
    else
        info_box(fm, "提示",
                 "“%s”是一个二进制文件（开头 4 KB 里有 0 字节），既不是文本，也不是图片或可执行程序。\n"
                 "SketchOS 里还没有能打开它的应用。",
                 name);
}

static void open_terminal(fm_app *fm, const char *dir)
{
    /* 参数是文件夹时，skterm 在那里打开一个 shell（相当于先 cd 过去） */
    launch(fm, "terminal", dir, "终端", base_of(dir));
}

/* ================================================================ 复制、粘贴 */

static void do_copy(fm_app *fm, const char *name)
{
    char full[FM_PATH_MAX];
    if (full_path(fm, name, full, sizeof full) != 0) {
        error_box(fm, "无法复制“%s”：%s", name, sk_strerror(ENAMETOOLONG));
        return;
    }
    sk_strlcpy(fm->clip, full, sizeof fm->clip);
    fm->has_clip = 1;
    /* 要复制哪个文件，我们自己记着；同时把它的路径文字放进系统剪贴板，
     * 这样在终端或记事本里按 Ctrl+V 就能粘贴出这个路径 */
    sk_clipboard_set(fm->app, full, -1);
    set_status(fm, "已复制“%s”，到目标文件夹按 Ctrl+V 粘贴", name);
}

static void do_paste(fm_app *fm)
{
    if (!fm->has_clip) {
        set_status(fm, "还没有复制任何文件（先选中一个文件，按 Ctrl+C）");
        return;
    }
    char src[FM_PATH_MAX];
    sk_strlcpy(src, fm->clip, sizeof src);
    struct stat st;
    if (lstat(src, &st) != 0) {
        error_box(fm, "要粘贴的“%s”已经不存在了：%s", src, fm_strerror(errno));
        return;
    }
    const char *base = base_of(src);
    if (S_ISDIR(st.st_mode) && fm_path_inside(src, fm->cwd)) {
        error_box(fm, "无法把文件夹“%s”粘贴到这里：%s。", base, fm_strerror(FM_ERR_INSIDE));
        return;
    }
    /* 同名的已经有了（例如在同一个文件夹里复制粘贴）：起名叫“a (副本).txt” */
    char name[FM_NAME_MAX], dst[FM_PATH_MAX];
    int err = fm_copy_name(fm->cwd, base, S_ISDIR(st.st_mode), name, sizeof name);
    if (!err && full_path(fm, name, dst, sizeof dst) != 0)
        err = ENAMETOOLONG;
    if (err) {
        error_box(fm, "无法粘贴“%s”：%s", base, fm_strerror(err));
        return;
    }
    char errpath[FM_DEEP_PATH] = "";
    err = fm_copy_tree(src, dst, errpath, sizeof errpath);
    if (err) {
        /* 复制了一半：目标是我们刚建的（名字原来不存在），把它清理掉 */
        struct stat dst_st;
        char ignore[FM_DEEP_PATH];
        if (lstat(dst, &dst_st) == 0)
            fm_delete_tree(dst, dst_st.st_dev, ignore, sizeof ignore);
        refresh(fm, NULL);
        error_box(fm, "复制“%s”时出错：%s\n出错的位置：%s", base, fm_strerror(err), errpath);
        return;
    }
    refresh(fm, name);
    set_status(fm, "已粘贴为“%s”", name);
}

/* ================================================================ 新建、重命名、删除 */

/* 检查用户输入的名字；不合法时弹出说明并返回 0 */
static int valid_name(fm_app *fm, const char *s)
{
    if (!*s) {
        error_box(fm, "名字不能是空的。");
        return 0;
    }
    if (strchr(s, '/')) {
        error_box(fm, "名字里不能有“/”。\n在 Linux 里“/”是路径的分隔符，它和 0 字节是文件名里仅有的两个不允许出现的字符。");
        return 0;
    }
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
        error_box(fm, "“.”和“..”是每个文件夹里都有的特殊名字（分别指自己和上一级），不能拿来用。");
        return 0;
    }
    if (strlen(s) > 255) {
        error_box(fm, "名字太长了：%s。\n一个名字最多 255 字节（一个汉字占 3 个字节）。", sk_strerror(ENAMETOOLONG));
        return 0;
    }
    return 1;
}

static void do_new(fm_app *fm, int folder)
{
    char def[FM_NAME_MAX];
    if (fm_fresh_name(fm->cwd, folder ? "新建文件夹" : "新建文本文件", folder ? "" : ".txt", def, sizeof def) != 0)
        sk_strlcpy(def, folder ? "新建文件夹" : "新建文本文件.txt", sizeof def);
    const char *what = folder ? "文件夹" : "文件";
    char *s = input_box(fm, folder ? "新建文件夹" : "新建文本文件", folder ? "新文件夹的名字：" : "新文件的名字：", def);
    if (!s)
        return;
    char name[FM_NAME_MAX * 2], full[FM_PATH_MAX];
    trimmed(s, name, sizeof name);
    free(s);
    if (!valid_name(fm, name))
        return;
    if (full_path(fm, name, full, sizeof full) != 0) {
        error_box(fm, "无法新建%s“%s”：%s", what, name, sk_strerror(ENAMETOOLONG));
        return;
    }
    int err = 0;
    if (folder) {
        /* 0755 = 所有者可读、可写、可进入，其他人可读、可进入。
         * 实际的权限还要去掉进程 umask 里的位（umask 022 时不变） */
        if (mkdir(full, 0755) != 0)
            err = errno;
    } else {
        /* O_CREAT | O_EXCL：不存在才创建，已经存在就报 EEXIST——绝不会把别人的文件清空 */
        int fd = open(full, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0)
            err = errno;
        else if (close(fd) != 0)
            err = errno;
    }
    if (err) {
        error_box(fm, "无法新建%s“%s”：%s", what, name, fm_strerror(err));
        return;
    }
    refresh(fm, name);
    set_status(fm, "已新建%s“%s”", what, name);
}

static void do_rename(fm_app *fm, const char *name_in)
{
    char name[FM_NAME_MAX];
    sk_strlcpy(name, name_in, sizeof name);
    char *s = input_box(fm, "重命名", "新的名字：", name);
    if (!s)
        return;
    char newname[FM_NAME_MAX * 2];
    trimmed(s, newname, sizeof newname);
    free(s);
    if (strcmp(newname, name) == 0 || !valid_name(fm, newname))
        return;
    char from[FM_PATH_MAX], to[FM_PATH_MAX];
    if (full_path(fm, name, from, sizeof from) != 0 || full_path(fm, newname, to, sizeof to) != 0) {
        error_box(fm, "无法重命名“%s”：%s", name, sk_strerror(ENAMETOOLONG));
        return;
    }
    /* rename() 如果发现新名字已经存在，会**悄悄地把它替换掉**（这是 POSIX 规定的原子替换，
     * 编辑器保存文件时正需要这个特性）。重命名时用户可不想覆盖别的文件，所以先检查一下。 */
    struct stat st;
    if (lstat(to, &st) == 0) {
        error_box(fm, "无法重命名：这里已经有一个叫“%s”的%s了。", newname, S_ISDIR(st.st_mode) ? "文件夹" : "文件");
        return;
    }
    /* rename 只是改了目录里“名字 → inode”的那一行，文件的 inode 和数据都不动，
     * 所以再大的文件也是瞬间改完。但它不能跨文件系统（inode 号只在一个文件系统里有意义），
     * 那时会失败并返回 EXDEV——mv 命令遇到这种情况会退回到“复制 + 删除”。 */
    if (rename(from, to) != 0) {
        int err = errno;
        error_box(fm, "无法把“%s”重命名为“%s”：%s", name, newname, fm_strerror(err));
        return;
    }
    if (fm->has_clip && strcmp(fm->clip, from) == 0)
        sk_strlcpy(fm->clip, to, sizeof fm->clip);     /* 剪贴板里的那个文件改名了，跟着改 */
    refresh(fm, newname);
    set_status(fm, "已把“%s”重命名为“%s”", name, newname);
}

static void do_delete(fm_app *fm, const char *name_in)
{
    char name[FM_NAME_MAX], full[FM_PATH_MAX];
    sk_strlcpy(name, name_in, sizeof name);
    if (full_path(fm, name, full, sizeof full) != 0) {
        error_box(fm, "无法删除“%s”：%s", name, sk_strerror(ENAMETOOLONG));
        return;
    }
    struct stat lst;
    if (lstat(full, &lst) != 0) {
        error_box(fm, "无法删除“%s”：%s", name, fm_strerror(errno));
        refresh(fm, NULL);
        return;
    }
    /* lstat：指向文件夹的符号链接只是一个链接，删掉它不会动到目标文件夹 */
    int is_dir = S_ISDIR(lst.st_mode);
    int r = is_dir ? ask(fm, "确认删除", "删除|取消",
                         "确定要删除文件夹“%s”以及里面的所有内容吗？\n删除后不能恢复（SketchOS 没有回收站）。", name)
                   : ask(fm, "确认删除", "删除|取消",
                         "确定要删除“%s”吗？\n删除后不能恢复（SketchOS 没有回收站）。", name);
    if (r != 0)
        return;
    int row = sk_list_selected(fm->list);
    char errpath[FM_DEEP_PATH];
    sk_strlcpy(errpath, full, sizeof errpath);
    int err = 0;
    if (is_dir) {
        struct stat pst;
        if (stat(fm->cwd, &pst) != 0)
            err = errno;
        else
            err = fm_delete_tree(full, pst.st_dev, errpath, sizeof errpath);
    } else if (unlink(full) != 0) {
        err = errno;
    }
    refresh(fm, NULL);
    if (err) {
        error_box(fm, "删除“%s”失败：%s\n出错的位置：%s", name, fm_strerror(err), errpath);
        return;
    }
    /* 选中原来位置上的下一项，连续按 Delete 可以一个个删下去 */
    int count = sk_list_count(fm->list);
    if (row >= 0 && count > 0)
        select_row(fm, row < count ? row : count - 1);
    set_status(fm, "已删除“%s”", name);
}

/* ================================================================ 属性 */

/*
 * 什么是 inode？
 * 每个文件在文件系统里都有一个“索引节点”（inode）：它记录文件的类型、权限、所有者、大小、
 * 时间戳，以及数据存放在哪里——但是**不记录文件名**。文件名记在目录里：目录的内容就是
 * 一张“名字 → inode 号”的表。所以：
 *   - 同一个 inode 可以有好几个名字（硬链接，ln a b），“硬链接数”就是有几个目录项指向它；
 *     文件夹的硬链接数 = 2 + 子文件夹个数（它自己的名字、它里面的“.”、每个子文件夹里的“..”）；
 *   - 重命名只改目录里的一行，inode 不变；
 *   - 删除（unlink）删的是名字，硬链接数变成 0 并且没有进程打开它时，内核才释放 inode 和数据。
 * stat() 系统调用读到的 struct stat，就是 inode 里的这些信息。
 */
static void show_properties(fm_app *fm, const char *name_in)
{
    char name[FM_NAME_MAX], full[FM_PATH_MAX];
    if (name_in) {
        sk_strlcpy(name, name_in, sizeof name);
        if (full_path(fm, name, full, sizeof full) != 0) {
            error_box(fm, "无法查看“%s”：%s", name, sk_strerror(ENAMETOOLONG));
            return;
        }
    } else {
        sk_strlcpy(full, fm->cwd, sizeof full);
        sk_strlcpy(name, strcmp(full, "/") == 0 ? "/" : base_of(full), sizeof name);
    }
    struct stat st;
    if (lstat(full, &st) != 0) {
        error_box(fm, "无法查看“%s”：%s", name, fm_strerror(errno));
        return;
    }
    char where[FM_PATH_MAX], perm[12], size[32], when[32] = "", user[64], group[64];
    parent_of(full, where, sizeof where);
    fm_mode_string(st.st_mode, perm);
    sk_fmt_size(size, sizeof size, (uint64_t)st.st_size);
    struct tm tm;
    if (localtime_r(&st.st_mtime, &tm))
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    sk_user_name(st.st_uid, user, sizeof user);
    sk_group_name(st.st_gid, group, sizeof group);

    char text[2600];
    size_t len = 0;
#define ADD(...)                                                                 \
    do {                                                                         \
        if (len < sizeof text) {                                                 \
            int k_ = snprintf(text + len, sizeof text - len, __VA_ARGS__);       \
            if (k_ > 0)                                                          \
                len += (size_t)k_;                                               \
        }                                                                        \
    } while (0)

    ADD("名称：%s\n位置：%s\n类型：%s", name, where, fm_kind_name(st.st_mode));
    if (name_in && S_ISREG(st.st_mode)) {
        int row = row_of(fm, name);
        const fm_entry *e = entry_at(fm, row);
        if (e && e->type_str[0])
            ADD("（%s）", e->type_str);
    }
    if (S_ISLNK(st.st_mode)) {
        /* 符号链接也是一个 inode，它的“内容”就是目标路径这串文字 */
        char target[FM_PATH_MAX];
        ssize_t tl = readlink(full, target, sizeof target - 1);
        if (tl >= 0) {
            target[tl] = 0;
            struct stat ts;
            if (stat(full, &ts) == 0)
                ADD("\n链接指向：%s（%s）", target, fm_kind_name(ts.st_mode));
            else
                ADD("\n链接指向：%s（目标不存在，这是一个失效的链接）", target);
        }
    }
    if (S_ISDIR(st.st_mode))
        ADD("\n大小：%s（只是目录表本身，不含里面的文件）", size);
    else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
        ADD("\n大小：%s（%lld 字节）", size, (long long)st.st_size);
    if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
        ADD("\n设备号：主 %u，次 %u（内核用它找到对应的驱动）", major(st.st_rdev), minor(st.st_rdev));
    ADD("\n权限：%s（八进制 %04o）", perm, (unsigned)(st.st_mode & 07777));
    if (S_ISLNK(st.st_mode))
        ADD("——链接自己的权限没有意义，访问时检查的是目标的权限");
    ADD("\n所有者：%s（uid %u），组：%s（gid %u）", user, (unsigned)st.st_uid, group, (unsigned)st.st_gid);
    ADD("\n修改时间：%s", when);
    ADD("\ninode 号：%llu，硬链接数：%lu", (unsigned long long)st.st_ino, (unsigned long)st.st_nlink);
#undef ADD
    info_box(fm, "详细信息", "%s", text);
}

/* ================================================================ 隐藏文件、排序 */

/* Unix 里“隐藏”并不是文件的一个属性，只是一个约定：名字以“.”开头的文件，ls 和
 * 文件管理器默认不显示。（据说起因是早期的 ls 为了跳过“.”和“..”只检查了第一个字符，
 * 结果所有以点开头的文件都被“藏”起来了，大家觉得好用，就成了放配置文件的惯例。） */
static void set_show_hidden(fm_app *fm, int on)
{
    sk_widget_set_checked(fm->hidden_chk, on);
    if (fm->show_hidden == on)
        return;
    fm->show_hidden = on;
    const fm_entry *e = selected(fm);
    char keep[FM_NAME_MAX] = "";
    if (e)
        sk_strlcpy(keep, e->name, sizeof keep);
    fill_list(fm, keep[0] ? keep : NULL, sk_list_scroll(fm->list));
    set_status(fm, on ? "显示隐藏文件（名字以“.”开头的文件）" : "不显示隐藏文件");
}

static void on_hidden_check(sk_widget *w, void *user)
{
    set_show_hidden(user, sk_widget_checked(w));
}

static void on_sort(sk_widget *w, int col, void *user)
{
    (void)w;
    fm_app *fm = user;
    /* 和列表表头小三角的规则一致：第一次点某列 = 升序，再点同一列 = 反过来 */
    if (fm->sort_col == col)
        fm->sort_desc = !fm->sort_desc;
    else {
        fm->sort_col = col;
        fm->sort_desc = 0;
    }
    const fm_entry *e = selected(fm);
    char keep[FM_NAME_MAX] = "";
    if (e)
        sk_strlcpy(keep, e->name, sizeof keep);
    /* 只在点表头时排一次序；重画、选中、滚动都不会重新排序 */
    fm_sort(fm->ents, fm->n, fm->sort_col, fm->sort_desc);
    fill_list(fm, keep[0] ? keep : NULL, sk_list_scroll(fm->list));
}

/* ================================================================ 动作分发 */

static void do_action(fm_app *fm, int act, const char *name)
{
    char dir[FM_PATH_MAX];
    switch (act) {
    case ACT_OPEN:          open_entry(fm, name); break;
    case ACT_TERMINAL:
        if (full_path(fm, name, dir, sizeof dir) == 0)
            open_terminal(fm, dir);
        break;
    case ACT_TERMINAL_HERE: open_terminal(fm, fm->cwd); break;
    case ACT_COPY:          do_copy(fm, name); break;
    case ACT_PASTE:         do_paste(fm); break;
    case ACT_RENAME:        do_rename(fm, name); break;
    case ACT_DELETE:        do_delete(fm, name); break;
    case ACT_NEW_FOLDER:    do_new(fm, 1); break;
    case ACT_NEW_FILE:      do_new(fm, 0); break;
    case ACT_PROPS:         show_properties(fm, name); break;
    case ACT_PROPS_DIR:     show_properties(fm, NULL); break;
    case ACT_REFRESH:       refresh(fm, NULL); break;
    case ACT_HIDDEN:        set_show_hidden(fm, !fm->show_hidden); break;
    default: break;
    }
}

/* 对选中项执行动作；没有选中时在状态栏提示 */
static void act_on_selected(fm_app *fm, int act, const char *hint)
{
    const fm_entry *e = selected(fm);
    if (!e) {
        set_status(fm, "%s", hint);
        return;
    }
    char name[FM_NAME_MAX];
    sk_strlcpy(name, e->name, sizeof name);
    do_action(fm, act, name);
}

/* ================================================================ 右键菜单 */

static void on_menu_pick(sk_ui *ui, int index, void *user)
{
    (void)ui;
    fm_app *fm = user;
    if (index < 0 || index >= MENU_MAX)
        return;
    char name[FM_NAME_MAX];
    sk_strlcpy(name, fm->menu_target, sizeof name);
    do_action(fm, fm->menu_act[index], name);
}

/* 文件列表里第一行的 y 坐标和行高。列表控件没有提供“坐标 ↔ 行”的接口，这里按 ui_list.c 的
 * 布局公式自己算：外框 3 像素，表头高 max(26, 字体行高 + 10)，每行 max(ROW_H, 字体行高 + 2) */
static void list_metrics(fm_app *fm, int *body_y, int *row_h)
{
    int lh = sk_font_line_height(sk_font_get(fm->list->font));
    *body_y = fm->list->r.y + 3 + SK_MAX(26, lh + 10);
    *row_h = SK_MAX(ROW_H, lh + 2);
}

/* 鼠标在文件列表的哪一行？返回行号；-1 = 列表里的空白处；-2 = 表头、滚动条（右边 12 像素）或者列表外面 */
static int list_row_at(fm_app *fm, int x, int y)
{
    sk_rect r = fm->list->r;
    if (!sk_rect_contains(r, x, y))
        return -2;
    int body_y, row_h;
    list_metrics(fm, &body_y, &row_h);
    int bar_x = r.x + r.w - 3 - 12 - 1 - 2;
    if (y < body_y || y >= r.y + r.h - 3 || x >= bar_x)
        return -2;
    int row = sk_list_scroll(fm->list) + (y - body_y) / row_h;
    return row < sk_list_count(fm->list) ? row : -1;
}

/* 弹出右键菜单：row 是针对的那一行（-1 = 空白处，针对当前文件夹），(x, y) 是菜单的位置 */
static void popup_menu(fm_app *fm, int row, int x, int y)
{
    const char *items[MENU_MAX];
    int n = 0;
#define ITEM(label, act)                \
    do {                                \
        if (n < MENU_MAX) {             \
            items[n] = (label);         \
            fm->menu_act[n++] = (act);  \
        }                               \
    } while (0)
    sk_ui_focus(fm->ui, fm->list);
    const fm_entry *e = entry_at(fm, row);
    if (e) {
        /* 右键点在某一项上：先选中它（列表控件只对左键做选中），菜单针对这一项 */
        select_row(fm, row);
        sk_strlcpy(fm->menu_target, e->name, sizeof fm->menu_target);
        ITEM("打开", ACT_OPEN);
        if (e->is_dir)
            ITEM("在终端中打开", ACT_TERMINAL);
        ITEM("-", ACT_NONE);
        ITEM("复制", ACT_COPY);
        if (fm->has_clip)
            ITEM("粘贴", ACT_PASTE);
        ITEM("重命名", ACT_RENAME);
        ITEM("删除", ACT_DELETE);
        ITEM("-", ACT_NONE);
        ITEM("属性", ACT_PROPS);
    } else {
        /* 空白处：针对当前文件夹 */
        fm->menu_target[0] = 0;
        if (fm->has_clip)
            ITEM("粘贴", ACT_PASTE);
        ITEM("新建文件夹", ACT_NEW_FOLDER);
        ITEM("新建文本文件", ACT_NEW_FILE);
        ITEM("-", ACT_NONE);
        ITEM("在此处打开终端", ACT_TERMINAL_HERE);
        ITEM("刷新", ACT_REFRESH);
        ITEM(fm->show_hidden ? "不显示隐藏文件" : "显示隐藏文件", ACT_HIDDEN);
        ITEM("-", ACT_NONE);
        ITEM("属性", ACT_PROPS_DIR);
    }
#undef ITEM
    sk_menu_popup(fm->ui, x, y, items, n, on_menu_pick, fm);
}

/* 键盘上的菜单键或 Shift+F10：在选中的那一行旁边弹出菜单（没有选中时针对当前文件夹） */
static void keyboard_menu(fm_app *fm)
{
    sk_rect r = fm->list->r;
    int body_y, row_h;
    list_metrics(fm, &body_y, &row_h);
    int row = sk_list_selected(fm->list);
    int y = row >= 0 ? body_y + (row - sk_list_scroll(fm->list)) * row_h + row_h - 4 : body_y + 4;
    /* 选中的行可能被滚轮滚出了视野：菜单至少要出现在列表里面 */
    y = SK_CLAMP(y, body_y + 4, r.y + r.h - 8);
    popup_menu(fm, row, r.x + 60, y);
}

/* ================================================================ 控件回调 */

static void on_list_change(sk_widget *w, void *user)
{
    (void)w;
    fm_app *fm = user;
    update_info(fm);
    const fm_entry *e = selected(fm);
    dbg(fm, "sel %s", e ? e->name : "");
}

/* 双击或回车 */
static void on_list_activate(sk_widget *w, void *user)
{
    (void)w;
    act_on_selected(user, ACT_OPEN, "");
}

static void on_place(sk_widget *w, void *user)
{
    fm_app *fm = user;
    int row = sk_list_selected(w);
    if (row < 0 || row >= fm->nplaces)
        return;
    char path[FM_PATH_MAX];
    sk_strlcpy(path, fm->places_tab[row].path, sizeof path);
    if (strcmp(path, fm->cwd) != 0 && navigate(fm, path, 1, NULL) != 0)
        update_places(fm);                   /* 没进去：左边的选中恢复成当前文件夹 */
    sk_ui_focus(fm->ui, fm->list);
}

/* 地址栏里按回车：可以输入绝对路径、相对路径（相对于当前文件夹）或者 ~ 开头的路径 */
static void on_path_enter(sk_widget *w, void *user)
{
    fm_app *fm = user;
    char text[FM_PATH_MAX], full[FM_PATH_MAX * 2];
    trimmed(sk_textbox_text(w), text, sizeof text);
    if (!text[0]) {
        sk_textbox_set_text(w, fm->cwd);
        return;
    }
    if (text[0] == '~' && (text[1] == 0 || text[1] == '/'))
        snprintf(full, sizeof full, "%s%s", fm->home, text + 1);
    else if (text[0] == '/')
        sk_strlcpy(full, text, sizeof full);
    else
        snprintf(full, sizeof full, "%s/%s", fm->cwd, text);
    if (strlen(full) >= FM_PATH_MAX) {
        error_box(fm, "路径太长了：%s", sk_strerror(ENAMETOOLONG));
        return;
    }
    sk_path_normalize(full);
    struct stat st;
    if (stat(full, &st) != 0) {
        int err = errno;
        error_box(fm, "找不到“%s”：%s", full, fm_strerror(err));
        sk_ui_focus(fm->ui, fm->path);
        return;
    }
    int ok;
    if (S_ISDIR(st.st_mode)) {
        ok = navigate(fm, full, 1, NULL) == 0;
    } else {
        /* 输入的是一个文件：打开它所在的文件夹并选中它 */
        char parent[FM_PATH_MAX], name[FM_NAME_MAX];
        parent_of(full, parent, sizeof parent);
        sk_strlcpy(name, base_of(full), sizeof name);
        ok = navigate(fm, parent, 1, name) == 0;
    }
    if (ok)
        sk_ui_focus(fm->ui, fm->list);
}

static void on_tool(sk_widget *w, void *user)
{
    fm_app *fm = user;
    switch (w->id) {
    case T_BACK:    go_back(fm); break;
    case T_UP:      go_up(fm); break;
    case T_HOME:    go_home(fm); break;
    case T_REFRESH: refresh(fm, NULL); break;
    case T_NEWDIR:  do_new(fm, 1); break;
    case T_DELETE:  act_on_selected(fm, ACT_DELETE, "先选中要删除的文件或文件夹"); break;
    default: break;
    }
}

/* ================================================================ 键盘和窗口事件 */

static int on_key(fm_app *fm, const sk_event *e)
{
    uint32_t kc = e->key.keycode;
    uint32_t mods = e->key.modifiers & (SK_MOD_CTRL | SK_MOD_ALT | SK_MOD_SUPER | SK_MOD_SHIFT);
    if (mods == SK_MOD_CTRL) {
        switch (kc) {
        case KEY_H: set_show_hidden(fm, !fm->show_hidden); return 1;
        case KEY_C: act_on_selected(fm, ACT_COPY, "先选中要复制的文件或文件夹"); return 1;
        case KEY_V: do_paste(fm); return 1;
        case KEY_R: refresh(fm, NULL); return 1;
        case KEY_L: sk_ui_focus(fm->ui, fm->path); return 1;
        default: return 0;
        }
    }
    if (mods == SK_MOD_ALT) {
        switch (kc) {
        case KEY_LEFT: go_back(fm); return 1;
        case KEY_UP:   go_up(fm); return 1;
        case KEY_HOME: go_home(fm); return 1;
        case KEY_ENTER:
        case KEY_KPENTER:
            if (selected(fm))
                act_on_selected(fm, ACT_PROPS, "");
            else
                show_properties(fm, NULL);
            return 1;
        default: return 0;
        }
    }
    if (mods == SK_MOD_SHIFT && kc == KEY_F10) {
        keyboard_menu(fm);
        return 1;
    }
    if (mods & ~SK_MOD_SHIFT)
        return 0;
    /* 走到这里的按键，焦点控件都没有处理（例如地址栏自己会处理退格和 Delete） */
    switch (kc) {
    case KEY_DELETE:    act_on_selected(fm, ACT_DELETE, "先选中要删除的文件或文件夹"); return 1;
    case KEY_F2:        act_on_selected(fm, ACT_RENAME, "先选中要重命名的文件或文件夹"); return 1;
    case KEY_F5:        refresh(fm, NULL); return 1;
    case KEY_COMPOSE:                               /* 键盘右下角的“菜单键” */
    case KEY_MENU:      keyboard_menu(fm); return 1;
    case KEY_BACKSPACE: go_back(fm); return 1;
    case KEY_ESC:
        if (sk_ui_focused(fm->ui) == fm->path) {
            sk_textbox_set_text(fm->path, fm->cwd);     /* 放弃输入到一半的地址 */
            sk_ui_focus(fm->ui, fm->list);
        }
        return 1;
    default:
        return 0;
    }
}

static int on_event(sk_window *win, const sk_event *e, void *user)
{
    (void)win;
    fm_app *fm = user;
    switch (e->type) {
    case SK_EVENT_MOUSE:
        if (e->mouse.kind == SK_MOUSE_DOWN && e->mouse.button == SK_BUTTON_RIGHT) {
            int row = list_row_at(fm, e->mouse.x, e->mouse.y);
            if (row != -2)
                popup_menu(fm, row, e->mouse.x, e->mouse.y);
            return 1;
        }
        return 0;
    case SK_EVENT_KEY:
        if (e->key.state == 0)
            return 0;
        return on_key(fm, e);
    case SK_EVENT_FOCUS:
        /* 窗口重新获得焦点：用户可能刚在终端里建了文件、在记事本里改了文件，刷新一下。
         * 内容没变时列表不会重画（见 fill_list），所以这很便宜。 */
        if (e->focused) {
            if (fm->dialog_depth > 0)
                fm->refresh_pending = 1;
            else
                refresh(fm, NULL);
        }
        return 0;
    default:
        return 0;
    }
}

/* ================================================================ 布局和背景 */

static int tool_label_width(int i)
{
    return 28 + 6 + sk_text_width(sk_font_get(SK_FONT_UI), tools[i].label) + 22;
}

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    fm_app *fm = user;
    const sk_font *f = sk_font_get(SK_FONT_UI);
    int chk_w = 26 + 8 + sk_text_width(f, "显示隐藏文件") + 8;

    /* 工具栏带文字放得下吗？放不下就只显示图标 */
    int need = 2 * PAD + 18 + 24 + chk_w;
    for (int i = 0; i < T_COUNT; i++)
        need += tool_label_width(i) + TOOL_GAP;
    int labels = need <= W;
    if (labels != fm->labels) {
        fm->labels = labels;
        for (int i = 0; i < T_COUNT; i++)
            sk_widget_set_text(fm->tool[i], labels ? tools[i].label : "");
    }
    int x = PAD;
    for (int i = 0; i < T_COUNT; i++) {
        if (i == T_NEWDIR) {
            fm->sep_x = x + 7;
            x += 18;
        }
        int w = labels ? tool_label_width(i) : 42;
        sk_widget_set_rect(fm->tool[i], sk_rect_make(x, TOOL_Y, w, TOOL_H));
        x += w + TOOL_GAP;
    }
    sk_widget_set_rect(fm->hidden_chk, sk_rect_make(W - PAD - chk_w, TOOL_Y + 5, chk_w, 28));
    sk_widget_set_rect(fm->path, sk_rect_make(PAD, PATH_Y, W - 2 * PAD, PATH_H));

    int body_h = H - STATUS_H - 4 - BODY_Y;
    int list_x = PAD;
    int show_places = W >= PLACES_MIN_W && fm->nplaces > 0;
    sk_widget_set_visible(fm->places, show_places);
    if (show_places) {
        sk_widget_set_rect(fm->places, sk_rect_make(PAD, BODY_Y, PLACES_W, body_h));
        list_x += PLACES_W + 8;
    }
    sk_widget_set_rect(fm->list, sk_rect_make(list_x, BODY_Y, W - PAD - list_x, body_h));
    sk_widget_set_rect(fm->status, sk_rect_make(PAD + 2, H - STATUS_H + 1, W - 2 * PAD - 4, STATUS_H - 4));

    if (fm->debug) {
        dbg(fm, "geom window 0 0 %d %d", W, H);
        for (int i = 0; i < T_COUNT; i++)
            dbg_geom(fm, tools[i].debug_name, fm->tool[i]);
        dbg_geom(fm, "hidden", fm->hidden_chk);
        dbg_geom(fm, "path", fm->path);
        if (show_places)
            dbg_geom(fm, "places", fm->places);
        dbg_geom(fm, "list", fm->list);
        dbg_geom(fm, "status", fm->status);
    }
}

/* 窗口背景：上面的工具栏是一条颜色深一点的纸（SK_PAPER_DARK），下面是普通的纸；
 * 加上几条手绘的分隔线。只画 dirty 这一块——局部重绘时其余部分保持原样。 */
static void background(sk_ui *ui, sk_canvas *c, sk_rect dirty, void *user)
{
    (void)ui;
    fm_app *fm = user;
    int W = sk_window_width(fm->win), H = sk_window_height(fm->win);
    sk_rect top = sk_rect_intersect(dirty, sk_rect_make(0, 0, W, BAND_H));
    sk_rect rest = sk_rect_intersect(dirty, sk_rect_make(0, BAND_H, W, H - BAND_H));
    if (!sk_rect_empty(top))
        sk_paper_fill(c, top, SK_PAPER_DARK, 3, 0xF11E5u);
    if (!sk_rect_empty(rest))
        sk_paper_fill(c, rest, SK_PAPER, 3, 0xF11E5u);
    if (dirty.y <= BAND_H + 4 && dirty.y + dirty.h >= BAND_H - 4)
        sk_draw_separator(c, 2, W - 2, BAND_H, 0x7001u);
    int sy = H - STATUS_H - 2;
    if (dirty.y <= sy + 4 && dirty.y + dirty.h >= sy - 4)
        sk_draw_separator(c, PAD, W - PAD, sy, 0x57A7u);
    if (fm->sep_x > 0 && !sk_rect_empty(sk_rect_intersect(dirty, sk_rect_make(fm->sep_x - 4, TOOL_Y, 8, TOOL_H)))) {
        sk_rough_style s;
        sk_style_pencil(&s, 0x5E9Au);
        s.stroke = SK_INK_FAINT;
        s.stroke_width = 1.2f;
        s.double_stroke = 0;
        s.roughness = 0.5f;
        sk_rough_line(c, (float)fm->sep_x, (float)(TOOL_Y + 7), (float)fm->sep_x, (float)(TOOL_Y + TOOL_H - 7), &s);
    }
}

/* ================================================================ 启动 */

static void add_place(fm_app *fm, const char *label, int icon, const char *path)
{
    struct stat st;
    if (fm->nplaces >= PLACES_MAX || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return;                     /* 不存在的就不列出来 */
    place *p = &fm->places_tab[fm->nplaces++];
    p->label = label;
    p->icon = icon;
    sk_strlcpy(p->path, path, sizeof p->path);
    sk_path_normalize(p->path);
    const char *cells[1] = { label };
    sk_list_add(fm->places, icon, cells, fm->nplaces - 1);
}

static void setup_places(fm_app *fm)
{
    char path[FM_PATH_MAX];
    add_place(fm, "主目录", SK_ICON_HOME, fm->home);
    static const struct { const char *name; int icon; } sub[] = {
        { "文档", SK_ICON_BOOK }, { "图片", SK_ICON_IMAGE }, { "示例", SK_ICON_TERMINAL },
    };
    for (size_t i = 0; i < SK_ARRAY_LEN(sub); i++) {
        sk_path_join(path, sizeof path, fm->home, sub[i].name);
        add_place(fm, sub[i].name, sub[i].icon, path);
    }
    add_place(fm, "根目录 /", SK_ICON_FOLDER, "/");
    /* /proc 不是磁盘上的文件，而是内核“伪装成文件”的运行状态：每个进程一个文件夹 */
    add_place(fm, "进程 /proc", SK_ICON_KERNEL, "/proc");
}

/* 命令行参数 → 起始文件夹（和要选中的文件）。失败时返回错误号 */
static int start_location(fm_app *fm, const char *arg, char *dir, size_t dn, char *sel, size_t sn)
{
    sel[0] = 0;
    sk_strlcpy(dir, fm->home, dn);
    if (!arg || !*arg)
        return 0;
    char full[FM_PATH_MAX * 2];
    if (arg[0] == '/') {
        sk_strlcpy(full, arg, sizeof full);
    } else {
        /* 相对路径：相对于进程的当前工作目录（每个进程都有一个，记在内核的进程描述符里） */
        char cwd[FM_PATH_MAX];
        if (!getcwd(cwd, sizeof cwd))
            return errno;
        snprintf(full, sizeof full, "%s/%s", cwd, arg);
    }
    if (strlen(full) >= FM_PATH_MAX)
        return ENAMETOOLONG;
    sk_path_normalize(full);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        sk_strlcpy(dir, full, dn);
        return 0;
    }
    if (lstat(full, &st) != 0)
        return errno;
    /* 是一个文件：打开它所在的文件夹并选中它 */
    parent_of(full, dir, dn);
    sk_strlcpy(sel, base_of(full), sn);
    return 0;
}

int main(int argc, char **argv)
{
    static fm_app fm;               /* 结构体比较大（历史、路径缓冲区），放在静态区 */
    const char *arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("用法: skfiles [文件夹或文件]\n"
                   "  打开文件夹；参数是文件时打开它所在的文件夹并选中它；没有参数时打开 $HOME。\n"
                   "  环境变量 SKFILES_DEBUG=1：把列表内容等状态打印到标准输出（测试用）。\n");
            return 0;
        }
        if (arg) {
            fprintf(stderr, "skfiles: 只接受一个参数（文件夹或文件）\n");
            return 2;
        }
        arg = argv[i];
    }
    const char *dbgenv = getenv("SKFILES_DEBUG");
    fm.debug = dbgenv && *dbgenv && strcmp(dbgenv, "0") != 0;
    const char *home = getenv("HOME");
    sk_strlcpy(fm.home, home && home[0] == '/' ? home : "/root", sizeof fm.home);
    sk_path_normalize(fm.home);
    fm.sort_col = -1;

    fm.app = sk_app_connect("skfiles");
    if (!fm.app)
        return 1;
    fm.win = sk_window_create(fm.app, "文件", 860, 560, SK_WIN_RESIZABLE | SK_WIN_CENTER, SK_ORANGE);
    if (!fm.win) {
        sk_app_disconnect(fm.app);
        return 1;
    }
    sk_window_set_min_size(fm.win, 600, 400);
    fm.ui = sk_ui_new(fm.win);
    if (!fm.ui) {
        sk_app_disconnect(fm.app);
        return 1;
    }

    /* 控件先随便放，真正的位置在 layout() 里算 */
    sk_rect z = sk_rect_make(0, 0, 10, 10);
    for (int i = 0; i < T_COUNT; i++) {
        fm.tool[i] = sk_icon_button_new(fm.ui, z, tools[i].icon, tools[i].label, on_tool, &fm);
        if (!fm.tool[i])
            return 1;
        fm.tool[i]->id = i;
    }
    sk_widget_set_accent(fm.tool[T_DELETE], SK_RED);
    sk_widget_set_accent(fm.tool[T_NEWDIR], SK_ORANGE);
    fm.labels = 1;
    fm.hidden_chk = sk_checkbox_new(fm.ui, z, "显示隐藏文件", 0, on_hidden_check, &fm);
    fm.path = sk_textbox_new(fm.ui, z, "", NULL, &fm);
    fm.places = sk_list_new(fm.ui, z, on_place, NULL, &fm);
    fm.list = sk_list_new(fm.ui, z, on_list_change, on_list_activate, &fm);
    fm.status = sk_label_new(fm.ui, z, "", SK_FONT_UI, SK_ALIGN_LEFT);
    if (!fm.hidden_chk || !fm.path || !fm.places || !fm.list || !fm.status)
        return 1;
    fm.path->on_click = on_path_enter;           /* 地址栏里按回车 */
    sk_textbox_set_placeholder(fm.path, "输入路径，按回车前往");
    sk_list_set_row_height(fm.places, PLACE_ROW_H);
    sk_list_set_columns(fm.list, col_titles, col_widths, 4);
    sk_list_set_row_height(fm.list, ROW_H);
    sk_list_set_sort_handler(fm.list, on_sort);
    sk_label_set_color(fm.status, SK_INK_SOFT);
    setup_places(&fm);

    sk_ui_set_background(fm.ui, background, &fm);
    sk_ui_set_layout(fm.ui, layout, &fm);
    sk_ui_set_handler(fm.ui, on_event, &fm);

    char dir[FM_PATH_MAX], sel[FM_NAME_MAX];
    int start_err = start_location(&fm, arg, dir, sizeof dir, sel, sizeof sel);
    int err = navigate_quiet(&fm, dir, 0, sel[0] ? sel : NULL);
    if (err && strcmp(dir, fm.home) != 0)
        err = navigate_quiet(&fm, fm.home, 0, NULL);
    if (err)
        navigate_quiet(&fm, "/", 0, NULL);
    sk_ui_focus(fm.ui, fm.list);
    if (start_err) {
        /* 先把主窗口画出来，再弹出说明 */
        sk_ui_redraw_now(fm.ui);
        error_box(&fm, "找不到“%s”：%s\n已打开主目录。", arg, fm_strerror(start_err));
    }

    int rc = sk_app_run(fm.app);
    sk_ui_free(fm.ui);
    sk_app_disconnect(fm.app);
    fm_free_entries(fm.ents, fm.n);
    for (int i = 0; i < fm.nhist; i++)
        free(fm.history[i]);
    return rc;
}
