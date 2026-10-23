/*
 * display_kms.c — 用 DRM/KMS 接口直接设置显示模式（不依赖 libdrm，全部是裸 ioctl）
 *
 * Linux 的显卡驱动分两层概念：
 *   DRM (Direct Rendering Manager)：管理显存、GPU 命令；
 *   KMS (Kernel Mode Setting)：由内核来设置分辨率、刷新率、显示哪块显存。
 * 以前是 X 服务器在用户态直接改显卡寄存器（UMS），一出错整机花屏；
 * 现在模式设置由内核完成，用户态只通过 /dev/dri/card0 上的 ioctl 发出请求。
 *
 * KMS 里的几个对象（显示管线从“内存”到“显示器”）：
 *
 *   framebuffer (fb)  ──▶  CRTC  ──▶  encoder  ──▶  connector  ──▶ 显示器
 *   一块像素内存          扫描时序      信号编码       接口（VGA/HDMI…）
 *                        （分辨率）
 *
 * 我们的步骤：
 *   1. SET_MASTER：成为“DRM 主控”，只有 master 才能改显示模式；
 *   2. GETRESOURCES：列出所有 CRTC / connector / encoder；
 *   3. 找第一个“已连接”的 connector，挑分辨率（sketch.mode → 1024x768 → 首选模式）；
 *   4. connector 当前的 encoder → 它驱动的 CRTC（没有就找一个兼容的）；
 *   5. GETCRTC 保存原来的状态（文字控制台 fbcon 用的那块 fb），退出时恢复；
 *   6. CREATE_DUMB 申请一块“哑缓冲区”（dumb buffer：CPU 可直接写的简单显存，
 *      不需要 GPU 加速），ADDFB 把它注册成 framebuffer，MAP_DUMB + mmap 映射到进程里；
 *   7. SETCRTC：让 CRTC 扫描我们的 framebuffer —— 屏幕上就是我们画的东西了。
 *   8. 每次改完像素调用 DIRTYFB 告诉驱动哪些矩形变了（有的驱动需要据此把影子缓冲区
 *      拷到真正的显存；bochs-drm 直接扫描显存，会返回 ENOSYS，那就不再调用）。
 */
#define _GNU_SOURCE
#include "sketchd.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define KMS_MAX_CLIPS 64

typedef struct {
    int      fd;
    uint32_t conn_id, crtc_id;
    uint32_t fb_id, handle;
    uint64_t size;
    void    *map;
    int      dirty_ok;                 /* 驱动支持 DIRTYFB */
    struct drm_mode_crtc saved;        /* 接管之前的 CRTC 状态 */
    int      saved_valid;
} kms_state;

/* 只有一个显示器，所以用一个静态实例；崩溃处理函数也需要访问它 */
static kms_state g_kms = { .fd = -1 };

/* ioctl 被信号打断（EINTR）或驱动暂时忙（EAGAIN）时重试：libdrm 的 drmIoctl 也是这样做的 */
static int drm_ioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static uint64_t ptr64(const void *p) { return (uint64_t)(uintptr_t)p; }

/* ---------- 资源枚举 ---------- */

typedef struct {
    uint32_t *crtcs, *conns, *encs;
    uint32_t  n_crtcs, n_conns, n_encs;
} kms_res;

static void res_free(kms_res *r)
{
    free(r->crtcs);
    free(r->conns);
    free(r->encs);
    memset(r, 0, sizeof *r);
}

/* GETRESOURCES 的标准用法是“调用两次”：第一次只问个数，第二次传入数组取内容。
 * 如果两次之间有热插拔导致个数变了，就重来。 */
static int get_resources(int fd, kms_res *out)
{
    memset(out, 0, sizeof *out);
    for (int attempt = 0; attempt < 5; attempt++) {
        struct drm_mode_card_res res;
        memset(&res, 0, sizeof res);
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
            sk_log("kms: GETRESOURCES 失败: %s", strerror(errno));
            return -1;
        }
        uint32_t nc = res.count_crtcs, nn = res.count_connectors, ne = res.count_encoders;
        res_free(out);
        out->crtcs = calloc(nc ? nc : 1, sizeof(uint32_t));
        out->conns = calloc(nn ? nn : 1, sizeof(uint32_t));
        out->encs  = calloc(ne ? ne : 1, sizeof(uint32_t));
        if (!out->crtcs || !out->conns || !out->encs) {
            res_free(out);
            return -1;
        }
        memset(&res, 0, sizeof res);
        res.count_crtcs = nc;
        res.count_connectors = nn;
        res.count_encoders = ne;
        res.crtc_id_ptr = ptr64(out->crtcs);
        res.connector_id_ptr = ptr64(out->conns);
        res.encoder_id_ptr = ptr64(out->encs);
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
            sk_log("kms: GETRESOURCES(2) 失败: %s", strerror(errno));
            res_free(out);
            return -1;
        }
        if (res.count_crtcs == nc && res.count_connectors == nn && res.count_encoders == ne) {
            out->n_crtcs = nc;
            out->n_conns = nn;
            out->n_encs = ne;
            return 0;
        }
    }
    res_free(out);
    return -1;
}

typedef struct {
    uint32_t id;
    uint32_t encoder_id;
    struct drm_mode_modeinfo *modes;
    uint32_t n_modes;
    uint32_t *encoders;
    uint32_t n_encoders;
} kms_conn;

static void conn_free(kms_conn *c)
{
    free(c->modes);
    free(c->encoders);
    memset(c, 0, sizeof *c);
}

/* 读取一个 connector：第一次调用时 count_modes=0，内核会顺便“探测”显示器
 * （读 EDID 等）并生成模式列表；第二次调用取回模式和 encoder 列表。
 * 返回 1 = 已连接且有模式，0 = 未连接，-1 = 出错 */
static int get_connector(int fd, uint32_t id, kms_conn *out)
{
    memset(out, 0, sizeof *out);
    for (int attempt = 0; attempt < 5; attempt++) {
        struct drm_mode_get_connector gc;
        memset(&gc, 0, sizeof gc);
        gc.connector_id = id;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0)
            return -1;
        if (gc.connection != 1 /* DRM_MODE_CONNECTED */ || gc.count_modes == 0)
            return 0;
        uint32_t nm = gc.count_modes, ne = gc.count_encoders;
        conn_free(out);
        out->modes = calloc(nm, sizeof(struct drm_mode_modeinfo));
        out->encoders = calloc(ne ? ne : 1, sizeof(uint32_t));
        if (!out->modes || !out->encoders) {
            conn_free(out);
            return -1;
        }
        memset(&gc, 0, sizeof gc);
        gc.connector_id = id;
        gc.count_modes = nm;
        gc.modes_ptr = ptr64(out->modes);
        gc.count_encoders = ne;
        gc.encoders_ptr = ptr64(out->encoders);
        /* count_props = 0：我们不需要属性，内核就不会拷贝 */
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) {
            conn_free(out);
            return -1;
        }
        if (gc.count_modes == nm && gc.count_encoders == ne) {
            out->id = id;
            out->encoder_id = gc.encoder_id;
            out->n_modes = nm;
            out->n_encoders = ne;
            return gc.connection == 1 ? 1 : 0;
        }
    }
    conn_free(out);
    return -1;
}

/* 选分辨率：sketch.mode 指定的 → 1024x768 → 驱动标记为“首选”的 → 第一个 */
static const struct drm_mode_modeinfo *pick_mode(const kms_conn *c, int want_w, int want_h)
{
    if (want_w > 0 && want_h > 0) {
        for (uint32_t i = 0; i < c->n_modes; i++)
            if (c->modes[i].hdisplay == want_w && c->modes[i].vdisplay == want_h)
                return &c->modes[i];
        sk_log("kms: 显示器不支持 %dx%d，改用默认分辨率", want_w, want_h);
    }
    for (uint32_t i = 0; i < c->n_modes; i++)
        if (c->modes[i].hdisplay == 1024 && c->modes[i].vdisplay == 768)
            return &c->modes[i];
    for (uint32_t i = 0; i < c->n_modes; i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED)
            return &c->modes[i];
    return &c->modes[0];
}

/* connector → encoder → CRTC。优先用当前已经连好的那条管线（fbcon 正在用的），
 * 否则在 encoder 的 possible_crtcs 位掩码里找一个 CRTC（第 i 位对应资源列表里第 i 个 CRTC）。 */
static uint32_t pick_crtc(int fd, const kms_res *res, const kms_conn *c)
{
    struct drm_mode_get_encoder enc;
    if (c->encoder_id) {
        memset(&enc, 0, sizeof enc);
        enc.encoder_id = c->encoder_id;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
            return enc.crtc_id;
    }
    for (uint32_t e = 0; e < c->n_encoders; e++) {
        memset(&enc, 0, sizeof enc);
        enc.encoder_id = c->encoders[e];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
            continue;
        for (uint32_t i = 0; i < res->n_crtcs && i < 32; i++)
            if (enc.possible_crtcs & (1u << i))
                return res->crtcs[i];
    }
    return 0;
}

/* ---------- 后端接口 ---------- */

static void kms_commit(skd_display *d, const sk_rect *rects, int n)
{
    (void)d;
    if (!g_kms.dirty_ok || n <= 0)
        return;
    struct drm_clip_rect clips[KMS_MAX_CLIPS];
    int nc = 0;
    for (int i = 0; i < n && nc < KMS_MAX_CLIPS; i++) {
        if (rects[i].w <= 0 || rects[i].h <= 0)
            continue;
        clips[nc].x1 = (unsigned short)rects[i].x;
        clips[nc].y1 = (unsigned short)rects[i].y;
        clips[nc].x2 = (unsigned short)(rects[i].x + rects[i].w);   /* 右下角是“开区间” */
        clips[nc].y2 = (unsigned short)(rects[i].y + rects[i].h);
        nc++;
    }
    if (nc == 0)
        return;
    struct drm_mode_fb_dirty_cmd dc;
    memset(&dc, 0, sizeof dc);
    dc.fb_id = g_kms.fb_id;
    dc.num_clips = (uint32_t)nc;
    dc.clips_ptr = ptr64(clips);
    if (drm_ioctl(g_kms.fd, DRM_IOCTL_MODE_DIRTYFB, &dc) < 0) {
        if (errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL) {
            /* 驱动直接扫描这块显存，不需要“脏矩形”通知：以后不再浪费这次系统调用 */
            SKD_DBG("kms: 驱动不需要 DIRTYFB (%s)，停用", strerror(errno));
            g_kms.dirty_ok = 0;
        } else {
            SKD_DBG("kms: DIRTYFB 失败: %s", strerror(errno));
        }
    }
}

/* 恢复接管之前的 CRTC（让文字控制台的 framebuffer 重新显示） */
static void restore_crtc(void)
{
    if (g_kms.fd < 0 || !g_kms.saved_valid)
        return;
    struct drm_mode_crtc sc = g_kms.saved;
    if (sc.fb_id && sc.mode_valid) {
        sc.set_connectors_ptr = ptr64(&g_kms.conn_id);
        sc.count_connectors = 1;
        ioctl(g_kms.fd, DRM_IOCTL_MODE_SETCRTC, &sc);
    }
}

static void kms_close(skd_display *d)
{
    if (g_kms.fd < 0)
        return;
    restore_crtc();
    if (g_kms.fb_id) {
        uint32_t id = g_kms.fb_id;
        drm_ioctl(g_kms.fd, DRM_IOCTL_MODE_RMFB, &id);
        g_kms.fb_id = 0;
    }
    if (g_kms.map && g_kms.map != MAP_FAILED)
        munmap(g_kms.map, (size_t)g_kms.size);
    g_kms.map = NULL;
    if (g_kms.handle) {
        struct drm_mode_destroy_dumb dd = { .handle = g_kms.handle };
        drm_ioctl(g_kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        g_kms.handle = 0;
    }
    /* 交出 master；文件关闭后内核的 fbdev 模拟层会把控制台的画面恢复回来 */
    drm_ioctl(g_kms.fd, DRM_IOCTL_DROP_MASTER, NULL);
    close(g_kms.fd);
    g_kms.fd = -1;
    g_kms.saved_valid = 0;
    if (d)
        d->map = NULL;
}

/* 信号处理函数里调用：只用 ioctl（异步信号安全），不 free、不打日志 */
void display_kms_emergency_restore(void)
{
    restore_crtc();
}

int display_kms_open(skd_display *d, int want_w, int want_h)
{
    const char *path = "/dev/dri/card0";
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        sk_log("kms: 打开 %s 失败: %s", path, strerror(errno));
        return -1;
    }
    g_kms.fd = fd;

    /* 第一个打开设备的进程自动成为 master；显式再要一次（sketchd 重启后也能拿回来）。
     * 失败也继续试：真正失败会在 SETCRTC 时暴露出来。 */
    if (drm_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0)
        SKD_DBG("kms: SET_MASTER: %s（可能已经是 master）", strerror(errno));

    kms_res res;
    if (get_resources(fd, &res) < 0)
        goto fail;

    kms_conn conn;
    memset(&conn, 0, sizeof conn);
    int found = 0;
    for (uint32_t i = 0; i < res.n_conns && !found; i++) {
        int r = get_connector(fd, res.conns[i], &conn);
        if (r == 1)
            found = 1;
        else
            conn_free(&conn);
    }
    if (!found) {
        sk_log("kms: 没有找到已连接的显示器");
        res_free(&res);
        goto fail;
    }

    struct drm_mode_modeinfo mode = *pick_mode(&conn, want_w, want_h);
    uint32_t crtc = pick_crtc(fd, &res, &conn);
    g_kms.conn_id = conn.id;
    conn_free(&conn);
    res_free(&res);
    if (!crtc) {
        sk_log("kms: 找不到可用的 CRTC");
        goto fail;
    }
    g_kms.crtc_id = crtc;

    /* 记下原来的状态（通常是 fbcon 的 framebuffer），退出时恢复 */
    memset(&g_kms.saved, 0, sizeof g_kms.saved);
    g_kms.saved.crtc_id = crtc;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &g_kms.saved) == 0)
        g_kms.saved_valid = 1;

    int w = mode.hdisplay, h = mode.vdisplay;
    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof cd);
    cd.width = (uint32_t)w;
    cd.height = (uint32_t)h;
    cd.bpp = 32;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        sk_log("kms: CREATE_DUMB %dx%d 失败: %s", w, h, strerror(errno));
        goto fail;
    }
    g_kms.handle = cd.handle;
    g_kms.size = cd.size;

    /* depth 24 / bpp 32 = XRGB8888：每像素 4 字节，最高字节不用。正好就是 sk_color 的格式 */
    struct drm_mode_fb_cmd fb;
    memset(&fb, 0, sizeof fb);
    fb.width = (uint32_t)w;
    fb.height = (uint32_t)h;
    fb.pitch = cd.pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = cd.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        sk_log("kms: ADDFB 失败: %s", strerror(errno));
        goto fail;
    }
    g_kms.fb_id = fb.fb_id;

    /* MAP_DUMB 不会真的映射，只返回一个“假偏移量”，再用这个偏移量对 DRM 设备 mmap */
    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        sk_log("kms: MAP_DUMB 失败: %s", strerror(errno));
        goto fail;
    }
    void *map = mmap(NULL, (size_t)cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
    if (map == MAP_FAILED) {
        sk_log("kms: mmap 显存失败: %s", strerror(errno));
        goto fail;
    }
    g_kms.map = map;

    /* 先涂成纸色，避免 SETCRTC 后闪一下上次残留的内容 */
    uint32_t paper = SK_DESK_PAPER;
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)map + (size_t)y * cd.pitch);
        for (int x = 0; x < w; x++)
            row[x] = paper;
    }

    struct drm_mode_crtc sc;
    memset(&sc, 0, sizeof sc);
    sc.crtc_id = crtc;
    sc.fb_id = fb.fb_id;
    sc.set_connectors_ptr = ptr64(&g_kms.conn_id);
    sc.count_connectors = 1;
    sc.mode = mode;
    sc.mode_valid = 1;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc) < 0) {
        sk_log("kms: SETCRTC 失败: %s", strerror(errno));
        goto fail;
    }

    g_kms.dirty_ok = 1;
    memset(d, 0, sizeof *d);
    d->name = "kms";
    d->w = w;
    d->h = h;
    d->map = map;
    d->pitch = (int)cd.pitch;
    d->bpp = 32;
    d->commit = kms_commit;
    d->close = kms_close;
    sk_log("kms: %dx%d@%u pitch=%u connector=%u crtc=%u fb=%u",
           w, h, mode.vrefresh, cd.pitch, g_kms.conn_id, crtc, fb.fb_id);
    return 0;

fail:
    kms_close(NULL);
    return -1;
}
