/*
 * compositor.c — 合成器：把桌面、窗口、任务栏、菜单、光标叠成最终画面
 *
 * “损坏区域”(damage) 是整个图形系统省电省 CPU 的关键：
 *   画面上任何东西变了（窗口内容更新、窗口移动、时钟走了一分钟、光标动了），
 *   变化的模块只调用 comp_damage(那一块矩形)。每轮事件循环结束时，comp_frame()
 *   只重新合成这些矩形，然后只把这些矩形拷到屏幕上。光标在屏幕上移动一下，
 *   要处理的只是两个 24x24 的小方块，而不是整个 1024x768 的屏幕（差了上千倍）。
 *
 * 合成顺序（画家算法：从下往上一层层盖）：
 *   桌面背景(预渲染好的整屏图) → 各窗口从底到顶（边框层“抠像”混合 + 内容不透明拷贝）
 *   → 任务栏 → 弹出菜单/提示 → 缩放时的虚线框
 * 结果放在内存里的“后备缓冲区”(back buffer)。光标不画进后备缓冲区，而是在拷贝到
 * 屏幕的那一刻临时叠上去：这样移动光标时只需要“重新拷贝”旧位置和新位置，
 * 连下面的窗口都不用重新合成。
 *
 * 遮挡优化：如果某个窗口的内容区完全盖住了要合成的矩形（例如在终端里打字），
 * 它下面的桌面和窗口都看不见，直接从这个窗口开始画。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_DAMAGE 16

typedef struct {
    sk_rect r[MAX_DAMAGE];
    int     n;
} rect_list;

static skd_display *g_disp;
static sk_canvas   *g_back;
static uint32_t    *g_line;          /* 一行临时缓冲（叠加光标用） */
static sk_rect      g_screen;
static rect_list    g_damage;        /* 需要重新合成并刷新 */
static rect_list    g_flush;         /* 只需要刷新到屏幕（光标） */

/* 调试统计（SKETCHD_DEBUG=1） */
static uint64_t g_stat_t0;
static unsigned g_stat_frames;
static uint64_t g_stat_pixels, g_stat_us, g_stat_max_us;

static long long area(sk_rect r) { return (long long)r.w * r.h; }

static int rect_contains_rect(sk_rect outer, sk_rect in)
{
    return in.x >= outer.x && in.y >= outer.y &&
           in.x + in.w <= outer.x + outer.w && in.y + in.h <= outer.y + outer.h;
}

/*
 * 往损坏列表里加一个矩形，尽量保持列表短小：
 *   - 被已有矩形包含 → 忽略；包含已有矩形 → 替换它们；
 *   - 和已有矩形合并后的包围盒“不怎么浪费”（面积 ≤ 两者之和的 1.25 倍）→ 合并；
 *   - 列表满了 → 和“合并后面积增加最少”的那个合并（最坏情况退化成一个包围盒）。
 * 多算一点面积没关系（只是多画几个像素），漏掉才会出现残影。
 */
static void list_add(rect_list *L, sk_rect r)
{
    r = sk_rect_intersect(r, g_screen);
    if (sk_rect_empty(r))
        return;
restart:
    for (int i = 0; i < L->n; i++) {
        if (rect_contains_rect(L->r[i], r))
            return;
        sk_rect u = sk_rect_union(L->r[i], r);
        if (rect_contains_rect(r, L->r[i]) || area(u) * 4 <= (area(L->r[i]) + area(r)) * 5) {
            r = u;
            L->r[i] = L->r[--L->n];
            goto restart;
        }
    }
    if (L->n < MAX_DAMAGE) {
        L->r[L->n++] = r;
        return;
    }
    int best = 0;
    long long best_cost = LLONG_MAX;
    for (int i = 0; i < L->n; i++) {
        long long cost = area(sk_rect_union(L->r[i], r)) - area(L->r[i]);
        if (cost < best_cost) {
            best_cost = cost;
            best = i;
        }
    }
    r = sk_rect_union(L->r[best], r);
    L->r[best] = L->r[--L->n];
    goto restart;
}

int comp_init(skd_display *d)
{
    g_disp = d;
    g_screen = sk_rect_make(0, 0, d->w, d->h);
    g_back = sk_canvas_new(d->w, d->h);
    g_line = malloc((size_t)d->w * sizeof(uint32_t));
    if (!g_back || !g_line) {
        sk_log("comp: 后备缓冲区分配失败（%dx%d）", d->w, d->h);
        return -1;
    }
    sk_clear(g_back, SK_DESK_PAPER);
    g_stat_t0 = skd_now_ms();
    return 0;
}

void comp_shutdown(void)
{
    if (g_back)
        sk_canvas_free(g_back);
    g_back = NULL;
    free(g_line);
    g_line = NULL;
}

sk_canvas *comp_back(void) { return g_back; }

void comp_damage(sk_rect r) { list_add(&g_damage, r); }
void comp_damage_all(void) { list_add(&g_damage, g_screen); }
void comp_flush_only(sk_rect r) { list_add(&g_flush, r); }
int comp_has_work(void) { return g_damage.n > 0 || g_flush.n > 0; }

/* 合成一个矩形到后备缓冲区 */
static void compose_rect(sk_rect r)
{
    sk_canvas *b = g_back;
    sk_set_clip(b, r);
    if (splash_active()) {
        splash_paint(b, r);
    } else {
        if (!taskbar_covers(r)) {
            int from = wm_covering_index(r);
            if (from < 0) {
                desktop_paint(b, r);
                from = 0;
            }
            wm_paint(b, r, from);
        }
        taskbar_paint(b, r);
        menu_paint(b, r);
        wm_paint_overlay(b, r);
    }
    sk_reset_clip(b);
}

/* 光标像素按 alpha 混合到一行上（整数运算：(s*a + d*(255-a)) / 255） */
static void blend_cursor_row(uint32_t *row, int row_x, int row_w, int y, sk_rect cr,
                             const sk_canvas *img)
{
    int x0 = SK_MAX(row_x, cr.x), x1 = SK_MIN(row_x + row_w, cr.x + cr.w);
    const uint32_t *src = img->pixels + (size_t)(y - cr.y) * img->stride;
    for (int x = x0; x < x1; x++) {
        uint32_t s = src[x - cr.x];
        uint32_t a = s >> 24;
        if (a == 0)
            continue;
        uint32_t *d = &row[x - row_x];
        if (a == 255) {
            *d = s;
            continue;
        }
        uint32_t dv = *d, ia = 255 - a;
        uint32_t rb = ((s & 0xFF00FFu) * a + (dv & 0xFF00FFu) * ia + 0x800080u);
        uint32_t g  = ((s & 0x00FF00u) * a + (dv & 0x00FF00u) * ia + 0x008000u);
        /* 用 x/255 ≈ (x + (x>>8)) >> 8 近似除法，分别处理 R/B 和 G 通道 */
        rb = ((rb + ((rb >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
        g  = ((g + ((g >> 8) & 0x00FF00u)) >> 8) & 0x00FF00u;
        *d = 0xFF000000u | rb | g;
    }
}

/* 把后备缓冲区的一个矩形拷到屏幕（顺便叠加光标、必要时转换成 16 位色） */
static void flush_rect(sk_rect r)
{
    sk_rect cr = cursor_rect();
    const sk_canvas *img = cursor_image();
    int has_cursor = img && !sk_rect_empty(sk_rect_intersect(cr, r));
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *src = g_back->pixels + (size_t)y * g_back->stride + r.x;
        if (has_cursor && y >= cr.y && y < cr.y + cr.h) {
            memcpy(g_line, src, (size_t)r.w * 4);
            blend_cursor_row(g_line, r.x, r.w, y, cr, img);
            src = g_line;
        }
        uint8_t *dst = g_disp->map + (size_t)y * g_disp->pitch;
        if (g_disp->bpp == 32) {
            memcpy(dst + (size_t)r.x * 4, src, (size_t)r.w * 4);
        } else {
            /* RGB565：取 R 的高 5 位、G 的高 6 位、B 的高 5 位拼成 16 位 */
            uint16_t *d16 = (uint16_t *)dst + r.x;
            for (int x = 0; x < r.w; x++) {
                uint32_t p = src[x];
                d16[x] = (uint16_t)(((p >> 8) & 0xF800u) | ((p >> 5) & 0x07E0u) | ((p >> 3) & 0x001Fu));
            }
        }
    }
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void comp_frame(void)
{
    if (!comp_has_work() || !g_back)
        return;
    uint64_t t0 = skd_debug ? now_us() : 0;
    long long px = 0;

    /* 1. 合成所有损坏矩形；它们当然也要刷新到屏幕 */
    rect_list dmg = g_damage;
    g_damage.n = 0;
    for (int i = 0; i < dmg.n; i++) {
        compose_rect(dmg.r[i]);
        px += area(dmg.r[i]);
        list_add(&g_flush, dmg.r[i]);
    }
    /* 2. 拷到屏幕 */
    rect_list fl = g_flush;
    g_flush.n = 0;
    for (int i = 0; i < fl.n; i++)
        flush_rect(fl.r[i]);
    /* 3. 通知驱动（KMS 的 DIRTYFB） */
    if (g_disp->commit)
        g_disp->commit(g_disp, fl.r, fl.n);

    if (skd_debug) {
        uint64_t us = now_us() - t0;
        g_stat_frames++;
        g_stat_pixels += (uint64_t)px;
        g_stat_us += us;
        if (us > g_stat_max_us)
            g_stat_max_us = us;
        if (us > 20000)
            sk_log("comp: 慢帧 %llu.%03llu ms（合成 %d 块 %lld 像素，刷新 %d 块）",
                   (unsigned long long)(us / 1000), (unsigned long long)(us % 1000), dmg.n, px, fl.n);
        uint64_t now = skd_now_ms();
        if (now - g_stat_t0 >= 2000) {
            sk_log("comp: %u 帧，平均 %llu us，最长 %llu us，合成 %llu 像素",
                   g_stat_frames, (unsigned long long)(g_stat_us / g_stat_frames),
                   (unsigned long long)g_stat_max_us, (unsigned long long)g_stat_pixels);
            g_stat_t0 = now;
            g_stat_frames = 0;
            g_stat_pixels = g_stat_us = g_stat_max_us = 0;
        }
    }
}

void comp_snapshot(sk_canvas *dst)
{
    comp_frame();      /* 先把待合成的区域补上，后备缓冲区就是完整的当前画面 */
    sk_rect all = sk_rect_make(0, 0, g_back->w, g_back->h);
    sk_blit(dst, 0, 0, g_back, all);
}
