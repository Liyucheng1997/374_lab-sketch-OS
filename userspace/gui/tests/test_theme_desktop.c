/*
 * test_theme_desktop.c — 用 gfx/rough/theme/icons 拼出一整张 1024×768 的“桌面”效果图
 *
 * 这不是 sketchd 的真实代码，只是用同样的零件摆出一个典型场景，检查整体风格是否协调：
 * 方格纸桌面 + 桌面图标、任务栏、开始菜单、终端（有焦点）、文件管理器、系统监视器。
 * 输出 theme_desktop.ppm。用法：test_theme_desktop [输出目录]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sketch/gfx.h"
#include "sketch/rough.h"
#include "sketch/theme.h"
#include "sketch/font.h"

static char g_out[512] = ".";

static void save(const sk_canvas *c, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_out, name);
    if (sk_image_save_ppm(c, path) != 0) {
        fprintf(stderr, "写入 %s 失败\n", path);
        exit(1);
    }
    printf("%s\n", path);
}

#define W 1024
#define H 768
#define TASKBAR_H 44

static void desktop_bg(sk_canvas *c, sk_rect r)
{
    sk_paper_fill(c, r, SK_DESK_PAPER, 5, 0xDE5Cu);
    sk_rect old = sk_push_clip(c, r);
    for (int x = 12; x < W; x += 24)
        sk_vline(c, x, 0, H - 1, SK_DESK_GRID);
    for (int y = 12; y < H; y += 24)
        sk_hline(c, 0, W - 1, y, SK_DESK_GRID);
    /* 笔记本左边的红色页边线 */
    sk_vline(c, 132, 0, H - 1, SK_WITH_ALPHA(SK_RED, 0x40));
    sk_pop_clip(c, old);
}

static void desk_icon(sk_canvas *c, int x, int y, int icon, const char *label, int selected, uint32_t seed)
{
    const sk_font *f = sk_font_get(SK_FONT_UI);
    if (selected) {
        sk_rough_style s;
        sk_style_pencil(&s, seed);
        s.stroke = SK_WITH_ALPHA(SK_BLUE, 0xC0);
        s.stroke_width = 1.3f;
        s.roughness = 0.6f;
        s.fill_style = SK_FILL_HACHURE;
        s.fill = SK_WITH_ALPHA(SK_BLUE, 0x50);
        s.hachure_gap = 4.5f;
        sk_rough_round_rect(c, x - 18, y - 6, 84, 84, 8, &s);
    }
    sk_draw_icon(c, icon, x, y, 48, seed);
    int tw = sk_text_width(f, label);
    sk_rect lr = sk_rect_make(x + 24 - tw / 2 - 4, y + 52, tw + 8, 20);
    if (selected)
        sk_marker_highlight(c, lr, SK_HIGHLIGHT, seed);
    sk_draw_text_in_rect(c, f, lr, label, SK_INK, SK_ALIGN_CENTER);
}

static void taskbar(sk_canvas *c)
{
    sk_rect tb = sk_rect_make(0, H - TASKBAR_H, W, TASKBAR_H);
    sk_paper_fill(c, tb, SK_PAPER_DARK, 5, 0x7A5Bu);
    sk_rough_style s;
    sk_style_pencil(&s, 0x7A5C);
    s.roughness = 0.6f;
    s.stroke_width = 1.6f;
    sk_rough_line(c, -4, H - TASKBAR_H + 0.5f, W + 4, H - TASKBAR_H + 0.5f, &s);
    /* 开始按钮 */
    sk_rect sb = sk_rect_make(6, H - TASKBAR_H + 5, 104, 34);
    sk_draw_button_frame(c, sb, SK_STATE_CHECKED, SK_YELLOW, 0x57A7);
    sk_draw_icon(c, SK_ICON_SKETCH_LOGO, sb.x + 8, sb.y + 5, 24, 1);
    sk_draw_text_in_rect(c, sk_font_get(SK_FONT_TITLE), sk_rect_make(sb.x + 36, sb.y, 64, sb.h), "开始", SK_INK, SK_ALIGN_LEFT);
    /* 任务按钮 */
    struct { int icon; const char *t; int st; } tasks[] = {
        { SK_ICON_TERMINAL, "终端", SK_STATE_CHECKED }, { SK_ICON_FOLDER, "文件", 0 },
        { SK_ICON_MONITOR, "系统监视器", SK_STATE_HOVER },
    };
    int x = 124;
    for (int i = 0; i < 3; i++) {
        sk_rect r = sk_rect_make(x, H - TASKBAR_H + 6, 150, 32);
        sk_draw_button_frame(c, r, tasks[i].st, SK_BLUE, 0x7A00 + i);
        sk_draw_icon(c, tasks[i].icon, r.x + 8, r.y + 6, 20, 2);
        sk_draw_text_in_rect(c, sk_font_get(SK_FONT_UI), sk_rect_make(r.x + 34, r.y, r.w - 40, r.h), tasks[i].t,
                             SK_INK, SK_ALIGN_LEFT);
        x += 156;
    }
    /* 托盘：时钟 */
    sk_draw_separator(c, W - 140, W - 140, H - TASKBAR_H + 8, 1);
    sk_rough_style v;
    sk_style_pencil(&v, 0x7AE0);
    v.stroke = SK_INK_FAINT;
    v.stroke_width = 1.2f;
    v.double_stroke = 0;
    sk_rough_line(c, W - 150, H - TASKBAR_H + 9, W - 150, H - 9, &v);
    sk_draw_icon(c, SK_ICON_CLOCK, W - 138, H - TASKBAR_H + 12, 20, 3);
    sk_draw_text_in_rect(c, sk_font_get(SK_FONT_TITLE), sk_rect_make(W - 112, H - TASKBAR_H, 100, TASKBAR_H),
                         "14:05", SK_INK, SK_ALIGN_LEFT);
}

static void terminal_content(sk_canvas *c, sk_rect r)
{
    sk_rect old = sk_push_clip(c, r);
    sk_paper_fill(c, r, SK_PAPER, 3, 0x7E7Au);
    const sk_font *m = sk_font_get(SK_FONT_MONO);
    int lh = sk_font_line_height(m);
    struct { const char *t; int col; } lines[] = {
        { "root@sketchos:~# uname -a", 0 },
        { "Linux sketchos 6.12.114 #1 SMP i686 GNU/Linux", 4 },
        { "root@sketchos:~# ls -l /root", 0 },
        { "-rw-r--r--  1 root root   812 笔记.txt", 7 },
        { "drwxr-xr-x  2 root root    60 画板", 12 },
        { "root@sketchos:~# cat /proc/meminfo | head -3", 0 },
        { "MemTotal:         126876 kB", 2 },
        { "MemFree:           98112 kB", 2 },
        { "MemAvailable:     110340 kB", 2 },
        { "root@sketchos:~# echo 你好，手绘世界", 0 },
        { "你好，手绘世界", 5 },
        { "root@sketchos:~# ", 0 },
    };
    int y = r.y + 6 + sk_font_ascent(m);
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        int x = sk_draw_text(c, m, r.x + 10, y, lines[i].t, sk_ansi_palette[lines[i].col]);
        if (i == sizeof lines / sizeof lines[0] - 1)
            sk_fill_rect(c, sk_rect_make(x, y - sk_font_ascent(m) + 2, 9, lh - 2), SK_WITH_ALPHA(SK_INK, 0xC0));
        y += lh;
    }
    sk_pop_clip(c, old);
}

static void files_content(sk_canvas *c, sk_rect r)
{
    sk_rect old = sk_push_clip(c, r);
    sk_paper_fill(c, r, SK_PAPER, 3, 0xF11Eu);
    const sk_font *f = sk_font_get(SK_FONT_UI);
    /* 工具栏 */
    sk_paper_fill(c, sk_rect_make(r.x, r.y, r.w, 42), SK_PAPER_DARK, 4, 0xF11Fu);
    int icons[4] = { SK_ICON_BACK, SK_ICON_UP, SK_ICON_HOME, SK_ICON_NEW_FOLDER };
    for (int i = 0; i < 4; i++) {
        sk_rect b = sk_rect_make(r.x + 6 + i * 40, r.y + 4, 34, 34);
        if (i == 2)
            sk_draw_button_frame(c, b, SK_STATE_HOVER, SK_GREEN, 0xB0 + i);
        sk_draw_icon(c, icons[i], b.x + 5, b.y + 5, 24, 4);
    }
    sk_rect path = sk_rect_make(r.x + 172, r.y + 6, r.w - 182, 30);
    sk_draw_textbox_frame(c, path, 0, 0x7A7Bu);
    sk_draw_text_in_rect(c, f, sk_rect_inset(path, 8, 0), "/root", SK_INK, SK_ALIGN_LEFT);
    sk_draw_separator(c, r.x + 4, r.x + r.w - 4, r.y + 43, 0x5E9);
    /* 列表 */
    struct { int icon; const char *n, *s; } rows[] = {
        { SK_ICON_FOLDER, "画板", "" }, { SK_ICON_FOLDER, "文档", "" },
        { SK_ICON_TEXT, "笔记.txt", "812 B" }, { SK_ICON_IMAGE, "小猫.ppm", "48.0 KB" },
        { SK_ICON_PROGRAM, "hello", "12.3 KB" }, { SK_ICON_BOOK, "开发规范.md", "9.1 KB" },
    };
    int y = r.y + 50;
    for (int i = 0; i < 6; i++) {
        sk_rect row = sk_rect_make(r.x + 6, y, r.w - 30, 28);
        if (i == 2)
            sk_marker_highlight(c, row, SK_HIGHLIGHT, 0x5E1);
        sk_draw_icon(c, rows[i].icon, row.x + 6, row.y + 4, 20, 5);
        sk_draw_text_in_rect(c, f, sk_rect_make(row.x + 34, row.y, 180, row.h), rows[i].n, SK_INK, SK_ALIGN_LEFT);
        sk_draw_text_in_rect(c, f, sk_rect_make(row.x + row.w - 90, row.y, 84, row.h), rows[i].s, SK_INK_SOFT,
                             SK_ALIGN_RIGHT);
        y += 30;
    }
    sk_draw_scrollbar(c, sk_rect_make(r.x + r.w - 18, r.y + 50, 14, r.h - 56), 12, 6, 2, 0, 0x5C2);
    sk_pop_clip(c, old);
}

static void monitor_content(sk_canvas *c, sk_rect r)
{
    sk_rect old = sk_push_clip(c, r);
    sk_paper_fill(c, r, SK_PAPER, 3, 0x30Fu);
    const sk_font *f = sk_font_get(SK_FONT_UI);
    const char *tabs[] = { "概览", "进程", "内存" };
    sk_draw_tabs(c, sk_rect_make(r.x + 6, r.y + 4, r.w - 12, 34), tabs, 3, 0, 1, 0x7AB);
    sk_draw_text(c, f, r.x + 14, r.y + 64, "处理器", SK_INK);
    sk_draw_meter(c, sk_rect_make(r.x + 80, r.y + 48, r.w - 96, 22), 370, SK_PURPLE, 0x3E1);
    sk_draw_text_in_rect(c, f, sk_rect_make(r.x + 80, r.y + 48, r.w - 96, 22), "37%", SK_INK, SK_ALIGN_CENTER);
    sk_draw_text(c, f, r.x + 14, r.y + 96, "内存", SK_INK);
    sk_draw_meter(c, sk_rect_make(r.x + 80, r.y + 80, r.w - 96, 22), 620, SK_GREEN, 0x3E2);
    sk_draw_text_in_rect(c, f, sk_rect_make(r.x + 80, r.y + 80, r.w - 96, 22), "77 / 124 MB", SK_INK, SK_ALIGN_CENTER);
    /* 曲线图 */
    sk_rect g = sk_rect_make(r.x + 14, r.y + 116, r.w - 28, r.h - 128);
    sk_rough_style s;
    sk_style_pencil(&s, 0x6A1);
    s.stroke = SK_INK_SOFT;
    s.stroke_width = 1.2f;
    s.roughness = 0.5f;
    s.double_stroke = 0;
    sk_rough_rect(c, g.x, g.y, g.w, g.h, &s);
    sk_pointf pts[24];
    int n = 0;
    for (int i = 0; i < 24; i++) {
        float t = (float)i / 23.0f;
        float v = 0.5f + 0.3f * __builtin_sinf(t * 9.0f) * __builtin_cosf(t * 3.0f) + 0.1f * ((i * 7) % 5) / 5.0f;
        pts[n].x = g.x + 4 + t * (g.w - 8);
        pts[n].y = g.y + g.h - 4 - v * (g.h - 8);
        n++;
    }
    sk_rough_style area;
    sk_style_hatched(&area, SK_WITH_ALPHA(SK_PURPLE, 0x90), 0x6A2);
    area.stroke_width = 0;
    area.hachure_gap = 4;
    sk_pointf poly[26];
    memcpy(poly, pts, sizeof pts);
    poly[24].x = pts[23].x; poly[24].y = g.y + g.h - 3;
    poly[25].x = pts[0].x;  poly[25].y = g.y + g.h - 3;
    sk_rough_fill_polygon(c, poly, 26, &area);
    sk_brush_stroke(c, pts, n, 2.2f, 2.2f, SK_PURPLE, 0, 9);
    sk_pop_clip(c, old);
}

static void start_menu(sk_canvas *c)
{
    sk_rect m = sk_rect_make(6, H - TASKBAR_H - 330, 230, 324);
    sk_paper_fill(c, m, SK_PAPER, 4, 0x3E9u);
    sk_rough_style s;
    sk_style_pencil(&s, 0x3EA);
    s.roughness = 0.7f;
    s.stroke_width = 1.6f;
    /* 菜单的阴影 */
    sk_rough_style sh;
    sk_style_hatched(&sh, SK_SHADOW, 0x3EB);
    sh.stroke_width = 0;
    sh.hachure_gap = 3.6f;
    sh.fill_weight = 1.1f;
    sh.hachure_angle = -50;
    sk_pointf L[6] = { { m.x + m.w + 1, m.y + 6 }, { m.x + m.w + 6, m.y + 6 }, { m.x + m.w + 6, m.y + m.h + 6 },
                       { m.x + 6, m.y + m.h + 6 }, { m.x + 6, m.y + m.h + 1 }, { m.x + m.w + 1, m.y + m.h + 1 } };
    sk_rough_fill_polygon(c, L, 6, &sh);
    sk_rough_rect(c, m.x + 0.5f, m.y + 0.5f, m.w - 1, m.h - 1, &s);
    struct { int icon; const char *t; } items[] = {
        { SK_ICON_TERMINAL, "终端" }, { SK_ICON_FOLDER, "文件" }, { SK_ICON_NOTEPAD, "记事本" },
        { SK_ICON_PAINT, "画板" }, { SK_ICON_MONITOR, "系统监视器" }, { SK_ICON_CLOCK, "时钟" },
        { SK_ICON_ABOUT, "关于本机" }, { -1, NULL }, { SK_ICON_REBOOT, "重新启动" }, { SK_ICON_POWER, "关机" },
    };
    int y = m.y + 8;
    const sk_font *f = sk_font_get(SK_FONT_UI);
    for (unsigned i = 0; i < sizeof items / sizeof items[0]; i++) {
        if (items[i].icon < 0) {
            sk_draw_separator(c, m.x + 12, m.x + m.w - 12, y + 6, 0x3EC);
            y += 12;
            continue;
        }
        sk_rect row = sk_rect_make(m.x + 6, y, m.w - 12, 30);
        if (i == 3)
            sk_marker_highlight(c, row, SK_HIGHLIGHT, 0x3ED);
        sk_draw_icon(c, items[i].icon, row.x + 8, row.y + 3, 24, 6);
        sk_draw_text_in_rect(c, f, sk_rect_make(row.x + 42, row.y, row.w - 46, row.h), items[i].t, SK_INK, SK_ALIGN_LEFT);
        y += 30;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(g_out, sizeof g_out, "%s", argv[1]);
    sk_canvas *c = sk_canvas_new(W, H);
    if (!c)
        return 1;
    desktop_bg(c, sk_rect_make(0, 0, W, H - TASKBAR_H));
    struct { int icon; const char *t; } di[] = {
        { SK_ICON_TERMINAL, "终端" }, { SK_ICON_FOLDER, "文件" }, { SK_ICON_NOTEPAD, "记事本" },
        { SK_ICON_PAINT, "画板" }, { SK_ICON_MONITOR, "系统监视器" }, { SK_ICON_CLOCK, "时钟" },
        { SK_ICON_ABOUT, "关于本机" },
    };
    for (int i = 0; i < 7; i++)
        desk_icon(c, 40, 22 + i * 96, di[i].icon, di[i].t, i == 1, 0xD1 + i);

    sk_rect mon = sk_rect_make(640, 70, 340, 300);
    monitor_content(c, mon);
    sk_draw_window_frame(c, mon, "系统监视器", 0, -1, SK_PURPLE, 0x30);
    sk_rect files = sk_rect_make(470, 380, 470, 270);
    files_content(c, files);
    sk_draw_window_frame(c, files, "文件 - /root", 0, -1, SK_ORANGE, 0x31);
    sk_rect term = sk_rect_make(200, 120, 560, 270);
    terminal_content(c, term);
    sk_draw_window_frame(c, term, "终端", 1, 0, SK_BLUE, 0x32);

    taskbar(c);
    start_menu(c);
    save(c, "theme_desktop.ppm");
    sk_canvas_free(c);
    return 0;
}
