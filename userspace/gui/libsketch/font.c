/*
 * font.c — 位图字体：加载 .skf、查字形、画文字、UTF-8、自动换行 (libsketch)
 *
 * 一段文字是怎么变成屏幕上的像素的？
 *   1. 文字在内存里是 UTF-8 字节串，先“解码”成一个个 Unicode 码点（sk_utf8_next）；
 *   2. 用码点在字体的索引表里二分查找，得到这个字的灰度位图和排版参数（font_find）；
 *   3. 按“覆盖率 × 颜色透明度”把位图混合到画布上（blit_mask），然后笔位右移 advance。
 * 字体文件格式见 sketch/font.h；生成它的脚本是 tools/mkfont.py。
 *
 * 排版术语（纵向）：
 *
 *      ┌──────────────┐ ← 行顶 = 基线 - ascent
 *      │   ██    ██   │
 *      │   ██ ▄▄ ██   │  top：基线到位图顶边的距离（向上为正）
 *  ────┼───██─────██──┼── 基线（baseline）：字母“站”在这条线上，所有字共用
 *      │          ▀▀  │  descent：基线到最低点（g、p、y 的尾巴）
 *      └──────────────┘ ← 行底 = 基线 + descent
 *   笔位(x) ─┘ left：笔位到位图左边的距离；画完笔位前进 advance
 *
 * 本文件只用 gfx.h 里的类型和宏（sk_canvas、sk_color……），不调用 gfx.c 的函数：
 * 字形混合有自己的裁剪循环，这样字体模块可以单独测试。
 */
#include "sketch/font.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* font_builtin.c（由 tools/mkfont.py 生成）：一个完整的 .skf 文件，只含 ASCII */
extern const uint8_t sk_font_builtin_data[];
extern const size_t sk_font_builtin_size;

#define SKF_HEADER_SIZE   32
#define SKF_GLYPH_SIZE    16
#define SKF_FLAG_MONO     1u
#define SKF_MAX_FILE      (64u * 1024u * 1024u)   /* 64 MB：再大一定是坏文件 */
#define FALLBACK_DEPTH    8                        /* 回退链最多走几层（防止有人把链接成环） */
#define TAB_SPACES        4                        /* 一个制表符 = 几个空格宽 */

/* 字体数据放在哪里，决定 sk_font_free 怎么释放 */
enum { STORE_STATIC = 0, STORE_MMAP = 1, STORE_HEAP = 2 };

struct sk_font {
    const uint8_t *data;        /* 整个 .skf 文件 */
    size_t         size;
    int            store;       /* STORE_* */
    int            shared;      /* sk_font_get 返回的系统字体：sk_font_free 不释放 */
    int            pixel_size, ascent, descent, line_height;
    int            mono, cell_width, avg_width;
    uint32_t       count;       /* 字形个数 */
    const uint8_t *index;       /* count 个 16 字节索引项 */
    const uint8_t *bitmaps;     /* 位图区起点 */
    int32_t        latin[256];  /* 码点 < 256 的字形直接查表（-1 表示没有）：ASCII 最常用，省掉二分查找 */
    sk_font       *fallback;    /* 缺字时去这里找 */
};

/* 一个字形解码后的样子 */
typedef struct {
    const uint8_t *bits;        /* w*h 字节覆盖率 */
    int w, h, left, top, advance;
} glyph_t;

/* 查字形的结果：画什么、占多宽 */
enum { DRAW_NOTHING = 0, DRAW_GLYPH = 1, DRAW_BOX = 2 };
typedef struct {
    int     kind;               /* DRAW_* */
    int     advance;            /* 笔位前进多少 */
    int     dx;                 /* 字形额外右移（等宽字体里借用回退字体的字形时居中用） */
    glyph_t g;
} shaped_t;

/* ======================================================================
 * 小工具
 * ====================================================================== */

/* 文件格式规定小端。逐字节拼起来，既不怕未对齐访问，也不依赖本机字节序；
 * 在 x86 上编译器会把它优化成一条普通的 32 位读指令。 */
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static inline int rd16s(const uint8_t *p) { return (int16_t)rd16(p); }

/* v / 255 四舍五入（v <= 255*255）。像素混合每个通道都要除以 255，用移位代替除法。 */
static inline unsigned div255(unsigned v)
{
    v += 128;
    return (v + (v >> 8)) >> 8;
}

static sk_rect rect_isect(sk_rect a, sk_rect b)
{
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w < b.x + b.w) ? a.x + a.w : b.x + b.w;
    int y1 = (a.y + a.h < b.y + b.h) ? a.y + a.h : b.y + b.h;
    sk_rect r = { x0, y0, x1 - x0, y1 - y0 };
    if (r.w <= 0 || r.h <= 0)
        r.w = r.h = 0;
    return r;
}

static void font_warn(const char *path, const char *why)
{
    fprintf(stderr, "[font] %s: %s\n", path ? path : "(内存)", why);
}

/* ======================================================================
 * 加载与校验
 * ====================================================================== */

/* 一次性检查整个文件头和索引表。字体文件来自磁盘，可能被截断、写坏，甚至是别的文件：
 * 这里把所有“下标可能越界”的地方都检查一遍，之后画字时就可以放心地直接访问内存，
 * 不必每个像素都做边界判断。检查是 O(字形数) 的，几千个字形只要几十微秒。 */
static const char *skf_validate(const uint8_t *d, size_t size)
{
    if (size < SKF_HEADER_SIZE)
        return "文件太小，连文件头都不完整";
    if (memcmp(d, "SKF1", 4) != 0)
        return "魔数不是 SKF1（不是 SketchOS 字体文件）";
    unsigned pixel_size  = rd16(d + 4);
    int      ascent      = rd16s(d + 6);
    int      descent     = rd16s(d + 8);
    unsigned line_height = rd16(d + 10);
    uint32_t count       = rd32(d + 12);
    uint32_t index_off   = rd32(d + 16);
    uint32_t bitmap_off  = rd32(d + 20);
    unsigned flags       = rd16(d + 24);
    unsigned cell        = rd16(d + 26);

    if (pixel_size == 0 || pixel_size > 255)
        return "字号不合理";
    if (ascent <= 0 || ascent > 255 || descent < 0 || descent > 255)
        return "ascent/descent 不合理";
    if (line_height == 0 || line_height > 512)
        return "行高不合理";
    if ((flags & SKF_FLAG_MONO) && (cell == 0 || cell > 255))
        return "等宽字体的格宽不合理";
    if (count == 0 || count > 0x110000)
        return "字形个数不合理";
    /* 用 64 位算术：count*16 在 32 位下可能溢出，溢出后的小数字会骗过检查 */
    if (index_off < SKF_HEADER_SIZE || (uint64_t)index_off + (uint64_t)count * SKF_GLYPH_SIZE > size)
        return "索引表超出文件末尾（文件被截断？）";
    if (bitmap_off > size)
        return "位图区偏移超出文件";

    uint64_t bitmap_size = size - bitmap_off;
    const uint8_t *e = d + index_off;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < count; i++, e += SKF_GLYPH_SIZE) {
        uint32_t cp = rd32(e);
        if (cp > 0x10FFFF)
            return "码点超出 Unicode 范围";
        if (i > 0 && cp <= prev)
            return "索引没有按码点严格升序排列（无法二分查找）";
        uint64_t need = (uint64_t)rd32(e + 4) + (uint64_t)e[8] * e[9];
        if (need > bitmap_size)
            return "字形位图超出文件末尾";
        prev = cp;
    }
    return NULL;
}

/* 校验通过后，把文件头解析进 sk_font 结构 */
static sk_font *font_from_memory(const uint8_t *d, size_t size, int store, const char *name)
{
    const char *err = skf_validate(d, size);
    if (err) {
        font_warn(name, err);
        return NULL;
    }
    sk_font *f = calloc(1, sizeof(*f));
    if (!f) {
        font_warn(name, "内存不足");
        return NULL;
    }
    f->data        = d;
    f->size        = size;
    f->store       = store;
    f->pixel_size  = rd16(d + 4);
    f->ascent      = rd16s(d + 6);
    f->descent     = rd16s(d + 8);
    f->line_height = rd16(d + 10);
    f->count       = rd32(d + 12);
    f->index       = d + rd32(d + 16);
    f->bitmaps     = d + rd32(d + 20);
    f->mono        = (rd16(d + 24) & SKF_FLAG_MONO) != 0;
    f->cell_width  = f->mono ? (int)rd16(d + 26) : 0;

    for (int i = 0; i < 256; i++)
        f->latin[i] = -1;
    /* 索引按码点升序，所以码点 < 256 的都在最前面 */
    for (uint32_t i = 0; i < f->count; i++) {
        uint32_t cp = rd32(f->index + (size_t)i * SKF_GLYPH_SIZE);
        if (cp >= 256)
            break;
        f->latin[cp] = (int32_t)i;
    }
    /* 比例字体的“平均宽度”：可打印 ASCII 的平均前进宽度（输入框估算列数等用） */
    int sum = 0, n = 0;
    for (int c = 0x20; c < 0x7F; c++) {
        if (f->latin[c] >= 0) {
            sum += f->index[(size_t)f->latin[c] * SKF_GLYPH_SIZE + 12];
            n++;
        }
    }
    f->avg_width = n ? (sum + n / 2) / n : (f->pixel_size + 1) / 2;
    if (f->avg_width <= 0)
        f->avg_width = 1;
    return f;
}

sk_font *sk_font_load(const char *path)
{
    if (!path)
        return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        font_warn(path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        font_warn(path, strerror(errno));
        close(fd);
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        font_warn(path, "不是普通文件");
        close(fd);
        return NULL;
    }
    if (st.st_size < SKF_HEADER_SIZE || (uint64_t)st.st_size > SKF_MAX_FILE) {
        font_warn(path, "文件大小不合理");
        close(fd);
        return NULL;
    }
    size_t size = (size_t)st.st_size;

    /* mmap：把文件“映射”进进程的地址空间，而不是 read() 拷贝一份。
     *   - 按需加载：内核只在第一次访问某一页（4KB）时才把它读进内存（缺页异常），
     *     一个 2MB 的中文字体，界面实际只用到几百个字，大部分页根本不会被读入；
     *   - 进程间共享：每个图形程序都映射同一个字体文件，它们看到的是页缓存里的同一份
     *     物理内存——10 个应用打开字体，也只占一份内存；
     *   - SketchOS 的根文件系统在内存里（initramfs），文件内容本来就在页缓存中，
     *     mmap 等于直接把那几页物理内存挂到我们的地址空间，连一次拷贝都没有。
     * MAP_PRIVATE + PROT_READ：只读映射，我们不会（也不能）改动文件。 */
    void *mem = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    int store = STORE_MMAP;
    if (mem == MAP_FAILED) {
        /* 有些文件系统不支持 mmap：退回到老办法，read() 进堆内存 */
        store = STORE_HEAP;
        mem = malloc(size);
        if (!mem) {
            font_warn(path, "内存不足");
            close(fd);
            return NULL;
        }
        size_t got = 0;
        while (got < size) {
            ssize_t n = read(fd, (char *)mem + got, size - got);
            if (n < 0 && errno == EINTR)
                continue;           /* 被信号打断，重新读 */
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        if (got != size) {
            font_warn(path, "读取不完整");
            free(mem);
            close(fd);
            return NULL;
        }
    }
    /* 映射建立后就可以关闭文件描述符：映射本身持有对文件的引用 */
    close(fd);

    sk_font *f = font_from_memory(mem, size, store, path);
    if (!f) {
        if (store == STORE_MMAP)
            munmap(mem, size);
        else
            free(mem);
    }
    return f;
}

void sk_font_free(sk_font *f)
{
    if (!f || f->shared)
        return;                 /* 系统字体被很多地方共用，不能释放 */
    if (f->store == STORE_MMAP)
        munmap((void *)f->data, f->size);
    else if (f->store == STORE_HEAP)
        free((void *)f->data);
    free(f);
}

void sk_font_set_fallback(sk_font *f, sk_font *fallback)
{
    if (f && f != fallback)
        f->fallback = fallback;
}

/* ======================================================================
 * 系统字体与内置后备字体
 * ====================================================================== */

static sk_font *g_builtin;              /* 内置 ASCII 字体（只解析一次） */
static sk_font *g_sys[SK_FONT_COUNT];   /* sk_font_get 的缓存 */

/* 万一内置字体也解析失败（只可能是构建出了问题），还有这个“空字体”兜底：
 * 一个字形都没有，所有字都画成方框——但至少不会返回 NULL 让调用者崩溃。 */
static sk_font g_empty;

static sk_font *builtin_font(void)
{
    if (g_builtin)
        return g_builtin;
    g_builtin = font_from_memory(sk_font_builtin_data, sk_font_builtin_size, STORE_STATIC,
                                 "内置字体");
    if (!g_builtin) {
        g_empty.pixel_size = 16;
        g_empty.ascent = 13;
        g_empty.descent = 3;
        g_empty.line_height = 16;
        g_empty.mono = 1;
        g_empty.cell_width = 8;
        g_empty.avg_width = 8;
        for (int i = 0; i < 256; i++)
            g_empty.latin[i] = -1;
        g_builtin = &g_empty;
    }
    g_builtin->shared = 1;
    return g_builtin;
}

/* 所有公开函数都允许传 NULL 字体：当作内置字体，保证不崩溃 */
#define FONT(f) ((f) ? (f) : builtin_font())

static sk_font *load_system_file(int id)
{
    static const char *const files[SK_FONT_COUNT] = {
        [SK_FONT_UI] = "sans-16.skf", [SK_FONT_TITLE] = "sans-20.skf",
        [SK_FONT_LARGE] = "sans-32.skf", [SK_FONT_MONO] = "mono-16.skf",
    };
    /* 环境变量：离线测试时让程序去仓库的 assets/fonts 找字体 */
    const char *dir = getenv("SKETCH_FONT_DIR");
    if (!dir || !*dir)
        dir = "/usr/share/fonts";
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/%s", dir, files[id]) >= (int)sizeof(path))
        return NULL;
    sk_font *f = sk_font_load(path);
    if (f)
        f->shared = 1;
    else
        fprintf(stderr, "[font] %s 不可用，改用后备字体\n", path);
    return f;
}

/* 第一次调用时加载，之后一直缓存到进程结束（字体是只读映射，进程退出时内核自动回收）。
 * 不是线程安全的：SketchOS 的图形程序都是单线程的事件循环。 */
sk_font *sk_font_get(int id)
{
    if (id < 0 || id >= SK_FONT_COUNT)
        id = SK_FONT_UI;
    if (g_sys[id])
        return g_sys[id];

    /* 回退链：大字缺字找标题字体，标题缺字找正文字体，正文缺字找内置字体。
     * 文件不存在时，直接用链上的下一个字体顶替（而不是返回 NULL）。 */
    sk_font *f = load_system_file(id);
    switch (id) {
    case SK_FONT_UI:
        if (f)
            f->fallback = builtin_font();
        else
            f = builtin_font();
        break;
    case SK_FONT_TITLE:
        if (f)
            f->fallback = sk_font_get(SK_FONT_UI);
        else
            f = sk_font_get(SK_FONT_UI);
        break;
    case SK_FONT_LARGE:
        if (f)
            f->fallback = sk_font_get(SK_FONT_TITLE);
        else
            f = sk_font_get(SK_FONT_TITLE);
        break;
    case SK_FONT_MONO:
        if (f) {
            f->fallback = sk_font_get(SK_FONT_UI);
        } else {
            /* 终端需要等宽字体，不能拿比例字体顶替：用内置字体（它本身就是等宽的）。
             * 复制一份结构体（共享同一块数据），让它缺字时去正文字体里找汉字。 */
            sk_font *ui = sk_font_get(SK_FONT_UI);
            sk_font *b = builtin_font();
            f = b;
            if (ui != b) {
                sk_font *copy = malloc(sizeof(*copy));
                if (copy) {
                    *copy = *b;
                    copy->fallback = ui;
                    f = copy;
                }
            }
        }
        break;
    }
    f->shared = 1;
    g_sys[id] = f;
    return f;
}

/* ======================================================================
 * 度量
 * ====================================================================== */
int sk_font_ascent(const sk_font *f)      { return FONT(f)->ascent; }
int sk_font_descent(const sk_font *f)     { return FONT(f)->descent; }
int sk_font_line_height(const sk_font *f) { return FONT(f)->line_height; }
int sk_font_is_mono(const sk_font *f)     { return FONT(f)->mono; }

int sk_font_cell_width(const sk_font *f)
{
    f = FONT(f);
    return f->mono ? f->cell_width : f->avg_width;
}

/* ======================================================================
 * UTF-8
 *
 * UTF-8 用 1~4 个字节表示一个码点：
 *   0xxxxxxx                              U+0000..U+007F（就是 ASCII）
 *   110xxxxx 10xxxxxx                     U+0080..U+07FF
 *   1110xxxx 10xxxxxx 10xxxxxx            U+0800..U+FFFF（汉字都在这里，3 字节）
 *   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx   U+10000..U+10FFFF
 * 文本可能来自任何地方（文件、网络、用户输入），所以解码器必须能扛住非法字节：
 * “超长编码”(C0 80 冒充 NUL)、代理区码点、超过 U+10FFFF、序列中途断掉……
 * 遇到这些一律输出替换字符 U+FFFD 并只前进 1 字节，下一个字节重新开始同步。
 * ====================================================================== */

/* avail：最多还能读几个字节（NUL 结尾的字符串传 SIZE_MAX） */
static uint32_t utf8_decode(const char **ps, size_t avail)
{
    const unsigned char *s = (const unsigned char *)*ps;
    if (avail == 0 || s[0] == 0)
        return 0;
    unsigned c = s[0];
    if (c < 0x80) {
        *ps += 1;
        return c;
    }
    int n;
    uint32_t cp, min;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 1; cp = c & 0x1F; min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 2; cp = c & 0x0F; min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 3; cp = c & 0x07; min = 0x10000;
    } else {
        goto bad;               /* 孤立的后续字节 80..BF、超长前导 C0/C1、F5..FF */
    }
    for (int i = 1; i <= n; i++) {
        if ((size_t)i >= avail)
            goto bad;           /* 序列被长度限制截断 */
        unsigned cc = s[i];
        if ((cc & 0xC0) != 0x80)
            goto bad;           /* 不是 10xxxxxx（包括遇到 NUL），序列断了 */
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        goto bad;
    *ps += n + 1;
    return cp;
bad:
    *ps += 1;
    return 0xFFFD;
}

uint32_t sk_utf8_next(const char **s)
{
    if (!s || !*s)
        return 0;
    return utf8_decode(s, SIZE_MAX);
}

int sk_utf8_encode(uint32_t cp, char *out)
{
    unsigned char b[4];
    int n;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;            /* 无法编码的值：编码成替换字符 */
    if (cp < 0x80) {
        b[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (out)
        memcpy(out, b, (size_t)n);
    return n;
}

int sk_utf8_len(const char *s)
{
    int n = 0;
    if (!s)
        return 0;
    while (utf8_decode(&s, SIZE_MAX))
        n++;
    return n;
}

/* ======================================================================
 * 字符宽度 sk_wcwidth 在 common/skutil.c 里实现（宽度表也在那里）。
 * skbox、sksh 不链接图形库，但同样要按显示宽度对齐（ls 分栏、cal、行编辑），
 * 所以这张表放在所有程序都链接的 libcommon 里，终端程序和图形界面用同一份。
 * ====================================================================== */

/* ======================================================================
 * 查字形
 * ====================================================================== */

/* 在一个字体里找码点，返回索引项下标，没有返回 -1 */
static int font_find(const sk_font *f, uint32_t cp)
{
    if (cp < 256)
        return f->latin[cp];
    /* 二分查找：几千个字形最多比较 13 次 */
    uint32_t lo = 0, hi = f->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t c = rd32(f->index + (size_t)mid * SKF_GLYPH_SIZE);
        if (c < cp)
            lo = mid + 1;
        else if (c > cp)
            hi = mid;
        else
            return (int)mid;
    }
    return -1;
}

static void glyph_at(const sk_font *f, int i, glyph_t *g)
{
    const uint8_t *e = f->index + (size_t)i * SKF_GLYPH_SIZE;
    g->bits    = f->bitmaps + rd32(e + 4);
    g->w       = e[8];
    g->h       = e[9];
    g->left    = (int8_t)e[10];
    g->top     = (int8_t)e[11];
    g->advance = e[12];
}

/* 缺字方框占多宽：等宽字体按格子算，比例字体按字号估算 */
static int box_advance(const sk_font *f, int wc)
{
    if (f->mono)
        return wc * f->cell_width;
    return wc == 2 ? f->pixel_size : (f->pixel_size + 1) / 2;
}

static int space_advance(const sk_font *f)
{
    if (f->mono)
        return f->cell_width;
    int i = font_find(f, ' ');
    return i >= 0 ? f->index[(size_t)i * SKF_GLYPH_SIZE + 12] : (f->pixel_size + 2) / 4;
}

/* 决定码点 cp 用主字体 f 怎么画：先查自己，再沿回退链查，最后画方框 */
static void shape(const sk_font *f, uint32_t cp, shaped_t *out)
{
    out->kind = DRAW_NOTHING;
    out->advance = 0;
    out->dx = 0;
    if (cp == '\t') {
        out->advance = TAB_SPACES * space_advance(f);
        return;
    }
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return;                 /* 换行、回车等控制字符：不画，不占宽度 */

    const sk_font *cur = f;
    for (int depth = 0; cur && depth < FALLBACK_DEPTH; depth++, cur = cur->fallback) {
        int i = font_find(cur, cp);
        if (i < 0)
            continue;
        glyph_at(cur, i, &out->g);
        out->kind = DRAW_GLYPH;
        out->advance = out->g.advance;
        if (cur != f && f->mono) {
            /* 等宽字体借用别的字体的字形：宽度必须仍然是整数个格子，字形在格子里居中 */
            int want = sk_wcwidth(cp) * f->cell_width;
            out->dx = (want - out->g.advance) / 2;
            out->advance = want;
        }
        return;
    }
    int wc = sk_wcwidth(cp);
    if (wc > 0) {               /* 组合字符缺字就算了，不画方框 */
        out->kind = DRAW_BOX;
        out->advance = box_advance(f, wc);
    }
}

int sk_glyph_advance(const sk_font *f, uint32_t cp)
{
    shaped_t s;
    shape(FONT(f), cp, &s);
    return s.advance;
}

int sk_text_width_n(const sk_font *f, const char *utf8, int nbytes)
{
    if (!utf8)
        return 0;
    f = FONT(f);
    size_t avail = nbytes < 0 ? SIZE_MAX : (size_t)nbytes;
    const char *p = utf8, *end = nbytes < 0 ? NULL : utf8 + nbytes;
    int w = 0;
    for (;;) {
        uint32_t cp = utf8_decode(&p, end ? (size_t)(end - p) : avail);
        if (!cp)
            break;
        shaped_t s;
        shape(f, cp, &s);
        w += s.advance;
    }
    return w;
}

int sk_text_width(const sk_font *f, const char *utf8)
{
    return sk_text_width_n(f, utf8, -1);
}

/* ======================================================================
 * 画字
 * ====================================================================== */

/* 把一块 8 位覆盖率蒙版用颜色 color 混合到画布上，左上角在 (x, y)，遵守 c->clip。
 *
 * 抗锯齿的原理：位图里每个字节表示这个像素被笔画“盖住了多少”（0..255）。
 * 盖住一半，就把文字颜色和背景各取一半混合——边缘因此是平滑的灰色过渡，
 * 而不是锯齿状的黑白台阶。最终透明度 a = 覆盖率 × 颜色自身的 alpha。 */
static void blit_mask(sk_canvas *c, int x, int y, const uint8_t *mask, int w, int h, sk_color color)
{
    sk_rect r = rect_isect(c->clip, (sk_rect){ x, y, w, h });
    if (r.w <= 0)
        return;
    unsigned ca = SK_A(color);
    if (ca == 0)
        return;
    unsigned cr = SK_R(color), cg = SK_G(color), cb = SK_B(color);
    uint32_t solid = 0xFF000000u | (color & 0x00FFFFFFu);

    for (int yy = r.y; yy < r.y + r.h; yy++) {
        const uint8_t *m = mask + (size_t)(yy - y) * w + (r.x - x);
        uint32_t *px = c->pixels + (size_t)yy * c->stride + r.x;
        for (int i = 0; i < r.w; i++) {
            unsigned a = m[i];
            if (a == 0)
                continue;       /* 字形的大部分是空白，直接跳过 */
            if (ca != 255)
                a = div255(a * ca);
            if (a >= 255) {
                px[i] = solid;
                continue;
            }
            uint32_t d = px[i];
            if ((d >> 24) == 0) {
                /* 完全透明的像素（sketchd 的边框层用 alpha==0 表示“透明”，只有透明/不透明两种）：
                 * 没有背景色可混合，覆盖过半就画成实色，否则保持透明，避免出现一圈黑边 */
                if (a >= 128)
                    px[i] = solid;
                continue;
            }
            unsigned ia = 255 - a;
            unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            unsigned rr = div255(cr * a + dr * ia);
            unsigned gg = div255(cg * a + dg * ia);
            unsigned bb = div255(cb * a + db * ia);
            px[i] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
        }
    }
}

/* 单个像素的混合（缺字方框用），自带裁剪 */
static void blend_px(sk_canvas *c, int x, int y, sk_color color, unsigned cov)
{
    if (x < c->clip.x || y < c->clip.y || x >= c->clip.x + c->clip.w || y >= c->clip.y + c->clip.h)
        return;
    uint8_t m = (uint8_t)cov;
    blit_mask(c, x, y, &m, 1, 1, color);
}

/* 缺字时画的“手绘小方框”：四条边各自稍稍伸出拐角，像铅笔随手画的框；
 * 伸出的方向由码点决定，同一个字每次都画得一样（手绘随机种子要稳定）。 */
static void draw_missing_box(sk_canvas *c, const sk_font *f, int x, int baseline, int adv,
                             uint32_t cp, sk_color color)
{
    if (adv < 4)
        return;
    uint32_t h = cp * 2654435761u;          /* 简单的乘法哈希 */
    int bw = adv - 2;                       /* 左右各留 1 像素，伸出的笔画不越过前进宽度 */
    int bh = f->ascent * 3 / 4;
    if (bh < 4)
        bh = 4;
    int x0 = x + 1, x1 = x0 + bw - 1;
    int y1 = baseline - 1, y0 = y1 - bh + 1;
    int o[4];
    for (int i = 0; i < 4; i++)
        o[i] = (h >> (i * 3 + 7)) & 1;      /* 每条边是否多伸出 1 像素 */
    for (int xx = x0 - o[0]; xx <= x1; xx++)            /* 上边，向左伸出 */
        blend_px(c, xx, y0, color, 200);
    for (int xx = x0; xx <= x1 + o[1]; xx++)            /* 下边，向右伸出 */
        blend_px(c, xx, y1, color, 220);
    for (int yy = y0 - o[2]; yy <= y1; yy++)            /* 左边，向上伸出 */
        blend_px(c, x0, yy, color, 210);
    for (int yy = y0 + 1; yy <= y1 + o[3]; yy++)        /* 右边，向下伸出 */
        blend_px(c, x1, yy, color, 190);
}

static void draw_shaped(sk_canvas *c, const sk_font *f, int x, int baseline, uint32_t cp,
                        const shaped_t *s, sk_color color)
{
    if (s->kind == DRAW_GLYPH) {
        const glyph_t *g = &s->g;
        if (g->w && g->h)
            blit_mask(c, x + s->dx + g->left, baseline - g->top, g->bits, g->w, g->h, color);
    } else if (s->kind == DRAW_BOX) {
        draw_missing_box(c, f, x, baseline, s->advance, cp, color);
    }
}

int sk_draw_glyph(sk_canvas *c, const sk_font *f, int x, int baseline, uint32_t cp, sk_color color)
{
    shaped_t s;
    f = FONT(f);
    shape(f, cp, &s);
    if (c)
        draw_shaped(c, f, x, baseline, cp, &s, color);
    return s.advance;
}

int sk_draw_text_n(sk_canvas *c, const sk_font *f, int x, int baseline, const char *utf8,
                   int nbytes, sk_color color)
{
    if (!utf8)
        return x;
    f = FONT(f);
    const char *p = utf8, *end = nbytes < 0 ? NULL : utf8 + nbytes;
    for (;;) {
        uint32_t cp = utf8_decode(&p, end ? (size_t)(end - p) : SIZE_MAX);
        if (!cp)
            break;
        shaped_t s;
        shape(f, cp, &s);
        if (c)
            draw_shaped(c, f, x, baseline, cp, &s, color);
        x += s.advance;
    }
    return x;
}

int sk_draw_text(sk_canvas *c, const sk_font *f, int x, int baseline, const char *utf8, sk_color color)
{
    return sk_draw_text_n(c, f, x, baseline, utf8, -1, color);
}

/* ======================================================================
 * 单行：在矩形里对齐、垂直居中、过长用 “…” 截断
 * ====================================================================== */
void sk_draw_text_in_rect(sk_canvas *c, const sk_font *f, sk_rect r, const char *utf8,
                          sk_color color, int align)
{
    if (!c || !utf8 || r.w <= 0)
        return;
    f = FONT(f);
    /* 垂直居中：让“ascent+descent”这一整条字带的中线对准矩形中线 */
    int baseline = r.y + (r.h - (f->ascent + f->descent)) / 2 + f->ascent;
    int len = (int)strlen(utf8);
    int tw = sk_text_width_n(f, utf8, len);
    int nbytes = len;
    const char *ell = NULL;

    if (tw > r.w) {
        /* 过长：找出最长的前缀，使“前缀 + …”放得下。逐个码点前进，
         * 所以截断点一定落在 UTF-8 字符的边界上，不会切出半个汉字。 */
        shaped_t es;
        shape(f, 0x2026, &es);
        ell = es.kind == DRAW_GLYPH ? "\xE2\x80\xA6" : "...";   /* 内置字体没有 “…” */
        int ew = sk_text_width(f, ell);
        int avail = r.w - ew;
        const char *p = utf8, *cut = utf8;
        int w = 0, wcut = 0;
        for (;;) {
            const char *q = p;
            uint32_t cp = utf8_decode(&q, SIZE_MAX);
            if (!cp)
                break;
            shaped_t s;
            shape(f, cp, &s);
            if (w + s.advance > avail)
                break;
            w += s.advance;
            p = q;
            if (cp != ' ')      /* 省略号前不留空格：“Hello …” → “Hello…” */
                cut = p, wcut = w;
        }
        nbytes = (int)(cut - utf8);
        tw = wcut + ew;
    }

    int x = r.x;
    if (align == SK_ALIGN_CENTER)
        x = r.x + (r.w - tw) / 2;
    else if (align == SK_ALIGN_RIGHT)
        x = r.x + r.w - tw;
    if (x < r.x)
        x = r.x;                /* 连省略号都放不下时从左边开始画，多出的部分被裁掉 */

    /* 只在水平方向裁剪到矩形：文字绝不会画出矩形左右边界；
     * 垂直方向不裁，矩形比字矮时宁可让笔画稍稍出界，也不要切掉 g、y 的尾巴。 */
    sk_rect old = c->clip;
    c->clip = rect_isect(old, (sk_rect){ r.x, old.y, r.w, old.h });
    if (c->clip.w > 0) {
        x = sk_draw_text_n(c, f, x, baseline, utf8, nbytes, color);
        if (ell)
            sk_draw_text(c, f, x, baseline, ell, color);
    }
    c->clip = old;
}

/* ======================================================================
 * 自动换行
 *
 * 规则：
 *   - 汉字（以及所有宽字符）前后都可以断行，所以中文一个字一个字地折；
 *   - 西文按单词折：只在空格处断开；一个单词比整行还宽时，才在字母之间硬拆；
 *   - 避头尾（kinsoku，中日文排版规则）：“，。、；：？！）》」”等闭标点不能出现在行首，
 *     “（《「“”等开标点不能留在行尾——它们和相邻的字绑在一起，一起挪到下一行；
 *   - 遇到 \n 强制换行（\r 宽度为 0，所以 \r\n 也一样）；每个 \n 都开始新的一行，
 *     包括结尾的 \n——"abc\n" 是两行，第二行是空行（和文本编辑器里光标能停的位置一致）；
 *   - 软换行后的行首空格丢掉，段首（\n 之后）的空格保留（用来缩进）；
 *   - r.w <= 0 表示不限宽度，只按 \n 分行；返回值 = 行数×行高 + (行数-1)×line_gap。
 * ====================================================================== */
static const uint32_t k_no_line_start[] = {     /* 闭标点：不能出现在行首（已排序） */
    '!', '%', ')', ',', '.', ':', ';', '?', ']', '}', 0x00B0, 0x00BB,
    0x2014, 0x2019, 0x201D, 0x2025, 0x2026, 0x2030, 0x2032, 0x2033, 0x203A, 0x2103,
    0x3001, 0x3002, 0x3005, 0x3009, 0x300B, 0x300D, 0x300F, 0x3011, 0x3015, 0x3017,
    0x3019, 0x301B, 0x301E, 0x301F, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063,
    0x3083, 0x3085, 0x3087, 0x308E, 0x309D, 0x309E, 0x30A1, 0x30A3, 0x30A5, 0x30A7,
    0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE, 0x30F5, 0x30F6, 0x30FB, 0x30FC,
    0x30FD, 0x30FE, 0xFE50, 0xFE52, 0xFE54, 0xFE55, 0xFE56, 0xFE57, 0xFF01, 0xFF05,
    0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF3D, 0xFF5D, 0xFF5E, 0xFF60,
};
static const uint32_t k_no_line_end[] = {       /* 开标点：不能留在行尾（已排序） */
    '(', '[', '{', 0x00AB, 0x2018, 0x201C, 0x2039, 0x3008, 0x300A, 0x300C, 0x300E,
    0x3010, 0x3014, 0x3016, 0x3018, 0x301A, 0x301D, 0xFF08, 0xFF3B, 0xFF5B, 0xFF5F,
};

static int in_list(const uint32_t *t, int n, uint32_t cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < t[mid])
            hi = mid - 1;
        else if (cp > t[mid])
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}
#define IN_LIST(t, cp) in_list((t), (int)(sizeof(t) / sizeof((t)[0])), (cp))

/* a、b 两个相邻字符（都不是空白）之间能不能断行 */
static int can_break_between(uint32_t a, uint32_t b)
{
    if (IN_LIST(k_no_line_start, b))
        return 0;               /* 避头 */
    if (IN_LIST(k_no_line_end, a))
        return 0;               /* 避尾 */
    if (sk_wcwidth(b) == 0)
        return 0;               /* 组合字符和前一个字是一体的 */
    return sk_wcwidth(a) == 2 || sk_wcwidth(b) == 2;   /* 宽字符前后可断；西文字母之间不断 */
}

static int is_wrap_space(uint32_t cp) { return cp == ' ' || cp == '\t'; }

/* 读一个“不可拆分的单元”（一个西文单词、一个汉字连同粘着它的标点……），
 * 返回宽度，*pp 前移到单元末尾 */
static int read_unit(const sk_font *f, const char **pp)
{
    const char *p = *pp;
    uint32_t prev = 0;
    int w = 0;
    for (;;) {
        const char *q = p;
        uint32_t cp = utf8_decode(&q, SIZE_MAX);
        if (cp == 0 || cp == '\n' || is_wrap_space(cp))
            break;
        if (prev && can_break_between(prev, cp))
            break;
        w += sk_glyph_advance(f, cp);
        prev = cp;
        p = q;
    }
    *pp = p;
    return w;
}

typedef struct {
    const char *start;          /* 这一行要画的文字 */
    int         len;            /* 字节数 */
    int         hard;           /* 以 \n 结束（下一行是新段落） */
} wrap_line_t;

/* 排一行：从 s 开始尽量多放内容，宽度不超过 maxw。返回下一行的开头，文字结束返回 NULL。 */
static const char *layout_line(const sk_font *f, const char *s, int maxw, int para_start,
                               wrap_line_t *ln)
{
    if (!para_start)
        while (is_wrap_space((unsigned char)*s))
            s++;                /* 软换行后的行首空格丢掉 */
    ln->start = s;
    ln->hard = 0;
    const char *p = s;
    const char *commit = s;     /* 已经确定放进本行的内容的结尾 */
    int have_unit = 0;          /* 本行是否已经放进了至少一个单元 */
    int w = 0;                  /* 从行首到 p 的宽度（含段首缩进和还没确定的空格） */

    for (;;) {
        unsigned char ch = (unsigned char)*p;
        if (ch == 0 || ch == '\n') {
            ln->len = (int)(commit - s);    /* 行尾的空格不画 */
            if (ch == 0)
                return NULL;
            ln->hard = 1;
            return p + 1;
        }
        if (is_wrap_space(ch)) {
            /* 空格是断行机会；它的宽度先记着，后面的单元放得下才真正算进本行 */
            w += sk_glyph_advance(f, ch);
            p++;
            continue;
        }
        const char *unit = p;
        int uw = read_unit(f, &p);
        if (w + uw <= maxw) {
            w += uw;
            commit = p;
            have_unit = 1;
            continue;
        }
        if (have_unit) {
            /* 放不下：在上一个断行机会处断开，这个单元整个挪到下一行 */
            ln->len = (int)(commit - s);
            return unit;
        }
        /* 行首的单元本身就比一行还宽（很长的单词、路径……）：只好在字符之间硬拆。
         * 至少放一个字符，保证每一行都有进展，不会死循环；
         * 组合字符（宽度 0）跟着前一个字，不和它拆开。 */
        const char *q = unit, *cut = unit;
        while (q < p) {
            const char *nq = q;
            uint32_t cp = utf8_decode(&nq, SIZE_MAX);
            int a = sk_glyph_advance(f, cp);
            if (cut != unit && w + a > maxw && sk_wcwidth(cp) != 0)
                break;
            w += a;
            q = nq;
            cut = q;
        }
        if (cut == p) {
            /* 整个单元都放进来了（单元只有一个字、比一行还宽）：当作已放入，
             * 继续看后面——否则文字结尾会多出一个空行 */
            commit = p;
            have_unit = 1;
            continue;
        }
        ln->len = (int)(cut - s);
        return cut;
    }
}

int sk_draw_text_wrapped(sk_canvas *c, const sk_font *f, sk_rect r, const char *utf8,
                         sk_color color, int line_gap)
{
    if (!utf8 || !*utf8)
        return 0;
    f = FONT(f);
    int lh = f->line_height;
    int step = lh + line_gap;
    int maxw = r.w > 0 ? r.w : INT_MAX;        /* 宽度 <= 0：不自动换行，只按 \n 分行 */
    const char *p = utf8;
    int lines = 0, para_start = 1;

    while (p) {
        wrap_line_t ln;
        const char *next = layout_line(f, p, maxw, para_start, &ln);
        if (c && ln.len > 0) {
            int baseline = r.y + lines * step + f->ascent;
            /* 整行都在裁剪区上下之外就不画了（长文档滚动时省很多时间）；
             * 留出一个行高的余量，防止回退字体的字形比本字体高时被误判 */
            if (baseline + f->descent + lh > c->clip.y &&
                baseline - f->ascent - lh < c->clip.y + c->clip.h)
                sk_draw_text_n(c, f, r.x, baseline, ln.start, ln.len, color);
        }
        lines++;
        para_start = ln.hard;
        p = next;
    }
    int h = lines * lh + (lines - 1) * line_gap;
    return h > 0 ? h : 0;
}
