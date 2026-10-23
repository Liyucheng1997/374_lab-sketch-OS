/*
 * player.c — 播放引擎：解码 → 调音量 → 写进声卡，并回答“现在播到哪儿了”
 *
 * ┌──────────────── /dev/dsp：用 write() 放音乐 ────────────────┐
 *
 *  Linux 的声卡驱动框架叫 ALSA。它也模拟了更古老的 OSS 接口：/dev/dsp 这个字符设备。
 *  用法和写文件几乎一样：
 *     fd = open("/dev/dsp", O_WRONLY);
 *     ioctl(fd, SNDCTL_DSP_SETFMT, 16 位有符号小端);   // 告诉驱动采样是什么格式
 *     ioctl(fd, SNDCTL_DSP_CHANNELS, 2);                // 几个声道
 *     ioctl(fd, SNDCTL_DSP_SPEED, 22050);               // 每秒多少帧
 *     write(fd, 采样, 字节数);                          // 声音就出来了
 *  ioctl()（I/O control）是“除了读写以外的所有设备操作”的统一入口：第二个参数是命令号，
 *  第三个参数指向命令要用的数据。驱动会把实际采用的值写回来（硬件可能不支持我们要的值）。
 *
 *  声卡怎么拿到数据？驱动在内存里准备了一块环形缓冲区（我们设成 12 段 × 4 KB ≈ 0.56 秒的声音）。
 *  write() 只是把采样拷进这块缓冲区；SB16 声卡通过 DMA（直接内存访问）自己从缓冲区里一段段取走播放，
 *  每播完一段就发一次中断，驱动由此知道哪里又空出来了。缓冲区满了以后 write() 就得等。
 *
 *  我们用非阻塞方式 (O_NONBLOCK) 打开，再把 fd 交给事件循环的 poll()：
 *  “缓冲区有空位（POLLOUT）了叫醒我”。这样单线程的程序在等声卡的同时还能响应鼠标键盘；
 *  缓冲区满着、又没有人点鼠标的时候，进程就睡在 poll() 里，一点 CPU 都不用。
 *
 * └──────────────────────────────────────────────────────────────┘
 *
 * 播放进度：我们写进去的帧数 ≠ 听众听到的帧数，中间差着缓冲区里还没播的那部分。
 * SNDCTL_DSP_GETODELAY 能问出“缓冲区里还有多少字节没播”，两者相减就是此刻喇叭里的位置——
 * 进度条和频谱都按这个位置来，所以画面和声音是对得上的。
 *
 * 暂停 / 拖动进度条：SNDCTL_DSP_RESET 立刻扔掉缓冲区里没播的声音（否则还会再响半秒），
 * 然后关掉 /dev/dsp（让别的程序也能用声卡）。继续播放时从记下的位置重新解码、重新打开。
 *
 * 没有声卡（比如内核没编进声卡驱动、QEMU 没加 -device sb16）时进入“静音模式”：
 * 用定时器按真实时间推进进度，界面照常工作，只是听不见。
 */
#include "skmusic.h"

#include <skutil.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK_FRAMES  1024           /* 每次解码多少帧（46 毫秒） */
#define FRAME_BYTES   (MUSIC_CHANNELS * 2)
#define PACE_MS       40             /* 静音/文件模式下推进进度的定时器间隔 */
#define DRAIN_MS      50             /* 曲子解码完以后，隔多久问一次“缓冲区播空了没有” */
#define RING_FRAMES   2048           /* 留着最近解码的这么多帧，给频谱用 */
#define FFT_N         512            /* 每次分析多少个采样（23 毫秒） */

struct player {
    sk_app  *app;
    char     path[PATH_MAX];
    player_end_fn on_end;
    void    *user;

    int      state;
    const track *t;
    decoder *dec;
    uint64_t hold;                   /* 暂停/停止时的位置（帧） */

    /* 当前这一“段”播放（从开始/继续/拖动算起） */
    int      out;
    int      fd;
    int      watching;               /* fd 已经交给事件循环 */
    int      timer;
    uint64_t base;                   /* 这一段从曲子的第几帧开始 */
    uint64_t written;                /* 已经交给声卡（或文件）的字节数 */
    uint64_t rendered;               /* 已经解码的帧数 */
    uint64_t start_ms;               /* 这一段开始时的单调时钟（静音/文件模式按它推进） */
    int      eof;
    int16_t  chunk[CHUNK_FRAMES * MUSIC_CHANNELS];
    int      pend_off, pend_len;     /* chunk 里还没写出去的字节 */

    int      volume;
    int32_t  gain;                   /* 音量换算成的乘数，65536 = 原样 */

    int16_t  ring[RING_FRAMES];      /* 最近解码的单声道采样；ring_end 是下一个要写的帧号（曲子里的） */
    uint64_t ring_end;
    float    levels[VIZ_BANDS];

    char     desc[192];
    char     err[256];
};

/* ================================================================ 输出设备 */

static void card_name(char *buf, size_t n)
{
    /* /proc/asound/cards 第二行是声卡的完整名字，例如 “Sound Blaster 16 at 0x220, irq 5, dma 1&5” */
    char *text = sk_read_file("/proc/asound/cards", NULL);
    buf[0] = 0;
    if (!text)
        return;
    char *nl = strchr(text, '\n');
    if (nl) {
        char *line = sk_trim(nl + 1);
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        char *comma = strstr(line, " at 0x");     /* 端口、中断这些留给“关于本机”去讲，这里只要名字 */
        if (comma)
            *comma = 0;
        sk_strlcpy(buf, sk_trim(line), n);
    }
    free(text);
}

static void out_open(player *p)
{
    p->out = OUT_NONE;
    p->fd = -1;
    if (strcmp(p->path, "none") == 0) {
        snprintf(p->desc, sizeof p->desc, "静音模式（SKMUSIC_DSP=none）");
        return;
    }
    int fd = open(p->path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        if (e == ENOENT || e == ENODEV || e == ENXIO)
            snprintf(p->desc, sizeof p->desc, "没有找到声卡（%.100s），静音播放", p->path);
        else if (e == EBUSY)
            snprintf(p->desc, sizeof p->desc, "声卡正被别的程序占用，静音播放");
        else
            snprintf(p->desc, sizeof p->desc, "打不开 %.100s：%s，静音播放", p->path, sk_strerror(e));
        return;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && !S_ISCHR(st.st_mode)) {
        /* 普通文件：自动化测试把声音“录”下来检查。暂停后继续时接着往后写，不覆盖前面的 */
        lseek(fd, 0, SEEK_END);
        p->out = OUT_FILE;
        p->fd = fd;
        snprintf(p->desc, sizeof p->desc, "写入文件 %.150s", p->path);
        return;
    }
    /* 缓冲区：高 16 位是段数，低 16 位是每段大小的 log2（2^12 = 4096 字节）。必须在设置格式之前 */
    int frag = (12 << 16) | 12;
    if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag) != 0)
        frag = 0;                                /* 不支持就用驱动的默认值，不算错 */
    int fmt = AFMT_S16_LE, ch = MUSIC_CHANNELS, rate = MUSIC_RATE;
    const char *what = NULL;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) != 0 || fmt != AFMT_S16_LE)
        what = "16 位采样";
    else if (ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) != 0 || ch != MUSIC_CHANNELS)
        what = "立体声";
    else if (ioctl(fd, SNDCTL_DSP_SPEED, &rate) != 0 || rate < MUSIC_RATE * 99 / 100 || rate > MUSIC_RATE * 101 / 100)
        what = "22050 Hz 采样率";
    if (what) {
        snprintf(p->desc, sizeof p->desc, "声卡不支持%s，静音播放", what);
        close(fd);
        return;
    }
    char name[96];
    card_name(name, sizeof name);
    p->out = OUT_DSP;
    p->fd = fd;
    snprintf(p->desc, sizeof p->desc, "%s（%.60s）· %d Hz · 16 位立体声", name[0] ? name : "声卡", p->path, rate);
}

static void out_close(player *p)
{
    if (p->watching) {
        sk_app_remove_fd(p->app, p->fd);
        p->watching = 0;
    }
    if (p->timer) {
        sk_app_remove_timer(p->app, p->timer);
        p->timer = 0;
    }
    if (p->fd >= 0) {
        /* 关闭前先扔掉没播完的声音：OSS 的 close() 默认会等缓冲区播完（最多半秒），界面就卡住了 */
        if (p->out == OUT_DSP)
            ioctl(p->fd, SNDCTL_DSP_RESET, 0);
        close(p->fd);
        p->fd = -1;
    }
}

/* ================================================================ 一段播放 */

static void on_writable(sk_app *app, int fd, short revents, void *user);
static void on_pace(sk_app *app, void *user);
static void on_drain(sk_app *app, void *user);

/* 已经播出去的帧数（这一段里） */
static uint64_t played_frames(player *p)
{
    if (p->out == OUT_DSP) {
        int odelay = 0;
        if (ioctl(p->fd, SNDCTL_DSP_GETODELAY, &odelay) != 0 || odelay < 0)
            odelay = 0;
        uint64_t w = p->written / FRAME_BYTES, q = (uint64_t)odelay / FRAME_BYTES;
        return w > q ? w - q : 0;
    }
    uint64_t f = (sk_now_ms() - p->start_ms) * MUSIC_RATE / 1000;
    return f < p->rendered ? f : p->rendered;
}

static void finish(player *p)
{
    out_close(p);
    decoder_close(p->dec);
    p->dec = NULL;
    p->state = PLAYER_STOPPED;
    p->hold = 0;
    memset(p->levels, 0, sizeof p->levels);
    if (p->on_end)
        p->on_end(p, p->user);       /* 可能马上开始播下一首 */
}

/* 解码一块，调好音量，放进 chunk 等着写出去 */
static int fill_chunk(player *p)
{
    int n = decoder_read(p->dec, p->chunk, CHUNK_FRAMES);
    if (n <= 0) {
        p->eof = 1;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        int32_t l = p->chunk[2 * i], r = p->chunk[2 * i + 1];
        p->ring[(p->ring_end + (uint64_t)i) % RING_FRAMES] = (int16_t)((l + r) / 2);
        /* 音量：乘一个 0..1 的系数（定点数，65536 = 1.0）。结果不会比原来大，不会溢出 */
        p->chunk[2 * i] = (int16_t)((l * p->gain) >> 16);
        p->chunk[2 * i + 1] = (int16_t)((r * p->gain) >> 16);
    }
    p->ring_end += (uint64_t)n;
    p->rendered += (uint64_t)n;
    p->pend_off = 0;
    p->pend_len = n * FRAME_BYTES;
    return n;
}

static int segment_start(player *p, uint64_t pos)
{
    decoder_close(p->dec);
    p->dec = decoder_open(p->t, pos, p->err, sizeof p->err);
    if (!p->dec)
        return -1;
    p->base = pos;
    p->ring_end = pos;
    p->written = p->rendered = 0;
    p->pend_off = p->pend_len = 0;
    p->eof = 0;
    p->start_ms = sk_now_ms();
    out_open(p);
    if (p->out == OUT_DSP) {
        if (sk_app_add_fd(p->app, p->fd, POLLOUT, on_writable, p) == 0)
            p->watching = 1;
        on_writable(p->app, p->fd, POLLOUT, p);   /* 先把缓冲区填满，不用等第一次 poll */
    } else {
        p->timer = sk_app_add_timer(p->app, PACE_MS, 1, on_pace, p);
    }
    return 0;
}

/* 声卡缓冲区有空位了：能写多少写多少 */
static void on_writable(sk_app *app, int fd, short revents, void *user)
{
    (void)app;
    (void)revents;
    player *p = user;
    if (p->state != PLAYER_PLAYING || fd != p->fd)
        return;
    for (;;) {
        if (p->pend_len == 0 && (p->eof || fill_chunk(p) == 0))
            break;
        ssize_t w = write(p->fd, (const char *)p->chunk + p->pend_off, (size_t)p->pend_len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                return;                          /* 缓冲区满了：等下一次 POLLOUT */
            snprintf(p->err, sizeof p->err, "写声卡出错：%s", sk_strerror(errno));
            p->eof = 1;
            p->pend_len = 0;
            break;
        }
        p->pend_off += (int)w;
        p->pend_len -= (int)w;
        p->written += (uint64_t)w;
    }
    /* 全部写完了：不再关心 POLLOUT（否则缓冲区一有空位 poll 就会不停地叫醒我们），
     * 改成定时问一问“播完了没有”。驱动按整段（4 KB）交给声卡，最后不满一段的那点声音
     * 要用 SNDCTL_DSP_POST 告诉它“后面暂时没有了，这些先播吧” */
    ioctl(p->fd, SNDCTL_DSP_POST, 0);
    if (p->watching) {
        sk_app_remove_fd(p->app, p->fd);
        p->watching = 0;
    }
    if (!p->timer)
        p->timer = sk_app_add_timer(p->app, DRAIN_MS, 1, on_drain, p);
}

static void on_drain(sk_app *app, void *user)
{
    (void)app;
    player *p = user;
    /* 第二个条件是保险：万一驱动一直报告“还有一点没播”，按时间算早该播完了也结束 */
    uint64_t expect_ms = p->written / FRAME_BYTES * 1000 / MUSIC_RATE;
    if (played_frames(p) * FRAME_BYTES >= p->written || sk_now_ms() - p->start_ms > expect_ms + 2000)
        finish(p);
}

/* 静音/文件模式：按真实时间推进 */
static void on_pace(sk_app *app, void *user)
{
    (void)app;
    player *p = user;
    uint64_t due = (sk_now_ms() - p->start_ms) * MUSIC_RATE / 1000;
    while (!p->eof && p->rendered < due) {
        if (fill_chunk(p) == 0)
            break;
        if (p->out == OUT_FILE) {
            const char *b = (const char *)p->chunk;
            while (p->pend_len > 0) {
                ssize_t w = write(p->fd, b + p->pend_off, (size_t)p->pend_len);
                if (w < 0 && errno == EINTR)
                    continue;
                if (w <= 0) {
                    snprintf(p->err, sizeof p->err, "写入 %.150s 出错：%s", p->path, sk_strerror(errno));
                    p->eof = 1;
                    break;
                }
                p->pend_off += (int)w;
                p->pend_len -= (int)w;
                p->written += (uint64_t)w;
            }
        }
        p->pend_len = 0;
    }
    if (p->eof && due >= p->rendered)
        finish(p);
}

/* ================================================================ 对外接口 */

player *player_new(sk_app *app, const char *dsp_path, player_end_fn on_end, void *user)
{
    player *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->app = app;
    p->fd = -1;
    p->on_end = on_end;
    p->user = user;
    sk_strlcpy(p->path, dsp_path && *dsp_path ? dsp_path : "/dev/dsp", sizeof p->path);
    player_set_volume(p, 80);
    /* 先试着打开一次声卡，只为了告诉用户“有没有声音”；真正播放时再打开 */
    out_open(p);
    out_close(p);
    return p;
}

void player_free(player *p)
{
    if (!p)
        return;
    out_close(p);
    decoder_close(p->dec);
    free(p);
}

static uint64_t ms_to_frames(uint64_t ms) { return ms * MUSIC_RATE / 1000; }
static uint64_t frames_to_ms(uint64_t f) { return f * 1000 / MUSIC_RATE; }

int player_play(player *p, const track *t, uint64_t pos_ms)
{
    out_close(p);
    p->t = t;
    p->err[0] = 0;
    p->state = PLAYER_PLAYING;
    uint64_t pos = ms_to_frames(pos_ms);
    if (pos >= t->frames)
        pos = 0;
    if (segment_start(p, pos) != 0) {
        p->state = PLAYER_STOPPED;
        p->hold = 0;
        return -1;
    }
    return 0;
}

void player_pause(player *p)
{
    if (p->state != PLAYER_PLAYING)
        return;
    p->hold = p->base + played_frames(p);
    out_close(p);
    p->state = PLAYER_PAUSED;
    memset(p->levels, 0, sizeof p->levels);
}

void player_resume(player *p)
{
    if (p->state != PLAYER_PAUSED || !p->t)
        return;
    p->state = PLAYER_PLAYING;
    if (segment_start(p, p->hold) != 0)
        p->state = PLAYER_STOPPED;
}

void player_stop(player *p)
{
    out_close(p);
    decoder_close(p->dec);
    p->dec = NULL;
    p->state = PLAYER_STOPPED;
    p->hold = 0;
    memset(p->levels, 0, sizeof p->levels);
}

void player_seek(player *p, uint64_t pos_ms)
{
    if (!p->t)
        return;
    uint64_t pos = ms_to_frames(pos_ms);
    if (pos > p->t->frames)
        pos = p->t->frames;
    if (p->state == PLAYER_PLAYING) {
        out_close(p);
        if (segment_start(p, pos) != 0)
            p->state = PLAYER_STOPPED;
    } else {
        p->hold = pos;
        if (p->state == PLAYER_STOPPED)
            p->state = PLAYER_PAUSED;          /* 停着的时候拖进度条 = 停在那儿，按“播放”从那儿开始 */
    }
}

void player_set_volume(player *p, int volume)
{
    p->volume = SK_CLAMP(volume, 0, 100);
    /* 人耳对音量的感觉大致是对数的：滑块移到一半，“听起来一半响”其实只要 1/4 的振幅左右，
     * 所以用平方，滑块的手感更均匀 */
    p->gain = (int32_t)(p->volume * p->volume * 65536 / 10000);
}

int player_state(const player *p) { return p->state; }
const track *player_track(const player *p) { return p->t; }
int player_output(const player *p) { return p->out; }
const char *player_output_desc(const player *p) { return p->desc; }
const char *player_error(const player *p) { return p->err; }

uint64_t player_position_ms(player *p)
{
    if (p->state != PLAYER_PLAYING)
        return frames_to_ms(p->hold);
    uint64_t f = p->base + played_frames(p);
    if (p->t && f > p->t->frames)
        f = p->t->frames;
    return frames_to_ms(f);
}

/*
 * 频谱：这一小段声音里，低音、中音、高音各有多强？
 * 用 Goertzel 算法——只算某一个频率的离散傅里叶变换（DFT），相当于“对着这个频率共振一下，看振得多厉害”。
 * 要 16 个频段就算 16 次，每次一个二阶递推，比完整的 FFT 简单，16 个频段时也更快。
 * 频段按对数间隔从 80 Hz 排到 6 kHz：人耳听音高也是对数的（每高一个八度频率翻倍）。
 */
void player_spectrum(player *p, int levels[VIZ_BANDS])
{
    /* 窗函数和每个频段的系数只和常数有关：算一次存起来（模拟器里 cosf 很贵） */
    static float hann[FFT_N], coeffs[VIZ_BANDS];
    static int tables_ready;
    if (!tables_ready) {
        for (int i = 0; i < FFT_N; i++)
            /* 汉宁窗：两头渐渐变小，减少“截断”带来的杂散频率 */
            hann[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * (float)i / (float)(FFT_N - 1));
        for (int b = 0; b < VIZ_BANDS; b++) {
            float f = 80.0f * powf(6000.0f / 80.0f, (float)b / (float)(VIZ_BANDS - 1));
            coeffs[b] = 2.0f * cosf(2.0f * 3.14159265f * f / (float)MUSIC_RATE);
        }
        tables_ready = 1;
    }
    if (p->state == PLAYER_PLAYING && p->rendered >= FFT_N) {
        uint64_t now = p->base + played_frames(p);
        /* 取“此刻正在响”的那一段；还没解码到（不太可能）或者早被覆盖了，就用最新的一段 */
        uint64_t end = now;
        if (end > p->ring_end || end + RING_FRAMES - FFT_N < p->ring_end || end < FFT_N)
            end = p->ring_end;
        float x[FFT_N];
        for (int i = 0; i < FFT_N; i++)
            x[i] = hann[i] * (float)p->ring[(end - FFT_N + (uint64_t)i) % RING_FRAMES] / 32768.0f;
        for (int b = 0; b < VIZ_BANDS; b++) {
            float coeff = coeffs[b];
            float s1 = 0.0f, s2 = 0.0f;
            for (int i = 0; i < FFT_N; i++) {
                float s = x[i] + coeff * s1 - s2;
                s2 = s1;
                s1 = s;
            }
            float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
            float mag = sqrtf(power > 0.0f ? power : 0.0f) / (float)(FFT_N / 4);
            /* 换算成分贝，-54 dB..0 dB 对应 0..1 */
            float db = 20.0f * log10f(mag + 1e-6f);
            float v = (db + 54.0f) / 54.0f;
            v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            /* 升得快、落得慢：看起来像真正的电平表 */
            p->levels[b] = v > p->levels[b] ? v : p->levels[b] - 0.12f > v ? p->levels[b] - 0.12f : v;
        }
    } else {
        for (int b = 0; b < VIZ_BANDS; b++)
            p->levels[b] = p->levels[b] > 0.12f ? p->levels[b] - 0.12f : 0.0f;
    }
    for (int b = 0; b < VIZ_BANDS; b++)
        levels[b] = (int)(p->levels[b] * 1000.0f);
}
