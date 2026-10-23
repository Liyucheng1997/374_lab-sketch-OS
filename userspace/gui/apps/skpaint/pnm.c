/*
 * pnm.c — 读写 Netpbm 图片：PPM（彩色）、PGM（灰度）、PBM（黑白）
 *
 * 为什么画板用 PPM 存图？因为它大概是世界上最简单的图片格式：
 *
 *     P6                ← “魔数”：P6 = 二进制彩色图
 *     # 注释            ← 以 # 开头到行尾，可以出现在头部任何空白处
 *     640 420           ← 宽、高（十进制文字，用任意空白隔开）
 *     255               ← 每个颜色分量的最大值
 *     RGBRGBRGB……       ← 紧跟一个空白字符之后，就是逐行、逐像素的原始字节
 *
 * 文字头 + 原始 RGB 字节，没有压缩、没有校验和、没有分块，几十行代码就能读写，
 * 用 hexdump 一眼就能看懂。PNG 要实现 zlib 解压，JPEG 要实现离散余弦变换，
 * 对一个“从零写起”的系统来说都太重了。代价是文件大：640×420 的画就是 806 KB。
 *
 * 同一家族的其它格式也一并支持（文件管理器会把 .ppm/.pgm/.pbm 都交给画板打开）：
 *   P1 / P4  PBM 黑白：P1 是文字 0/1，P4 是每像素 1 位（1 = 黑）
 *   P2 / P5  PGM 灰度：文字 / 二进制
 *   P3 / P6  PPM 彩色：文字 / 二进制
 *   最大值不是 255 时按比例换算；超过 255 时每个分量占 2 字节（大端序）。
 *
 * 读文件时要假设文件可能是坏的（被截断、头部乱写、宽高大得离谱……）：
 * 每一步都检查，出错时给出中文原因，绝不能让画板崩溃。
 */
#include "skpaint.h"
#include "skutil.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 读取用的上下文：stdio 自带缓冲，逐字节 getc 也不会每个字节一次系统调用 */
typedef struct {
    FILE  *f;
    char  *err;
    size_t errlen;
} reader;

static int fail(reader *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(reader *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, r->errlen, fmt, ap);
    va_end(ap);
    return -1;
}

static int is_space(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
}

/* 跳过空白和 # 注释，返回下一个有意义的字符（已经读掉了）；文件结束返回 EOF */
static int next_char(FILE *f)
{
    for (;;) {
        int ch = getc(f);
        if (ch == '#') {
            while ((ch = getc(f)) != '\n' && ch != EOF)
                ;
            if (ch == EOF)
                return EOF;
            continue;
        }
        if (!is_space(ch))
            return ch;
    }
}

/* 读一个十进制正整数（头部的宽/高/最大值，或文字格式里的一个像素值）。
 * 返回 0 成功；-1 文件提前结束；-2 不是数字；-3 数字大于 limit */
static int read_number(FILE *f, unsigned limit, unsigned *out)
{
    int ch = next_char(f);
    if (ch == EOF)
        return -1;
    if (ch < '0' || ch > '9')
        return -2;
    unsigned long long v = 0;
    while (ch >= '0' && ch <= '9') {
        v = v * 10 + (unsigned)(ch - '0');
        if (v > limit)
            return -3;
        ch = getc(f);
    }
    if (ch != EOF)
        ungetc(ch, f);
    *out = (unsigned)v;
    return 0;
}

/* 头部的数字：出错时直接写好错误信息 */
static int header_number(reader *r, const char *what, unsigned limit, unsigned *out)
{
    switch (read_number(r->f, limit, out)) {
    case 0:
        return 0;
    case -1:
        return fail(r, "图片文件头不完整：还没读到%s，文件就结束了。", what);
    case -3:
        return fail(r, "图片的%s大得离谱（超过 %u），文件可能已经损坏。", what, limit);
    default:
        return fail(r, "图片文件头损坏：%s应该是一个数字。", what);
    }
}

/*
 * 内存检查。Linux 默认“过量分配”（overcommit）：malloc 一大块内存时内核只是记个账，
 * 真正用到某一页时才分配物理内存。所以 malloc 成功不代表真有那么多内存——
 * 等到逐行写像素时才发现不够，内核的 OOM killer 就会直接杀掉进程（甚至别的进程）。
 * SketchOS 的虚拟机一共只有 256 MB，所以打开大图之前先读 /proc/meminfo 的
 * MemAvailable（“还能给新程序用多少”）估算一下：图片本身 + 撤销记录大约要两份图片的内存。
 */
static int memory_ok(reader *r, unsigned w, unsigned h)
{
    uint64_t need = (uint64_t)w * h * 4 * 2 + 4u * 1024 * 1024;
    sk_meminfo m;
    if (sk_read_meminfo(&m) != 0 || m.available_kb == 0)
        return 0;                       /* 读不到就不检查，交给 malloc 的返回值 */
    uint64_t avail = m.available_kb * 1024u;
    if (need > avail)
        return fail(r, "内存不够：打开这张 %u×%u 的图片大约需要 %llu MB，现在只有 %llu MB 可用。",
                    w, h, (unsigned long long)(need >> 20), (unsigned long long)(avail >> 20));
    return 0;
}

/* 0..maxval 的分量值换算成 0..255（四舍五入） */
static uint8_t *make_scale(unsigned maxval)
{
    uint8_t *lut = malloc((size_t)maxval + 1);
    if (!lut)
        return NULL;
    for (unsigned v = 0; v <= maxval; v++)
        lut[v] = (uint8_t)((v * 255u + maxval / 2) / maxval);
    return lut;
}

static int truncated(reader *r, int y, int h)
{
    return fail(r, "图片数据不完整：文件在第 %d 行就结束了（应该有 %d 行），可能没有传完或已经损坏。", y + 1, h);
}

/* 二进制格式（P4/P5/P6）：一次读一整行 */
static int read_binary(reader *r, sk_canvas *c, int fmt, unsigned maxval, const uint8_t *lut)
{
    int w = c->w, h = c->h;
    int bps = maxval > 255 ? 2 : 1;                         /* 每个分量几个字节 */
    size_t rowbytes = fmt == 4 ? (size_t)(w + 7) / 8 : (size_t)w * (fmt == 6 ? 3 : 1) * bps;
    uint8_t *row = malloc(rowbytes);
    if (!row)
        return fail(r, "内存不足，无法读取图片。");
    for (int y = 0; y < h; y++) {
        size_t got = fread(row, 1, rowbytes, r->f);
        if (got != rowbytes) {
            int e = ferror(r->f) ? errno : 0;
            free(row);
            if (e)
                return fail(r, "读取图片时出错：%s", sk_strerror(e));
            return truncated(r, y, h);
        }
        uint32_t *p = c->pixels + (size_t)y * c->stride;
        for (int x = 0; x < w; x++) {
            unsigned v[3];
            if (fmt == 4) {
                /* PBM：1 = 黑，0 = 白（和“墨水”的直觉一致，和亮度相反） */
                int bit = (row[x >> 3] >> (7 - (x & 7))) & 1;
                v[0] = v[1] = v[2] = bit ? 0 : 255;
            } else {
                int ncomp = fmt == 6 ? 3 : 1;
                for (int k = 0; k < ncomp; k++) {
                    const uint8_t *s = row + ((size_t)x * ncomp + k) * bps;
                    unsigned val = bps == 2 ? ((unsigned)s[0] << 8) | s[1] : s[0];
                    if (val > maxval)
                        val = maxval;               /* 超出声明的最大值：按最大值算 */
                    v[k] = lut[val];
                }
                if (ncomp == 1)
                    v[1] = v[2] = v[0];
            }
            p[x] = SK_RGB(v[0], v[1], v[2]);
        }
    }
    free(row);
    return 0;
}

/* 文字格式（P1/P2/P3）：一个数一个数地读 */
static int read_ascii(reader *r, sk_canvas *c, int fmt, unsigned maxval, const uint8_t *lut)
{
    int w = c->w, h = c->h;
    int ncomp = fmt == 3 ? 3 : 1;
    for (int y = 0; y < h; y++) {
        uint32_t *p = c->pixels + (size_t)y * c->stride;
        for (int x = 0; x < w; x++) {
            unsigned v[3];
            for (int k = 0; k < ncomp; k++) {
                unsigned val = 0;
                if (fmt == 1) {
                    /* P1 的像素可以不用空白隔开（"0110"），所以按字符读 */
                    int ch = next_char(r->f);
                    if (ch == EOF)
                        return truncated(r, y, h);
                    if (ch != '0' && ch != '1')
                        return fail(r, "图片数据损坏：第 %d 行出现了不是 0/1 的字符。", y + 1);
                    v[0] = v[1] = v[2] = ch == '1' ? 0 : 255;
                    break;
                }
                int rc = read_number(r->f, 65535, &val);
                if (rc == -1)
                    return truncated(r, y, h);
                if (rc != 0)
                    return fail(r, "图片数据损坏：第 %d 行有一个像素值不是合法的数字。", y + 1);
                if (val > maxval)
                    val = maxval;
                v[k] = lut[val];
            }
            if (fmt == 2)
                v[1] = v[2] = v[0];
            p[x] = SK_RGB(v[0], v[1], v[2]);
        }
    }
    return 0;
}

sk_canvas *pnm_load(const char *path, char *err, size_t errlen)
{
    reader r = { NULL, err, errlen };
    if (errlen)
        err[0] = 0;
    /* O_NONBLOCK：万一路径是一个命名管道，open 不会一直卡住（下面会拒绝非普通文件） */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fail(&r, "打不开文件：%s", sk_strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        fail(&r, "读不到文件信息：%s", sk_strerror(errno));
        close(fd);
        return NULL;
    }
    if (S_ISDIR(st.st_mode)) {
        fail(&r, "这是一个文件夹，不是图片。");
        close(fd);
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        /* 设备文件（/dev/zero……）可以无穷无尽地读下去，只接受普通文件 */
        fail(&r, "这不是一个普通文件，画板只能打开图片文件。");
        close(fd);
        return NULL;
    }
    r.f = fdopen(fd, "rb");
    if (!r.f) {
        fail(&r, "打不开文件：%s", sk_strerror(errno));
        close(fd);
        return NULL;
    }

    sk_canvas *c = NULL;
    uint8_t *lut = NULL;
    int c0 = getc(r.f), c1 = getc(r.f);
    if (c0 != 'P' || c1 < '1' || c1 > '6') {
        if (c0 == EOF)
            fail(&r, "这是一个空文件，里面没有图片。");
        else
            fail(&r, "这不是 PPM/PGM/PBM 图片：文件开头应该是 P1～P6。\n画板只认识 Netpbm 格式的图片。");
        goto out;
    }
    int fmt = c1 - '0';
    int ch = getc(r.f);
    if (!is_space(ch) && ch != '#') {
        fail(&r, "这不是 PPM/PGM/PBM 图片：文件开头的 P%d 后面应该是空白。", fmt);
        goto out;
    }
    ungetc(ch, r.f);

    unsigned w, h, maxval = 1;
    if (header_number(&r, "宽度", 1000000, &w) || header_number(&r, "高度", 1000000, &h))
        goto out;
    if (fmt != 1 && fmt != 4 && header_number(&r, "颜色最大值", 65535, &maxval))
        goto out;
    if (w == 0 || h == 0) {
        fail(&r, "图片的宽或高是 0（%u×%u），没有东西可以显示。", w, h);
        goto out;
    }
    if (maxval == 0) {
        fail(&r, "图片文件头损坏：颜色最大值不能是 0。");
        goto out;
    }
    if (w > PNM_MAX_SIDE || h > PNM_MAX_SIDE) {
        fail(&r, "图片太大了：%u×%u。画板最大只能打开 %d×%d 的图片。", w, h, PNM_MAX_SIDE, PNM_MAX_SIDE);
        goto out;
    }
    /* 二进制格式：头部最后一个数后面正好是一个空白字符，然后就是像素 */
    if (fmt >= 4) {
        ch = getc(r.f);
        if (!is_space(ch)) {
            fail(&r, "图片文件头损坏：头部后面应该紧跟一个空白字符，然后才是像素数据。");
            goto out;
        }
    }
    if (memory_ok(&r, w, h))
        goto out;
    c = sk_canvas_new((int)w, (int)h);
    if (!c) {
        fail(&r, "内存不足，无法打开 %u×%u 的图片。", w, h);
        goto out;
    }
    if (fmt != 1 && fmt != 4) {
        lut = make_scale(maxval);
        if (!lut) {
            fail(&r, "内存不足，无法读取图片。");
            goto bad;
        }
    }
    if ((fmt >= 4 ? read_binary(&r, c, fmt, maxval, lut) : read_ascii(&r, c, fmt, maxval, lut)) != 0)
        goto bad;
    goto out;
bad:
    sk_canvas_free(c);
    c = NULL;
out:
    free(lut);
    fclose(r.f);                       /* 也关闭了 fd */
    return c;
}

/* ---------------------------------------------------------------- 保存 */

/*
 * 为什么先写临时文件再 rename()？
 * 如果直接打开原文件、截断、再写，写到一半时出错（磁盘满、程序被杀、断电），
 * 原来的画就没了，新的也没写完。rename() 是内核保证“原子”的操作：在同一个文件系统里，
 * 把目录项从旧文件换成新文件是一步完成的——任何时刻别的程序看到的要么是完整的旧图，
 * 要么是完整的新图，绝不会是半张。所以：先把新图完整地写进旁边的临时文件，
 * fsync() 让内核把数据真正写到存储设备上，最后 rename() 盖过去。
 * （SketchOS 的根文件系统在内存里，fsync 几乎什么都不做，但这是该养成的好习惯。）
 */
int pnm_save(const sk_canvas *c, const char *path, char *err, size_t errlen)
{
    reader r = { NULL, err, errlen };
    if (errlen)
        err[0] = 0;
    if (!c || !c->pixels || c->w <= 0 || c->h <= 0)
        return fail(&r, "没有图片可以保存。");

    char tmp[4200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp-%d", path, (int)getpid()) >= (int)sizeof tmp)
        return fail(&r, "文件路径太长了。");
    /* O_EXCL：临时文件名如果已经存在就失败，不会误写别人的文件 */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return fail(&r, "无法创建文件：%s", sk_strerror(errno));
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        int e = errno;
        close(fd);
        unlink(tmp);
        return fail(&r, "无法写入文件：%s", sk_strerror(e));
    }

    int ok = fprintf(f, "P6\n# SketchOS 画板\n%d %d\n255\n", c->w, c->h) > 0;
    unsigned char *row = malloc((size_t)c->w * 3);
    if (!row) {
        errno = ENOMEM;
        ok = 0;
    }
    for (int y = 0; ok && y < c->h; y++) {
        const uint32_t *p = c->pixels + (size_t)y * c->stride;
        for (int x = 0; x < c->w; x++) {
            row[x * 3 + 0] = (unsigned char)SK_R(p[x]);
            row[x * 3 + 1] = (unsigned char)SK_G(p[x]);
            row[x * 3 + 2] = (unsigned char)SK_B(p[x]);
        }
        if (fwrite(row, 1, (size_t)c->w * 3, f) != (size_t)c->w * 3)
            ok = 0;
    }
    free(row);
    int e = ok ? 0 : (errno ? errno : EIO);
    /* fflush 把 stdio 缓冲交给内核，fsync 再让内核把数据落到存储设备上 */
    if (ok && fflush(f) != 0) {
        ok = 0;
        e = errno;
    }
    if (ok && fsync(fileno(f)) != 0 && errno != EINVAL && errno != EROFS) {
        ok = 0;
        e = errno;
    }
    if (fclose(f) != 0 && ok) {
        ok = 0;
        e = errno;
    }
    if (!ok) {
        unlink(tmp);
        return fail(&r, "写入文件失败：%s", sk_strerror(e));
    }
    if (rename(tmp, path) != 0) {
        e = errno;
        unlink(tmp);
        return fail(&r, "无法保存到这个位置：%s", sk_strerror(e));
    }
    return 0;
}
