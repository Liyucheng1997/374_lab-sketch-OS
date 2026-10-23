/*
 * wm.c — 窗口管理器：窗口的层叠顺序、焦点、装饰（边框/标题栏）、移动/缩放/最大化/最小化，
 *        以及鼠标键盘事件的分发和全局快捷键
 *
 * 几个概念：
 *   层叠顺序 (z-order)：窗口像一叠纸，g_z[0] 在最底下，g_z[g_nz-1] 在最上面。
 *       点击窗口把它“提到最上面”(raise)。合成时从底往上画。
 *   焦点 (focus)：键盘事件发给谁。点击窗口 = 提到最上面 + 获得焦点（click-to-focus）。
 *   服务器端装饰 (server-side decoration)：边框、标题栏、关闭按钮由 sketchd 画，
 *       应用只画内容区。好处是所有窗口风格统一、应用卡死时照样能拖动/关闭它。
 *   隐式抓取 (implicit grab)：在窗口里按下鼠标后，直到松开为止，所有鼠标事件都发给它，
 *       即使指针已经移出了窗口——这样在画板里拖出画布边缘也不会“断笔”。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 鼠标状态机 */
typedef enum {
    P_IDLE = 0,     /* 没有按钮按下 */
    P_CONTENT,      /* 在窗口内容区按下：隐式抓取，事件都发给 g_grab */
    P_MOVE,         /* 拖动标题栏 */
    P_RESIZE,       /* 拖动右下角/右边/下边改变大小（显示虚线框） */
    P_BUTTON,       /* 按住标题栏上的按钮（关闭/最大化/最小化），松开时才生效 */
    P_MENU,         /* 按在弹出菜单上 */
    P_TASKBAR,      /* 按在任务栏上 */
    P_SWALLOW,      /* 吞掉这次按下对应的松开（桌面、边框、双击标题栏……） */
} pstate;

enum { HIT_NONE = 0, HIT_CONTENT, HIT_TITLE, HIT_BUTTON, HIT_RESIZE, HIT_BORDER };
enum { RS_RIGHT = 1, RS_BOTTOM = 2 };

#define RESIZE_GRIP 14       /* 右下角“抓手”的大小 */
#define MOVE_THRESHOLD 3     /* 按下后移动超过这么多像素才开始拖动（避免双击时抖动） */

static skd_window *g_z[SKD_MAX_WINDOWS];
static int         g_nz;
static uint32_t    g_next_id = 1;
static uint32_t    g_next_order = 1;
static int         g_cascade;
static skd_window *g_focus;

static int         g_px, g_py;
static uint32_t    g_btns;
static pstate      g_ps;
static skd_window *g_grab;
static int         g_grab_btn;           /* P_BUTTON：按下的是哪个标题栏按钮 */
static int         g_drag_dx, g_drag_dy; /* P_MOVE：指针相对内容区左上角的偏移 */
static int         g_drag_started;
static int         g_press_x, g_press_y;
static sk_rect     g_rs_rect;            /* P_RESIZE：虚线框代表的新内容区 */
static int         g_rs_mode, g_rs_w0, g_rs_h0;
static skd_window *g_hover;              /* 指针在其内容区内的窗口（ENTER/LEAVE 用） */
static skd_window *g_hover_frame;        /* 指针悬停在其标题栏按钮上的窗口 */
static int         g_hover_btn = -1;

static uint64_t    g_click_ms;
static int         g_click_x, g_click_y, g_click_n;
static uint32_t    g_click_btn;

/* Alt+Tab 切换器 */
static int         g_switching;
static skd_window *g_sw[SKD_MAX_WINDOWS];
static int         g_sw_n, g_sw_sel;

/* 被全局快捷键“吃掉”的键：它们的松开（和自动重复）也不能发给应用 */
static uint8_t     g_swallow[(KEY_MAX + 1) / 8 + 1];
static int         g_super_alone;

static void update_hover(int x, int y);
static void focus_topmost(void);

/* ====================================================================== */
/*                               几何                                      */
/* ====================================================================== */

static int is_visible(const skd_window *w) { return w->mapped && !w->minimized; }
static int has_decor(const skd_window *w) { return !(w->flags & SK_WIN_NO_DECOR); }
static int rect_right(sk_rect r) { return r.x + r.w; }
static int rect_bottom(sk_rect r) { return r.y + r.h; }

/* 装饰的“实体”部分（不含阴影）：标题栏在内容上方，左右下各一圈边框 */
static sk_rect frame_box(const skd_window *w)
{
    if (!has_decor(w))
        return w->rect;
    return sk_rect_make(w->rect.x - SK_BORDER, w->rect.y - SK_TITLE_H,
                        w->rect.w + 2 * SK_BORDER, w->rect.h + SK_TITLE_H + SK_BORDER);
}

/* 窗口在屏幕上占的全部范围（含阴影）：移动/显示/隐藏时损坏它 */
static sk_rect outer_rect(const skd_window *w)
{
    return has_decor(w) ? sk_frame_outer_rect(w->rect) : w->rect;
}

/* 边框层的四条：上（标题栏）、下、左、右。坐标相对内容区左上角 */
static void strip_rects(int cw, int ch, sk_rect out[SKD_STRIP_COUNT])
{
    sk_rect o = sk_frame_outer_rect(sk_rect_make(0, 0, cw, ch));
    int top = -o.y, bottom = rect_bottom(o) - ch, left = -o.x, right = rect_right(o) - cw;
    out[SKD_STRIP_TOP]    = sk_rect_make(o.x, o.y, o.w, SK_MAX(top, 0));
    out[SKD_STRIP_BOTTOM] = sk_rect_make(o.x, ch, o.w, SK_MAX(bottom, 0));
    out[SKD_STRIP_LEFT]   = sk_rect_make(o.x, 0, SK_MAX(left, 0), ch);
    out[SKD_STRIP_RIGHT]  = sk_rect_make(cw, 0, SK_MAX(right, 0), ch);
}

static void damage_window(const skd_window *w)
{
    if (is_visible(w))
        comp_damage(outer_rect(w));
}

/* 只损坏边框（焦点变化、悬停按钮变化时内容区不用重画） */
static void damage_frame(const skd_window *w)
{
    if (!is_visible(w) || !has_decor(w))
        return;
    sk_rect s[SKD_STRIP_COUNT];
    strip_rects(w->rect.w, w->rect.h, s);
    for (int i = 0; i < SKD_STRIP_COUNT; i++)
        comp_damage(sk_rect_offset(s[i], w->rect.x, w->rect.y));
}

/* 让窗口保持“抓得住”：标题栏不能跑到屏幕上边外面或任务栏下面，左右至少露出一截 */
static void keep_reachable(skd_window *w)
{
    sk_rect wa = skd_work_area();
    int top_min = wa.y + (has_decor(w) ? SK_TITLE_H : 0);
    if (w->rect.y < top_min)
        w->rect.y = top_min;
    if (w->rect.y > rect_bottom(wa) - 8)
        w->rect.y = rect_bottom(wa) - 8;
    int keep = SK_MIN(80, w->rect.w);
    if (w->rect.x + w->rect.w < wa.x + keep)
        w->rect.x = wa.x + keep - w->rect.w;
    if (w->rect.x > rect_right(wa) - keep)
        w->rect.x = rect_right(wa) - keep;
}

/* 内容区在工作区里最大能有多大（留出边框和阴影） */
static void max_content(const skd_window *w, int *mw, int *mh)
{
    sk_rect wa = skd_work_area();
    if (has_decor(w)) {
        *mw = wa.w - 2 * SK_BORDER - SK_SHADOW_OFF;
        *mh = wa.h - SK_TITLE_H - SK_BORDER - SK_SHADOW_OFF;
    } else {
        *mw = wa.w;
        *mh = wa.h;
    }
    if (*mw < 1) *mw = 1;
    if (*mh < 1) *mh = 1;
}

/* ====================================================================== */
/*                             边框层渲染                                   */
/* ====================================================================== */

static void frame_free(skd_window *w)
{
    for (int i = 0; i < SKD_STRIP_COUNT; i++) {
        if (w->strip[i])
            sk_canvas_free(w->strip[i]);
        w->strip[i] = NULL;
    }
    w->frame_valid = 0;
}

/*
 * 确保边框层是最新的。边框只在“标题、焦点、悬停按钮、尺寸”变化时才重画，
 * 平时合成直接用缓存的四条小画布（移动窗口也不用重画：坐标是相对内容区的）。
 * 四条分别调用一次 sk_draw_window_frame：每条画布只有那一条那么大，
 * 画到外面的部分被自动裁掉——手绘随机数只由 seed 决定，所以四条能严丝合缝地拼起来。
 */
static void frame_ensure(skd_window *w)
{
    if (!has_decor(w))
        return;
    int focused = (w == g_focus);
    int hover = (w == g_hover_frame) ? g_hover_btn : -1;
    if (w->frame_valid && w->frame_focused == focused && w->frame_hover == hover &&
        w->frame_w == w->rect.w && w->frame_h == w->rect.h && strcmp(w->frame_title, w->title) == 0)
        return;
    sk_rect s[SKD_STRIP_COUNT];
    strip_rects(w->rect.w, w->rect.h, s);
    for (int i = 0; i < SKD_STRIP_COUNT; i++) {
        if (w->strip[i] && (w->strip[i]->w != s[i].w || w->strip[i]->h != s[i].h)) {
            sk_canvas_free(w->strip[i]);
            w->strip[i] = NULL;
        }
        if (s[i].w <= 0 || s[i].h <= 0)
            continue;
        if (!w->strip[i] && !(w->strip[i] = sk_canvas_new(s[i].w, s[i].h)))
            continue;
        sk_canvas *c = w->strip[i];
        sk_reset_clip(c);
        /* alpha=0 表示透明；RGB 预先填成纸色，这样抗锯齿边缘混合出来是“纸上的铅笔灰”而不是黑边 */
        sk_clear(c, SK_WITH_ALPHA(SK_DESK_PAPER, 0));
        sk_rect content_local = sk_rect_make(-s[i].x, -s[i].y, w->rect.w, w->rect.h);
        sk_draw_window_frame(c, content_local, w->title, focused, hover, w->accent, w->seed);
        w->strip_off[i] = s[i];
    }
    w->frame_valid = 1;
    w->frame_focused = focused;
    w->frame_hover = hover;
    w->frame_w = w->rect.w;
    w->frame_h = w->rect.h;
    sk_strlcpy(w->frame_title, w->title, sizeof w->frame_title);
}

/* ====================================================================== */
/*                              层叠与焦点                                  */
/* ====================================================================== */

static int z_index(const skd_window *w)
{
    for (int i = 0; i < g_nz; i++)
        if (g_z[i] == w)
            return i;
    return -1;
}

static void z_remove(const skd_window *w)
{
    int i = z_index(w);
    if (i < 0)
        return;
    memmove(&g_z[i], &g_z[i + 1], (size_t)(g_nz - i - 1) * sizeof g_z[0]);
    g_nz--;
}

static void z_push_top(skd_window *w)
{
    z_remove(w);
    g_z[g_nz++] = w;
}

/* 提到最上面；同一个应用的对话框总是保持在它的其它窗口之上 */
static void raise_window(skd_window *w)
{
    int before = z_index(w);
    z_push_top(w);
    if (!(w->flags & SK_WIN_DIALOG)) {
        skd_window *dlg[SKD_MAX_WINDOWS];
        int nd = 0;
        for (int i = 0; i < g_nz; i++)
            if (g_z[i] != w && g_z[i]->client == w->client && (g_z[i]->flags & SK_WIN_DIALOG))
                dlg[nd++] = g_z[i];
        for (int i = 0; i < nd; i++) {
            z_push_top(dlg[i]);
            damage_window(dlg[i]);
        }
    }
    if (before != z_index(w))
        damage_window(w);
}

skd_window *wm_focused(void) { return g_focus; }

void wm_set_focus(skd_window *w)
{
    if (w == g_focus)
        return;
    skd_window *old = g_focus;
    g_focus = w;
    if (old) {
        server_send_focus(old->client, old->id, 0);
        damage_frame(old);
    }
    if (w) {
        server_send_focus(w->client, w->id, 1);
        damage_frame(w);
    }
    taskbar_windows_changed();
}

/* 焦点交给最上面的可见窗口（当前焦点窗口被关闭/最小化时） */
static void focus_topmost(void)
{
    for (int i = g_nz - 1; i >= 0; i--)
        if (is_visible(g_z[i])) {
            wm_set_focus(g_z[i]);
            return;
        }
    wm_set_focus(NULL);
}

void wm_activate(skd_window *w)
{
    if (!w)
        return;
    if (w->minimized) {
        w->minimized = 0;
        damage_window(w);
    }
    raise_window(w);
    wm_set_focus(w);
    taskbar_windows_changed();
    if (g_ps == P_IDLE)
        update_hover(g_px, g_py);
}

skd_window *wm_find(uint32_t id)
{
    for (int i = 0; i < g_nz; i++)
        if (g_z[i]->id == id)
            return g_z[i];
    return NULL;
}

int wm_window_count(void) { return g_nz; }
int wm_pointer_x(void) { return g_px; }
int wm_pointer_y(void) { return g_py; }

/* 任务栏上的窗口：已显示的普通窗口（对话框和无边框窗口不上任务栏），按创建顺序 */
int wm_task_list(skd_window **out, int max)
{
    int n = 0;
    for (int i = 0; i < g_nz; i++) {
        skd_window *w = g_z[i];
        if (w->mapped && !(w->flags & (SK_WIN_DIALOG | SK_WIN_NO_DECOR)))
            out[n++] = w;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j - 1]->order > out[j]->order; j--) {
            skd_window *t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return SK_MIN(n, max);
}

/* ====================================================================== */
/*                         创建 / 销毁 / 更新                               */
/* ====================================================================== */

/* 根据启动记录或程序名推断应用图标（任务栏用） */
static int guess_icon(skd_client *c, const char *app_name)
{
    const skd_app *a = apps_find(launch_app_of_pid(server_client_pid(c)));
    if (a)
        return a->icon;
    for (int i = 0; i < apps_count(); i++) {
        const skd_app *p = apps_get(i);
        const char *cmd = p->command;
        const char *sp = strchr(cmd, ' ');
        size_t len = sp ? (size_t)(sp - cmd) : strlen(cmd);
        const char *base = cmd;
        for (size_t k = 0; k < len; k++)
            if (cmd[k] == '/')
                base = cmd + k + 1;
        size_t blen = len - (size_t)(base - cmd);
        if (blen == strlen(app_name) && strncmp(base, app_name, blen) == 0)
            return p->icon;
    }
    return SK_ICON_PROGRAM;
}

static void place_window(skd_window *w)
{
    sk_rect wa = skd_work_area();
    int bw = w->rect.w + (has_decor(w) ? 2 * SK_BORDER : 0);
    int bh = w->rect.h + (has_decor(w) ? SK_TITLE_H + SK_BORDER : 0);
    int top = has_decor(w) ? SK_TITLE_H : 0, left = has_decor(w) ? SK_BORDER : 0;
    if (w->flags & SK_WIN_DIALOG) {
        /* 对话框：居中盖在同一应用最上面的那个窗口上 */
        skd_window *parent = NULL;
        for (int i = g_nz - 1; i >= 0; i--)
            if (g_z[i] != w && g_z[i]->client == w->client && is_visible(g_z[i])) {
                parent = g_z[i];
                break;
            }
        if (parent) {
            sk_rect pb = frame_box(parent);
            w->rect.x = pb.x + (pb.w - bw) / 2 + left;
            w->rect.y = pb.y + (pb.h - bh) / 2 + top;
        } else {
            w->rect.x = wa.x + (wa.w - bw) / 2 + left;
            w->rect.y = wa.y + (wa.h - bh) / 2 + top;
        }
    } else if (w->flags & SK_WIN_CENTER) {
        w->rect.x = wa.x + (wa.w - bw) / 2 + left;
        w->rect.y = wa.y + (wa.h - bh) / 2 + top;
    } else {
        /* 层叠摆放：每个新窗口往右下错开一点，避开左边的桌面图标列 */
        int k = g_cascade++ % 8;
        w->rect.x = wa.x + 132 + 28 * k + left;
        w->rect.y = wa.y + 24 + 28 * k + top;
        if (w->rect.x - left + bw > rect_right(wa))
            w->rect.x = SK_MAX(wa.x + left, rect_right(wa) - bw + left);
        if (w->rect.y - top + bh > rect_bottom(wa))
            w->rect.y = SK_MAX(wa.y + top, rect_bottom(wa) - bh + top);
    }
    /* 尽量整个放进工作区 */
    if (w->rect.x - left + bw > rect_right(wa))
        w->rect.x = rect_right(wa) - bw + left;
    if (w->rect.y - top + bh > rect_bottom(wa))
        w->rect.y = rect_bottom(wa) - bh + top;
    if (w->rect.x - left < wa.x)
        w->rect.x = wa.x + left;
    if (w->rect.y - top < wa.y)
        w->rect.y = wa.y + top;
}

static int g_pending_cfg_w[SKD_MAX_WINDOWS], g_pending_cfg_h[SKD_MAX_WINDOWS];
static uint32_t g_pending_cfg_id[SKD_MAX_WINDOWS];

skd_window *wm_create_window(skd_client *c, const char *app, const sk_req_create_window *req,
                             skd_buffer *buf)
{
    if (g_nz >= SKD_MAX_WINDOWS)
        return NULL;
    skd_window *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->id = g_next_id++;
    if (g_next_id == 0)
        g_next_id = 1;
    w->client = c;
    w->order = g_next_order++;
    sk_strlcpy(w->title, req->title, sizeof w->title);
    sk_strlcpy(w->app, app, sizeof w->app);
    w->flags = req->flags;
    w->accent = req->accent ? (req->accent | 0xFF000000u) : SK_BLUE;
    w->min_w = req->min_w > 0 ? req->min_w : SKD_DEFAULT_MIN_W;
    w->min_h = req->min_h > 0 ? req->min_h : SKD_DEFAULT_MIN_H;
    w->cursor = SK_CURSOR_ARROW;
    w->seed = sk_hash_u32(w->id, 0x5EEDF00Du);
    w->icon = guess_icon(c, app);
    w->buf = *buf;
    sk_canvas_init(&w->content, w->buf.pixels, w->buf.w, w->buf.h, w->buf.w);
    w->rect = sk_rect_make(0, 0, w->buf.w, w->buf.h);
    w->frame_hover = -1;
    place_window(w);
    g_z[g_nz++] = w;
    if (!(w->flags & SK_WIN_DIALOG)) {
        /* 新的普通窗口放到最上面，但别的应用不会被压到它的对话框下面：再把对话框提上来 */
        for (int i = 0; i < g_nz; i++)
            if ((g_z[i]->flags & SK_WIN_DIALOG) && g_z[i]->client == c)
                z_push_top(g_z[i]);
    }
    /* 客户端正在响应（比如关窗口时弹出“是否保存”对话框）：取消强制关闭计时 */
    for (int i = 0; i < g_nz; i++)
        if (g_z[i]->client == c)
            g_z[i]->close_req_ms = 0;

    /* 初始尺寸超过工作区：缩小，等客户端知道窗口 id 后再发 CONFIGURE（见 wm_window_announced） */
    int mw, mh;
    max_content(w, &mw, &mh);
    if (w->rect.w > mw || w->rect.h > mh) {
        int slot = (int)(w->id % SKD_MAX_WINDOWS);
        g_pending_cfg_id[slot] = w->id;
        g_pending_cfg_w[slot] = SK_MIN(w->rect.w, SK_MAX(mw, 1));
        g_pending_cfg_h[slot] = SK_MIN(w->rect.h, SK_MAX(mh, 1));
    }
    sk_log_console("window %u created app=%s title=%s", w->id, w->app, w->title);
    return w;
}

void wm_window_announced(skd_window *w)
{
    wm_activate(w);
    int slot = (int)(w->id % SKD_MAX_WINDOWS);
    if (g_pending_cfg_id[slot] == w->id) {
        g_pending_cfg_id[slot] = 0;
        server_send_configure(w->client, w->id, g_pending_cfg_w[slot], g_pending_cfg_h[slot]);
    }
}

void wm_destroy_window(skd_window *w)
{
    sk_log_console("window %u destroyed", w->id);
    damage_window(w);
    z_remove(w);
    /* 清除所有指向它的引用：这里漏掉一个，就是一次“释放后使用”(use-after-free) */
    if (g_grab == w) {
        if (g_ps == P_RESIZE)
            comp_damage_all();
        g_grab = NULL;
        g_ps = g_btns ? P_SWALLOW : P_IDLE;
    }
    if (g_hover == w)
        g_hover = NULL;
    if (g_hover_frame == w) {
        g_hover_frame = NULL;
        g_hover_btn = -1;
    }
    if (g_switching) {
        int k = 0;
        for (int i = 0; i < g_sw_n; i++)
            if (g_sw[i] != w)
                g_sw[k++] = g_sw[i];
        g_sw_n = k;
        if (g_sw_n == 0) {
            g_switching = 0;
            menu_switcher_hide();
        } else {
            g_sw_sel %= g_sw_n;
            menu_switcher_show(g_sw, g_sw_n, g_sw_sel);
        }
    }
    int slot = (int)(w->id % SKD_MAX_WINDOWS);
    if (g_pending_cfg_id[slot] == w->id)
        g_pending_cfg_id[slot] = 0;
    if (g_focus == w) {
        g_focus = NULL;     /* 不给已销毁的窗口发 FOCUS 0 */
        focus_topmost();
    }
    server_window_gone(w->client, w->buf.map_len);
    buffer_release(&w->buf);
    frame_free(w);
    free(w);
    taskbar_windows_changed();
    if (g_ps == P_IDLE)
        update_hover(g_px, g_py);
}

void wm_destroy_client_windows(skd_client *c)
{
    for (int i = g_nz - 1; i >= 0; i--) {
        if (i < g_nz && g_z[i]->client == c) {
            wm_destroy_window(g_z[i]);
            i = g_nz;     /* 数组变了，从头再找 */
        }
    }
}

void wm_attach_buffer(skd_window *w, skd_buffer *buf)
{
    damage_window(w);
    server_buffer_replaced(w->client, w->buf.map_len);
    buffer_release(&w->buf);
    w->buf = *buf;
    sk_canvas_init(&w->content, w->buf.pixels, w->buf.w, w->buf.h, w->buf.w);
    w->rect.w = w->buf.w;
    w->rect.h = w->buf.h;
    keep_reachable(w);
    damage_window(w);
    if (g_ps == P_IDLE)
        update_hover(g_px, g_py);
}

void wm_damage_content(skd_window *w, sk_rect r)
{
    r = sk_rect_intersect(r, sk_rect_make(0, 0, w->rect.w, w->rect.h));
    if (!w->mapped) {
        /* 第一次 DAMAGE = 应用画好了第一帧，这时才让窗口出现 */
        w->mapped = 1;
        damage_window(w);
        taskbar_windows_changed();
        if (g_ps == P_IDLE)
            update_hover(g_px, g_py);
        return;
    }
    if (sk_rect_empty(r) || !is_visible(w))
        return;
    comp_damage(sk_rect_offset(r, w->rect.x, w->rect.y));
}

void wm_set_title(skd_window *w, const char *title)
{
    if (strcmp(w->title, title) == 0)
        return;
    sk_strlcpy(w->title, title, sizeof w->title);
    damage_frame(w);
    taskbar_windows_changed();
}

void wm_show_window(skd_window *w)
{
    if (!w->mapped) {
        w->mapped = 1;
        damage_window(w);
    }
    wm_activate(w);
}

void wm_move_window(skd_window *w, int x, int y)
{
    damage_window(w);
    w->maximized = 0;
    w->rect.x = SK_CLAMP(x, -SK_MAX_WIN_SIZE, 2 * SK_MAX_WIN_SIZE);
    w->rect.y = SK_CLAMP(y, -SK_MAX_WIN_SIZE, 2 * SK_MAX_WIN_SIZE);
    keep_reachable(w);
    damage_window(w);
    if (g_ps == P_IDLE)
        update_hover(g_px, g_py);
}

void wm_set_cursor(skd_window *w, int cursor)
{
    w->cursor = cursor;
    if ((g_ps == P_IDLE && g_hover == w) || (g_ps == P_CONTENT && g_grab == w))
        cursor_set_shape(cursor);
}

/* ====================================================================== */
/*                       最小化 / 最大化 / 关闭                             */
/* ====================================================================== */

void wm_minimize(skd_window *w)
{
    if (!w || w->minimized)
        return;
    damage_window(w);
    w->minimized = 1;
    if (g_grab == w) {
        g_grab = NULL;
        g_ps = g_btns ? P_SWALLOW : P_IDLE;
    }
    if (g_focus == w)
        focus_topmost();
    taskbar_windows_changed();
    if (g_ps == P_IDLE)
        update_hover(g_px, g_py);
}

void wm_toggle_maximize(skd_window *w)
{
    if (!w || !(w->flags & SK_WIN_RESIZABLE) || !has_decor(w))
        return;
    damage_window(w);
    if (!w->maximized) {
        sk_rect wa = skd_work_area();
        w->restore_rect = w->rect;
        int cw = SK_MAX(wa.w - 2 * SK_BORDER, w->min_w);
        int ch = SK_MAX(wa.h - SK_TITLE_H - SK_BORDER, w->min_h);
        w->rect.x = wa.x + SK_BORDER;
        w->rect.y = wa.y + SK_TITLE_H;
        w->maximized = 1;
        server_send_configure(w->client, w->id, cw, ch);
    } else {
        w->maximized = 0;
        w->rect.x = w->restore_rect.x;
        w->rect.y = w->restore_rect.y;
        keep_reachable(w);
        server_send_configure(w->client, w->id, w->restore_rect.w, w->restore_rect.h);
    }
    damage_window(w);
}

void wm_request_close(skd_window *w)
{
    if (!w)
        return;
    uint64_t now = skd_now_ms();
    if (w->close_req_ms && now - w->close_req_ms >= SKD_FORCE_CLOSE_MS) {
        /* 3 秒前就请求过关闭，应用一直没反应：多半卡死了，直接结束它的进程 */
        char msg[SK_TITLE_MAX + 48];
        snprintf(msg, sizeof msg, "「%s」没有响应，已强制关闭", w->title);
        menu_toast(msg, 3000);
        server_kill_client(w->client);
        return;
    }
    if (!w->close_req_ms)
        w->close_req_ms = now;
    server_send_close(w->client, w->id);
}

/* ====================================================================== */
/*                             合成                                        */
/* ====================================================================== */

int wm_covering_index(sk_rect r)
{
    for (int i = g_nz - 1; i >= 0; i--) {
        const skd_window *w = g_z[i];
        if (!is_visible(w))
            continue;
        sk_rect c = w->rect;
        if (r.x >= c.x && r.y >= c.y && rect_right(r) <= rect_right(c) && rect_bottom(r) <= rect_bottom(c))
            return i;
    }
    return -1;
}

void wm_paint(sk_canvas *dst, sk_rect r, int from)
{
    for (int i = SK_MAX(from, 0); i < g_nz; i++) {
        skd_window *w = g_z[i];
        if (!is_visible(w))
            continue;
        if (sk_rect_empty(sk_rect_intersect(outer_rect(w), r)))
            continue;
        if (has_decor(w)) {
            frame_ensure(w);
            for (int s = 0; s < SKD_STRIP_COUNT; s++) {
                if (!w->strip[s])
                    continue;
                sk_rect sr = sk_rect_offset(w->strip_off[s], w->rect.x, w->rect.y);
                sk_rect ir = sk_rect_intersect(sr, r);
                if (sk_rect_empty(ir))
                    continue;
                sk_blit_keyed(dst, ir.x, ir.y, w->strip[s],
                              sk_rect_make(ir.x - sr.x, ir.y - sr.y, ir.w, ir.h));
            }
        }
        sk_rect ir = sk_rect_intersect(w->rect, r);
        if (!sk_rect_empty(ir))
            sk_blit(dst, ir.x, ir.y, &w->content,
                    sk_rect_make(ir.x - w->rect.x, ir.y - w->rect.y, ir.w, ir.h));
    }
}

/* 缩放虚线框：外框（含标题栏）的位置 */
static sk_rect outline_box(void)
{
    return sk_rect_make(g_rs_rect.x - SK_BORDER, g_rs_rect.y - SK_TITLE_H,
                        g_rs_rect.w + 2 * SK_BORDER, g_rs_rect.h + SK_TITLE_H + SK_BORDER);
}

/* 虚线框右下角的尺寸标注（"宽 × 高"）：文字、基线位置和背景矩形 */
static sk_rect outline_label(char *txt, size_t n, int *tx, int *ty)
{
    snprintf(txt, n, "%d × %d", g_rs_rect.w, g_rs_rect.h);
    sk_font *f = sk_font_get(SK_FONT_UI);
    sk_rect b = outline_box();
    int tw = sk_text_width(f, txt);
    *tx = rect_right(b) - tw - 12;
    *ty = rect_bottom(b) - 12;
    return sk_rect_make(*tx - 6, *ty - sk_font_ascent(f) - 4, tw + 12, sk_font_line_height(f) + 6);
}

/* 只损坏虚线框的四条边和尺寸标注（而不是整个框）：拖动缩放时不需要重新合成窗口中间 */
static void damage_outline(void)
{
    sk_rect b = outline_box();
    const int m = 5;
    comp_damage(sk_rect_make(b.x - m, b.y - m, b.w + 2 * m, 2 * m));
    comp_damage(sk_rect_make(b.x - m, rect_bottom(b) - m, b.w + 2 * m, 2 * m));
    comp_damage(sk_rect_make(b.x - m, b.y - m, 2 * m, b.h + 2 * m));
    comp_damage(sk_rect_make(rect_right(b) - m, b.y - m, 2 * m, b.h + 2 * m));
    char txt[32];
    int tx, ty;
    comp_damage(sk_rect_inset(outline_label(txt, sizeof txt, &tx, &ty), -2, -2));
}

/* 手绘虚线：沿一条边画一段段短的铅笔线 */
static void dashed_line(sk_canvas *dst, sk_rect clip, float x1, float y1, float x2, float y2, uint32_t seed)
{
    sk_rough_style st;
    sk_rough_default(&st);
    st.roughness = 0.9f;
    st.bowing = 0.4f;
    st.stroke_width = 1.7f;
    st.stroke = SK_RGBA(0x2B, 0x2B, 0x2B, 0xD0);
    st.double_stroke = 0;
    float len = (x2 - x1) + (y2 - y1);   /* 只有水平或竖直线 */
    const float dash = 11, gap = 6;
    int k = 0;
    for (float t = 0; t < len; t += dash + gap, k++) {
        float e = t + dash < len ? t + dash : len;
        float ax = x1 + (x2 - x1) * (t / len), ay = y1 + (y2 - y1) * (t / len);
        float bx = x1 + (x2 - x1) * (e / len), by = y1 + (y2 - y1) * (e / len);
        sk_rect bb = sk_rect_make((int)SK_MIN(ax, bx) - 3, (int)SK_MIN(ay, by) - 3,
                                  (int)(SK_MAX(ax, bx) - SK_MIN(ax, bx)) + 7,
                                  (int)(SK_MAX(ay, by) - SK_MIN(ay, by)) + 7);
        if (sk_rect_empty(sk_rect_intersect(bb, clip)))
            continue;
        st.seed = seed + (uint32_t)k * 7919u;
        sk_rough_line(dst, ax, ay, bx, by, &st);
    }
}

void wm_paint_overlay(sk_canvas *dst, sk_rect r)
{
    if (g_ps != P_RESIZE || !g_grab)
        return;
    sk_rect b = outline_box();
    sk_rect hit = sk_rect_intersect(sk_rect_inset(b, -6, -6), r);
    if (sk_rect_empty(hit))
        return;
    float x0 = (float)b.x, y0 = (float)b.y, x1 = (float)rect_right(b), y1 = (float)rect_bottom(b);
    uint32_t seed = g_grab->seed;
    dashed_line(dst, r, x0, y0, x1, y0, seed + 1);
    dashed_line(dst, r, x0, y1, x1, y1, seed + 2);
    dashed_line(dst, r, x0, y0, x0, y1, seed + 3);
    dashed_line(dst, r, x1, y0, x1, y1, seed + 4);
    /* 右下角写上新尺寸，像在纸上标注 */
    char txt[32];
    int tx, ty;
    sk_rect tr = outline_label(txt, sizeof txt, &tx, &ty);
    if (!sk_rect_empty(sk_rect_intersect(tr, r))) {
        sk_fill_rect(dst, tr, SK_RGBA(0xFF, 0xFD, 0xF7, 0xE0));
        sk_draw_text(dst, sk_font_get(SK_FONT_UI), tx, ty, txt, SK_INK_SOFT);
    }
}

/* ====================================================================== */
/*                              命中测试                                    */
/* ====================================================================== */

static skd_window *hit_test(int x, int y, int *hit, int *btn, int *rsmode)
{
    *hit = HIT_NONE;
    *btn = -1;
    *rsmode = 0;
    for (int i = g_nz - 1; i >= 0; i--) {
        skd_window *w = g_z[i];
        if (!is_visible(w))
            continue;
        if (sk_rect_contains(w->rect, x, y)) {
            *hit = HIT_CONTENT;
            return w;
        }
        if (!has_decor(w))
            continue;
        for (int b = 0; b < 3; b++)
            if (sk_rect_contains(sk_frame_button_rect(w->rect, b), x, y)) {
                *hit = HIT_BUTTON;
                *btn = b;
                return w;
            }
        sk_rect box = frame_box(w);
        /* 缩放抓手比边框稍微宽一点（往外多 3px），边框只有 6px，太细不好抓 */
        sk_rect grab = sk_rect_make(box.x, box.y, box.w + 3, box.h + 3);
        if (!sk_rect_contains(grab, x, y))
            continue;
        if ((w->flags & SK_WIN_RESIZABLE) && !w->maximized) {
            int right = x >= rect_right(w->rect), bottom = y >= rect_bottom(w->rect);
            if ((right && y >= rect_bottom(w->rect) - RESIZE_GRIP) ||
                (bottom && x >= rect_right(w->rect) - RESIZE_GRIP)) {
                *hit = HIT_RESIZE;
                *rsmode = RS_RIGHT | RS_BOTTOM;
                return w;
            }
            if (right && y >= w->rect.y) {
                *hit = HIT_RESIZE;
                *rsmode = RS_RIGHT;
                return w;
            }
            if (bottom && x >= w->rect.x) {
                *hit = HIT_RESIZE;
                *rsmode = RS_BOTTOM;
                return w;
            }
        }
        if (!sk_rect_contains(box, x, y))
            continue;
        *hit = (y < w->rect.y) ? HIT_TITLE : HIT_BORDER;
        return w;
    }
    return NULL;
}

/* ====================================================================== */
/*                              鼠标                                        */
/* ====================================================================== */

static void send_mouse(skd_window *w, uint32_t kind, uint32_t button, int wheel, uint32_t clicks)
{
    server_send_mouse(w->client, w->id, kind, g_px - w->rect.x, g_py - w->rect.y, g_btns,
                      button, wheel, clicks, input_modifiers());
}

/* 空闲时（没按按钮）根据指针位置更新：ENTER/LEAVE、内容区 MOVE、标题栏按钮悬停、光标形状 */
static void update_hover(int x, int y)
{
    skd_window *w = NULL;
    int hit = HIT_NONE, btn = -1, rsmode = 0;
    int on_taskbar = taskbar_contains(x, y);
    int on_menu = menu_is_open() && menu_pointer_motion(x, y);
    if (!on_taskbar && !on_menu)
        w = hit_test(x, y, &hit, &btn, &rsmode);
    taskbar_pointer_motion(on_taskbar && !on_menu ? x : -1, y);

    skd_window *content = (hit == HIT_CONTENT) ? w : NULL;
    if (content != g_hover) {
        if (g_hover)
            send_mouse(g_hover, SK_MOUSE_LEAVE, 0, 0, 0);
        g_hover = content;
        if (content)
            send_mouse(content, SK_MOUSE_ENTER, 0, 0, 0);
    } else if (content) {
        send_mouse(content, SK_MOUSE_MOVE, 0, 0, 0);
    }

    skd_window *fw = (hit == HIT_BUTTON) ? w : NULL;
    int fb = fw ? btn : -1;
    if (fw != g_hover_frame || fb != g_hover_btn) {
        skd_window *old = g_hover_frame;
        g_hover_frame = fw;
        g_hover_btn = fb;
        if (old)
            damage_frame(old);
        if (fw && fw != old)
            damage_frame(fw);
    }

    if (hit == HIT_CONTENT)
        cursor_set_shape(w->cursor);
    else if (hit == HIT_RESIZE)
        cursor_set_shape(SK_CURSOR_RESIZE);
    else
        cursor_set_shape(SK_CURSOR_ARROW);
}

void wm_refresh_cursor(void)
{
    if (g_ps == P_IDLE)
        update_hover(g_px, g_py);
}

void wm_cancel_grabs(void)
{
    if (g_ps == P_RESIZE)
        damage_outline();
    if (g_ps == P_MOVE || g_ps == P_RESIZE || g_ps == P_BUTTON) {
        g_ps = g_btns ? P_SWALLOW : P_IDLE;
        g_grab = NULL;
        cursor_set_shape(SK_CURSOR_ARROW);
    }
}

static int count_clicks(uint32_t button, int x, int y)
{
    uint64_t now = skd_now_ms();
    int n = 1;
    if (button == g_click_btn && g_click_n == 1 && now - g_click_ms <= SKD_DCLICK_MS &&
        abs(x - g_click_x) <= SKD_DCLICK_DIST && abs(y - g_click_y) <= SKD_DCLICK_DIST)
        n = 2;
    g_click_btn = button;
    g_click_ms = now;
    g_click_x = x;
    g_click_y = y;
    g_click_n = n;
    return n;
}

static void do_move(int x, int y)
{
    skd_window *w = g_grab;
    if (!g_drag_started) {
        if (abs(x - g_press_x) < MOVE_THRESHOLD && abs(y - g_press_y) < MOVE_THRESHOLD)
            return;
        g_drag_started = 1;
        cursor_set_shape(SK_CURSOR_MOVE);
        if (w->maximized) {
            /* 拖动最大化的窗口 = 先还原大小，指针保持在标题栏上相同比例的位置 */
            w->maximized = 0;
            int rw = w->restore_rect.w;
            g_drag_dx = (int)((long)g_drag_dx * rw / SK_MAX(w->rect.w, 1));
            server_send_configure(w->client, w->id, w->restore_rect.w, w->restore_rect.h);
        }
    }
    int nx = x - g_drag_dx, ny = y - g_drag_dy;
    if (nx == w->rect.x && ny == w->rect.y)
        return;
    damage_window(w);
    w->rect.x = nx;
    w->rect.y = ny;
    keep_reachable(w);
    damage_window(w);
}

static void do_resize(int x, int y)
{
    skd_window *w = g_grab;
    int nw = g_rs_w0, nh = g_rs_h0;
    if (g_rs_mode & RS_RIGHT)
        nw = g_rs_w0 + (x - g_press_x);
    if (g_rs_mode & RS_BOTTOM)
        nh = g_rs_h0 + (y - g_press_y);
    nw = SK_CLAMP(nw, SK_MAX(w->min_w, 1), SK_MAX_WIN_SIZE);
    nh = SK_CLAMP(nh, SK_MAX(w->min_h, 1), SK_MAX_WIN_SIZE);
    if (nw == g_rs_rect.w && nh == g_rs_rect.h)
        return;
    damage_outline();
    g_rs_rect.w = nw;
    g_rs_rect.h = nh;
    damage_outline();
}

void wm_pointer_motion(int x, int y)
{
    g_px = x;
    g_py = y;
    cursor_set_pos(x, y);
    if (splash_active())
        return;
    switch (g_ps) {
    case P_CONTENT:
        if (g_grab)
            send_mouse(g_grab, SK_MOUSE_MOVE, 0, 0, 0);
        return;
    case P_MOVE:
        if (g_grab)
            do_move(x, y);
        return;
    case P_RESIZE:
        if (g_grab)
            do_resize(x, y);
        return;
    case P_BUTTON: {
        if (!g_grab)
            return;
        /* 按住按钮移出去：按钮恢复原样，松开时不触发 */
        int inside = sk_rect_contains(sk_frame_button_rect(g_grab->rect, g_grab_btn), x, y);
        int want = inside ? g_grab_btn : -1;
        if (g_hover_frame != g_grab || g_hover_btn != want) {
            g_hover_frame = g_grab;
            g_hover_btn = want;
            damage_frame(g_grab);
        }
        return;
    }
    case P_MENU:
        menu_pointer_motion(x, y);
        return;
    case P_TASKBAR:
        taskbar_pointer_motion(taskbar_contains(x, y) ? x : -1, y);
        return;
    case P_SWALLOW:
        return;
    case P_IDLE:
        update_hover(x, y);
        return;
    }
}

static void press(uint32_t button)
{
    int x = g_px, y = g_py;
    if (g_ps == P_CONTENT && g_grab) {
        /* 抓取期间又按下另一个按钮：照样发给被抓取的窗口 */
        send_mouse(g_grab, SK_MOUSE_DOWN, button, 0, 1);
        return;
    }
    if (g_ps != P_IDLE)
        return;
    int clicks = count_clicks(button, x, y);

    if (menu_is_open() && menu_pointer_down(x, y, button)) {
        g_ps = P_MENU;
        return;
    }
    if (taskbar_contains(x, y)) {
        taskbar_pointer_down(x, y, button);
        g_ps = P_TASKBAR;
        return;
    }
    int hit, btn, rsmode;
    skd_window *w = hit_test(x, y, &hit, &btn, &rsmode);
    if (!w) {
        /* 桌面：窗口都失去焦点 */
        wm_set_focus(NULL);
        desktop_pointer_down(x, y, button, clicks);
        g_ps = P_SWALLOW;
        return;
    }
    wm_activate(w);
    switch (hit) {
    case HIT_CONTENT:
        g_ps = P_CONTENT;
        g_grab = w;
        g_hover = w;
        send_mouse(w, SK_MOUSE_DOWN, button, 0, (uint32_t)clicks);
        return;
    case HIT_TITLE:
        if (button == SK_BUTTON_LEFT && clicks == 2 && (w->flags & SK_WIN_RESIZABLE)) {
            wm_toggle_maximize(w);
            g_ps = P_SWALLOW;
        } else if (button == SK_BUTTON_LEFT) {
            g_ps = P_MOVE;
            g_grab = w;
            g_drag_dx = x - w->rect.x;
            g_drag_dy = y - w->rect.y;
            g_drag_started = 0;
            g_press_x = x;
            g_press_y = y;
        } else {
            g_ps = P_SWALLOW;
        }
        return;
    case HIT_BUTTON:
        if (button == SK_BUTTON_LEFT) {
            g_ps = P_BUTTON;
            g_grab = w;
            g_grab_btn = btn;
        } else {
            g_ps = P_SWALLOW;
        }
        return;
    case HIT_RESIZE:
        if (button == SK_BUTTON_LEFT) {
            g_ps = P_RESIZE;
            g_grab = w;
            g_rs_mode = rsmode;
            g_rs_rect = w->rect;
            g_rs_w0 = w->rect.w;
            g_rs_h0 = w->rect.h;
            g_press_x = x;
            g_press_y = y;
            cursor_set_shape(SK_CURSOR_RESIZE);
            damage_outline();
        } else {
            g_ps = P_SWALLOW;
        }
        return;
    default:
        g_ps = P_SWALLOW;
        return;
    }
}

static void release(uint32_t button)
{
    int x = g_px, y = g_py;
    switch (g_ps) {
    case P_CONTENT:
        if (g_grab)
            send_mouse(g_grab, SK_MOUSE_UP, button, 0, 0);
        if (g_btns)
            return;
        break;
    case P_MOVE:
        break;
    case P_RESIZE:
        if (g_grab) {
            damage_outline();
            skd_window *w = g_grab;
            if (g_rs_rect.w != w->rect.w || g_rs_rect.h != w->rect.h) {
                w->maximized = 0;
                /* 新尺寸告诉客户端；它会建新缓冲区、重画、ATTACH，那时窗口才真正变大小 */
                server_send_configure(w->client, w->id, g_rs_rect.w, g_rs_rect.h);
            }
        }
        break;
    case P_BUTTON:
        if (g_grab) {
            skd_window *w = g_grab;
            int which = g_grab_btn;
            int inside = sk_rect_contains(sk_frame_button_rect(w->rect, which), x, y);
            g_ps = P_IDLE;
            g_grab = NULL;
            if (inside) {
                if (which == 0)
                    wm_request_close(w);
                else if (which == 1)
                    wm_toggle_maximize(w);
                else
                    wm_minimize(w);
            }
        }
        break;
    case P_MENU:
        menu_pointer_up(x, y, button);
        break;
    case P_TASKBAR:
        taskbar_pointer_up(x, y, button);
        break;
    case P_SWALLOW:
    case P_IDLE:
        break;
    }
    if (g_btns)
        return;
    g_ps = P_IDLE;
    g_grab = NULL;
    update_hover(x, y);
}

void wm_pointer_button(uint32_t button, int pressed)
{
    if (pressed)
        g_btns |= button;
    else
        g_btns &= ~button;
    if (splash_active()) {
        if (pressed)
            splash_skip();
        return;
    }
    if (pressed)
        press(button);
    else
        release(button);
}

void wm_pointer_wheel(int delta)
{
    if (splash_active() || menu_is_open())
        return;
    if (g_ps == P_CONTENT && g_grab) {
        send_mouse(g_grab, SK_MOUSE_WHEEL, 0, delta, 0);
        return;
    }
    if (taskbar_contains(g_px, g_py))
        return;
    int hit, btn, rsmode;
    skd_window *w = hit_test(g_px, g_py, &hit, &btn, &rsmode);
    if (w && hit == HIT_CONTENT)
        send_mouse(w, SK_MOUSE_WHEEL, 0, delta, 0);
}

/* ====================================================================== */
/*                         键盘与全局快捷键                                 */
/* ====================================================================== */

static void swallow_set(uint32_t k, int on)
{
    if (k > KEY_MAX)
        return;
    if (on)
        g_swallow[k / 8] |= (uint8_t)(1u << (k % 8));
    else
        g_swallow[k / 8] &= (uint8_t)~(1u << (k % 8));
}

static int swallowed(uint32_t k) { return k <= KEY_MAX && (g_swallow[k / 8] >> (k % 8)) & 1; }

/* PrintScreen：把当前画面存成 PPM 图片 */
static void take_screenshot(void)
{
    const char *home = getenv("HOME");
    char dir[128], path[192];
    snprintf(dir, sizeof dir, "%s/图片", home && *home ? home : "/root");
    if (sk_mkdir_p(dir, 0755) != 0) {
        sk_log("wm: 创建 %s 失败: %s", dir, strerror(errno));
        menu_toast("截图失败：无法创建目录", 2500);
        return;
    }
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(path, sizeof path, "%s/截图-%s.ppm", dir, stamp);
    sk_canvas *shot = sk_canvas_new(skd_screen_w(), skd_screen_h());
    if (!shot) {
        menu_toast("截图失败：内存不足", 2500);
        return;
    }
    comp_snapshot(shot);
    int r = sk_image_save_ppm(shot, path);
    sk_canvas_free(shot);
    if (r != 0) {
        sk_log("wm: 保存截图 %s 失败", path);
        menu_toast("截图保存失败", 2500);
        return;
    }
    sk_log("wm: 截图已保存到 %s", path);
    menu_toast("已保存截图", 2000);
}

/* Alt+Tab 候选：所有任务栏窗口，按层叠顺序从上到下（≈最近使用），最小化的排后面 */
static void switcher_build(void)
{
    g_sw_n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = g_nz - 1; i >= 0; i--) {
            skd_window *w = g_z[i];
            if (!w->mapped || (w->flags & (SK_WIN_DIALOG | SK_WIN_NO_DECOR)))
                continue;
            if ((pass == 0) == (w->minimized == 0))
                g_sw[g_sw_n++] = w;
        }
}

/* 全局快捷键。返回 1 表示已处理（不再发给应用） */
static int global_shortcut(uint32_t keycode, uint32_t mods)
{
    int alt = (mods & SK_MOD_ALT) != 0, ctrl = (mods & SK_MOD_CTRL) != 0;
    if (alt && !ctrl && keycode == KEY_TAB) {
        switcher_build();
        if (g_sw_n == 0)
            return 1;
        g_switching = 1;
        g_sw_sel = (g_sw_n > 1 && g_sw[0] == g_focus) ? 1 : 0;
        if (mods & SK_MOD_SHIFT)
            g_sw_sel = g_sw_n - 1;
        menu_switcher_show(g_sw, g_sw_n, g_sw_sel);
        return 1;
    }
    if (alt && !ctrl && keycode == KEY_F4) {
        if (g_focus)
            wm_request_close(g_focus);
        return 1;
    }
    if (ctrl && alt && keycode == KEY_T) {
        launch_app("terminal", NULL);
        return 1;
    }
    if (ctrl && !alt && keycode == KEY_ESC) {
        menu_toggle_start();
        return 1;
    }
    if (ctrl && alt && keycode == KEY_BACKSPACE) {
        sk_log("wm: Ctrl+Alt+Backspace，退出图形界面");
        skd_quit();
        return 1;
    }
    if (ctrl && alt && keycode == KEY_DELETE) {
        sk_log("wm: Ctrl+Alt+Delete，请求 sinit 重新启动");
        menu_toast("正在重新启动…", 3000);
        if (kill(1, SIGINT) < 0)
            sk_log("wm: kill(1, SIGINT) 失败: %s", strerror(errno));
        return 1;
    }
    if (keycode == KEY_SYSRQ || keycode == KEY_PRINT) {
        take_screenshot();
        return 1;
    }
    return 0;
}

void wm_key(uint32_t keycode, uint32_t state, uint32_t mods, uint32_t cp)
{
    if (splash_active()) {
        if (state == 1)
            splash_skip();
        return;
    }
    /* 被快捷键吃掉的键：它的自动重复和松开也不发给应用 */
    if (swallowed(keycode)) {
        if (state == 0)
            swallow_set(keycode, 0);
        if (!(g_switching && keycode == KEY_TAB && state == 2))
            return;
    }

    /* 单独按一下 Super（Windows 键）再松开 = 开关开始菜单 */
    if (keycode == KEY_LEFTMETA || keycode == KEY_RIGHTMETA) {
        if (state == 1) {
            g_super_alone = 1;
        } else if (state == 0 && g_super_alone) {
            g_super_alone = 0;
            menu_toggle_start();
        }
    } else if (state == 1) {
        g_super_alone = 0;
    }

    if (g_switching) {
        if (keycode == KEY_TAB && state != 0) {
            g_sw_sel = (mods & SK_MOD_SHIFT) ? (g_sw_sel + g_sw_n - 1) % g_sw_n : (g_sw_sel + 1) % g_sw_n;
            menu_switcher_show(g_sw, g_sw_n, g_sw_sel);
            swallow_set(keycode, 1);
            return;
        }
        if (keycode == KEY_ESC && state == 1) {
            g_switching = 0;
            menu_switcher_hide();
            swallow_set(keycode, 1);
            return;
        }
        if ((keycode == KEY_LEFTALT || keycode == KEY_RIGHTALT) && state == 0) {
            /* 松开 Alt：切换到选中的窗口 */
            skd_window *sel = g_sw[g_sw_sel];
            g_switching = 0;
            menu_switcher_hide();
            wm_activate(sel);
        }
    }

    if (state == 1 && global_shortcut(keycode, mods)) {
        swallow_set(keycode, 1);
        return;
    }
    if (menu_is_open() && menu_key(keycode, state, mods))
        return;
    if (g_focus)
        server_send_key(g_focus->client, g_focus->id, keycode, state, mods, cp);
}

/* ====================================================================== */

void wm_init(void)
{
    g_px = skd_screen_w() / 2;
    g_py = skd_screen_h() / 2;
    cursor_set_pos(g_px, g_py);
}

void wm_shutdown(void)
{
    while (g_nz > 0) {
        skd_window *w = g_z[--g_nz];
        buffer_release(&w->buf);
        frame_free(w);
        free(w);
    }
    g_focus = g_grab = g_hover = g_hover_frame = NULL;
}
