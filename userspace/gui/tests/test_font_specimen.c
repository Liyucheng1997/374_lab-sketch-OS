/*
 * test_font_specimen.c — 渲染一张“字体样张”，用眼睛检查字体效果（宿主机运行）
 *
 *   SKETCH_FONT_DIR=<仓库>/assets/fonts <BUILD>/host/test_font_specimen [输出.ppm]
 *   python3 tools/ppm2png.py 输出.ppm 输出.png
 *
 * 样张内容：各字号的中英文和基线/ascent/descent 参考线（检查基线跳动、下伸部被裁）、
 * 自动换行（中文按字、英文按词、避头尾、长单词硬拆）、“…”截断与对齐、
 * 终端风格的等宽网格（制表符必须上下左右连成线、汉字正好占两格）、颜色与透明度、缺字方框。
 * 把 SKETCH_FONT_DIR 指向不存在的目录再运行一次，可以看到内置后备字体的效果。
 *
 * 只链接 font.c，不依赖 gfx.c：画布、矩形、线条用本文件里的小工具函数。
 */
#include "sketch/font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1180
#define H 1580

#define PAPER   SK_RGB(0xFF, 0xFD, 0xF7)
#define GRID    SK_RGB(0xE6, 0xEE, 0xF6)
#define INK     SK_RGB(0x2B, 0x2B, 0x2B)
#define SOFT    SK_RGB(0x5C, 0x5C, 0x5C)
#define FAINT   SK_RGB(0xC8, 0xC8, 0xC8)
#define BLUE    SK_RGB(0x4A, 0x7B, 0xD0)
#define RED     SK_RGB(0xE0, 0x60, 0x5A)
#define GREEN   SK_RGB(0x5B, 0xAA, 0x62)
#define ORANGE  SK_RGB(0xF2, 0x9E, 0x4C)
#define PURPLE  SK_RGB(0x9B, 0x7B, 0xD4)

static sk_canvas g_c;

static void put(int x, int y, sk_color col)
{
    if (x >= g_c.clip.x && y >= g_c.clip.y && x < g_c.clip.x + g_c.clip.w && y < g_c.clip.y + g_c.clip.h)
        g_c.pixels[(size_t)y * g_c.stride + x] = col;
}

static void fill(sk_rect r, sk_color col)
{
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            put(x, y, col);
}

static void hline(int x0, int x1, int y, sk_color col)
{
    for (int x = x0; x <= x1; x++)
        put(x, y, col);
}

static void vline(int x, int y0, int y1, sk_color col)
{
    for (int y = y0; y <= y1; y++)
        put(x, y, col);
}

static void outline(sk_rect r, sk_color col)
{
    hline(r.x, r.x + r.w - 1, r.y, col);
    hline(r.x, r.x + r.w - 1, r.y + r.h - 1, col);
    vline(r.x, r.y, r.y + r.h - 1, col);
    vline(r.x + r.w - 1, r.y, r.y + r.h - 1, col);
}

static int save_ppm(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        return -1;
    }
    fprintf(fp, "P6\n%d %d\n255\n", g_c.w, g_c.h);
    for (int y = 0; y < g_c.h; y++)
        for (int x = 0; x < g_c.w; x++) {
            uint32_t p = g_c.pixels[(size_t)y * g_c.stride + x];
            unsigned char rgb[3] = { (unsigned char)SK_R(p), (unsigned char)SK_G(p), (unsigned char)SK_B(p) };
            fwrite(rgb, 1, 3, fp);
        }
    return fclose(fp);
}

/* 小节标题：返回下一行的 y */
static int section(int x, int y, const char *title)
{
    sk_font *t = sk_font_get(SK_FONT_TITLE);
    sk_draw_text(&g_c, t, x, y + sk_font_ascent(t), title, BLUE);
    return y + sk_font_line_height(t) + 6;
}

/* 一行带参考线的文字：红=基线，蓝=ascent/descent */
static int metric_line(int x, int y, sk_font *f, const char *label, const char *text)
{
    int base = y + sk_font_ascent(f);
    hline(x, x + 1080, base, SK_RGBA(0xE0, 0x60, 0x5A, 255));
    hline(x, x + 1080, base - sk_font_ascent(f), SK_RGB(0xB8, 0xCC, 0xEE));
    hline(x, x + 1080, base + sk_font_descent(f), SK_RGB(0xB8, 0xCC, 0xEE));
    sk_font *ui = sk_font_get(SK_FONT_UI);
    sk_draw_text(&g_c, ui, x, base, label, RED);
    sk_draw_text(&g_c, f, x + 110, base, text, INK);
    return y + sk_font_line_height(f) + 8;
}

/* 终端风格：按格子画一行（宽字符占两格），返回占用的格数 */
static int term_row(int x0, int y0, sk_font *mono, int row, const char *s, sk_color col)
{
    int cw = sk_font_cell_width(mono), lh = sk_font_line_height(mono);
    int base = y0 + row * lh + sk_font_ascent(mono);
    int colno = 0;
    uint32_t cp;
    while ((cp = sk_utf8_next(&s)) != 0) {
        int w = sk_wcwidth(cp);
        if (w == 0)
            continue;
        sk_draw_glyph(&g_c, mono, x0 + colno * cw, base, cp, col);
        colno += w;
    }
    return colno;
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "font-specimen.ppm";
    g_c.pixels = malloc((size_t)W * H * 4);
    g_c.w = W;
    g_c.h = H;
    g_c.stride = W;
    g_c.clip = (sk_rect){ 0, 0, W, H };

    /* 方格纸 */
    fill((sk_rect){ 0, 0, W, H }, PAPER);
    for (int y = 0; y < H; y += 24)
        hline(0, W - 1, y, GRID);
    for (int x = 0; x < W; x += 24)
        vline(x, 0, H - 1, GRID);

    sk_font *ui = sk_font_get(SK_FONT_UI), *title = sk_font_get(SK_FONT_TITLE);
    sk_font *large = sk_font_get(SK_FONT_LARGE), *mono = sk_font_get(SK_FONT_MONO);
    int y = 16;

    sk_draw_text(&g_c, large, 30, y + sk_font_ascent(large), "SketchOS 手绘操作系统 · 字体样张", INK);
    y += sk_font_line_height(large) + 4;
    char info[256];
    snprintf(info, sizeof(info), "UI %d/%d/%d  TITLE %d/%d/%d  LARGE %d/%d/%d  MONO %d/%d/%d 格宽 %d   （ascent/descent/行高）",
             sk_font_ascent(ui), sk_font_descent(ui), sk_font_line_height(ui),
             sk_font_ascent(title), sk_font_descent(title), sk_font_line_height(title),
             sk_font_ascent(large), sk_font_descent(large), sk_font_line_height(large),
             sk_font_ascent(mono), sk_font_descent(mono), sk_font_line_height(mono), sk_font_cell_width(mono));
    sk_draw_text(&g_c, ui, 30, y + sk_font_ascent(ui), info, SOFT);
    y += sk_font_line_height(ui) + 10;

    /* ---- 1. 各字号与参考线 ---- */
    y = section(30, y, "一、字号与基线（红线=基线，蓝线=ascent/descent）");
    const char *sample = "永和九年，岁在癸丑。gjpqy ÀÉÎ Quick brown fox 0123456789 ←→ ×÷ ★☆";
    y = metric_line(30, y, ui, "正文 16", sample);
    y = metric_line(30, y, title, "标题 20", sample);
    y = metric_line(30, y, large, "大字 32", "2026年10月23日 星期五 12:34 gjpqy ÀÉ");
    y = metric_line(30, y, mono, "等宽 16", "int main(void) { return 0; } // 中文注释 →");
    y += 6;

    /* ---- 2. 自动换行 ---- */
    y = section(30, y, "二、自动换行（中文按字、英文按词；框是排版宽度）");
    const char *zh =
        "操作系统（Operating System）是管理计算机硬件与软件资源的程序。它负责进程调度、内存管理、"
        "文件系统和设备驱动，为应用程序提供“系统调用”这一统一的接口。\n"
        "  SketchOS 使用未经修改的 Linux 6.12 内核，上层的 init、shell、显示服务器和桌面应用全部自己编写。";
    const char *en =
        "An operating system manages hardware and software resources and provides common services "
        "for programs. The kernel schedules processes, manages memory, and talks to devices; "
        "everything else in SketchOS - init, the shell, the display server - is written from scratch.";
    sk_rect bz = { 30, y, 520, 0 }, be = { 600, y, 520, 0 };
    int hz = sk_draw_text_wrapped(&g_c, ui, bz, zh, INK, 4);
    int he = sk_draw_text_wrapped(&g_c, ui, be, en, INK, 4);
    outline((sk_rect){ bz.x - 1, bz.y - 1, bz.w + 2, hz + 2 }, FAINT);
    outline((sk_rect){ be.x - 1, be.y - 1, be.w + 2, he + 2 }, FAINT);
    y += (hz > he ? hz : he) + 14;

    /* 避头尾 + 长单词 */
    const char *kin = "他说：“内核（kernel）是系统的核心！”真的吗？是的。进程、线程、文件……都由它管理。";
    const char *lw = "路径 /usr/share/fonts/sans-16.skf 太长 supercalifragilisticexpialidocious 结束";
    sk_rect bk = { 30, y, 16 * 9, 0 }, bl = { 220, y, 150, 0 }, bt = { 420, y, 300, 0 };
    int hk = sk_draw_text_wrapped(&g_c, ui, bk, kin, INK, 2);
    int hl = sk_draw_text_wrapped(&g_c, ui, bl, lw, INK, 2);
    int ht = sk_draw_text_wrapped(&g_c, title, bt, "标题字体也能换行：Hand-drawn style, 手绘风格。", GREEN, 2);
    outline((sk_rect){ bk.x - 1, bk.y - 1, bk.w + 2, hk + 2 }, FAINT);
    outline((sk_rect){ bl.x - 1, bl.y - 1, bl.w + 2, hl + 2 }, FAINT);
    outline((sk_rect){ bt.x - 1, bt.y - 1, bt.w + 2, ht + 2 }, FAINT);
    sk_draw_text_wrapped(&g_c, ui, (sk_rect){ 760, y, 380, 0 },
                         "← 左：9 字宽，标点不在行首、开括号不在行尾\n← 中：长路径和长单词在字母间硬拆\n← 右：20px 标题字体",
                         SOFT, 2);
    int hmax = hk > hl ? hk : hl;
    y += (hmax > ht ? hmax : ht) + 14;

    /* ---- 3. 截断 ---- */
    y = section(30, y, "三、单行截断与对齐（sk_draw_text_in_rect）");
    const char *longtitle = "这是一个非常非常长的窗口标题 - A Very Long Window Title";
    int widths[] = { 420, 300, 200, 120, 60, 24 };
    for (int i = 0; i < 6; i++) {
        for (int a = 0; a < 3; a++) {
            sk_rect r = { 30 + a * 380, y, widths[i] * 360 / 420, 26 };
            fill(r, SK_RGB(0xF4, 0xEF, 0xE1));
            outline(r, FAINT);
            sk_draw_text_in_rect(&g_c, ui, r, longtitle, INK, a);
        }
        y += 30;
    }
    const char *labels[] = { "确定", "取消", "关于本机", "OK" };
    for (int i = 0; i < 4; i++) {
        sk_rect r = { 30 + i * 130, y, 110, 34 };
        fill(r, SK_RGB(0xFB, 0xF8, 0xEF));
        outline(r, SOFT);
        sk_draw_text_in_rect(&g_c, title, r, labels[i], INK, SK_ALIGN_CENTER);
    }
    sk_draw_text_in_rect(&g_c, ui, (sk_rect){ 560, y, 600, 34 }, "← 按钮：20px 标题字体，水平+垂直居中", SOFT, SK_ALIGN_LEFT);
    y += 46;

    /* ---- 4. 终端网格 ---- */
    y = section(30, y, "四、等宽终端（每格 8×19，制表符必须连成线，汉字占两格）");
    int cw = sk_font_cell_width(mono), lh = sk_font_line_height(mono);
    const char *term[] = {
        "root@sketchos:~# ls -l /usr/share/fonts",
        "-rw-r--r--  1 root root 1762216 10月 23 14:22 sans-16.skf",
        "drwxr-xr-x  2 root root    4096 10月 23 14:22 文档",
        "┌──────────┬────────┬──────┐  ╔═══╦═══╗  ╭────╮  ┏━━┳━━┓",
        "│ 名称     │ 大小   │ 状态 │  ║ A ║ B ║  │圆角│  ┃粗┃线┃",
        "├──────────┼────────┼──────┤  ╠═══╬═══╣  ╰────╯  ┣━━╋━━┫",
        "│ init     │  52 KB │ 运行 │  ║ C ║ D ║  ╱╲╳     ┗━━┻━━┛",
        "│ sketchd  │ 310 KB │ 睡眠 │  ╚═══╩═══╝  ┄┄┈┈╌╌    ╒══╤══╕",
        "└──────────┴────────┴──────┘  ░░▒▒▓▓███  ▀▄▌▐▖▗▘▝  ╘══╧══╛",
        "CPU [████████▌     ] 62%  内存 [▁▂▃▄▅▆▇█] ═╪═ ─╫─ ╞╡╟╢",
        "“引号”…省略 —破折号 ←↑→↓ ●○■□ αβγ Ж  ＡＢＣ１２３ ！？",
    };
    int rows = (int)(sizeof(term) / sizeof(term[0]));
    int cols = 72;
    sk_rect tr = { 30, y, cols * cw, rows * lh };
    fill(tr, SK_RGB(0xFF, 0xFF, 0xFF));
    for (int cidx = 0; cidx <= cols; cidx++)
        vline(tr.x + cidx * cw, tr.y, tr.y + tr.h - 1, SK_RGB(0xF0, 0xF0, 0xF0));
    for (int r = 0; r <= rows; r++)
        hline(tr.x, tr.x + tr.w - 1, tr.y + r * lh, SK_RGB(0xF0, 0xF0, 0xF0));
    for (int r = 0; r < rows; r++)
        term_row(tr.x, tr.y, mono, r, term[r], r == 0 ? GREEN : INK);
    /* 右边：放大 3 倍看格子交界处的像素 */
    int zx = tr.x + tr.w + 20, zy = y, zs = 3;
    int srcx = tr.x + 30 * cw, srcy = tr.y + 3 * lh;
    for (int yy = 0; yy < 4 * lh; yy++)
        for (int xx = 0; xx < 12 * cw; xx++) {
            uint32_t p = g_c.pixels[(size_t)(srcy + yy) * W + srcx + xx];
            fill((sk_rect){ zx + xx * zs, zy + yy * zs, zs, zs }, p);
        }
    outline((sk_rect){ zx - 1, zy - 1, 12 * cw * zs + 2, 4 * lh * zs + 2 }, FAINT);
    y += rows * lh + 16;

    /* ---- 5. 颜色、透明度、缺字 ---- */
    y = section(30, y, "五、颜色、透明度、缺字与非法字节");
    int x = 30, base = y + sk_font_ascent(title);
    sk_color cols5[] = { INK, BLUE, RED, GREEN, ORANGE, PURPLE };
    const char *names[] = { "铅笔", "蓝", "红", "绿", "橙", "紫" };
    for (int i = 0; i < 6; i++)
        x = sk_draw_text(&g_c, title, x, base, names[i], cols5[i]) + 14;
    x = sk_draw_text(&g_c, title, x, base, "半透明 50%", SK_RGBA(0x2B, 0x2B, 0x2B, 128)) + 14;
    fill((sk_rect){ x, y, 200, sk_font_line_height(title) }, SK_RGB(0x4A, 0x7B, 0xD0));
    sk_draw_text(&g_c, title, x + 8, base, "深色底白字", SK_RGB(0xFF, 0xFF, 0xFF));
    y += sk_font_line_height(title) + 8;
    sk_draw_text(&g_c, ui, 30, y + sk_font_ascent(ui),
                 "缺字（画手绘方框）：emoji \xF0\x9F\x98\x80 \xF0\x9F\x8E\x89  私用区 \xEE\x80\x80  "
                 "非法字节：[\xFF\xFE\xC0\x80]  截断的汉字：[\xE4\xB8]  "
                 "组合字符（字体没有，宽 0 不画）：e\xCC\x81 a\xCC\x88  预组合：é ä",
                 INK);
    y += sk_font_line_height(ui) + 4;
    sk_draw_text(&g_c, mono, 30, y + sk_font_ascent(mono), "等宽缺字：[\xF0\x9F\x98\x80][\xEE\x80\x80] 制表符\t后面", INK);
    y += sk_font_line_height(mono) + 4;
    sk_draw_text(&g_c, large, 30, y + sk_font_ascent(large), "大字回退：鬣狗、麒麟、饕餮", INK);
    y += sk_font_line_height(large) + 4;

    if (y > H)
        fprintf(stderr, "样张内容超出画布（y=%d）\n", y);
    if (save_ppm(out) != 0)
        return 1;
    printf("%s (%dx%d，内容高度 %d)\n", out, W, H, y);
    free(g_c.pixels);
    return 0;
}
