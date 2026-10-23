/*
 * test_font.c — 字体模块 (font.c) 的离线单元测试，在宿主机上运行
 *
 *   make -C userspace BUILD=<目录> host-tests
 *   SKETCH_FONT_DIR=<仓库>/assets/fonts <目录>/host/test_font
 *
 * 覆盖：系统字体的度量和字符覆盖、.skf 文件格式（测试里独立实现一份解析器做交叉验证）、
 * UTF-8 编解码与非法输入、sk_wcwidth、宽度测量、回退链（含成环）、缺字方框、
 * 覆盖率混合的精确数值、裁剪（绝不写出 clip 和画布）、单行截断、自动换行与避头尾、
 * 损坏/截断/随机字体文件（必须返回 NULL 而不是崩溃）、mmap 失败时的 read() 回退、
 * 字体文件全部缺失时的内置后备字体。
 *
 * 只链接 font.c、font_builtin.c 和 common/skutil.c（sk_wcwidth），不依赖 gfx.c：
 * 画布用本文件里的小工具函数创建。
 * 建议再用 -fsanitize=address,undefined -Wl,--wrap=mmap 编译运行一次：
 * 前者能发现任何越界访问，后者启用 read() 回退路径的测试（见第 9 节）。
 * 退出码 = 失败的检查数（0 表示全部通过）。
 */
#include "sketch/font.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_fail, g_pass;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            printf("  失败 %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

#define BG     0xFFFFFFFFu        /* 白纸 */
#define INK    SK_RGB(0, 0, 0)
#define GUARD  0xFF123456u        /* 哨兵颜色：检查有没有画出界 */

static const char *g_fontdir;
static char g_tmpdir[256];

/* ---------------------------------------------------------------------
 * 测试用的小画布（不依赖 gfx.c）
 * --------------------------------------------------------------------- */
static sk_canvas *tc_new(int w, int h, int stride, uint32_t fill)
{
    sk_canvas *c = calloc(1, sizeof(*c));
    c->pixels = malloc((size_t)stride * h * sizeof(uint32_t));
    for (int i = 0; i < stride * h; i++)
        c->pixels[i] = fill;
    c->w = w;
    c->h = h;
    c->stride = stride;
    c->clip = (sk_rect){ 0, 0, w, h };
    return c;
}

static void tc_free(sk_canvas *c)
{
    free(c->pixels);
    free(c);
}

static void tc_fill(sk_canvas *c, uint32_t v)
{
    for (int i = 0; i < c->stride * c->h; i++)
        c->pixels[i] = v;
}

/* area 内和 bg 不同的像素的包围盒，返回这样的像素个数 */
static int ink_bbox(const sk_canvas *c, sk_rect area, uint32_t bg, sk_rect *out)
{
    int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (int y = area.y; y < area.y + area.h; y++) {
        if (y < 0 || y >= c->h)
            continue;
        for (int x = area.x; x < area.x + area.w; x++) {
            if (x < 0 || x >= c->w)
                continue;
            if (c->pixels[(size_t)y * c->stride + x] != bg) {
                n++;
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
        }
    }
    if (out)
        *out = n ? (sk_rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 } : (sk_rect){ 0, 0, 0, 0 };
    return n;
}

/* ---------------------------------------------------------------------
 * 独立实现的 .skf 解析（用来交叉验证生成的字体文件）
 * --------------------------------------------------------------------- */
typedef struct {
    uint8_t *data;
    size_t size;
    int px, asc, desc, lh, mono, cell;
    uint32_t count, index_off, bitmap_off;
} tskf;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int le16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static int read_all(const char *path, uint8_t **data, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    *data = malloc(n > 0 ? (size_t)n : 1);
    *size = fread(*data, 1, (size_t)n, fp);
    fclose(fp);
    return *size == (size_t)n ? 0 : -1;
}

static int write_all(const char *path, const void *data, size_t size)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    size_t n = fwrite(data, 1, size, fp);
    fclose(fp);
    return n == size ? 0 : -1;
}

static int tskf_open(const char *name, tskf *t)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_fontdir, name);
    memset(t, 0, sizeof(*t));
    if (read_all(path, &t->data, &t->size) < 0 || t->size < 32)
        return -1;
    const uint8_t *d = t->data;
    t->px = d[4] | d[5] << 8;
    t->asc = le16s(d + 6);
    t->desc = le16s(d + 8);
    t->lh = d[10] | d[11] << 8;
    t->count = le32(d + 12);
    t->index_off = le32(d + 16);
    t->bitmap_off = le32(d + 20);
    t->mono = (d[24] & 1) != 0;
    t->cell = d[26] | d[27] << 8;
    return 0;
}

static const uint8_t *tskf_entry(const tskf *t, uint32_t i) { return t->data + t->index_off + 16 * i; }

static const uint8_t *tskf_find(const tskf *t, uint32_t cp)
{
    uint32_t lo = 0, hi = t->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, c = le32(tskf_entry(t, mid));
        if (c == cp)
            return tskf_entry(t, mid);
        if (c < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* GB2312 的全部汉字（用 glibc 的 iconv 解码区位码），返回个数；iconv 不可用返回 -1 */
static int gb2312_hanzi(uint32_t *out, int max)
{
    iconv_t cd = iconv_open("UTF-32LE", "GB2312");
    if (cd == (iconv_t)-1)
        return -1;
    int n = 0;
    for (int row = 0xB0; row <= 0xF7; row++) {
        for (int col = 0xA1; col <= 0xFE; col++) {
            char in[2] = { (char)row, (char)col };
            uint32_t u = 0;
            char *ip = in, *op = (char *)&u;
            size_t il = 2, ol = 4;
            if (iconv(cd, &ip, &il, &op, &ol) == (size_t)-1) {
                iconv(cd, NULL, NULL, NULL, NULL);
                continue;
            }
            /* GB2312 的空位（如 D7FA..D7FE）：glibc 报错，musl 按 GBK/GB18030 解码成私用区码点，都跳过 */
            if (ol == 0 && n < max && !(u >= 0xE000 && u <= 0xF8FF))
                out[n++] = u;
        }
    }
    iconv_close(cd);
    return n;
}

/* ---------------------------------------------------------------------
 * 生成合成的小字体文件（测试回退、混合、损坏文件用）
 * --------------------------------------------------------------------- */
typedef struct {
    uint32_t cp;
    int w, h, left, top, adv;
    const uint8_t *bits;        /* NULL = 全部 255 */
} tglyph;

static size_t build_skf(uint8_t *out, size_t cap, int px, int asc, int desc, int lh, int mono,
                        int cell, const tglyph *g, int n)
{
    size_t idx = 32, bm = idx + 16 * (size_t)n, off = 0;
    size_t total = bm;
    for (int i = 0; i < n; i++)
        total += (size_t)g[i].w * g[i].h;
    if (total > cap)
        return 0;
    memset(out, 0, total);
    memcpy(out, "SKF1", 4);
    out[4] = (uint8_t)px; out[6] = (uint8_t)asc; out[8] = (uint8_t)desc; out[10] = (uint8_t)lh;
    uint32_t v[3] = { (uint32_t)n, (uint32_t)idx, (uint32_t)bm };
    for (int k = 0; k < 3; k++)
        for (int b = 0; b < 4; b++)
            out[12 + 4 * k + b] = (uint8_t)(v[k] >> (8 * b));
    out[24] = mono ? 1 : 0;
    out[26] = (uint8_t)cell;
    for (int i = 0; i < n; i++) {
        uint8_t *e = out + idx + 16 * (size_t)i;
        for (int b = 0; b < 4; b++) {
            e[b] = (uint8_t)(g[i].cp >> (8 * b));
            e[4 + b] = (uint8_t)(off >> (8 * b));
        }
        e[8] = (uint8_t)g[i].w; e[9] = (uint8_t)g[i].h;
        e[10] = (uint8_t)(int8_t)g[i].left; e[11] = (uint8_t)(int8_t)g[i].top;
        e[12] = (uint8_t)g[i].adv;
        size_t sz = (size_t)g[i].w * g[i].h;
        if (g[i].bits)
            memcpy(out + bm + off, g[i].bits, sz);
        else
            memset(out + bm + off, 255, sz);
        off += sz;
    }
    return total;
}

static sk_font *load_synthetic(const char *name, int px, int asc, int desc, int lh, int mono,
                               int cell, const tglyph *g, int n)
{
    static uint8_t buf[65536];
    char path[512];
    size_t sz = build_skf(buf, sizeof(buf), px, asc, desc, lh, mono, cell, g, n);
    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, name);
    if (!sz || write_all(path, buf, sz) < 0)
        return NULL;
    return sk_font_load(path);
}

/* =====================================================================
 * 1. 系统字体：度量、文件格式、字符覆盖
 * ===================================================================== */
static void test_system_fonts(void)
{
    printf("[系统字体与 .skf 文件]\n");
    static const char *files[SK_FONT_COUNT] = { "sans-16.skf", "sans-20.skf", "sans-32.skf", "mono-16.skf" };
    static const int sizes[SK_FONT_COUNT] = { 16, 20, 32, 16 };
    static uint32_t hanzi[8000];
    int nh = gb2312_hanzi(hanzi, 8000);
    if (nh < 0)
        printf("  （iconv 不支持 GB2312，跳过 GB2312 全集检查）\n");
    else
        CHECK(nh == 6763, "GB2312 汉字应为 6763 个，iconv 得到 %d", nh);

    for (int id = 0; id < SK_FONT_COUNT; id++) {
        sk_font *f = sk_font_get(id);
        CHECK(f != NULL, "sk_font_get(%d) 返回 NULL", id);
        CHECK(sk_font_get(id) == f, "sk_font_get(%d) 两次结果不同（应缓存）", id);
        tskf t;
        if (tskf_open(files[id], &t) < 0) {
            CHECK(0, "打不开 %s/%s（SKETCH_FONT_DIR 设置对了吗？）", g_fontdir, files[id]);
            continue;
        }
        CHECK(memcmp(t.data, "SKF1", 4) == 0, "%s 魔数", files[id]);
        CHECK(t.px == sizes[id], "%s 字号 %d", files[id], t.px);
        CHECK(t.index_off == 32 && t.bitmap_off == 32 + 16 * t.count, "%s 偏移", files[id]);
        /* 确认 sk_font_get 真的加载了文件（而不是退回内置字体） */
        CHECK(sk_font_ascent(f) == t.asc && sk_font_descent(f) == t.desc &&
              sk_font_line_height(f) == t.lh, "%s 度量与文件头不一致", files[id]);
        CHECK(t.asc > 0 && t.desc > 0 && t.lh >= t.asc + t.desc, "%s ascent/descent/行高", files[id]);
        CHECK(sk_font_is_mono(f) == (id == SK_FONT_MONO), "%s 等宽标志", files[id]);

        /* 索引严格升序、位图都在文件内、ASCII 齐全 */
        uint32_t prev = 0;
        int sorted = 1, inside = 1, ascii = 1, mono_ok = 1, first_bad = 0;
        for (uint32_t i = 0; i < t.count; i++) {
            const uint8_t *e = tskf_entry(&t, i);
            uint32_t cp = le32(e);
            if (i && cp <= prev)
                sorted = 0;
            if ((uint64_t)t.bitmap_off + le32(e + 4) + (uint64_t)e[8] * e[9] > t.size)
                inside = 0;
            /* 等宽字体：每个字的前进宽度必须正好是 sk_wcwidth 格（mkfont.py 和 C 代码用同一张宽度表） */
            if (t.mono && e[12] != sk_wcwidth(cp) * t.cell) {
                if (mono_ok)
                    first_bad = (int)cp;
                mono_ok = 0;
            }
            prev = cp;
        }
        for (uint32_t cp = 0x20; cp < 0x7F; cp++)
            if (!tskf_find(&t, cp))
                ascii = 0;
        CHECK(sorted, "%s 索引未排序", files[id]);
        CHECK(inside, "%s 有位图超出文件", files[id]);
        CHECK(ascii, "%s 缺少 ASCII 字符", files[id]);
        CHECK(mono_ok, "%s U+%04X 的前进宽度不等于 sk_wcwidth×格宽", files[id], first_bad);
        CHECK(tskf_find(&t, 0xFFFD) != NULL, "%s 缺少替换字符 U+FFFD", files[id]);

        if (id != SK_FONT_LARGE && nh > 0) {
            int missing = 0;
            uint32_t first = 0;
            for (int i = 0; i < nh; i++)
                if (!tskf_find(&t, hanzi[i]) && !missing++)
                    first = hanzi[i];
            CHECK(missing == 0, "%s 缺 %d 个 GB2312 汉字（第一个 U+%04X）", files[id], missing, first);
        }
        if (id != SK_FONT_LARGE) {
            int box = 1;
            for (uint32_t cp = 0x2500; cp <= 0x259F; cp++)
                if (!tskf_find(&t, cp))
                    box = 0;
            CHECK(box, "%s 缺少制表符/方块元素", files[id]);
        }
        /* 大字体至少要有时钟和启动画面的字 */
        const char *must = (id == SK_FONT_LARGE) ? "0123456789:年月日时分秒星期一二三四五六天手绘操作系统"
                                                 : "，。、；：？！“”《》（）…—";
        const char *p = must;
        uint32_t cp;
        int have = 1;
        while ((cp = sk_utf8_next(&p)) != 0)
            if (!tskf_find(&t, cp)) {
                have = 0;
                printf("    %s 缺 U+%04X\n", files[id], cp);
            }
        CHECK(have, "%s 缺少必需的字", files[id]);
        free(t.data);
    }

    sk_font *ui = sk_font_get(SK_FONT_UI), *title = sk_font_get(SK_FONT_TITLE);
    sk_font *large = sk_font_get(SK_FONT_LARGE), *mono = sk_font_get(SK_FONT_MONO);
    CHECK(sk_font_ascent(ui) < sk_font_ascent(title) && sk_font_ascent(title) < sk_font_ascent(large),
          "字号顺序 UI < TITLE < LARGE");
    CHECK(sk_font_cell_width(mono) == 8, "mono 格宽 = %d", sk_font_cell_width(mono));
    CHECK(sk_font_line_height(mono) == sk_font_ascent(mono) + sk_font_descent(mono),
          "等宽字体行高必须等于 ascent+descent（终端格子）");
    int avg = sk_font_cell_width(ui);
    CHECK(avg >= 5 && avg <= 12, "比例字体平均宽度 %d", avg);
    CHECK(sk_glyph_advance(mono, 'M') == 8 && sk_glyph_advance(mono, 'i') == 8, "mono 半角 8px");
    CHECK(sk_glyph_advance(mono, 0x4E2D) == 16, "mono 汉字两格");
    CHECK(sk_glyph_advance(mono, 0xFF0C) == 16 && sk_glyph_advance(mono, 0x3002) == 16, "mono 中文标点两格");
    CHECK(sk_glyph_advance(mono, 0x2500) == 8 && sk_glyph_advance(mono, 0x2588) == 8, "mono 制表符一格");
    CHECK(sk_glyph_advance(mono, 0x201C) == 8 && sk_glyph_advance(mono, 0x2026) == 8, "mono 歧义宽度字符一格");
    CHECK(sk_glyph_advance(ui, 0x4E2D) == 16, "UI 汉字宽 16");
    CHECK(sk_glyph_advance(title, 0x4E2D) == 20 && sk_glyph_advance(large, 0x4E2D) == 32, "TITLE/LARGE 汉字宽");
    CHECK(sk_glyph_advance(ui, 'i') < sk_glyph_advance(ui, 'M'), "比例字体 i 比 M 窄");
    /* 大字体里没有的汉字走回退链（用 20px 的字形），宽度应是 20 而不是方框 */
    CHECK(sk_glyph_advance(large, 0x9B23 /* 鬣 */) == 20, "LARGE 回退到 TITLE");
}

/* =====================================================================
 * 2. UTF-8
 * ===================================================================== */
static int decode_all(const char *s, uint32_t *out, int max)
{
    int n = 0;
    uint32_t cp;
    while (n < max && (cp = sk_utf8_next(&s)) != 0)
        out[n++] = cp;
    return n;
}

static void expect_decode(const char *in, const uint32_t *want, int nwant, const char *what)
{
    uint32_t got[16];
    int n = decode_all(in, got, 16);
    int ok = n == nwant;
    for (int i = 0; ok && i < n; i++)
        ok = got[i] == want[i];
    CHECK(ok, "UTF-8 解码 %s：得到 %d 个码点", what, n);
}

static void test_utf8(void)
{
    printf("[UTF-8]\n");
    /* 全部码点编码再解码，必须还原 */
    char buf[8];
    int bad = 0;
    for (uint32_t cp = 1; cp <= 0x10FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            continue;
        int n = sk_utf8_encode(cp, buf);
        buf[n] = 0;
        const char *p = buf;
        int want = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (n != want || sk_utf8_next(&p) != cp || p != buf + n || *p != 0)
            bad++;
    }
    CHECK(bad == 0, "编码/解码往返失败 %d 个", bad);
    CHECK(sk_utf8_encode(0xD800, buf) == 3 && memcmp(buf, "\xEF\xBF\xBD", 3) == 0, "代理区编码成 U+FFFD");
    CHECK(sk_utf8_encode(0x110000, buf) == 3 && memcmp(buf, "\xEF\xBF\xBD", 3) == 0, "超范围编码成 U+FFFD");
    CHECK(sk_utf8_encode(0x4E2D, NULL) == 3, "out 为 NULL 时只返回长度");

    const uint32_t F = 0xFFFD;
    expect_decode("\x80", (uint32_t[]){ F }, 1, "孤立后续字节");
    expect_decode("\xC0\x80", (uint32_t[]){ F, F }, 2, "超长编码 C0 80");
    expect_decode("\xC1\xBF", (uint32_t[]){ F, F }, 2, "超长编码 C1 BF");
    expect_decode("\xE0\x80\x80", (uint32_t[]){ F, F, F }, 3, "超长编码 E0 80 80");
    expect_decode("\xF0\x80\x80\x80", (uint32_t[]){ F, F, F, F }, 4, "超长编码 F0 80 80 80");
    expect_decode("\xED\xA0\x80", (uint32_t[]){ F, F, F }, 3, "代理区 D800");
    expect_decode("\xF4\x90\x80\x80", (uint32_t[]){ F, F, F, F }, 4, "超过 U+10FFFF");
    expect_decode("\xF5\x80", (uint32_t[]){ F, F }, 2, "F5");
    expect_decode("\xFF\xFE", (uint32_t[]){ F, F }, 2, "FF FE");
    expect_decode("\xE4\xB8", (uint32_t[]){ F, F }, 2, "截断的“中”");
    expect_decode("\xE4\xB8" "A", (uint32_t[]){ F, F, 'A' }, 3, "截断后接 ASCII");
    expect_decode("a\xE4\xB8\xAD" "b", (uint32_t[]){ 'a', 0x4E2D, 'b' }, 3, "a中b");
    expect_decode("\xF0\x9F\x98\x80", (uint32_t[]){ 0x1F600 }, 1, "emoji");
    expect_decode("\xEF\xBB\xBF", (uint32_t[]){ 0xFEFF }, 1, "BOM");
    expect_decode("", NULL, 0, "空串");
    const char *p = "";
    CHECK(sk_utf8_next(&p) == 0 && *p == 0, "到末尾返回 0 且不前移");
    CHECK(sk_utf8_next(NULL) == 0, "NULL 指针安全");
    p = NULL;
    CHECK(sk_utf8_next(&p) == 0, "*s 为 NULL 安全");
    CHECK(sk_utf8_len("中a\xFF") == 3, "utf8_len 含非法字节");
    CHECK(sk_utf8_len("") == 0 && sk_utf8_len(NULL) == 0, "utf8_len 空串/NULL");
    CHECK(sk_utf8_len("手绘操作系统 SketchOS") == 15, "utf8_len 中英混合");
}

/* =====================================================================
 * 3. sk_wcwidth
 * ===================================================================== */
static void test_wcwidth(void)
{
    printf("[sk_wcwidth]\n");
    static const struct { uint32_t cp; int w; } cases[] = {
        { 'A', 1 }, { ' ', 1 }, { '~', 1 }, { 0, 0 }, { '\n', 0 }, { '\t', 0 }, { 0x1B, 0 },
        { 0x7F, 0 }, { 0x85, 0 }, { 0x9F, 0 }, { 0xA0, 1 }, { 0xE9, 1 }, { 0xD7, 1 },
        { 0x0301, 0 }, { 0x0308, 0 }, { 0x20DD, 0 }, { 0x200B, 0 }, { 0x200D, 0 }, { 0xFE0F, 0 },
        { 0xFEFF, 0 }, { 0x1160, 0 }, { 0x3099, 0 },
        { 0x4E00, 2 }, { 0x4E2D, 2 }, { 0x9FA5, 2 }, { 0x3400, 2 }, { 0x20000, 2 }, { 0x2A6D6, 2 },
        { 0x3000, 2 }, { 0x3001, 2 }, { 0x3002, 2 }, { 0x300A, 2 }, { 0x303E, 2 }, { 0x303F, 1 },
        { 0xFF01, 2 }, { 0xFF0C, 2 }, { 0xFF21, 2 }, { 0xFF5E, 2 }, { 0xFF61, 1 }, { 0xFFE5, 2 },
        { 0x3042, 2 }, { 0x30A2, 2 }, { 0xAC00, 2 }, { 0xD7A3, 2 }, { 0x1100, 2 },
        { 0x1F600, 2 }, { 0x1F389, 2 }, { 0x1F680, 2 }, { 0x2705, 2 }, { 0x231A, 2 },
        { 0x2026, 1 }, { 0x201C, 1 }, { 0x2014, 1 }, { 0x2500, 1 }, { 0x2588, 1 }, { 0x25A0, 1 },
        { 0x2605, 1 }, { 0x2190, 1 }, { 0x03B1, 1 }, { 0x0416, 1 }, { 0xE000, 1 }, { 0xFFFD, 1 },
        { 0x10FFFF, 1 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK(sk_wcwidth(cases[i].cp) == cases[i].w, "sk_wcwidth(U+%04X) = %d，期望 %d",
              cases[i].cp, sk_wcwidth(cases[i].cp), cases[i].w);
}

/* =====================================================================
 * 4. 测量
 * ===================================================================== */
static void test_measure(void)
{
    printf("[测量]\n");
    sk_font *ui = sk_font_get(SK_FONT_UI), *mono = sk_font_get(SK_FONT_MONO);
    int a = sk_glyph_advance(ui, 'a'), b = sk_glyph_advance(ui, 'b'), zh = sk_glyph_advance(ui, 0x4E2D);
    CHECK(sk_text_width(ui, "") == 0 && sk_text_width(ui, NULL) == 0, "空串/NULL 宽度 0");
    CHECK(sk_text_width(ui, "ab") == a + b, "ab 宽度");
    CHECK(sk_text_width(ui, "中文") == zh + sk_glyph_advance(ui, 0x6587), "中文宽度");
    CHECK(sk_text_width_n(ui, "中文", 3) == zh, "width_n 只量前 3 字节");
    CHECK(sk_text_width_n(ui, "中文", 0) == 0, "width_n 0 字节");
    CHECK(sk_text_width_n(ui, "中文", -1) == sk_text_width(ui, "中文"), "width_n -1 = 整串");
    CHECK(sk_text_width_n(ui, "中", 2) == 2 * sk_glyph_advance(ui, 0xFFFD), "被长度截断的序列当作非法字节");
    CHECK(sk_text_width_n(ui, "ab\0cd", 5) == a + b, "width_n 遇到 NUL 停止");
    CHECK(sk_glyph_advance(ui, '\t') == 4 * sk_glyph_advance(ui, ' '), "制表符 = 4 个空格");
    CHECK(sk_glyph_advance(ui, '\n') == 0 && sk_glyph_advance(ui, '\r') == 0, "控制字符宽 0");
    CHECK(sk_glyph_advance(ui, 0x1F600) == 16, "缺字（宽字符）方框宽 = 字号");
    CHECK(sk_glyph_advance(ui, 0xE000) == 8, "缺字（窄字符）方框宽 = 字号/2");
    CHECK(sk_glyph_advance(mono, 0x1F600) == 16 && sk_glyph_advance(mono, 0xE000) == 8, "mono 缺字方框按格子");
    CHECK(sk_glyph_advance(ui, 0x10FFFF + 5) > 0, "超出 Unicode 的码点也不崩溃");
    CHECK(sk_text_width(mono, "ls -l 文件") == 8 * 6 + 16 * 2, "mono 混排宽度");
    /* NULL 字体 = 内置字体 */
    CHECK(sk_font_ascent(NULL) > 0 && sk_glyph_advance(NULL, 'A') == 8, "NULL 字体当作内置字体");
}

/* =====================================================================
 * 5. 混合、裁剪、缺字方框、回退链（合成字体）
 * ===================================================================== */
static void test_blend_clip_fallback(void)
{
    printf("[混合/裁剪/回退链]\n");
    static const uint8_t bits4[4] = { 0, 128, 255, 64 };
    tglyph ga[] = {
        { 'X', 4, 1, 0, 1, 5, bits4 },
        { 'a', 3, 5, 0, 5, 4, NULL },
        { 0x4E2D, 10, 10, 1, 9, 12, NULL },
    };
    sk_font *A = load_synthetic("a.skf", 12, 9, 3, 12, 0, 0, ga, 3);
    CHECK(A != NULL, "合成字体 A 加载");
    if (!A)
        return;

    sk_canvas *c = tc_new(32, 8, 32, BG);
    /* 精确的混合数值：白底黑字，覆盖率 128 → 127，64 → 191，255 → 纯黑，0 → 不变 */
    int x = sk_draw_text(c, A, 2, 3, "X", INK);
    CHECK(x == 7, "sk_draw_text 返回结束 x = %d", x);
    uint32_t *row = c->pixels + 2 * 32;
    CHECK(row[2] == BG, "覆盖率 0 不改变像素");
    CHECK(row[3] == 0xFF7F7F7Fu, "覆盖率 128 → %08X", row[3]);
    CHECK(row[4] == 0xFF000000u, "覆盖率 255 → %08X", row[4]);
    CHECK(row[5] == 0xFFBFBFBFu, "覆盖率 64 → %08X", row[5]);
    /* 颜色带透明度：覆盖率 255 × alpha 128 */
    tc_fill(c, BG);
    sk_draw_text(c, A, 0, 1, "X", SK_RGBA(0, 0, 0, 128));
    CHECK(c->pixels[2] == 0xFF7F7F7Fu, "alpha 128 → %08X", c->pixels[2]);
    tc_fill(c, BG);
    sk_draw_text(c, A, 0, 1, "X", SK_RGBA(0, 0, 0, 0));
    CHECK(ink_bbox(c, (sk_rect){ 0, 0, 32, 8 }, BG, NULL) == 0, "alpha 0 什么都不画");
    /* 透明目标像素（边框层）：覆盖过半画实色，否则保持透明 */
    tc_fill(c, 0x00000000u);
    sk_draw_text(c, A, 0, 1, "X", SK_RGB(10, 20, 30));
    CHECK(c->pixels[0] == 0 && c->pixels[3] == 0, "透明像素 + 低覆盖率保持透明");
    CHECK(c->pixels[1] == SK_RGB(10, 20, 30) && c->pixels[2] == SK_RGB(10, 20, 30), "透明像素 + 高覆盖率画实色");
    tc_free(c);

    /* 裁剪：clip 之外、画布之外（含 stride 填充区）一个像素都不许碰 */
    c = tc_new(40, 30, 48, GUARD);
    c->clip = (sk_rect){ 5, 6, 20, 15 };
    for (int y = c->clip.y; y < c->clip.y + c->clip.h; y++)
        for (int xx = c->clip.x; xx < c->clip.x + c->clip.w; xx++)
            c->pixels[y * 48 + xx] = BG;
    sk_font *fonts[] = { A, sk_font_get(SK_FONT_UI), sk_font_get(SK_FONT_LARGE), sk_font_get(SK_FONT_MONO) };
    for (int fi = 0; fi < 4; fi++)
        for (int yy = -40; yy <= 70; yy += 7)
            for (int xx = -60; xx <= 60; xx += 11) {
                sk_draw_text(c, fonts[fi], xx, yy, "中Xa缺\xF0\x9F\x98\x80g|", INK);
                sk_draw_glyph(c, fonts[fi], xx, yy, 0x2588, INK);
                sk_draw_text_in_rect(c, fonts[fi], (sk_rect){ xx, yy, 25, 10 }, "很长的标题文字", INK, SK_ALIGN_CENTER);
                sk_draw_text_wrapped(c, fonts[fi], (sk_rect){ xx, yy, 20, 0 }, "换行 text 测试", INK, 0);
            }
    int leaked = 0;
    for (int y = 0; y < 30; y++)
        for (int xx = 0; xx < 48; xx++) {
            int inside = xx >= 5 && xx < 25 && y >= 6 && y < 21;
            if (!inside && c->pixels[y * 48 + xx] != GUARD)
                leaked++;
        }
    CHECK(leaked == 0, "有 %d 个 clip 之外的像素被改写", leaked);
    CHECK(ink_bbox(c, c->clip, BG, NULL) > 0, "clip 之内应该画上了东西");
    tc_free(c);

    /* 缺字方框：画在前进宽度之内，有墨迹 */
    c = tc_new(40, 20, 40, BG);
    int adv = sk_draw_glyph(c, A, 10, 15, 'q', INK);
    sk_rect bb;
    int n = ink_bbox(c, (sk_rect){ 0, 0, 40, 20 }, BG, &bb);
    CHECK(adv == 6 && n > 8, "缺字方框：advance=%d 墨迹=%d", adv, n);
    CHECK(bb.x >= 10 && bb.x + bb.w <= 10 + adv && bb.y + bb.h <= 15, "方框在前进宽度内、基线之上");
    tc_fill(c, BG);
    sk_draw_glyph(c, A, 10, 15, 0x0301, INK);   /* 缺失的组合字符：什么都不画 */
    CHECK(ink_bbox(c, (sk_rect){ 0, 0, 40, 20 }, BG, NULL) == 0 && sk_glyph_advance(A, 0x0301) == 0,
          "缺失的组合字符不画方框");
    tc_free(c);

    /* 回退链 A → B → C，以及成环 */
    tglyph gb[] = { { 'b', 3, 5, 0, 5, 7, NULL } };
    tglyph gc[] = { { 'c', 3, 5, 0, 5, 9, NULL } };
    sk_font *B = load_synthetic("b.skf", 12, 9, 3, 12, 0, 0, gb, 1);
    sk_font *C = load_synthetic("c.skf", 12, 9, 3, 12, 0, 0, gc, 1);
    CHECK(B && C, "合成字体 B C 加载");
    if (B && C) {
        CHECK(sk_text_width(A, "abc") == 4 + 6 + 6, "无回退时 b c 是方框（宽 6）");
        sk_font_set_fallback(A, B);
        sk_font_set_fallback(B, C);
        CHECK(sk_text_width(A, "abc") == 4 + 7 + 9, "回退链 A→B→C：%d", sk_text_width(A, "abc"));
        sk_font_set_fallback(C, A);                 /* 成环 */
        CHECK(sk_text_width(A, "abcz") == 4 + 7 + 9 + 6, "成环的回退链也能结束");
        sk_font_set_fallback(A, A);                 /* 自己回退到自己：忽略 */
        CHECK(sk_text_width(A, "ab") == 4 + 7, "set_fallback(A, A) 被忽略");

        /* 等宽主字体借用比例字体的字形：宽度仍然是整数格，字形居中 */
        tglyph gm[] = { { 'm', 4, 5, 1, 5, 6, NULL } };
        sk_font *M = load_synthetic("m.skf", 12, 9, 3, 12, 1, 6, gm, 1);
        CHECK(M && sk_font_is_mono(M) && sk_font_cell_width(M) == 6, "合成等宽字体");
        if (M) {
            sk_font_set_fallback(M, A);
            CHECK(sk_glyph_advance(M, 'a') == 6, "等宽借用窄字：1 格");
            CHECK(sk_glyph_advance(M, 0x4E2D) == 12, "等宽借用宽字：2 格");
            c = tc_new(40, 20, 40, BG);
            sk_draw_glyph(c, M, 0, 15, 'b', INK);   /* B 里 'b' 宽 7，居中进 6 像素格子：dx = -1/2 = 0 */
            n = ink_bbox(c, (sk_rect){ 0, 0, 40, 20 }, BG, &bb);
            CHECK(n > 0 && bb.x >= 0 && bb.x + bb.w <= 6, "借来的字形在格子里");
            tc_free(c);
            sk_font_free(M);
        }
        sk_font_set_fallback(C, NULL);
        sk_font_set_fallback(A, NULL);
        sk_font_free(B);
        sk_font_free(C);
    }
    sk_font_free(A);
    sk_font_free(NULL);
    sk_font_free(sk_font_get(SK_FONT_UI));          /* 系统字体：不会被释放 */
    CHECK(sk_text_width(sk_font_get(SK_FONT_UI), "ok") > 0, "系统字体 free 之后仍可用");
}

/* =====================================================================
 * 6. 单行截断与对齐
 * ===================================================================== */
static void test_in_rect(void)
{
    printf("[单行：对齐与 “…” 截断]\n");
    sk_font *ui = sk_font_get(SK_FONT_UI);
    sk_canvas *c = tc_new(400, 60, 400, BG);
    const char *longtxt = "这是一个非常非常长的窗口标题 Very Long Title";
    sk_rect bb;

    for (int w = 0; w <= 300; w++) {
        tc_fill(c, BG);
        sk_rect r = { 50, 10, w, 30 };
        sk_draw_text_in_rect(c, ui, r, longtxt, INK, w % 3);
        int n = ink_bbox(c, (sk_rect){ 0, 0, 400, 60 }, BG, &bb);
        if (n && (bb.x < r.x || bb.x + bb.w > r.x + r.w)) {
            CHECK(0, "宽度 %d：文字画出了矩形 [%d,%d)，墨迹 [%d,%d)", w, r.x, r.x + r.w, bb.x, bb.x + bb.w);
            break;
        }
    }
    CHECK(1, "截断不出界");

    /* 截断时最右边是省略号：右端附近有墨迹 */
    tc_fill(c, BG);
    sk_draw_text_in_rect(c, ui, (sk_rect){ 10, 10, 150, 30 }, longtxt, INK, SK_ALIGN_LEFT);
    ink_bbox(c, (sk_rect){ 0, 0, 400, 60 }, BG, &bb);
    CHECK(bb.x + bb.w > 10 + 150 - 18, "截断后右端有省略号（墨迹到 %d）", bb.x + bb.w);

    /* 不截断：居中、右对齐 */
    const char *s = "确定";
    int tw = sk_text_width(ui, s);
    tc_fill(c, BG);
    sk_draw_text_in_rect(c, ui, (sk_rect){ 100, 10, 200, 40 }, s, INK, SK_ALIGN_CENTER);
    ink_bbox(c, (sk_rect){ 0, 0, 400, 60 }, BG, &bb);
    int cx = bb.x + bb.w / 2, cy = bb.y + bb.h / 2;
    CHECK(cx >= 200 - 3 && cx <= 200 + 3, "水平居中：墨迹中心 x=%d（矩形中心 200）", cx);
    CHECK(cy >= 30 - 3 && cy <= 30 + 3, "垂直居中：墨迹中心 y=%d（矩形中心 30）", cy);
    tc_fill(c, BG);
    sk_draw_text_in_rect(c, ui, (sk_rect){ 100, 10, 200, 40 }, s, INK, SK_ALIGN_RIGHT);
    ink_bbox(c, (sk_rect){ 0, 0, 400, 60 }, BG, &bb);
    CHECK(bb.x + bb.w <= 300 && bb.x + bb.w >= 300 - 3 && bb.x >= 300 - tw - 1, "右对齐");
    tc_fill(c, BG);
    sk_draw_text_in_rect(c, ui, (sk_rect){ 100, 10, 200, 40 }, s, INK, SK_ALIGN_LEFT);
    ink_bbox(c, (sk_rect){ 0, 0, 400, 60 }, BG, &bb);
    CHECK(bb.x >= 100 && bb.x <= 103, "左对齐");

    /* 刚好放下（宽度 == 文字宽度）不截断：和直接画的结果逐像素相同 */
    sk_canvas *c2 = tc_new(400, 60, 400, BG);
    tc_fill(c, BG);
    sk_draw_text_in_rect(c, ui, (sk_rect){ 20, 0, tw, 60 }, s, INK, SK_ALIGN_LEFT);
    int base = (60 - (sk_font_ascent(ui) + sk_font_descent(ui))) / 2 + sk_font_ascent(ui);
    sk_draw_text(c2, ui, 20, base, s, INK);
    CHECK(memcmp(c->pixels, c2->pixels, 400 * 60 * 4) == 0, "宽度刚好时不截断");
    tc_free(c2);

    /* NULL / 空串 / 零宽矩形：安全 */
    sk_draw_text_in_rect(c, ui, (sk_rect){ 0, 0, 100, 20 }, NULL, INK, 0);
    sk_draw_text_in_rect(NULL, ui, (sk_rect){ 0, 0, 100, 20 }, "x", INK, 0);
    sk_draw_text_in_rect(c, ui, (sk_rect){ 0, 0, -5, 20 }, "x", INK, 0);
    sk_draw_text_in_rect(c, ui, (sk_rect){ 0, 0, 100, 20 }, "", INK, 0);
    CHECK(1, "异常参数不崩溃");
    tc_free(c);
}

/* =====================================================================
 * 7. 自动换行
 * ===================================================================== */
static int lines_of(const sk_font *f, int w, const char *s)
{
    int h = sk_draw_text_wrapped(NULL, f, (sk_rect){ 0, 0, w, 0 }, s, INK, 0);
    return h / sk_font_line_height(f);
}

/* 画出来，返回第 line 行的墨迹包围盒 */
static int line_ink(const sk_font *f, int w, const char *s, int line, sk_rect *bb)
{
    int lh = sk_font_line_height(f);
    sk_canvas *c = tc_new(600, lh * 8, 600, BG);
    sk_draw_text_wrapped(c, f, (sk_rect){ 0, 0, w, 0 }, s, INK, 0);
    int n = ink_bbox(c, (sk_rect){ 0, line * lh, 600, lh }, BG, bb);
    tc_free(c);
    return n;
}

static void test_wrap(void)
{
    printf("[自动换行]\n");
    sk_font *ui = sk_font_get(SK_FONT_UI);
    int lh = sk_font_line_height(ui);
    int cw = sk_glyph_advance(ui, 0x4E00);
    sk_rect bb;

    CHECK(sk_draw_text_wrapped(NULL, ui, (sk_rect){ 0, 0, 100, 0 }, "", INK, 0) == 0, "空串高度 0");
    CHECK(sk_draw_text_wrapped(NULL, ui, (sk_rect){ 0, 0, 100, 0 }, NULL, INK, 0) == 0, "NULL 高度 0");
    CHECK(lines_of(ui, 100, "abc") == 1, "一行");
    CHECK(lines_of(ui, 100, "a\nb") == 2 && lines_of(ui, 100, "a\n\nb") == 3, "\\n 换行");
    CHECK(lines_of(ui, 100, "\n") == 2 && lines_of(ui, 100, "a\n") == 2, "结尾的 \\n 也开新行");
    CHECK(lines_of(ui, 100, "a\r\nb") == 2, "CRLF");
    CHECK(lines_of(ui, cw * 4, "一二三四五六七八九十") == 3, "中文按字折行：10 字 / 每行 4 字 = 3 行");
    CHECK(lines_of(ui, cw * 4 - 1, "一二三四") == 2, "宽度差 1 像素就要折行");
    int hw = sk_text_width(ui, "hello world");
    CHECK(lines_of(ui, hw, "hello world") == 1, "宽度刚好放下");
    CHECK(lines_of(ui, hw - 1, "hello world") == 2, "英文按词折行");
    CHECK(lines_of(ui, sk_text_width(ui, "hello"), "hello     ") == 1, "行尾空格不导致多一行");
    CHECK(lines_of(ui, 5 * sk_glyph_advance(ui, 'a'), "aaaaaaaaaaaaaaaaaaaa") == 4, "超长单词硬拆");
    CHECK(lines_of(ui, 5, "中文") == 2, "宽度比一个字还小：每行至少一个字");
    CHECK(lines_of(ui, 0, "很长很长很长很长很长很长很长很长很长的一行文字") == 1, "宽度 0：不自动换行");
    CHECK(sk_draw_text_wrapped(NULL, ui, (sk_rect){ 0, 0, 100, 0 }, "a\nb\nc", INK, 4) == 3 * lh + 8,
          "line_gap");

    /* 避头：闭标点不能在行首 → “三，”一起挪到下一行 */
    line_ink(ui, cw * 3, "一二三，", 0, &bb);
    CHECK(bb.x + bb.w <= cw * 2 + 1, "避头：第一行只有“一二”（墨迹到 %d）", bb.x + bb.w);
    CHECK(line_ink(ui, cw * 3, "一二三，", 1, &bb) > 0 && lines_of(ui, cw * 3, "一二三，") == 2, "避头：第二行“三，”");
    /* 避尾：开标点不能在行尾 → “（三”一起挪到下一行 */
    line_ink(ui, cw * 3, "一二（三", 0, &bb);
    CHECK(bb.x + bb.w <= cw * 2 + 1, "避尾：第一行只有“一二”（墨迹到 %d）", bb.x + bb.w);
    /* 连续的闭标点都粘在前一个字上 */
    CHECK(lines_of(ui, cw * 3, "一二三。”") == 2, "多个闭标点");
    /* 英文单词能整体挪下去就不拆 */
    int w2 = 2 * cw + sk_text_width(ui, "Lin");
    line_ink(ui, w2, "使用Linux内核", 0, &bb);
    CHECK(bb.x + bb.w <= 2 * cw + 1, "英文单词整体挪到下一行（墨迹到 %d）", bb.x + bb.w);
    /* 段首缩进保留，软换行后的行首空格丢掉 */
    sk_rect b1, b2;
    line_ink(ui, 300, "abc", 0, &b1);
    line_ink(ui, 300, "    abc", 0, &b2);
    CHECK(b2.x - b1.x == 4 * sk_glyph_advance(ui, ' '), "段首缩进保留");
    int aw = sk_text_width(ui, "aaaa");
    line_ink(ui, aw, "aaaa      bbbb", 1, &b2);
    line_ink(ui, aw, "bbbb", 0, &b1);
    CHECK(b2.x == b1.x, "软换行后行首空格丢掉");
    /* 组合字符不和前一个字拆开 */
    CHECK(lines_of(ui, sk_glyph_advance(ui, 'e'), "e\xCC\x81" "e\xCC\x81") == 2, "组合字符跟着前一个字");
    /* 画和量的结果一致：画出来的行数 = 返回的高度 */
    const char *para = "操作系统（Operating System）是管理计算机硬件与软件资源的程序。"
                       "它负责进程调度、内存管理、文件系统和设备驱动。\n第二段：Linux 内核。";
    int h = sk_draw_text_wrapped(NULL, ui, (sk_rect){ 0, 0, 200, 0 }, para, INK, 3);
    sk_canvas *c = tc_new(300, 400, 300, BG);
    int h2 = sk_draw_text_wrapped(c, ui, (sk_rect){ 10, 5, 200, 0 }, para, INK, 3);
    int n = ink_bbox(c, (sk_rect){ 0, 0, 300, 400 }, BG, &bb);
    CHECK(h == h2 && n > 0, "测量与绘制高度一致");
    CHECK(bb.x >= 10 && bb.x + bb.w <= 10 + 200 + 1, "换行后不超出宽度（墨迹 [%d,%d)）", bb.x, bb.x + bb.w);
    CHECK(bb.y + bb.h <= 5 + h + 2, "墨迹不超出返回的高度");
    tc_free(c);
}

/* =====================================================================
 * 8. 损坏的字体文件
 * ===================================================================== */
static uint32_t g_rng = 12345;
static uint32_t rnd(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return g_rng >> 8;
}

static int load_bytes(const void *data, size_t size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/bad.skf", g_tmpdir);
    if (write_all(path, data, size) < 0)
        return -1;
    sk_font *f = sk_font_load(path);
    if (!f)
        return 0;
    /* 居然加载成功了：那就画一大堆字，确保不会越界（配合 AddressSanitizer） */
    sk_canvas *c = tc_new(64, 64, 64, BG);
    for (uint32_t cp = 0; cp < 0x300; cp++)
        sk_draw_glyph(c, f, 10, 40, cp, INK);
    for (int i = 0; i < 200; i++)
        sk_draw_glyph(c, f, (int)(rnd() % 80) - 10, (int)(rnd() % 80) - 10, rnd() % 0x110000, INK);
    sk_draw_text_wrapped(c, f, (sk_rect){ 0, 0, 30, 0 }, "abc 中文 xyz", INK, 0);
    tc_free(c);
    sk_font_free(f);
    return 1;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* 几千个坏文件的 [font] 报错没必要全打出来：暂时把 stderr 指向 /dev/null */
static int g_saved_stderr = -1;
static void quiet_stderr(int on)
{
    fflush(stderr);
    if (on) {
        int fd = open("/dev/null", O_WRONLY);
        g_saved_stderr = dup(2);
        if (fd >= 0) {
            dup2(fd, 2);
            close(fd);
        }
    } else if (g_saved_stderr >= 0) {
        dup2(g_saved_stderr, 2);
        close(g_saved_stderr);
        g_saved_stderr = -1;
    }
}

static void test_corrupt(void)
{
    printf("[损坏的字体文件]（下面的 [font] 报错是预期的）\n");
    char path[512];
    CHECK(sk_font_load(NULL) == NULL, "NULL 路径");
    CHECK(sk_font_load("/nonexistent/x.skf") == NULL, "不存在的文件");
    CHECK(sk_font_load(g_tmpdir) == NULL, "目录");
    snprintf(path, sizeof(path), "%s/empty.skf", g_tmpdir);
    write_all(path, "", 0);
    CHECK(sk_font_load(path) == NULL, "空文件");

    /* 一个合法的小字体作为基准 */
    tglyph g[40];
    for (int i = 0; i < 40; i++)
        g[i] = (tglyph){ 0x20 + (uint32_t)i * 7, 1 + i % 5, 1 + i % 7, i % 3 - 1, 8, 6, NULL };
    static uint8_t good[8192], bad[8192];
    size_t gsz = build_skf(good, sizeof(good), 12, 9, 3, 12, 0, 0, g, 40);
    CHECK(load_bytes(good, gsz) == 1, "合法的合成字体能加载");

#define MUTATE(desc, stmt)                                  \
    do {                                                    \
        memcpy(bad, good, gsz);                             \
        size_t bsz = gsz;                                   \
        stmt;                                               \
        CHECK(load_bytes(bad, bsz) == 0, "%s 应该被拒绝", desc); \
    } while (0)

    MUTATE("31 字节", bsz = 31);
    MUTATE("只有文件头", bsz = 32);
    MUTATE("魔数错", bad[0] = 'X');
    MUTATE("字号 0", bad[4] = 0);
    MUTATE("ascent 0", bad[6] = 0);
    MUTATE("descent 负数", (bad[8] = 0xFF, bad[9] = 0xFF));
    MUTATE("行高 0", bad[10] = 0);
    MUTATE("字形数 0", put32(bad + 12, 0));
    MUTATE("字形数巨大（乘法溢出）", put32(bad + 12, 0x10000001u));
    MUTATE("字形数多一个", put32(bad + 12, 41));
    MUTATE("字形数超出文件", put32(bad + 12, 41 + 400));
    MUTATE("索引偏移越界", put32(bad + 16, 0xFFFFFFF0u));
    MUTATE("索引偏移在文件头里", put32(bad + 16, 8));
    MUTATE("位图区偏移越界", put32(bad + 20, (uint32_t)gsz + 1));
    MUTATE("等宽但格宽 0", bad[24] = 1);
    MUTATE("码点乱序", put32(bad + 32 + 16 * 5, 0x20));
    MUTATE("码点重复", put32(bad + 32 + 16 * 6, le32(bad + 32 + 16 * 5)));
    MUTATE("码点超出 Unicode", put32(bad + 32 + 16 * 39, 0x110000));
    MUTATE("位图偏移越界", put32(bad + 32 + 16 * 39 + 4, 0x7FFFFFFFu));
    MUTATE("位图尺寸越界", (bad[32 + 16 * 39 + 8] = 255, bad[32 + 16 * 39 + 9] = 255));
    MUTATE("截掉最后一个字节", bsz = gsz - 1);

    /* 真实字体文件截断在各种位置 */
    tskf t;
    if (tskf_open("sans-32.skf", &t) == 0) {
        size_t cuts[] = { 1, 16, 31, 32, 33, 32 + 16 * 10, t.bitmap_off - 1, t.bitmap_off,
                          t.bitmap_off + 1000, t.size - 1 };
        int rejected = 0;
        for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++)
            rejected += load_bytes(t.data, cuts[i]) == 0;
        CHECK(rejected == (int)(sizeof(cuts) / sizeof(cuts[0])), "截断的 sans-32 全部被拒绝（%d 个）", rejected);
        CHECK(load_bytes(t.data, t.size) == 1, "完整的 sans-32 能加载");
        free(t.data);
    }

    /* 随机垃圾文件 */
    quiet_stderr(1);
    int loaded = 0;
    for (int i = 0; i < 300; i++) {
        size_t sz = rnd() % 4096;
        for (size_t k = 0; k < sz; k++)
            bad[k] = (uint8_t)rnd();
        if (i % 2 && sz >= 32)
            memcpy(bad, "SKF1", 4);                 /* 一半带正确的魔数，走得更深 */
        loaded += load_bytes(bad, sz) > 0;
    }
    CHECK(1, "300 个随机文件不崩溃（意外加载成功 %d 个）", loaded);

    /* 变异测试：在合法文件上随机改几个字节。有的会被拒绝，有的仍然合法（比如只改了位图），
     * 无论哪种都不许崩溃或越界。 */
    int ok = 0, rej = 0;
    for (int i = 0; i < 3000; i++) {
        memcpy(bad, good, gsz);
        int nmut = 1 + (int)(rnd() % 6);
        for (int k = 0; k < nmut; k++) {
            size_t pos = (rnd() % 4 == 0) ? rnd() % 32 : rnd() % (32 + 16 * 40);
            bad[pos] = (uint8_t)rnd();
        }
        int r = load_bytes(bad, gsz);
        if (r > 0)
            ok++;
        else
            rej++;
    }
    quiet_stderr(0);
    CHECK(rej > 0 && ok > 0, "3000 次变异：%d 个被拒绝，%d 个仍可用，都没有崩溃", rej, ok);
#undef MUTATE
}

/* =====================================================================
 * 9. mmap 失败时退回 read()
 *
 * 用链接器的 --wrap=mmap 把 font.c 里对 mmap 的调用换成下面的 __wrap_mmap，
 * 不改产品代码就能模拟“这个文件系统不支持 mmap”。
 * 没有用 -Wl,--wrap=mmap 链接时（例如 make host-tests），__wrap_mmap 不会被调用，
 * 这项测试自动跳过。__real_mmap 声明成弱符号，没有 --wrap 时它是 NULL，链接也不会失败。
 * ===================================================================== */
extern void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
    __attribute__((weak));
void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
static int g_mmap_fail, g_mmap_calls;

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    g_mmap_calls++;
    if (g_mmap_fail || !__real_mmap) {
        errno = ENODEV;
        return MAP_FAILED;
    }
    return __real_mmap(addr, len, prot, flags, fd, off);
}

static void test_read_fallback(void)
{
    printf("[mmap 失败时退回 read()]\n");
    char path[512];
    snprintf(path, sizeof(path), "%s/sans-32.skf", g_fontdir);
    g_mmap_calls = 0;
    sk_font *a = sk_font_load(path);
    if (g_mmap_calls == 0) {
        printf("  （没有用 -Wl,--wrap=mmap 链接，跳过）\n");
        sk_font_free(a);
        return;
    }
    g_mmap_fail = 1;
    sk_font *b = sk_font_load(path);
    g_mmap_fail = 0;
    CHECK(a && b, "mmap 和 read() 两种方式都能加载");
    if (a && b) {
        sk_canvas *c1 = tc_new(400, 50, 400, BG), *c2 = tc_new(400, 50, 400, BG);
        sk_draw_text(c1, a, 5, 40, "大字 12:34 星期五 Ok", INK);
        sk_draw_text(c2, b, 5, 40, "大字 12:34 星期五 Ok", INK);
        CHECK(ink_bbox(c1, (sk_rect){ 0, 0, 400, 50 }, BG, NULL) > 100 &&
              memcmp(c1->pixels, c2->pixels, 400 * 50 * 4) == 0, "两种方式画出来逐像素相同");
        tc_free(c1);
        tc_free(c2);
    }
    sk_font_free(a);            /* munmap */
    sk_font_free(b);            /* free（AddressSanitizer 会检查释放方式是否正确） */
}

/* =====================================================================
 * 10. 性能（只打印，不判定；AddressSanitizer 下数字会偏大）
 * ===================================================================== */
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void bench(void)
{
    printf("[性能参考]\n");
    sk_font *mono = sk_font_get(SK_FONT_MONO), *ui = sk_font_get(SK_FONT_UI);
    sk_canvas *c = tc_new(1024, 768, 1024, BG);
    const char *line = "drwxr-xr-x  2 root root 4096 10月 23 14:22 文档 sketchd 显示服务器 ┃█▌ 0123456789";
    int cw = sk_font_cell_width(mono), lh = sk_font_line_height(mono);
    int glyphs = 0;
    double t0 = now_sec();
    for (int frame = 0; frame < 20; frame++) {
        for (int row = 0; row < 768 / lh; row++) {
            const char *p = line;
            int col = 0;
            uint32_t cp;
            while ((cp = sk_utf8_next(&p)) != 0 && col < 1024 / cw) {
                sk_draw_glyph(c, mono, col * cw, row * lh + sk_font_ascent(mono), cp, INK);
                col += sk_wcwidth(cp) ? sk_wcwidth(cp) : 0;
                glyphs++;
            }
        }
    }
    double t1 = now_sec();
    printf("  终端整屏（等宽，按格子画）：%.0f ns/字，一屏约 %.2f ms\n",
           (t1 - t0) * 1e9 / glyphs, (t1 - t0) * 1e3 / 20);
    const char *para = "操作系统是管理计算机硬件与软件资源的程序，它负责进程调度、内存管理、文件系统和设备驱动。"
                       "The kernel schedules processes and manages memory. ";
    t0 = now_sec();
    int h = 0;
    for (int i = 0; i < 200; i++)
        h += sk_draw_text_wrapped(c, ui, (sk_rect){ 10, 10, 400, 0 }, para, INK, 2);
    t1 = now_sec();
    printf("  自动换行一段 %d 字：%.1f µs/次（含绘制）\n", sk_utf8_len(para), (t1 - t0) * 1e6 / 200);
    CHECK(h > 0, "性能测试正常运行");
    tc_free(c);
}

/* =====================================================================
 * 11. 字体文件全部缺失：内置后备字体（在子进程里测，因为 sk_font_get 有全局缓存）
 * ===================================================================== */
static int builtin_child(void)
{
    int fail0 = g_fail;
    setenv("SKETCH_FONT_DIR", "/nonexistent-sketchos-fonts", 1);
    sk_font *ui = sk_font_get(SK_FONT_UI);
    sk_font *title = sk_font_get(SK_FONT_TITLE), *large = sk_font_get(SK_FONT_LARGE);
    sk_font *mono = sk_font_get(SK_FONT_MONO);
    CHECK(ui && title && large && mono, "后备：sk_font_get 永远不返回 NULL");
    CHECK(title == ui && large == ui, "后备：TITLE/LARGE 退回 UI");
    CHECK(sk_font_get(99) == ui && sk_font_get(-1) == ui, "非法 id 当作 UI");
    CHECK(sk_font_is_mono(ui) && sk_font_cell_width(ui) == 8, "内置字体是 8 像素宽的等宽字体");
    CHECK(sk_font_is_mono(mono) && sk_font_cell_width(mono) == 8, "后备 MONO 等宽");
    CHECK(sk_font_line_height(ui) == sk_font_ascent(ui) + sk_font_descent(ui), "内置字体行高");
    CHECK(sk_glyph_advance(ui, 'A') == 8 && sk_glyph_advance(ui, 0x4E2D) == 16, "内置字体宽度");
    sk_canvas *c = tc_new(200, 40, 200, BG);
    sk_draw_text(c, ui, 2, 20, "Hello", INK);
    CHECK(ink_bbox(c, (sk_rect){ 0, 0, 200, 40 }, BG, NULL) > 20, "内置字体能画 ASCII");
    tc_fill(c, BG);
    sk_draw_text(c, ui, 2, 20, "中", INK);
    CHECK(ink_bbox(c, (sk_rect){ 0, 0, 200, 40 }, BG, NULL) > 8, "内置字体缺汉字时画方框");
    tc_fill(c, BG);
    sk_draw_text_in_rect(c, ui, (sk_rect){ 10, 0, 60, 40 }, "A very long title", INK, SK_ALIGN_LEFT);
    sk_rect bb;
    ink_bbox(c, (sk_rect){ 0, 0, 200, 40 }, BG, &bb);
    CHECK(bb.x >= 10 && bb.x + bb.w <= 70 && bb.x + bb.w > 60, "内置字体截断用 \"...\"");
    tc_free(c);
    sk_font_free(ui);
    CHECK(sk_text_width(ui, "ok") == 16, "内置字体 free 后仍可用");
    return g_fail - fail0;
}

static void test_builtin_fallback(void)
{
    printf("[字体文件缺失 → 内置后备字体]（子进程）\n");
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        CHECK(0, "fork 失败: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        int f = builtin_child();
        fflush(stdout);
        _exit(f > 100 ? 100 : f);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "子进程：%s，失败 %d 项",
          WIFEXITED(st) ? "正常退出" : "异常终止（崩溃？）", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

int main(void)
{
    g_fontdir = getenv("SKETCH_FONT_DIR");
    if (!g_fontdir || !*g_fontdir) {
        g_fontdir = "/usr/share/fonts";
        printf("提示：没有设置 SKETCH_FONT_DIR，使用 %s\n", g_fontdir);
    }
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/test_font.XXXXXX");
    if (!mkdtemp(g_tmpdir)) {
        perror("mkdtemp");
        return 1;
    }

    test_builtin_fallback();        /* 必须最先：子进程要从“还没加载任何字体”的状态开始 */
    test_system_fonts();
    test_utf8();
    test_wcwidth();
    test_measure();
    test_blend_clip_fallback();
    test_in_rect();
    test_wrap();
    test_corrupt();
    test_read_fallback();
    bench();

    /* 删除临时目录（不用 system("rm -rf")：测试也要能在没有 shell 的最小系统里跑） */
    DIR *d = opendir(g_tmpdir);
    if (d) {
        struct dirent *de;
        char path[512];
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "%s/%s", g_tmpdir, de->d_name);
            if (unlink(path) < 0)
                perror(path);
        }
        closedir(d);
    }
    if (rmdir(g_tmpdir) < 0)
        perror(g_tmpdir);
    printf("\n结果：%d 项通过，%d 项失败\n", g_pass, g_fail);
    return g_fail > 100 ? 100 : g_fail;
}
