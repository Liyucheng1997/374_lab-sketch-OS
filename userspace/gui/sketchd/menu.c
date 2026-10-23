/*
 * menu.c — 浮在最上层的小东西：开始菜单、桌面右键菜单、关机确认框、Alt+Tab 切换器、提示条
 *
 * 它们都是“弹层”(popup)：一块自己的小画布（透明处 alpha=0），在合成时盖在任务栏之上。
 * 弹层的内容很少变化（悬停的菜单项变了才重画），重画时也只重画这一小块画布。
 *
 * 开始菜单做成一页“活页笔记本”：左边一排装订孔，红色页边线，每行下面一道淡蓝横线。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_SEED   0x3E7051A5u
#define SHADOW      6

typedef struct {
    int        visible;
    sk_rect    r;          /* 屏幕坐标 */
    sk_canvas *c;
} popup;

enum { ACT_APP = 0, ACT_REBOOT, ACT_POWEROFF, ACT_TERMINAL, ACT_WALLPAPER, ACT_ABOUT };

typedef struct {
    const char *label;     /* NULL = 分隔线 */
    int         icon;
    int         action;
    int         app;       /* ACT_APP：apps.conf 里的序号 */
    sk_rect     r;         /* 弹层内坐标 */
} mitem;

#define MAX_ITEMS (SKD_MAX_APPS + 6)

/* 当前打开的菜单（开始菜单或右键菜单，同一时间只有一个） */
static popup g_menu;
static int   g_menu_is_start;
static mitem g_items[MAX_ITEMS];
static int   g_nitems;
static int   g_sel = -1;          /* 悬停/键盘选中的项 */
static int   g_pressed = -1;

/* 确认框 */
static popup   g_confirm;
static int     g_confirm_reboot;
static int     g_confirm_focus;   /* 0 = 确认按钮，1 = 取消 */
static int     g_confirm_hover = -1, g_confirm_pressed = -1;
static sk_rect g_cbtn[2];

/* Alt+Tab 切换器、提示条 */
static popup    g_switch;
static popup    g_toast;
static uint64_t g_toast_until;

static int g_w, g_h;

/* ---------- 弹层基础 ---------- */

static int popup_alloc(popup *p, sk_rect r)
{
    if (p->c && (p->c->w != r.w || p->c->h != r.h)) {
        sk_canvas_free(p->c);
        p->c = NULL;
    }
    if (!p->c && !(p->c = sk_canvas_new(r.w, r.h)))
        return -1;
    if (p->visible)
        comp_damage(p->r);
    p->r = r;
    p->visible = 1;
    sk_reset_clip(p->c);
    sk_clear(p->c, SK_WITH_ALPHA(SK_DESK_PAPER, 0));
    comp_damage(r);
    return 0;
}

static void popup_hide(popup *p)
{
    if (!p->visible)
        return;
    comp_damage(p->r);
    p->visible = 0;
    if (p->c)
        sk_canvas_free(p->c);
    p->c = NULL;
}

static void popup_blit(const popup *p, sk_canvas *dst, sk_rect r)
{
    if (!p->visible || !p->c)
        return;
    sk_rect ir = sk_rect_intersect(p->r, r);
    if (sk_rect_empty(ir))
        return;
    sk_blit_keyed(dst, ir.x, ir.y, p->c, sk_rect_make(ir.x - p->r.x, ir.y - p->r.y, ir.w, ir.h));
}

/* 一张手绘的纸片：右下方排线阴影 + 纸色实心 + 铅笔轮廓 */
static void paper_card(sk_canvas *c, int w, int h, sk_color paper, uint32_t seed)
{
    sk_rough_style s;
    sk_rough_default(&s);
    s.seed = seed;
    s.stroke_width = 0;
    s.fill_style = SK_FILL_HACHURE;
    s.fill = SK_SHADOW;
    s.hachure_gap = 3.5f;
    s.hachure_angle = -45;
    s.fill_weight = 1.0f;
    sk_rough_rect(c, SHADOW, SHADOW, (float)(w - SHADOW - 2), (float)(h - SHADOW - 2), &s);
    sk_rect inner = sk_rect_make(2, 2, w - SHADOW - 4, h - SHADOW - 4);
    sk_paper_fill(c, inner, paper, 6, seed);
    sk_rough_default(&s);
    s.seed = seed + 1;
    s.stroke = SK_INK;
    s.stroke_width = 1.6f;
    sk_rough_rect(c, 2, 2, (float)inner.w, (float)inner.h, &s);
}

/* ---------- 菜单项构建 ---------- */

static void add_item(const char *label, int icon, int action, int app)
{
    if (g_nitems >= MAX_ITEMS)
        return;
    mitem *m = &g_items[g_nitems++];
    memset(m, 0, sizeof *m);
    m->label = label;
    m->icon = icon;
    m->action = action;
    m->app = app;
}

#define START_W      268
#define START_HEAD   70
#define START_ROW    36
#define START_SEP    14
#define CTX_W        190
#define CTX_ROW      34

static void render_menu(void);

static void open_start(void)
{
    g_nitems = 0;
    for (int i = 0; i < apps_count(); i++)
        add_item(apps_get(i)->name, apps_get(i)->icon, ACT_APP, i);
    add_item(NULL, 0, 0, 0);
    add_item("重新启动", SK_ICON_REBOOT, ACT_REBOOT, 0);
    add_item("关机", SK_ICON_POWER, ACT_POWEROFF, 0);
    int y = START_HEAD;
    for (int i = 0; i < g_nitems; i++) {
        int hgt = g_items[i].label ? START_ROW : START_SEP;
        g_items[i].r = sk_rect_make(30, y, START_W - SHADOW - 40, hgt);
        y += hgt;
    }
    int h = y + 16 + SHADOW;
    sk_rect sr = taskbar_start_rect();
    int top = sr.y - 10 - h;
    if (top < 4)
        top = 4;
    if (popup_alloc(&g_menu, sk_rect_make(sr.x - 2, top, START_W, h)) < 0)
        return;
    g_menu_is_start = 1;
    g_sel = g_pressed = -1;
    render_menu();
    taskbar_set_start_pressed(1);
}

static void open_context(int x, int y)
{
    g_nitems = 0;
    add_item("打开终端", SK_ICON_TERMINAL, ACT_TERMINAL, 0);
    add_item("换一张纸", SK_ICON_PAINT, ACT_WALLPAPER, 0);
    add_item(NULL, 0, 0, 0);
    add_item("关于本机", SK_ICON_ABOUT, ACT_ABOUT, 0);
    int yy = 10;
    for (int i = 0; i < g_nitems; i++) {
        int hgt = g_items[i].label ? CTX_ROW : 12;
        g_items[i].r = sk_rect_make(8, yy, CTX_W - SHADOW - 18, hgt);
        yy += hgt;
    }
    int h = yy + 10 + SHADOW;
    if (x + CTX_W > g_w)
        x = g_w - CTX_W;
    if (y + h > g_h - SKD_TASKBAR_H)
        y = g_h - SKD_TASKBAR_H - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (popup_alloc(&g_menu, sk_rect_make(x, y, CTX_W, h)) < 0)
        return;
    g_menu_is_start = 0;
    g_sel = g_pressed = -1;
    render_menu();
}

static void render_menu(void)
{
    if (!g_menu.visible || !g_menu.c)
        return;
    sk_canvas *c = g_menu.c;
    int w = g_menu.r.w, h = g_menu.r.h;
    sk_reset_clip(c);
    sk_clear(c, SK_WITH_ALPHA(SK_DESK_PAPER, 0));
    paper_card(c, w, h, SK_PAPER, MENU_SEED + (uint32_t)g_menu_is_start);
    sk_font *ui = sk_font_get(SK_FONT_UI);
    if (g_menu_is_start) {
        /* 笔记本：装订孔 + 红色页边线 + 每行一道淡蓝横线 */
        for (int y = 18; y < h - SHADOW - 10; y += 24) {
            sk_fill_circle_aa(c, 13, (float)y, 4.2f, sk_mix(SK_PAPER_DARK, SK_SHADOW, 90));
            sk_circle_aa(c, 13, (float)y, 4.2f, 1.0f, SK_INK_FAINT);
        }
        sk_vline(c, 26, 4, h - SHADOW - 5, sk_mix(SK_PAPER, SK_RED, 140));
        sk_draw_icon(c, SK_ICON_SKETCH_LOGO, 36, 14, 40, MENU_SEED + 3);
        sk_font *tf = sk_font_get(SK_FONT_TITLE);
        sk_draw_text(c, tf, 86, 36, "SketchOS", SK_INK);
        sk_draw_text(c, ui, 86, 58, "手绘操作系统", SK_INK_SOFT);
        sk_rough_underline(c, 84, 84 + (float)sk_text_width(tf, "SketchOS") + 6, 42,
                           SK_RGBA(0xE0, 0x60, 0x5A, 0xB0), 1.6f, MENU_SEED + 4);
    }
    sk_color rule = sk_mix(SK_PAPER, SK_DESK_GRID, 200);
    for (int i = 0; i < g_nitems; i++) {
        const mitem *m = &g_items[i];
        if (!m->label) {
            sk_draw_separator(c, m->r.x + 4, m->r.x + m->r.w - 4, m->r.y + m->r.h / 2, MENU_SEED + 50 + (uint32_t)i);
            continue;
        }
        if (g_menu_is_start)
            sk_hline(c, 27, w - SHADOW - 4, m->r.y + m->r.h - 1, rule);
        if (i == g_sel || i == g_pressed)
            sk_marker_highlight(c, sk_rect_inset(m->r, 0, 3), SK_HIGHLIGHT, MENU_SEED + 100 + (uint32_t)i);
        int isz = g_menu_is_start ? 26 : 22;
        sk_draw_icon(c, m->icon, m->r.x + 8, m->r.y + (m->r.h - isz) / 2, isz,
                     MENU_SEED + 200 + (uint32_t)i);
        sk_rect tr = sk_rect_make(m->r.x + isz + 18, m->r.y, m->r.w - isz - 22, m->r.h);
        sk_color col = (m->action == ACT_POWEROFF && i == g_sel) ? SK_RED : SK_INK;
        sk_draw_text_in_rect(c, ui, tr, m->label, col, SK_ALIGN_LEFT);
    }
    comp_damage(g_menu.r);
}

static void close_menu(void)
{
    if (!g_menu.visible)
        return;
    popup_hide(&g_menu);
    if (g_menu_is_start)
        taskbar_set_start_pressed(0);
    g_sel = g_pressed = -1;
    wm_refresh_cursor();
}

static void activate_item(int i)
{
    if (i < 0 || i >= g_nitems || !g_items[i].label)
        return;
    mitem m = g_items[i];
    close_menu();
    char msg[96];
    switch (m.action) {
    case ACT_APP: {
        const skd_app *a = apps_get(m.app);
        if (a && launch_app(a->id, NULL) > 0) {
            snprintf(msg, sizeof msg, "正在打开「%s」…", a->name);
            menu_toast(msg, 1500);
        }
        break;
    }
    case ACT_REBOOT:    menu_confirm_power(1); break;
    case ACT_POWEROFF:  menu_confirm_power(0); break;
    case ACT_TERMINAL:  launch_app("terminal", NULL); break;
    case ACT_WALLPAPER: desktop_cycle_wallpaper(); break;
    case ACT_ABOUT:     launch_app("about", NULL); break;
    }
}

static int item_at(int x, int y)
{
    if (!g_menu.visible || !sk_rect_contains(g_menu.r, x, y))
        return -1;
    int lx = x - g_menu.r.x, ly = y - g_menu.r.y;
    for (int i = 0; i < g_nitems; i++)
        if (g_items[i].label && sk_rect_contains(g_items[i].r, lx, ly))
            return i;
    return -1;
}

/* ---------- 确认框 ---------- */

#define CONF_W 360
#define CONF_H 172

static void render_confirm(void)
{
    if (!g_confirm.visible || !g_confirm.c)
        return;
    sk_canvas *c = g_confirm.c;
    sk_reset_clip(c);
    sk_clear(c, SK_WITH_ALPHA(SK_DESK_PAPER, 0));
    paper_card(c, CONF_W, CONF_H, SK_PAPER, MENU_SEED + 300);
    sk_draw_icon(c, g_confirm_reboot ? SK_ICON_REBOOT : SK_ICON_POWER, 24, 26, 44, MENU_SEED + 301);
    sk_font *tf = sk_font_get(SK_FONT_TITLE), *ui = sk_font_get(SK_FONT_UI);
    const char *q = g_confirm_reboot ? "确定要重新启动吗？" : "确定要关机吗？";
    sk_draw_text(c, tf, 84, 48, q, SK_INK);
    sk_draw_text(c, ui, 84, 74, "打开的应用都会被关闭。", SK_INK_SOFT);
    const char *labels[2] = { g_confirm_reboot ? "重新启动" : "关机", "取消" };
    sk_color accents[2] = { SK_RED, SK_BLUE };
    for (int i = 0; i < 2; i++) {
        int st = 0;
        if (g_confirm_hover == i) st |= SK_STATE_HOVER;
        if (g_confirm_pressed == i) st |= SK_STATE_PRESSED;
        if (g_confirm_focus == i) st |= SK_STATE_FOCUSED;
        sk_draw_button(c, g_cbtn[i], labels[i], st, accents[i], MENU_SEED + 310 + (uint32_t)i);
    }
    comp_damage(g_confirm.r);
}

void menu_confirm_power(int reboot)
{
    close_menu();
    sk_rect r = sk_rect_make((g_w - CONF_W) / 2, (g_h - SKD_TASKBAR_H - CONF_H) / 2, CONF_W, CONF_H);
    int was_visible = g_confirm.visible;
    if (popup_alloc(&g_confirm, r) < 0)
        return;
    g_confirm_reboot = reboot;
    g_confirm_focus = 0;
    g_confirm_hover = g_confirm_pressed = -1;
    g_cbtn[0] = sk_rect_make(CONF_W - SHADOW - 236, CONF_H - SHADOW - 58, 108, 38);
    g_cbtn[1] = sk_rect_make(CONF_W - SHADOW - 116, CONF_H - SHADOW - 58, 96, 38);
    if (!was_visible)
        comp_damage_all();       /* 打开时整个屏幕要盖上一层淡淡的灰（见 menu_paint） */
    render_confirm();
    wm_cancel_grabs();
    wm_refresh_cursor();
}

static void close_confirm(void)
{
    if (!g_confirm.visible)
        return;
    popup_hide(&g_confirm);
    comp_damage_all();
    wm_refresh_cursor();
}

static void confirm_activate(int which)
{
    int reboot = g_confirm_reboot;
    close_confirm();
    if (which != 0)
        return;
    /* 关机/重启由 1 号进程 sinit 负责：它会先通知所有服务退出、卸载文件系统，再调用 reboot()。
     * 约定：SIGUSR2 = 关机，SIGTERM = 重启（SIGINT 是 Ctrl+Alt+Delete，也是重启） */
    int sig = reboot ? SIGTERM : SIGUSR2;
    sk_log("menu: 请求 sinit %s", reboot ? "重新启动" : "关机");
    menu_toast(reboot ? "正在重新启动…" : "正在关机…", 5000);
    if (kill(1, sig) < 0) {
        sk_log("menu: kill(1, %d) 失败: %s", sig, strerror(errno));
        menu_toast("无法通知 sinit（不是 1 号进程？）", 3000);
    }
}

static int confirm_button_at(int x, int y)
{
    if (!g_confirm.visible)
        return -1;
    int lx = x - g_confirm.r.x, ly = y - g_confirm.r.y;
    for (int i = 0; i < 2; i++)
        if (sk_rect_contains(g_cbtn[i], lx, ly))
            return i;
    return -1;
}

/* ---------- Alt+Tab 切换器 ---------- */

#define SW_W    400
#define SW_ROW  38
#define SW_MAXROWS 10

void menu_switcher_show(skd_window **wins, int n, int sel)
{
    int rows = SK_MIN(n, SW_MAXROWS);
    int first = 0;
    if (sel >= rows)
        first = sel - rows + 1;
    int h = 56 + rows * SW_ROW + 14 + SHADOW;
    sk_rect r = sk_rect_make((g_w - SW_W) / 2, (g_h - SKD_TASKBAR_H - h) / 2, SW_W, h);
    if (popup_alloc(&g_switch, r) < 0)
        return;
    sk_canvas *c = g_switch.c;
    paper_card(c, SW_W, h, SK_PAPER, MENU_SEED + 400);
    sk_font *tf = sk_font_get(SK_FONT_TITLE), *ui = sk_font_get(SK_FONT_UI);
    sk_draw_text(c, tf, 22, 36, "切换窗口", SK_INK);
    sk_rough_underline(c, 20, 20 + (float)sk_text_width(tf, "切换窗口") + 6, 42,
                       SK_RGBA(0x4A, 0x7B, 0xD0, 0xB0), 1.6f, MENU_SEED + 401);
    sk_draw_text(c, ui, SW_W - SHADOW - 150, 36, "按住 Alt，按 Tab", SK_INK_FAINT);
    for (int i = 0; i < rows; i++) {
        int k = first + i;
        skd_window *w = wins[k];
        sk_rect row = sk_rect_make(16, 52 + i * SW_ROW, SW_W - SHADOW - 32, SW_ROW);
        if (k == sel)
            sk_marker_highlight(c, sk_rect_inset(row, 0, 3), SK_HIGHLIGHT, MENU_SEED + 410 + (uint32_t)k);
        sk_draw_icon(c, w->icon, row.x + 8, row.y + (SW_ROW - 24) / 2, 24, w->seed);
        sk_rect tr = sk_rect_make(row.x + 44, row.y, row.w - 50, row.h);
        char t[SK_TITLE_MAX + 32];
        snprintf(t, sizeof t, "%s%s", w->title[0] ? w->title : w->app, w->minimized ? "（已最小化）" : "");
        sk_draw_text_in_rect(c, ui, tr, t, k == sel ? SK_INK : SK_INK_SOFT, SK_ALIGN_LEFT);
    }
}

void menu_switcher_hide(void) { popup_hide(&g_switch); }

/* ---------- 提示条 ---------- */

void menu_toast(const char *text, int ms)
{
    sk_font *ui = sk_font_get(SK_FONT_UI);
    int tw = sk_text_width(ui, text);
    int w = SK_MIN(tw + 64 + SHADOW, g_w - 20), h = 44 + SHADOW;
    sk_rect r = sk_rect_make(g_w - w - 14, g_h - SKD_TASKBAR_H - h - 16, w, h);
    if (popup_alloc(&g_toast, r) < 0)
        return;
    sk_canvas *c = g_toast.c;
    paper_card(c, w, h, sk_mix(SK_PAPER, SK_YELLOW, 90), MENU_SEED + 500);
    /* 顶上一条透明胶带 */
    sk_rect tape = sk_rect_make(w / 2 - 18, 0, 36, 9);
    sk_fill_rect(c, tape, SK_RGBA(0xFF, 0xFF, 0xF4, 0xB0));
    sk_rect_outline(c, tape, SK_RGBA(0xA0, 0xA0, 0xA0, 0x70));
    sk_draw_icon(c, SK_ICON_PENCIL, 12, (h - SHADOW - 22) / 2 + 1, 22, MENU_SEED + 501);
    sk_draw_text_in_rect(c, ui, sk_rect_make(40, 2, w - SHADOW - 46, h - SHADOW - 4), text, SK_INK,
                         SK_ALIGN_LEFT);
    g_toast_until = skd_now_ms() + (uint64_t)(ms > 0 ? ms : 2000);
}

int menu_timeout_ms(void)
{
    if (!g_toast.visible)
        return -1;
    uint64_t now = skd_now_ms();
    return now >= g_toast_until ? 0 : (int)(g_toast_until - now);
}

void menu_tick(void)
{
    if (g_toast.visible && skd_now_ms() >= g_toast_until)
        popup_hide(&g_toast);
}

/* ---------- 合成与输入 ---------- */

void menu_paint(sk_canvas *dst, sk_rect r)
{
    popup_blit(&g_menu, dst, r);
    if (g_confirm.visible) {
        /* 模态确认框：整个屏幕蒙上一层很淡的铅笔灰，突出中间的问题 */
        sk_fill_rect(dst, r, SK_RGBA(0x2B, 0x2B, 0x2B, 0x38));
        popup_blit(&g_confirm, dst, r);
    }
    popup_blit(&g_switch, dst, r);
    popup_blit(&g_toast, dst, r);
}

int menu_is_open(void) { return g_menu.visible || g_confirm.visible; }

void menu_close_all(void)
{
    close_menu();
    close_confirm();
}

void menu_toggle_start(void)
{
    if (g_confirm.visible)
        return;
    if (g_menu.visible && g_menu_is_start) {
        close_menu();
        return;
    }
    close_menu();
    wm_cancel_grabs();
    open_start();
    wm_refresh_cursor();
}

void menu_open_desktop(int x, int y)
{
    if (g_confirm.visible)
        return;
    close_menu();
    open_context(x, y);
    wm_refresh_cursor();
}

int menu_pointer_motion(int x, int y)
{
    if (g_confirm.visible) {
        int b = confirm_button_at(x, y);
        if (b != g_confirm_hover) {
            g_confirm_hover = b;
            render_confirm();
        }
        return 1;
    }
    if (!g_menu.visible)
        return 0;
    int i = item_at(x, y);
    if (i != g_sel && (i >= 0 || sk_rect_contains(g_menu.r, x, y) || g_pressed < 0)) {
        g_sel = i;
        render_menu();
    }
    return sk_rect_contains(g_menu.r, x, y);
}

int menu_pointer_down(int x, int y, uint32_t button)
{
    if (g_confirm.visible) {
        g_confirm_pressed = button == SK_BUTTON_LEFT ? confirm_button_at(x, y) : -1;
        render_confirm();
        return 1;     /* 模态：点哪里都不会穿透到下面 */
    }
    if (!g_menu.visible)
        return 0;
    if (!sk_rect_contains(g_menu.r, x, y)) {
        /* 点菜单外面 = 关闭菜单（这次点击被“吃掉”，不会误点到下面的东西） */
        close_menu();
        return 1;
    }
    g_pressed = item_at(x, y);
    g_sel = g_pressed;
    render_menu();
    return 1;
}

int menu_pointer_up(int x, int y, uint32_t button)
{
    if (g_confirm.visible) {
        int b = confirm_button_at(x, y), was = g_confirm_pressed;
        g_confirm_pressed = -1;
        render_confirm();
        if (button == SK_BUTTON_LEFT && b >= 0 && b == was)
            confirm_activate(b);
        return 1;
    }
    if (!g_menu.visible)
        return 0;
    int i = item_at(x, y), was = g_pressed;
    g_pressed = -1;
    if (i >= 0 && i == was)
        activate_item(i);
    else
        render_menu();
    return 1;
}

static int next_item(int from, int dir)
{
    for (int k = 0; k < g_nitems; k++) {
        from = (from + dir + g_nitems) % g_nitems;
        if (g_items[from].label)
            return from;
    }
    return -1;
}

int menu_key(uint32_t keycode, uint32_t state, uint32_t mods)
{
    (void)mods;
    if (!menu_is_open())
        return 0;
    if (state == 0)
        return 0;        /* 松开事件照常交给应用，避免应用以为某个键还按着 */
    if (g_confirm.visible) {
        switch (keycode) {
        case KEY_LEFT: case KEY_RIGHT: case KEY_TAB:
            g_confirm_focus = !g_confirm_focus;
            render_confirm();
            break;
        case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
            if (state == 1)
                confirm_activate(g_confirm_focus);
            break;
        case KEY_ESC:
            close_confirm();
            break;
        default:
            break;
        }
        return 1;
    }
    switch (keycode) {
    case KEY_UP:
        g_sel = next_item(g_sel < 0 ? 0 : g_sel, -1);
        render_menu();
        break;
    case KEY_DOWN:
        g_sel = next_item(g_sel < 0 ? g_nitems - 1 : g_sel, 1);
        render_menu();
        break;
    case KEY_HOME:
        g_sel = next_item(g_nitems - 1, 1);
        render_menu();
        break;
    case KEY_END:
        g_sel = next_item(0, -1);
        render_menu();
        break;
    case KEY_ENTER: case KEY_KPENTER:
        if (state == 1 && g_sel >= 0)
            activate_item(g_sel);
        break;
    case KEY_ESC:
        close_menu();
        break;
    default:
        break;     /* 菜单打开时其它按键都吞掉，不发给后面的窗口 */
    }
    return 1;
}

void menu_init(int w, int h)
{
    g_w = w;
    g_h = h;
}

void menu_shutdown(void)
{
    popup_hide(&g_menu);
    popup_hide(&g_confirm);
    popup_hide(&g_switch);
    popup_hide(&g_toast);
}
