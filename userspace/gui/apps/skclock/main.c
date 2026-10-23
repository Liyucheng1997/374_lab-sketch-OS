/*
 * skclock — SketchOS 时钟（手绘表盘 + 数字时间 + 秒表）
 *
 * 两个标签页：
 *   时钟  手绘的指针式表盘，下面是大号数字时间和日期；
 *   秒表  开始/暂停、计次、复位，计次记录列在下面的表格里。
 *
 * ┌──────────────── 时间是从哪儿来的？ ────────────────┐
 *
 *  内核只知道一件事：从 1970-01-01 00:00:00 UTC 起已经过了多少秒（clock_gettime(CLOCK_REALTIME)，
 *  开机时从主板上的实时时钟 RTC 读出来，之后靠定时器中断往前数）。
 *  “现在是北京时间几点”完全是用户态 C 库的事：localtime_r() 读环境变量 TZ，按其中的规则把
 *  UTC 秒数换算成本地的年月日时分秒。SketchOS 的 TZ 由 1 号进程 sinit 根据内核命令行参数
 *  sketch.tz 设置（默认 CST-8），所有进程都从父进程那里继承这个环境变量。
 *  POSIX 的 TZ 写法 "CST-8"：CST 是时区的名字，-8 的意思是“本地时间 + (-8) 小时 = UTC”，
 *  也就是比 UTC 快 8 小时——符号和平常说的“UTC+8”正好相反，这是 POSIX 的历史约定。
 *
 * └────────────────────────────────────────────────────┘
 *
 * ┌──────────────── 每秒只重画一小块（网页版的 CPU 慢几十倍） ────────────────┐
 *
 *  1. 定时器对齐到整秒：不是简单地“每 1000 毫秒一次”（那样第一次在 x.7 秒启动，秒针就永远
 *     比真实时间晚 0.7 秒，而且误差会慢慢累积），而是每次都算出“离下一个整秒还有几毫秒”，
 *     用一次性定时器在整秒刚过的时候醒来，然后再定下一次。两次之间进程睡在 poll() 里。
 *  2. 表盘缓存：表圈、刻度、数字每秒都一模一样，只有指针在动。手绘图元（尤其是排线）很费 CPU，
 *     所以表盘只画一次，存进一块离屏画布；以后要重画某一块时，直接从缓存里拷贝像素，再把指针画上去。
 *     手绘的随机种子全是常数，所以表盘每次画出来都一模一样，局部刷新不会出现“补丁”。
 *  3. 只让变了的地方失效：秒针每秒走 6°，失效区域 = 秒针旧位置的外接矩形 + 新位置的外接矩形。
 *     分针像瑞士火车站的钟一样每分钟跳一格（时针也每分钟挪一点），所以一分钟里只有一秒要多画它们。
 *     数字时间只重画那一行字。整个窗口从不会因为“又过了一秒”而全部重画。
 *  4. 标签页隐藏时对应的定时器停掉：看不见的东西不需要每秒醒来。
 *
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * 秒表用的是单调时钟（sk_now_ms → CLOCK_MONOTONIC）：它只会匀速往前走，不受“改系统时间”影响；
 * 墙上时间（CLOCK_REALTIME）可能被人往回拨，用来计时就会出现负数。
 *
 * 调试（自动化测试 tests/apps/test_skclock.py 用）：
 *   SKCLOCK_DEBUG=1        向标准输出打印
 *                            skclock: layout face=x,y,w,h digital=... date=... tab0=... tab1=...
 *                                     display=... start=... lap=... reset=...   （每次布局）
 *                            skclock: tick 2026-10-23 14:05:09 星期五            （时钟页显示的时间变了）
 *                            skclock: page <0|1>
 *                            skclock: stopwatch running=<0|1> elapsed=<毫秒> laps=<次数>
 *   SKCLOCK_FREEZE=10:08:30  让时钟一直显示这个时刻（截图和“重画是否一模一样”的测试用）
 */
#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>
#include <sketch/rough.h>
#include <skutil.h>

#include <errno.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI_F          3.14159265f
#define MARGIN        16
#define TABS_Y        8
#define TABS_H        40
#define CONTENT_Y     (TABS_Y + TABS_H + 10)
#define DIGITAL_H     46
#define DATE_H        30
#define BG_SEED       0xC10Cu        /* 纸张纹理的种子：背景和表盘缓存必须用同一个 */
#define FACE_SEED     0xFACEu        /* 表盘各部分的手绘种子都由它派生（常数 → 每次一模一样） */
#define HAND_SEED     0x4A2Du
#define PAPER_GRAIN   3
#define MAX_LAPS      99
#define SW_TICK_MS    100

enum { HAND_HOUR, HAND_MINUTE, HAND_SECOND, NHANDS };

/* 三根指针的样子：长度和尾巴是表盘半径的比例 */
static const struct {
    float len, tail, width;
    sk_color color;
} hand_style[NHANDS] = {
    [HAND_HOUR]   = { 0.50f, 0.10f, 4.4f, SK_INK },
    [HAND_MINUTE] = { 0.74f, 0.12f, 3.0f, SK_INK },
    [HAND_SECOND] = { 0.80f, 0.20f, 1.6f, SK_RED },
};

static const char *const weekday_names[7] = { "日", "一", "二", "三", "四", "五", "六" };
static const char *const tab_labels[2] = { "时钟", "秒表" };

typedef struct {
    sk_app    *app;
    sk_window *win;
    sk_ui     *ui;
    sk_widget *tabs;
    int        page;            /* 0 = 时钟，1 = 秒表 */
    int        debug;
    int        frozen, fz_h, fz_m, fz_s;

    /* ---- 时钟页 ---- */
    sk_widget *face, *digital, *date;
    float      cx, cy, R;       /* 表盘圆心和半径（窗口坐标） */
    sk_canvas *cache;           /* 表盘缓存（坐标与窗口相同，只有 cache_r 这一块有内容），NULL = 要重画 */
    sk_rect    cache_r;
    int        shown;           /* 下面的指针角度是否有效（已经画到屏幕上） */
    float      ang[NHANDS];     /* 屏幕上指针的角度（弧度，0 = 12 点方向，顺时针） */
    char       time_text[32];
    int        tick_timer;

    /* ---- 秒表页 ---- */
    sk_widget *display, *btn_start, *btn_lap, *btn_reset, *laps;
    int        running;
    uint64_t   start_ms;        /* 本次开始时的单调时钟 */
    uint64_t   acc_ms;          /* 之前各段累计的时间 */
    uint64_t   lap_ms[MAX_LAPS];
    int        nlaps;
    char       sw_text[32];
    int        sw_timer;
} clock_app;

/* ================================================================ 小工具 */

static void dbg(clock_app *k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dbg(clock_app *k, const char *fmt, ...)
{
    if (!k->debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("skclock: ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    fflush(stdout);
}

static int rect_inside(sk_rect outer, sk_rect in)
{
    return in.x >= outer.x && in.y >= outer.y && in.x + in.w <= outer.x + outer.w &&
           in.y + in.h <= outer.y + outer.h;
}

/* 等宽数字：字体里“1”比“0”窄，直接画的话数字一变整串字就左右晃动。
 * 这里让每个数字占同样宽的格子（取 0~9 里最宽的那个），其它字符（冒号、点）用自己的宽度。 */
static int digit_cell(const sk_font *f)
{
    int w = 0;
    for (uint32_t d = '0'; d <= '9'; d++) {
        int a = sk_glyph_advance(f, d);
        if (a > w)
            w = a;
    }
    return w;
}

static int tabular_width(const sk_font *f, const char *s)
{
    int cell = digit_cell(f), w = 0;
    for (const char *p = s; *p;) {
        uint32_t cp = sk_utf8_next(&p);
        w += (cp >= '0' && cp <= '9') ? cell : sk_glyph_advance(f, cp);
    }
    return w;
}

/* 文字在 r 里水平、垂直居中时占的矩形（重画时只让这一块失效） */
static sk_rect tabular_rect(const sk_font *f, sk_rect r, const char *s)
{
    int w = tabular_width(f, s), h = sk_font_line_height(f);
    return sk_rect_make(r.x + (r.w - w) / 2 - 2, r.y + (r.h - h) / 2 - 2, w + 4, h + 4);
}

static void draw_tabular(sk_canvas *c, const sk_font *f, sk_rect r, const char *s, sk_color col)
{
    int cell = digit_cell(f);
    int x = r.x + (r.w - tabular_width(f, s)) / 2;
    int base = r.y + (r.h - sk_font_line_height(f)) / 2 + sk_font_ascent(f);
    for (const char *p = s; *p;) {
        uint32_t cp = sk_utf8_next(&p);
        if (cp >= '0' && cp <= '9') {
            int a = sk_glyph_advance(f, cp);
            sk_draw_glyph(c, f, x + (cell - a) / 2, base, cp, col);   /* 窄的数字在格子里居中 */
            x += cell;
        } else {
            x += sk_draw_glyph(c, f, x, base, cp, col);
        }
    }
}

/* ================================================================ 表盘 */

/* 表盘上一个角度方向的单位向量（0 = 12 点，顺时针；屏幕的 y 轴向下，所以是 -cos） */
static void dir_of(float a, float *dx, float *dy)
{
    *dx = sinf(a);
    *dy = -cosf(a);
}

/* 画不会动的部分：阴影、表圈、表盘、刻度、数字。c 可以是窗口画布，也可以是表盘缓存 */
static void draw_dial(sk_canvas *c, const clock_app *k)
{
    float cx = k->cx, cy = k->cy, R = k->R;
    if (R < 20.0f)
        return;
    sk_rough_style s;

    /* 1. 排线阴影：往右下错开一点的灰色圆（和窗口阴影一个画法），像一个放在纸上的钟 */
    sk_style_hatched(&s, SK_SHADOW, sk_hash_u32(FACE_SEED, 1));
    s.stroke_width = 0.0f;
    s.hachure_gap = 4.5f;
    s.fill_weight = 1.0f;
    sk_rough_circle(c, cx + R * 0.035f + 2.0f, cy + R * 0.045f + 2.0f, 2.0f * R, &s);

    /* 2. 表圈：先用纸色实心盖住阴影，再用同一个种子画蓝绿色排线（轮廓的随机数一样，两层严丝合缝） */
    uint32_t bezel = sk_hash_u32(FACE_SEED, 2);
    sk_style_hatched(&s, SK_PAPER, bezel);
    s.fill_style = SK_FILL_SOLID;
    s.stroke_width = 0.0f;
    sk_rough_circle(c, cx, cy, 2.0f * R, &s);
    sk_style_hatched(&s, SK_TEAL, bezel);
    s.stroke_width = 2.2f;
    s.hachure_gap = R > 120.0f ? 5.5f : 4.5f;
    s.fill_weight = 1.4f;
    sk_rough_circle(c, cx, cy, 2.0f * R, &s);

    /* 3. 表盘：纸色实心 + 铅笔轮廓，盖住中间的排线，只留下一圈表圈 */
    float rd = R * 0.85f;
    sk_style_hatched(&s, SK_PAPER, sk_hash_u32(FACE_SEED, 3));
    s.fill_style = SK_FILL_SOLID;
    s.stroke_width = 1.6f;
    s.roughness = 0.8f;
    sk_rough_circle(c, cx, cy, 2.0f * rd, &s);

    /* 4. 刻度：60 个分钟刻度，逢 5 是粗一点、长一点的小时刻度 */
    for (int i = 0; i < 60; i++) {
        int hour = i % 5 == 0;
        if (!hour && R < 70.0f)
            continue;                          /* 太小时只画小时刻度 */
        float dx, dy;
        dir_of((float)i * (2.0f * PI_F / 60.0f), &dx, &dy);
        float r2 = rd - R * 0.04f, r1 = r2 - (hour ? R * 0.11f : R * 0.045f);
        sk_style_pencil(&s, sk_hash_u32(FACE_SEED, 100u + (uint32_t)i));
        s.stroke = hour ? SK_INK : SK_INK_SOFT;
        s.stroke_width = hour ? (R > 120.0f ? 2.6f : 2.0f) : 1.1f;
        s.roughness = 0.5f;
        s.double_stroke = hour;
        sk_rough_line(c, cx + dx * r1, cy + dy * r1, cx + dx * r2, cy + dy * r2, &s);
    }

    /* 5. 数字 1~12：表盘大时用 32 像素的大字（它恰好包含 0~9），小了换小字 */
    const sk_font *f = sk_font_get(R >= 150.0f ? SK_FONT_LARGE : R >= 85.0f ? SK_FONT_TITLE : SK_FONT_UI);
    int fh = sk_font_ascent(f) - sk_font_descent(f);      /* 约等于数字的高度 */
    float rin = rd - R * 0.15f - R * 0.035f;               /* 小时刻度内端再往里留一点空 */
    for (int h = 1; h <= 12; h++) {
        char num[4];
        snprintf(num, sizeof num, "%d", h);
        float dx, dy;
        dir_of((float)h * (2.0f * PI_F / 12.0f), &dx, &dy);
        int tw = sk_text_width(f, num);
        /* 数字的外接框在这个方向上“伸出去”多少：3 点、9 点看宽度的一半，12 点、6 点看高度的一半。
         * 圆心离刻度的距离减去它，“12”这种宽数字也不会碰到刻度 */
        float rn = rin - (fabsf(dx) * (float)tw + fabsf(dy) * (float)fh) * 0.5f;
        int x = (int)lroundf(cx + dx * rn) - tw / 2;
        int base = (int)lroundf(cy + dy * rn) + fh / 2;
        sk_draw_text(c, f, x, base, num, h % 3 == 0 ? SK_INK : SK_INK_SOFT);
    }

    /* 6. 表盘上的小字 */
    if (R >= 90.0f) {
        const sk_font *uf = sk_font_get(SK_FONT_UI);
        const char *brand = "SketchOS";
        int tw = sk_text_width(uf, brand);
        sk_draw_text(c, uf, (int)cx - tw / 2, (int)(cy + R * 0.36f), brand, SK_INK_FAINT);
    }
}

/* 指针的粗细随表盘大小缩放（以半径 170 像素为 1 倍） */
static float hand_width(const clock_app *k, int which)
{
    float scale = k->R / 170.0f;
    scale = scale < 0.7f ? 0.7f : scale > 1.5f ? 1.5f : scale;
    return hand_style[which].width * scale;
}

/* 指针的两个端点（尾巴、针尖） */
static void hand_points(const clock_app *k, int which, float a, float *x1, float *y1, float *x2, float *y2)
{
    float dx, dy;
    dir_of(a, &dx, &dy);
    float L = k->R * hand_style[which].len, T = k->R * hand_style[which].tail;
    *x1 = k->cx - dx * T;
    *y1 = k->cy - dy * T;
    *x2 = k->cx + dx * L;
    *y2 = k->cy + dy * L;
}

/* 指针在角度 a 时可能画到的范围：两端点的外接矩形，四周加上笔宽和手绘抖动的余量
 * （手绘线的端点和中间的控制点最多偏离直线 2~3 像素，见 rough.c 的 rough_line_bez） */
static sk_rect hand_rect(const clock_app *k, int which, float a)
{
    float x1, y1, x2, y2;
    hand_points(k, which, a, &x1, &y1, &x2, &y2);
    float pad = hand_width(k, which) + 6.0f;
    int l = (int)floorf(fminf(x1, x2) - pad), t = (int)floorf(fminf(y1, y2) - pad);
    int r = (int)ceilf(fmaxf(x1, x2) + pad), b = (int)ceilf(fmaxf(y1, y2) + pad);
    return sk_rect_make(l, t, r - l, b - t);
}

/* 圆心的“轴”：画在所有指针上面 */
static float cap_diameter(const clock_app *k)
{
    float d = k->R * 0.07f;
    return d < 8.0f ? 8.0f : d;
}

static sk_rect cap_rect(const clock_app *k)
{
    int r = (int)ceilf(cap_diameter(k) / 2.0f) + 5;
    return sk_rect_make((int)k->cx - r, (int)k->cy - r, 2 * r + 1, 2 * r + 1);
}

static void draw_hands(sk_canvas *c, const clock_app *k)
{
    if (!k->shown || k->R < 20.0f)
        return;
    sk_rough_style s;
    for (int i = 0; i < NHANDS; i++) {
        float x1, y1, x2, y2;
        hand_points(k, i, k->ang[i], &x1, &y1, &x2, &y2);
        /* 种子只和“哪根针”有关，和角度无关：同一时刻重画多少次都一模一样 */
        sk_style_pencil(&s, sk_hash_u32(HAND_SEED, (uint32_t)i));
        s.stroke = hand_style[i].color;
        s.stroke_width = hand_width(k, i);
        s.roughness = 0.6f;
        s.bowing = 0.6f;
        sk_rough_line(c, x1, y1, x2, y2, &s);
    }
    sk_style_hatched(&s, SK_INK, sk_hash_u32(HAND_SEED, 10));
    s.fill_style = SK_FILL_SOLID;
    s.stroke_width = 0.0f;
    s.roughness = 0.5f;
    sk_rough_circle(c, k->cx, k->cy, cap_diameter(k), &s);
    s.fill = SK_RED;
    s.seed = sk_hash_u32(HAND_SEED, 11);
    sk_rough_circle(c, k->cx, k->cy, cap_diameter(k) * 0.45f, &s);
}

/* 表盘控件的 draw：clip 已经设成“这次要重画的那一块”。先从缓存拷贝表盘，再画指针 */
static void face_draw(sk_widget *w, sk_canvas *c, void *user)
{
    clock_app *k = user;
    sk_rect r = w->r;
    sk_rect cr = k->cache_r;
    if (!k->cache || cr.x != r.x || cr.y != r.y || cr.w != r.w || cr.h != r.h) {
        /* 表盘缓存：一块和窗口坐标系相同的画布（只用到 r 这一块），这样纸张纹理、铅笔颗粒
         * 这些“按像素坐标决定”的细节，和直接画在窗口上完全一样 */
        sk_canvas_free(k->cache);
        k->cache = sk_canvas_new(r.x + r.w, r.y + r.h);
        if (k->cache) {
            sk_set_clip(k->cache, r);
            sk_paper_fill(k->cache, r, SK_PAPER, PAPER_GRAIN, BG_SEED);
            draw_dial(k->cache, k);
            k->cache_r = r;
        }
    }
    if (k->cache) {
        sk_rect src = sk_rect_intersect(c->clip, r);
        if (!sk_rect_empty(src))
            sk_blit(c, src.x, src.y, k->cache, src);
    } else {
        draw_dial(c, k);                       /* 内存不够做缓存：每次直接画（慢一点，但结果一样） */
    }
    draw_hands(c, k);
}

/* 数字时间（等宽数字，见 draw_tabular） */
static void digital_draw(sk_widget *w, sk_canvas *c, void *user)
{
    clock_app *k = user;
    draw_tabular(c, sk_font_get(SK_FONT_LARGE), w->r, k->time_text, SK_INK);
}

/* ================================================================ 时钟的“心跳” */

static void tick_fn(sk_app *app, void *user);

/* 定一个一次性定时器，在下一个整秒刚过的时候醒来 */
static void schedule_tick(clock_app *k, const struct timespec *now)
{
    if (k->tick_timer)
        return;
    int ms = 1000 - (int)(now->tv_nsec / 1000000);
    /* 多等 3 毫秒：定时器按毫秒计时，可能比整秒早醒一点点，那样读到的还是上一秒 */
    k->tick_timer = sk_app_add_timer(k->app, ms + 3, 0, tick_fn, k);
}

/* 读当前时间，更新指针和文字；只让变了的地方失效 */
static void update_clock(clock_app *k)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        perror("skclock: clock_gettime");
        return;
    }
    struct tm tm;
    time_t sec = ts.tv_sec;
    if (!localtime_r(&sec, &tm)) {             /* 按 TZ 把 UTC 秒数换算成本地时间 */
        perror("skclock: localtime_r");
        return;
    }
    if (k->frozen) {
        tm.tm_hour = k->fz_h;
        tm.tm_min = k->fz_m;
        tm.tm_sec = k->fz_s;
    }
    int s = tm.tm_sec > 59 ? 59 : tm.tm_sec;   /* 闰秒时 tm_sec 可能是 60 */
    float a[NHANDS];
    a[HAND_SECOND] = (float)s * (2.0f * PI_F / 60.0f);
    a[HAND_MINUTE] = (float)tm.tm_min * (2.0f * PI_F / 60.0f);
    a[HAND_HOUR] = ((float)(tm.tm_hour % 12) + (float)tm.tm_min / 60.0f) * (2.0f * PI_F / 12.0f);

    int moved = 0;
    for (int i = 0; i < NHANDS; i++) {
        if (k->shown && k->ang[i] == a[i])
            continue;
        if (k->shown)
            sk_ui_invalidate(k->ui, hand_rect(k, i, k->ang[i]));     /* 擦掉旧位置 */
        sk_ui_invalidate(k->ui, hand_rect(k, i, a[i]));              /* 画上新位置 */
        k->ang[i] = a[i];
        moved = 1;
    }
    if (moved)
        sk_ui_invalidate(k->ui, cap_rect(k));
    k->shown = 1;

    char t[sizeof k->time_text];
    snprintf(t, sizeof t, "%02u:%02u:%02u", (unsigned)tm.tm_hour % 24u, (unsigned)tm.tm_min % 60u, (unsigned)s % 60u);
    if (strcmp(t, k->time_text) != 0) {
        /* 只让这一行字实际占的矩形失效（新旧文字等宽，宽度相同），而不是整个控件 */
        sk_rect old = tabular_rect(sk_font_get(SK_FONT_LARGE), k->digital->r, k->time_text);
        sk_strlcpy(k->time_text, t, sizeof k->time_text);
        sk_ui_invalidate(k->ui, sk_rect_union(old, tabular_rect(sk_font_get(SK_FONT_LARGE), k->digital->r, t)));
        dbg(k, "tick %04d-%02d-%02d %s 星期%s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, t,
            weekday_names[tm.tm_wday % 7]);
    }
    char d[64];
    snprintf(d, sizeof d, "%d年%d月%d日 星期%s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             weekday_names[tm.tm_wday % 7]);
    sk_widget_set_text(k->date, d);            /* 一天只变一次；没变时什么也不做 */
    if (k->page == 0)
        schedule_tick(k, &ts);
}

static void tick_fn(sk_app *app, void *user)
{
    (void)app;
    clock_app *k = user;
    k->tick_timer = 0;                         /* 一次性定时器：触发时库已经把它注销了 */
    update_clock(k);
}

/* ================================================================ 秒表 */

static uint64_t sw_elapsed(const clock_app *k)
{
    return k->acc_ms + (k->running ? sk_now_ms() - k->start_ms : 0);
}

static void fmt_sw(char *buf, size_t n, uint64_t ms)
{
    unsigned long long t = ms / 100;
    unsigned tenth = (unsigned)(t % 10);
    t /= 10;
    unsigned s = (unsigned)(t % 60), m = (unsigned)((t / 60) % 60), h = (unsigned)(t / 3600);
    if (h)
        snprintf(buf, n, "%u:%02u:%02u.%u", h, m, s, tenth);
    else
        snprintf(buf, n, "%02u:%02u.%u", m, s, tenth);
}

/* 秒表显示框：手绘圆角框 + 等宽大字 */
static sk_rect sw_frame(const sk_widget *w)
{
    return sk_rect_inset(w->r, 4, 4);
}

static void display_draw(sk_widget *w, sk_canvas *c, void *user)
{
    clock_app *k = user;
    sk_rect fr = sw_frame(w);
    /* 每 100 毫秒只有中间的数字要重画；那时 clip 完全在框里面，就不必再描一遍框 */
    if (!rect_inside(sk_rect_inset(fr, 8, 8), c->clip)) {
        sk_rough_style s;
        sk_style_pencil(&s, w->seed);
        s.stroke_width = 1.8f;
        sk_rough_round_rect(c, (float)fr.x, (float)fr.y, (float)fr.w, (float)fr.h, 14.0f, &s);
        /* 框底下一道蓝绿色的荧光笔，和时钟的表圈呼应 */
        int mw = fr.w / 3;
        sk_marker_highlight(c, sk_rect_make(fr.x + (fr.w - mw) / 2, fr.y + fr.h - 12, mw, 6),
                            SK_WITH_ALPHA(SK_TEAL, 0x70), w->seed ^ 0x77u);
    }
    draw_tabular(c, sk_font_get(SK_FONT_LARGE), sk_rect_make(fr.x, fr.y, fr.w, fr.h - 8), k->sw_text,
                 k->running ? SK_INK : SK_INK_SOFT);
}

static sk_rect sw_text_rect(const clock_app *k, const char *text)
{
    sk_rect fr = sw_frame(k->display);
    return tabular_rect(sk_font_get(SK_FONT_LARGE), sk_rect_make(fr.x, fr.y, fr.w, fr.h - 8), text);
}

/* 刷新秒表数字：文字变了才让它那一块失效 */
static void sw_refresh(clock_app *k, int force)
{
    char t[32];
    fmt_sw(t, sizeof t, sw_elapsed(k));
    if (!force && strcmp(t, k->sw_text) == 0)
        return;
    sk_rect old = sw_text_rect(k, k->sw_text);
    sk_strlcpy(k->sw_text, t, sizeof k->sw_text);
    sk_ui_invalidate(k->ui, sk_rect_union(old, sw_text_rect(k, t)));
}

static void sw_tick(sk_app *app, void *user)
{
    (void)app;
    sw_refresh(user, 0);
}

/* 秒表的定时器只在“正在计时而且看得见”的时候运行 */
static void sw_update_timer(clock_app *k)
{
    int want = k->running && k->page == 1;
    if (want && !k->sw_timer)
        k->sw_timer = sk_app_add_timer(k->app, SW_TICK_MS, 1, sw_tick, k);
    else if (!want && k->sw_timer) {
        sk_app_remove_timer(k->app, k->sw_timer);
        k->sw_timer = 0;
    }
}

static void sw_fill_laps(clock_app *k)
{
    sk_list_clear(k->laps);
    for (int i = k->nlaps - 1; i >= 0; i--) {          /* 最新的一次在最上面 */
        char no[24], split[32], total[32];
        snprintf(no, sizeof no, "第 %d 次", i + 1);
        fmt_sw(split, sizeof split, k->lap_ms[i] - (i ? k->lap_ms[i - 1] : 0));
        fmt_sw(total, sizeof total, k->lap_ms[i]);
        const char *cells[3] = { no, split, total };
        sk_list_add(k->laps, -1, cells, i);
    }
}

static void sw_update_buttons(clock_app *k)
{
    sk_widget_set_text(k->btn_start, k->running ? "暂停" : (k->acc_ms ? "继续" : "开始"));
    sk_widget_set_accent(k->btn_start, k->running ? SK_ORANGE : SK_GREEN);
    sk_widget_set_enabled(k->btn_lap, k->running && k->nlaps < MAX_LAPS);
    sk_widget_set_enabled(k->btn_reset, !k->running && (k->acc_ms || k->nlaps));
    dbg(k, "stopwatch running=%d elapsed=%llu laps=%d", k->running, (unsigned long long)sw_elapsed(k), k->nlaps);
}

static void on_start(sk_widget *w, void *user)
{
    (void)w;
    clock_app *k = user;
    uint64_t now = sk_now_ms();
    if (k->running) {
        k->acc_ms += now - k->start_ms;       /* 暂停：把这一段累计起来 */
        k->running = 0;
    } else {
        k->start_ms = now;
        k->running = 1;
    }
    sw_update_timer(k);
    sw_refresh(k, 1);                          /* 颜色变了（计时时黑色、暂停时灰色）：强制重画 */
    sw_update_buttons(k);
}

static void on_lap(sk_widget *w, void *user)
{
    (void)w;
    clock_app *k = user;
    if (!k->running || k->nlaps >= MAX_LAPS)
        return;
    k->lap_ms[k->nlaps++] = sw_elapsed(k);
    sw_fill_laps(k);
    sw_update_buttons(k);
}

static void on_reset(sk_widget *w, void *user)
{
    (void)w;
    clock_app *k = user;
    if (k->running)
        return;
    k->acc_ms = 0;
    k->nlaps = 0;
    sw_fill_laps(k);
    sw_refresh(k, 0);
    sw_update_buttons(k);
}

/* ================================================================ 页面、布局、事件 */

static void show_page(clock_app *k, int p)
{
    k->page = p;
    sk_widget *clock_w[] = { k->face, k->digital, k->date };
    sk_widget *sw_w[] = { k->display, k->btn_start, k->btn_lap, k->btn_reset, k->laps };
    for (size_t i = 0; i < SK_ARRAY_LEN(clock_w); i++)
        sk_widget_set_visible(clock_w[i], p == 0);
    for (size_t i = 0; i < SK_ARRAY_LEN(sw_w); i++)
        sk_widget_set_visible(sw_w[i], p == 1);
    if (p == 0) {
        update_clock(k);                       /* 马上对一次表，并重新开始每秒的心跳 */
    } else {
        if (k->tick_timer) {                   /* 表盘看不见了：时钟的心跳停下来 */
            sk_app_remove_timer(k->app, k->tick_timer);
            k->tick_timer = 0;
        }
        sw_refresh(k, 0);                      /* 秒表在后台一直在走：先把数字更新到现在 */
    }
    sw_update_timer(k);
    dbg(k, "page %d", p);
}

static void on_tab(sk_widget *w, void *user)
{
    show_page(user, sk_tabs_active(w));
}

static void dbg_rect(char *buf, size_t n, const char *name, sk_rect r)
{
    size_t len = strlen(buf);
    snprintf(buf + len, n - len, " %s=%d,%d,%d,%d", name, r.x, r.y, r.w, r.h);
}

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    clock_app *k = user;
    sk_widget_set_rect(k->tabs, sk_rect_make(MARGIN, TABS_Y, W - 2 * MARGIN, TABS_H));

    /* 时钟页：表盘是正方形，尽量大；数字时间和日期在它下面，整组上下居中 */
    int avail_h = H - CONTENT_Y - 12 - DIGITAL_H - DATE_H;
    int size = W - 2 * MARGIN < avail_h ? W - 2 * MARGIN : avail_h;
    if (size < 60)
        size = 60;
    int group_h = size + DIGITAL_H + DATE_H;
    int y0 = CONTENT_Y + (H - CONTENT_Y - 12 - group_h) / 2;
    if (y0 < CONTENT_Y)
        y0 = CONTENT_Y;
    sk_rect fr = sk_rect_make((W - size) / 2, y0, size, size);
    sk_widget_set_rect(k->face, fr);
    k->cx = (float)fr.x + (float)size / 2.0f;
    k->cy = (float)fr.y + (float)size / 2.0f;
    k->R = (float)size / 2.0f - 10.0f;         /* 留出阴影和手绘线“出头”的地方 */
    sk_widget_set_rect(k->digital, sk_rect_make(MARGIN, fr.y + size, W - 2 * MARGIN, DIGITAL_H));
    sk_widget_set_rect(k->date, sk_rect_make(MARGIN, fr.y + size + DIGITAL_H, W - 2 * MARGIN, DATE_H));
    /* 表盘尺寸变了，缓存作废（face_draw 发现矩形对不上会重画；这里先释放内存） */
    if (k->cache && (k->cache_r.w != fr.w || k->cache_r.x != fr.x || k->cache_r.y != fr.y)) {
        sk_canvas_free(k->cache);
        k->cache = NULL;
    }

    /* 秒表页 */
    int y = CONTENT_Y + 8;
    sk_widget_set_rect(k->display, sk_rect_make(MARGIN, y, W - 2 * MARGIN, 92));
    y += 92 + 14;
    int bw = (W - 2 * MARGIN - 2 * 12) / 3;
    sk_widget_set_rect(k->btn_start, sk_rect_make(MARGIN, y, bw, 40));
    sk_widget_set_rect(k->btn_lap, sk_rect_make(MARGIN + bw + 12, y, bw, 40));
    sk_widget_set_rect(k->btn_reset, sk_rect_make(MARGIN + 2 * (bw + 12), y, bw, 40));
    y += 40 + 14;
    sk_widget_set_rect(k->laps, sk_rect_make(MARGIN, y, W - 2 * MARGIN, H - 12 - y > 60 ? H - 12 - y : 60));

    if (k->debug) {
        char buf[512] = "layout";
        dbg_rect(buf, sizeof buf, "face", fr);
        dbg_rect(buf, sizeof buf, "digital", k->digital->r);
        dbg_rect(buf, sizeof buf, "date", k->date->r);
        for (int i = 0; i < 2; i++) {
            char name[8];
            snprintf(name, sizeof name, "tab%d", i);
            dbg_rect(buf, sizeof buf, name, sk_tabs_item_rect(k->tabs->r, tab_labels, 2, i));
        }
        dbg_rect(buf, sizeof buf, "display", k->display->r);
        dbg_rect(buf, sizeof buf, "start", k->btn_start->r);
        dbg_rect(buf, sizeof buf, "lap", k->btn_lap->r);
        dbg_rect(buf, sizeof buf, "reset", k->btn_reset->r);
        dbg(k, "%s", buf);
    }
}

static void background(sk_ui *ui, sk_canvas *c, sk_rect dirty, void *user)
{
    (void)ui;
    (void)user;
    sk_paper_fill(c, dirty, SK_PAPER, PAPER_GRAIN, BG_SEED);
}

/* 秒表页的键盘：空格 = 开始/暂停，回车 = 计次，退格 = 复位 */
static int on_event(sk_window *win, const sk_event *e, void *user)
{
    (void)win;
    clock_app *k = user;
    if (e->type != SK_EVENT_KEY || e->key.state == 0 || k->page != 1 ||
        (e->key.modifiers & (SK_MOD_CTRL | SK_MOD_ALT | SK_MOD_SUPER)))
        return 0;
    uint32_t kc = e->key.keycode, cp = e->key.codepoint;
    sk_widget_fn action = NULL;
    sk_widget *button = NULL;
    if (kc == KEY_SPACE || (kc == 0 && cp == ' ')) {
        action = on_start;
        button = k->btn_start;
    } else if (kc == KEY_ENTER || kc == KEY_KPENTER) {
        action = on_lap;
        button = k->btn_lap;
    } else if (kc == KEY_BACKSPACE) {
        action = on_reset;
        button = k->btn_reset;
    }
    if (!action)
        return 0;
    if (e->key.state == 1)                     /* 2 = 按住不放的自动重复：吃掉，但不重复执行 */
        action(button, k);
    return 1;
}

/* ================================================================ main */

int main(int argc, char **argv)
{
    (void)argv;
    clock_app k;
    memset(&k, 0, sizeof k);
    if (argc > 1) {
        fprintf(stderr, "用法: skclock\n");
        return 2;
    }
    const char *dv = getenv("SKCLOCK_DEBUG");
    k.debug = dv && *dv && strcmp(dv, "0") != 0;
    const char *fz = getenv("SKCLOCK_FREEZE");
    if (fz && sscanf(fz, "%d:%d:%d", &k.fz_h, &k.fz_m, &k.fz_s) == 3 && k.fz_h >= 0 && k.fz_h < 24 &&
        k.fz_m >= 0 && k.fz_m < 60 && k.fz_s >= 0 && k.fz_s < 60)
        k.frozen = 1;

    k.app = sk_app_connect("skclock");
    if (!k.app)
        return 1;
    k.win = sk_window_create(k.app, "时钟", 400, 540, SK_WIN_RESIZABLE, SK_TEAL);
    if (!k.win) {
        sk_app_disconnect(k.app);
        return 1;
    }
    sk_window_set_min_size(k.win, 300, 420);
    k.ui = sk_ui_new(k.win);
    if (!k.ui) {
        sk_app_disconnect(k.app);
        return 1;
    }

    sk_rect z = sk_rect_make(0, 0, 10, 10);
    k.tabs = sk_tabs_new(k.ui, z, tab_labels, 2, on_tab, &k);
    /* 表盘、数字时间没有事件回调：对鼠标是“透明”的 */
    k.face = sk_custom_new(k.ui, z, face_draw, NULL, 0, &k);
    k.digital = sk_custom_new(k.ui, z, digital_draw, NULL, 0, &k);
    k.date = sk_label_new(k.ui, z, "", SK_FONT_TITLE, SK_ALIGN_CENTER);
    k.display = sk_custom_new(k.ui, z, display_draw, NULL, 0, &k);
    k.btn_start = sk_button_new(k.ui, z, "开始", on_start, &k);
    k.btn_lap = sk_button_new(k.ui, z, "计次", on_lap, &k);
    k.btn_reset = sk_button_new(k.ui, z, "复位", on_reset, &k);
    k.laps = sk_list_new(k.ui, z, NULL, NULL, &k);
    if (!k.tabs || !k.face || !k.digital || !k.date || !k.display || !k.btn_start || !k.btn_lap ||
        !k.btn_reset || !k.laps) {
        fprintf(stderr, "skclock: 内存不足\n");
        return 1;
    }
    sk_label_set_color(k.date, SK_INK_SOFT);
    static const char *const cols[3] = { "计次", "单次", "总计" };
    static const int widths[3] = { 0, -110, -110 };
    sk_list_set_columns(k.laps, cols, widths, 3);
    sk_widget_set_text(k.laps, "（按“计次”记录每一圈的时间）");
    fmt_sw(k.sw_text, sizeof k.sw_text, 0);

    sk_ui_set_background(k.ui, background, &k);
    sk_ui_set_layout(k.ui, layout, &k);
    sk_ui_set_handler(k.ui, on_event, &k);
    sw_update_buttons(&k);
    show_page(&k, 0);                          /* 第一次对表，并开始每秒的心跳 */

    int rc = sk_app_run(k.app);
    sk_canvas_free(k.cache);
    sk_ui_free(k.ui);
    sk_app_disconnect(k.app);
    return rc;
}
