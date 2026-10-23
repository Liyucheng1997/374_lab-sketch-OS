/*
 * test_theme_widgets.c — 标准控件样张：每种控件 × 每种状态
 *
 * 焦点列模仿 ui.c 的做法：控件以 FOCUSED 状态绘制，再在控件矩形外 3 像素画焦点虚线框。
 * 输出 theme_widgets.ppm。用法：test_theme_widgets [输出目录]
 * 离线测试需要字体：SKETCH_FONT_DIR=<仓库>/assets/fonts
 */
#include <stdio.h>
#include <stdlib.h>

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

/* ui.c 的重绘流程：每个控件画之前把裁剪设为“控件矩形外扩 4 像素”（SK_WIDGET_BLEED） */
static sk_rect bleed(sk_rect r) { return sk_rect_inset(r, -4, -4); }

static const int states[6] = {
    SK_STATE_NORMAL, SK_STATE_HOVER, SK_STATE_PRESSED | SK_STATE_HOVER, SK_STATE_FOCUSED,
    SK_STATE_DISABLED, SK_STATE_CHECKED,
};
static const char *state_names[6] = { "普通", "悬停", "按下", "焦点", "禁用", "选中" };

int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(g_out, sizeof g_out, "%s", argv[1]);
    const int W = 1100, H = 760;
    sk_canvas *c = sk_canvas_new(W, H);
    if (!c)
        return 1;
    sk_paper_fill(c, sk_rect_make(0, 0, W, H), SK_PAPER, 3, 11);
    const sk_font *ui = sk_font_get(SK_FONT_UI);
    const sk_font *title = sk_font_get(SK_FONT_TITLE);

    sk_draw_text(c, title, 20, 30, "控件样张（每列一种状态）", SK_INK);
    int colx[6], cw = 165;
    for (int i = 0; i < 6; i++) {
        colx[i] = 150 + i * cw;
        sk_draw_text_in_rect(c, ui, sk_rect_make(colx[i], 44, cw - 20, 22), state_names[i], SK_INK_SOFT, SK_ALIGN_CENTER);
    }
    const char *rows[] = { "按钮", "小按钮", "复选框", "输入框", "图标按钮框", "滚动条", "占用率" };
    for (int r = 0; r < 7; r++)
        sk_draw_text(c, ui, 20, 98 + r * 62, rows[r], SK_INK_SOFT);

    for (int i = 0; i < 6; i++) {
        int st = states[i];
        uint32_t seed = 1000 + (uint32_t)i;
        int x = colx[i];
        /* 按钮（36 高：20px 字） */
        sk_rect br = sk_rect_make(x, 74, 130, 36);
        sk_set_clip(c, bleed(br));
        sk_draw_button(c, br, "确定", st, SK_BLUE, seed);
        if (st & SK_STATE_FOCUSED)
            sk_draw_focus_ring(c, sk_rect_inset(br, -3, -3), seed ^ 0xF0C5u);
        /* 小按钮（28 高：16px 字） */
        sk_rect sb = sk_rect_make(x + 10, 140, 110, 28);
        sk_set_clip(c, bleed(sb));
        sk_draw_button(c, sb, "取消 Cancel", st, SK_ORANGE, seed + 50);
        if (st & SK_STATE_FOCUSED)
            sk_draw_focus_ring(c, sk_rect_inset(sb, -3, -3), seed ^ 0xF0C5u);
        /* 复选框 */
        sk_rect cb = sk_rect_make(x, 196, 150, 26);
        sk_set_clip(c, bleed(cb));
        sk_draw_checkbox(c, cb, "显示隐藏文件", st | (i == 3 ? SK_STATE_CHECKED : 0), seed + 100);
        if (st & SK_STATE_FOCUSED)
            sk_draw_focus_ring(c, sk_rect_inset(cb, -3, -3), seed ^ 0xF0C5u);
        /* 输入框 */
        sk_rect tb = sk_rect_make(x, 252, 145, 32);
        sk_set_clip(c, bleed(tb));
        sk_draw_textbox_frame(c, tb, st, seed + 200);
        sk_draw_text_in_rect(c, ui, sk_rect_inset(tb, 8, 0), i == 4 ? "不可编辑" : "/root/笔记.txt",
                             i == 4 ? SK_INK_FAINT : SK_INK, SK_ALIGN_LEFT);
        /* 图标按钮框（只有框） */
        sk_rect ib = sk_rect_make(x + 40, 310, 40, 40);
        sk_set_clip(c, bleed(ib));
        sk_draw_button_frame(c, ib, st, SK_GREEN, seed + 300);
        /* 横向滚动条 */
        sk_rect hs = sk_rect_make(x, 380, 145, 14);
        sk_set_clip(c, bleed(hs));
        sk_draw_scrollbar(c, hs, 1000, 300, 200 + i * 80, st, seed + 400);
        /* 占用率条 */
        sk_rect mr = sk_rect_make(x, 432, 145, 22);
        sk_set_clip(c, bleed(mr));
        static const sk_color mc[6] = { SK_BLUE, SK_GREEN, SK_ORANGE, SK_PURPLE, SK_RED, SK_TEAL };
        sk_draw_meter(c, mr, i * 200, mc[i], seed + 500);
        char cap[32];
        snprintf(cap, sizeof cap, "%d%%", i * 20);
        sk_draw_text_in_rect(c, ui, mr, cap, SK_INK, SK_ALIGN_CENTER);
    }
    sk_reset_clip(c);

    /* 竖滚动条：各种位置 */
    for (int i = 0; i < 6; i++) {
        sk_rect vs = sk_rect_make(150 + i * 40, 490, 14, 200);
        int st = i == 3 ? SK_STATE_HOVER : i == 4 ? SK_STATE_PRESSED : i == 5 ? SK_STATE_DISABLED : 0;
        sk_draw_scrollbar(c, vs, i == 5 ? 100 : 1000, i == 5 ? 200 : 150 + i * 120, i * 170, st, 600 + i);
    }
    sk_draw_text(c, ui, 20, 520, "竖滚动条", SK_INK_SOFT);

    /* 标签页 */
    const char *tabs[] = { "进程", "内存", "处理器", "关于内核" };
    sk_draw_text(c, ui, 420, 520, "标签页", SK_INK_SOFT);
    sk_draw_tabs(c, sk_rect_make(420, 530, 420, 36), tabs, 4, 0, 2, 700);
    sk_draw_tabs(c, sk_rect_make(420, 590, 420, 36), tabs, 4, 2, -1, 701);
    sk_draw_tabs(c, sk_rect_make(420, 650, 200, 36), tabs, 4, 1, 3, 702);   /* 太窄：压缩 */
    for (int i = 0; i < 4; i++) {
        sk_rect tr = sk_tabs_item_rect(sk_rect_make(420, 590, 420, 36), tabs, 4, i);
        sk_rect_outline(c, sk_rect_make(tr.x, 628, tr.w, 2), SK_WITH_ALPHA(SK_RED, 0x80));
    }

    /* 分隔线、焦点框、荧光笔选中、下划线 */
    sk_draw_text(c, ui, 880, 520, "分隔线", SK_INK_SOFT);
    sk_draw_separator(c, 880, 1080, 540, 800);
    sk_draw_separator(c, 880, 1000, 560, 801);
    sk_draw_focus_ring(c, sk_rect_make(880, 580, 200, 40), 802);
    sk_marker_highlight(c, sk_rect_make(890, 640, 110, 24), SK_HIGHLIGHT, 803);
    sk_draw_text(c, ui, 892, 658, "选中的文字 abc", SK_INK);
    sk_draw_text(c, ui, 892, 700, "重要的话", SK_INK);
    sk_rough_underline(c, 890, 960, 706, SK_RED, 1.8f, 804);

    save(c, "theme_widgets.ppm");
    sk_canvas_free(c);
    return 0;
}
