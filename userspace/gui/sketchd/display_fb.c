/*
 * display_fb.c — 后备显示后端：fbdev (/dev/fb0) 以及测试用的 headless（纯内存）
 *
 * fbdev 是 Linux 很老的帧缓冲接口：内核把“一块显存 + 当前分辨率”包装成 /dev/fb0，
 * 程序 mmap 之后直接写像素即可。它不能（好好地）切换分辨率，也没有多显示器、
 * 双缓冲这些概念，但胜在简单，几乎所有平台都有。
 * 我们的内核里 /dev/fb0 是 DRM 驱动提供的“fbdev 模拟”(DRM_FBDEV_EMULATION)，
 * 就是文字控制台 fbcon 正在用的那块缓冲区——所以必须先把 VT 切到 KD_GRAPHICS，
 * 否则 fbcon 还会在上面画字、画闪烁的光标（见 vt.c）。
 *
 * 像素格式：
 *   32 位 XRGB8888：和 sk_color 一样，整行 memcpy；
 *   16 位 RGB565：  每像素 2 字节，红 5 位、绿 6 位、蓝 5 位，需要逐像素转换
 *                   （人眼对绿色最敏感，所以绿色多给 1 位）。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    int    fd;
    void  *base;      /* mmap 返回的起始地址（可能有平移偏移，d->map 指向可见区域） */
    size_t len;
} fb_state;

static fb_state g_fb = { .fd = -1 };

static void fb_close(skd_display *d)
{
    if (g_fb.base && g_fb.base != MAP_FAILED)
        munmap(g_fb.base, g_fb.len);
    g_fb.base = NULL;
    if (g_fb.fd >= 0)
        close(g_fb.fd);
    g_fb.fd = -1;
    if (d)
        d->map = NULL;
}

int display_fb_open(skd_display *d)
{
    const char *path = "/dev/fb0";
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        sk_log("fbdev: 打开 %s 失败: %s", path, strerror(errno));
        return -1;
    }
    g_fb.fd = fd;

    struct fb_var_screeninfo var;   /* “可变”信息：分辨率、色深、像素格式、平移位置 */
    struct fb_fix_screeninfo fix;   /* “固定”信息：显存大小、每行字节数 */
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        sk_log("fbdev: 读取屏幕信息失败: %s", strerror(errno));
        goto fail;
    }
    if (fix.type != FB_TYPE_PACKED_PIXELS ||
        (fix.visual != FB_VISUAL_TRUECOLOR && fix.visual != FB_VISUAL_DIRECTCOLOR)) {
        sk_log("fbdev: 不支持的帧缓冲类型 type=%u visual=%u", fix.type, fix.visual);
        goto fail;
    }

    int bpp = (int)var.bits_per_pixel;
    if (bpp == 32) {
        if (var.red.offset != 16 || var.green.offset != 8 || var.blue.offset != 0) {
            sk_log("fbdev: 不支持的 32 位像素排列 R@%u G@%u B@%u",
                   var.red.offset, var.green.offset, var.blue.offset);
            goto fail;
        }
    } else if (bpp == 16) {
        if (var.red.offset != 11 || var.red.length != 5 || var.green.offset != 5 ||
            var.green.length != 6 || var.blue.offset != 0 || var.blue.length != 5) {
            sk_log("fbdev: 不支持的 16 位像素排列（只支持 RGB565）");
            goto fail;
        }
    } else {
        sk_log("fbdev: 不支持 %d 位色深", bpp);
        goto fail;
    }

    g_fb.len = fix.smem_len;
    g_fb.base = mmap(NULL, g_fb.len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_fb.base == MAP_FAILED) {
        g_fb.base = NULL;
        sk_log("fbdev: mmap 失败: %s", strerror(errno));
        goto fail;
    }
    /* 可见区域可能被“平移”(panning) 到缓冲区中间某处 */
    size_t off = (size_t)var.yoffset * fix.line_length + (size_t)var.xoffset * (size_t)(bpp / 8);
    size_t need = off + (size_t)var.yres * fix.line_length;
    if (need > g_fb.len) {
        sk_log("fbdev: 显存太小（%zu < %zu）", g_fb.len, need);
        goto fail;
    }

    memset(d, 0, sizeof *d);
    d->name = "fbdev";
    d->w = (int)var.xres;
    d->h = (int)var.yres;
    d->map = (uint8_t *)g_fb.base + off;
    d->pitch = (int)fix.line_length;
    d->bpp = bpp;
    d->commit = NULL;     /* fbdev 直接就是显存，写完立刻可见 */
    d->close = fb_close;
    sk_log("fbdev: %dx%d %dbpp pitch=%d", d->w, d->h, bpp, d->pitch);
    return 0;

fail:
    fb_close(NULL);
    return -1;
}

/* ---------- headless：没有屏幕，只在内存里合成（在 WSL 里做离线测试用） ---------- */

static void headless_close(skd_display *d)
{
    free(d->priv);
    d->priv = NULL;
    d->map = NULL;
}

int display_headless_open(skd_display *d, int w, int h)
{
    if (w <= 0 || h <= 0) {
        w = 1024;
        h = 768;
    }
    void *mem = calloc((size_t)w * (size_t)h, 4);
    if (!mem)
        return -1;
    memset(d, 0, sizeof *d);
    d->name = "headless";
    d->w = w;
    d->h = h;
    d->map = mem;
    d->pitch = w * 4;
    d->bpp = 32;
    d->close = headless_close;
    d->priv = mem;
    return 0;
}
