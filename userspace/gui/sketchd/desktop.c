/*
 * desktop.c — 桌面：纸张壁纸、铅笔涂鸦、手写签名、左侧一列应用图标
 *
 * 整个桌面背景（纸 + 格线 + 涂鸦 + 图标）预先渲染进一张整屏大小的画布 g_bg，
 * 合成时直接拷贝（memcpy），不在每一帧重画。只有图标选中状态变了，才重画那一个图标格子；
 * 换壁纸时才整张重画。“预渲染 + 局部更新”是慢机器上做漂亮界面的基本功。
 *
 * 局部重画的关键：每个绘制步骤都必须“只由坐标决定”。纸张颗粒噪点由 (seed, x, y) 决定，
 * 手绘线条由各自固定的 seed 决定，所以重画一个小矩形时得到的像素和整张重画完全一样，
 * 不会出现“补丁”的痕迹。
 */
#include "sketchd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BLEED      6      /* 手绘线条、荧光笔会画出格子外面一点 */
#define CELL_W     92
#define CELL_H     88
#define ICON_SIZE  48
#define GRID       32
#define DESK_SEED  0xDE5C7A11u

enum { WP_GRID = 0, WP_RULED, WP_BLANK, WP_KRAFT, WP_COUNT };
static const char *const g_wp_names[WP_COUNT] = { "方格纸", "横线本", "白纸", "牛皮纸" };

static sk_canvas *g_bg;
static int        g_w, g_h;
static int        g_wallpaper = WP_GRID;
static int        g_sel = -1;
static sk_rect    g_cells[SKD_MAX_APPS];
static int        g_ncells;

const char *desktop_wallpaper_name(void) { return g_wp_names[g_wallpaper]; }

static int intersects(sk_rect a, sk_rect b) { return !sk_rect_empty(sk_rect_intersect(a, b)); }

/* ---------- 纸 ---------- */

static void paint_paper(sk_canvas *c, sk_rect r)
{
    if (g_wallpaper == WP_KRAFT) {
        sk_paper_fill(c, r, SKD_KRAFT, 16, DESK_SEED);
        return;
    }
    sk_paper_fill(c, r, SK_DESK_PAPER, 6, DESK_SEED);
    if (g_wallpaper == WP_GRID) {
        /* 方格纸：每 32px 一条淡蓝线，每 4 格一条稍深的“主线” */
        sk_color major = sk_mix(SK_DESK_GRID, SK_BLUE, 38);
        for (int x = (r.x / GRID) * GRID; x < r.x + r.w; x += GRID)
            if (x >= r.x)
                sk_vline(c, x, r.y, r.y + r.h - 1, (x / GRID) % 4 == 0 ? major : SK_DESK_GRID);
        for (int y = (r.y / GRID) * GRID; y < r.y + r.h; y += GRID)
            if (y >= r.y)
                sk_hline(c, r.x, r.x + r.w - 1, y, (y / GRID) % 4 == 0 ? major : SK_DESK_GRID);
    } else if (g_wallpaper == WP_RULED) {
        /* 横线本：浅蓝横线 + 左边一条红色页边线（图标正好在页边空白里） */
        sk_color line = sk_mix(SK_DESK_GRID, SK_BLUE, 60);
        for (int y = ((r.y - 8) / GRID) * GRID + 8; y < r.y + r.h; y += GRID)
            if (y >= r.y && y > 64)
                sk_hline(c, r.x, r.x + r.w - 1, y, line);
        sk_color margin = sk_mix(SK_PAPER, SK_RED, 150);
        int mx = 14 + CELL_W + 10;
        if (mx >= r.x && mx < r.x + r.w) {
            sk_vline(c, mx, r.y, r.y + r.h - 1, margin);
            sk_vline(c, mx + 3, r.y, r.y + r.h - 1, sk_mix(SK_PAPER, SK_RED, 90));
        }
    }
}

/* ---------- 涂鸦 ---------- */

static void style(sk_rough_style *s, sk_color stroke, float width, uint32_t seed)
{
    sk_rough_default(s);
    s->stroke = stroke;
    s->stroke_width = width;
    s->seed = seed;
}

/* 右上角：一个排线涂色的小太阳，旁边一朵云 */
static sk_rect sun_box(void) { return sk_rect_make(g_w - 250, 30, 220, 130); }

static void paint_sun(sk_canvas *c)
{
    float cx = (float)(g_w - 118), cy = 86, d = 52;
    sk_rough_style s;
    style(&s, SK_RGBA(0xF2, 0x9E, 0x4C, 0xE0), 1.6f, DESK_SEED + 11);
    s.fill_style = SK_FILL_HACHURE;
    s.fill = SK_RGBA(0xF2, 0xC9, 0x4C, 0xC0);
    s.hachure_gap = 4.5f;
    s.hachure_angle = -50;
    s.fill_weight = 1.3f;
    sk_rough_circle(c, cx, cy, d, &s);
    /* 光芒：12 根长短交替的短线 */
    style(&s, SK_RGBA(0xF2, 0x9E, 0x4C, 0xD0), 1.5f, DESK_SEED + 12);
    s.double_stroke = 0;
    static const float dirs[12][2] = {
        { 1, 0 }, { 0.866f, 0.5f }, { 0.5f, 0.866f }, { 0, 1 }, { -0.5f, 0.866f }, { -0.866f, 0.5f },
        { -1, 0 }, { -0.866f, -0.5f }, { -0.5f, -0.866f }, { 0, -1 }, { 0.5f, -0.866f }, { 0.866f, -0.5f },
    };
    for (int i = 0; i < 12; i++) {
        float r0 = d / 2 + 7, r1 = d / 2 + ((i % 2) ? 15 : 21);
        s.seed = DESK_SEED + 100 + (uint32_t)i;
        sk_rough_line(c, cx + dirs[i][0] * r0, cy + dirs[i][1] * r0,
                      cx + dirs[i][0] * r1, cy + dirs[i][1] * r1, &s);
    }
    /* 云：先用纸色实心盖住太阳的一角，再用平滑曲线描出鼓鼓的轮廓 */
    float ox = (float)(g_w - 226), oy = 104;
    static const sk_pointf cloud[] = {
        { 8, 34 }, { 4, 24 }, { 14, 16 }, { 24, 18 }, { 30, 6 }, { 46, 2 }, { 58, 10 },
        { 66, 4 }, { 82, 8 }, { 88, 20 }, { 98, 24 }, { 98, 34 }, { 90, 40 }, { 14, 40 },
    };
    sk_pointf pts[SK_ARRAY_LEN(cloud) + 1];
    for (size_t i = 0; i < SK_ARRAY_LEN(cloud); i++) {
        pts[i].x = ox + cloud[i].x;
        pts[i].y = oy + cloud[i].y;
    }
    pts[SK_ARRAY_LEN(cloud)] = pts[0];
    sk_fill_polygon(c, pts, (int)SK_ARRAY_LEN(cloud), sk_mix(SK_DESK_PAPER, SK_PAPER, 128));
    style(&s, SK_RGBA(0x4A, 0x7B, 0xD0, 0xB0), 1.5f, DESK_SEED + 13);
    s.roughness = 0.7f;
    sk_rough_curve(c, pts, (int)SK_ARRAY_LEN(cloud) + 1, &s);
    /* 云底下的两道淡蓝排线，像铅笔随手涂的阴影 */
    style(&s, SK_RGBA(0x4A, 0x7B, 0xD0, 0x70), 1.2f, DESK_SEED + 14);
    s.double_stroke = 0;
    sk_rough_line(c, ox + 22, oy + 46, ox + 70, oy + 46, &s);
    s.seed++;
    sk_rough_line(c, ox + 34, oy + 51, ox + 62, oy + 51, &s);
}

/* 顶部中间：一架纸飞机，后面拖着一条打了个圈的虚线轨迹 */
static sk_rect plane_box(void) { return sk_rect_make(g_w / 2 - 250, 24, 380, 160); }

static void paint_plane(sk_canvas *c)
{
    float px = (float)(g_w / 2 + 40), py = 70;
    /* 轨迹：从左下方飞来、中途翻了一个跟头的曲线，用一小段一小段的铅笔画成虚线 */
    enum { NPTS = 120 };
    sk_pointf path[NPTS];
    for (int i = 0; i < NPTS; i++) {
        float t = (float)i / (NPTS - 1);
        float x = (px - 262) + t * 250;
        float y = py + 92 - t * 66 - 10.0f * sinf(t * 3.1416f);
        /* t 在 0.38..0.62 之间画一个圈（圆周运动叠加在直线上） */
        float lt = (t - 0.38f) / 0.24f;
        if (lt > 0 && lt < 1) {
            float a = lt * 6.2832f;
            x -= 24.0f * sinf(a);
            y -= 24.0f * (1.0f - cosf(a));
        }
        path[i].x = x;
        path[i].y = y;
    }
    for (int i = 0; i + 3 < NPTS; i += 5) {
        sk_pointf seg[4] = { path[i], path[i + 1], path[i + 2], path[i + 3] };
        sk_brush_stroke(c, seg, 4, 1.4f, 1.0f, SK_RGBA(0x5C, 0x5C, 0x5C, 0x88), 1, DESK_SEED + 40 + (uint32_t)i);
    }
    /* 飞机本体（机头朝右上）：上翼排线涂蓝，下翼留白，中间一道折痕 */
    sk_pointf nose = { px + 66, py - 8 }, tail = { px - 6, py + 18 }, fold = { px + 22, py + 26 },
              bottom = { px + 30, py + 46 }, keel = { px + 26, py + 34 };
    sk_pointf wing1[] = { nose, tail, fold };
    sk_pointf wing2[] = { nose, fold, bottom };
    sk_rough_style s;
    style(&s, SK_RGBA(0x2B, 0x2B, 0x2B, 0xC8), 1.5f, DESK_SEED + 22);
    s.fill_style = SK_FILL_SOLID;
    s.fill = SK_PAPER;
    sk_rough_polygon(c, wing2, 3, &s);
    style(&s, SK_RGBA(0x2B, 0x2B, 0x2B, 0xC8), 1.5f, DESK_SEED + 21);
    s.fill_style = SK_FILL_HACHURE;
    s.fill = SK_RGBA(0x4A, 0x7B, 0xD0, 0x90);
    s.hachure_gap = 3.6f;
    s.hachure_angle = 25;
    sk_rough_polygon(c, wing1, 3, &s);
    style(&s, SK_RGBA(0x2B, 0x2B, 0x2B, 0xA0), 1.2f, DESK_SEED + 23);
    s.double_stroke = 0;
    sk_rough_line(c, fold.x, fold.y, keel.x, keel.y, &s);
}

/* 右下角：手写签名 “SketchOS” + 一道红色的飘逸下划线 + 小星星 */
static sk_rect sig_box(void)
{
    return sk_rect_make(g_w - 280, g_h - SKD_TASKBAR_H - 110, 270, 100);
}

static void paint_signature(sk_canvas *c)
{
    sk_font *big = sk_font_get(SK_FONT_LARGE);
    sk_font *ui = sk_font_get(SK_FONT_UI);
    const char *name = "SketchOS";
    int tw = sk_text_width(big, name);
    int x = g_w - 48 - tw;
    int base = g_h - SKD_TASKBAR_H - 52;
    sk_color ink = g_wallpaper == WP_KRAFT ? SK_RGBA(0x2B, 0x2B, 0x2B, 0xB0) : SK_RGBA(0x5C, 0x5C, 0x5C, 0xB8);
    sk_draw_text(c, big, x, base, name, ink);
    sk_rough_underline(c, (float)(x - 6), (float)(x + tw + 10), (float)(base + 9),
                       SK_RGBA(0xE0, 0x60, 0x5A, 0xC0), 2.2f, DESK_SEED + 31);
    const char *sub = "手绘操作系统 · 用来学习的小系统";
    int sw = sk_text_width(ui, sub);
    sk_draw_text(c, ui, x + tw - sw + 8, base + 32, sub, SK_RGBA(0x5C, 0x5C, 0x5C, 0x90));
    /* 五角星 */
    float sx = (float)(x - 22), sy = (float)(base - 22), R = 9, r = 4;
    sk_pointf star[10];
    for (int i = 0; i < 10; i++) {
        float a = -1.5708f + (float)i * 0.6283f;
        float rad = (i % 2) ? r : R;
        star[i].x = sx + rad * cosf(a);
        star[i].y = sy + rad * sinf(a);
    }
    sk_rough_style s;
    style(&s, SK_RGBA(0xF2, 0x9E, 0x4C, 0xD0), 1.3f, DESK_SEED + 32);
    s.fill_style = SK_FILL_HACHURE;
    s.fill = SK_RGBA(0xF2, 0xC9, 0x4C, 0xC0);
    s.hachure_gap = 3;
    sk_rough_polygon(c, star, 10, &s);
}

/* ---------- 图标 ---------- */

static void layout_icons(void)
{
    int rows = (g_h - SKD_TASKBAR_H - 20) / CELL_H;
    if (rows < 1)
        rows = 1;
    g_ncells = apps_count();
    for (int i = 0; i < g_ncells; i++)
        g_cells[i] = sk_rect_make(14 + (i / rows) * (CELL_W + 8), 14 + (i % rows) * CELL_H, CELL_W, CELL_H);
}

static void paint_icon(sk_canvas *c, int i)
{
    const skd_app *a = apps_get(i);
    if (!a)
        return;
    sk_rect cell = g_cells[i];
    uint32_t seed = sk_hash_str(a->id);
    if (i == g_sel)
        sk_marker_highlight(c, sk_rect_inset(cell, 4, 4), SK_HIGHLIGHT, seed);
    int ix = cell.x + (cell.w - ICON_SIZE) / 2, iy = cell.y + 6;
    sk_draw_icon(c, a->icon, ix, iy, ICON_SIZE, seed);
    sk_font *f = sk_font_get(SK_FONT_UI);
    sk_rect lr = sk_rect_make(cell.x + 2, iy + ICON_SIZE + 6, cell.w - 4, sk_font_line_height(f) + 2);
    sk_draw_text_in_rect(c, f, lr, a->name, SK_INK, SK_ALIGN_CENTER);
}

/* ---------- 渲染与接口 ---------- */

static void render(sk_rect r)
{
    r = sk_rect_intersect(r, sk_rect_make(0, 0, g_w, g_h));
    if (sk_rect_empty(r) || !g_bg)
        return;
    sk_set_clip(g_bg, r);
    paint_paper(g_bg, r);
    if (intersects(r, sun_box()))
        paint_sun(g_bg);
    if (intersects(r, plane_box()))
        paint_plane(g_bg);
    if (intersects(r, sig_box()))
        paint_signature(g_bg);
    for (int i = 0; i < g_ncells; i++)
        if (intersects(r, sk_rect_inset(g_cells[i], -BLEED, -BLEED)))
            paint_icon(g_bg, i);
    sk_reset_clip(g_bg);
}

void desktop_init(int w, int h)
{
    g_w = w;
    g_h = h;
    g_bg = sk_canvas_new(w, h);
    if (!g_bg) {
        sk_log("desktop: 背景画布分配失败");
        return;
    }
    layout_icons();
    render(sk_rect_make(0, 0, w, h));
}

void desktop_shutdown(void)
{
    if (g_bg)
        sk_canvas_free(g_bg);
    g_bg = NULL;
}

void desktop_paint(sk_canvas *dst, sk_rect r)
{
    if (g_bg)
        sk_blit(dst, r.x, r.y, g_bg, r);
    else
        sk_fill_rect(dst, r, SK_DESK_PAPER);
}

static void select_icon(int i)
{
    if (i == g_sel)
        return;
    int old = g_sel;
    g_sel = i;
    if (old >= 0) {
        sk_rect r = sk_rect_inset(g_cells[old], -BLEED, -BLEED);
        render(r);
        comp_damage(r);
    }
    if (i >= 0) {
        sk_rect r = sk_rect_inset(g_cells[i], -BLEED, -BLEED);
        render(r);
        comp_damage(r);
    }
}

void desktop_clear_selection(void) { select_icon(-1); }

void desktop_pointer_down(int x, int y, uint32_t button, int clicks)
{
    int hit = -1;
    for (int i = 0; i < g_ncells; i++)
        if (sk_rect_contains(g_cells[i], x, y))
            hit = i;
    if (button == SK_BUTTON_LEFT) {
        select_icon(hit);
        if (hit >= 0 && clicks == 2) {
            const skd_app *a = apps_get(hit);
            char msg[96];
            snprintf(msg, sizeof msg, "正在打开「%s」…", a->name);
            if (launch_app(a->id, NULL) > 0)
                menu_toast(msg, 1500);
        }
    } else if (button == SK_BUTTON_RIGHT) {
        select_icon(hit);
        menu_open_desktop(x, y);
    }
}

void desktop_cycle_wallpaper(void)
{
    g_wallpaper = (g_wallpaper + 1) % WP_COUNT;
    render(sk_rect_make(0, 0, g_w, g_h));
    comp_damage_all();
    char msg[64];
    snprintf(msg, sizeof msg, "换成了%s", g_wp_names[g_wallpaper]);
    menu_toast(msg, 1500);
}
