/*
 * track.c — 曲目和解码：内置曲子（简谱 → 实时合成）、WAV 文件（RIFF 解析 + 采样率换算）
 *
 * ┌──────────────── 声音在计算机里是什么？ ────────────────┐
 *
 *  声音是空气压力的振动。麦克风把它变成一个随时间变化的电压，声卡每秒对这个电压“拍照”
 *  很多次（采样率，比如 22050 次），每次记下一个整数（16 位：-32768..32767）。
 *  反过来，把一串整数按同样的速度交给声卡，它就能把电压“画”回来，推动喇叭振动——
 *  这就是 PCM（脉冲编码调制）。WAV 文件基本上就是“一个文件头 + 一大串 PCM 采样”。
 *
 *  所以播放器要做的事情其实很朴素：算出（或者从文件里读出）一串整数，写进 /dev/dsp。
 *
 * └────────────────────────────────────────────────────────┘
 *
 * 内置曲子用“简谱”写成，播放时实时合成，不需要任何音频文件。简谱写法（空格分隔，| 是小节线，会被忽略）：
 *   1 2 3 4 5 6 7   do re mi fa sol la si，每个一拍
 *   0               休止（不出声）一拍
 *   '  ,            写在数字后面：高八度 / 低八度，可以写多个（5,, 是低两个八度的 sol）
 *   #  b            写在数字前面：升半音 / 降半音
 *   _               写在后面：时值减半（八分音符），__ 是十六分音符
 *   .               写在最后：附点，时值 × 1.5
 *   -               单独一个：前一个音再延长一拍
 *
 * 合成方法（“波表合成”）：
 *   一个音 = 一个周期性的波形 × 一个随时间变化的音量（包络）。
 *   - 波形：基频正弦波加上几个倍频（泛音），泛音的比例决定音色——只有正弦波听起来像“哔”，
 *     加上逐渐减弱的泛音就像八音盒/钢琴。提前把一个周期算好存进 1024 格的表里（波表），
 *     播放时只要按频率在表里“跳着”取值，不用每个采样都算 sin()——模拟器里 CPU 很慢，这很重要。
 *   - 频率：十二平均律，A4 = 440 Hz，每升一个半音频率乘 2^(1/12)。
 *   - 包络：按下琴键时音量很快升到最大（起音 2 毫秒，太快会听到“咔”的一声），然后慢慢衰减；
 *     松开琴键（音符时值结束）后快速衰减到 0。
 *   - 相位累加器：32 位无符号整数，每个采样加上 频率/采样率 × 2^32，溢出自然回绕，
 *     正好对应波形的“一圈又一圈”；高 10 位就是波表的下标。
 */
#include "skmusic.h"

#include <skutil.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ================================================================ 简谱解析 */

typedef struct {
    uint32_t start, len;      /* 帧 */
    uint8_t  midi, voice;
} note_ev;

#define MAX_NOTES 2048

/* 大调音阶：1..7 离“1”有几个半音 */
static const int scale_semitones[8] = { 0, 0, 2, 4, 5, 7, 9, 11 };

/* 解析一个声部，音符追加到 ev[*n] 之后。返回 0 成功；出错返回 -1 并在 err 里说明第几个记号错了 */
static int parse_voice(const char *s, int key, int bpm, int voice, note_ev *ev, int *n, int max,
                       char *err, size_t errlen)
{
    double frames_per_beat = (double)MUSIC_RATE * 60.0 / (double)bpm;
    double t = 0.0;              /* 当前位置（拍） */
    double last_len = 0.0;
    int last = -1;               /* 上一个音符在 ev 里的下标；上一个是休止符时为 -1 */
    int tok = 0;
    while (*s) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '|') {
            s++;
            continue;
        }
        tok++;
        if (*s == '-') {         /* 延长线：上一个音（或休止）再多一拍 */
            s++;
            t += 1.0;
            if (last >= 0) {
                last_len += 1.0;
                ev[last].len = (uint32_t)(last_len * frames_per_beat + 0.5);
            }
            continue;
        }
        int acc = 0;
        while (*s == '#' || *s == 'b') {
            acc += *s == '#' ? 1 : -1;
            s++;
        }
        if (*s < '0' || *s > '7') {
            snprintf(err, errlen, "简谱第 %d 个记号写错了（“%c”不是 0~7）", tok, *s ? *s : '?');
            return -1;
        }
        int degree = *s++ - '0';
        int octave = 0;
        while (*s == '\'' || *s == ',')
            octave += *s++ == '\'' ? 1 : -1;
        double len = 1.0;
        while (*s == '_') {
            len /= 2.0;
            s++;
        }
        if (*s == '.') {
            len *= 1.5;
            s++;
        }
        if (degree == 0) {
            last = -1;
        } else {
            if (*n >= max) {
                snprintf(err, errlen, "曲子太长（超过 %d 个音）", max);
                return -1;
            }
            int midi = key + scale_semitones[degree] + acc + 12 * octave;
            note_ev *e = &ev[*n];
            /* 起点和长度都从“拍”换算成帧再四舍五入，而不是一个音一个音地累加帧数：
             * 那样每个音的舍入误差会越积越多，曲子越放越不准 */
            e->start = (uint32_t)(t * frames_per_beat + 0.5);
            e->len = (uint32_t)(len * frames_per_beat + 0.5);
            e->midi = (uint8_t)SK_CLAMP(midi, 12, 120);
            e->voice = (uint8_t)voice;
            last = (*n)++;
            last_len = len;
        }
        t += len;
    }
    return 0;
}

static int cmp_note(const void *a, const void *b)
{
    const note_ev *x = a, *y = b;
    if (x->start != y->start)
        return x->start < y->start ? -1 : 1;
    return (int)x->voice - (int)y->voice;
}

/* ================================================================ 乐器 */

#define WAVE_BITS 10
#define WAVE_SIZE (1 << WAVE_BITS)

typedef struct {
    float harmonics[6];       /* 第 1、2、3……次谐波的振幅（第 1 次 = 基频） */
    float decay_s;            /* 按住时的衰减时间常数（秒）：每过这么久音量变成 1/e */
    float release_s;          /* 松开后的衰减时间常数 */
    float gain;               /* 占满幅的比例 */
    float pan_l, pan_r;       /* 左右声道的音量：两个声部稍微分开，听起来有“立体感” */
    int16_t wave[WAVE_SIZE];
    float k_decay, k_release; /* 每个采样乘一次的衰减系数 = e^(-1 / (时间常数 × 采样率)) */
    uint32_t release_frames;  /* 松开后再响多久就当作完全没声了 */
} instrument;

static instrument instruments[2] = {
    /* 旋律：泛音丰富、衰减较快，像八音盒/钢琴 */
    { { 1.0f, 0.45f, 0.25f, 0.12f, 0.06f, 0.03f }, 0.55f, 0.05f, 0.40f, 0.80f, 1.00f, { 0 }, 0, 0, 0 },
    /* 低音：泛音少、更圆润，衰减慢一点 */
    { { 1.0f, 0.35f, 0.10f, 0.0f, 0.0f, 0.0f }, 0.90f, 0.06f, 0.32f, 1.00f, 0.75f, { 0 }, 0, 0, 0 },
};
static int instruments_ready;

static void init_instruments(void)
{
    if (instruments_ready)
        return;
    for (int v = 0; v < 2; v++) {
        instrument *ins = &instruments[v];
        float sum = 0.0f;
        for (int h = 0; h < 6; h++)
            sum += ins->harmonics[h];
        for (int i = 0; i < WAVE_SIZE; i++) {
            float x = 0.0f, ph = 2.0f * 3.14159265f * (float)i / (float)WAVE_SIZE;
            for (int h = 0; h < 6; h++)
                if (ins->harmonics[h] > 0.0f)
                    x += ins->harmonics[h] * sinf(ph * (float)(h + 1));
            ins->wave[i] = (int16_t)(x / sum * 32767.0f);
        }
        ins->k_decay = expf(-1.0f / (ins->decay_s * (float)MUSIC_RATE));
        ins->k_release = expf(-1.0f / (ins->release_s * (float)MUSIC_RATE));
        ins->release_frames = (uint32_t)(ins->release_s * 6.0f * (float)MUSIC_RATE);  /* e^-6 ≈ 0.25% */
    }
    instruments_ready = 1;
}

#define ATTACK_FRAMES (MUSIC_RATE / 500)   /* 2 毫秒 */

static uint32_t note_end(const note_ev *e)
{
    return e->start + e->len + instruments[e->voice].release_frames;
}

/* 第 t 帧（从音符开始算）时的音量，0..1 */
static float envelope_at(const instrument *ins, const note_ev *e, uint32_t t)
{
    float env = expf(-(float)t / (ins->decay_s * (float)MUSIC_RATE));
    if (t > e->len)
        env *= expf(-(float)(t - e->len) / (ins->release_s * (float)MUSIC_RATE));
    return env;
}

/* 把一个音符在 [pos, pos+n) 这一段里的声音叠加到 mix（左右交错的 32 位整数）上 */
static void render_note(const note_ev *e, uint64_t pos, int n, int32_t *mix)
{
    const instrument *ins = &instruments[e->voice];
    int64_t t0 = (int64_t)pos - (int64_t)e->start;   /* 块开头离音符开始有多少帧（可能是负数：还没开始） */
    int i0 = t0 < 0 ? (int)-t0 : 0;
    int64_t end = (int64_t)note_end(e) - (int64_t)pos;
    int i1 = end < n ? (int)end : n;
    if (i0 >= i1)
        return;
    double f = 440.0 * pow(2.0, ((double)e->midi - 69.0) / 12.0);
    uint32_t inc = (uint32_t)(f / (double)MUSIC_RATE * 4294967296.0);
    uint32_t t = (uint32_t)(t0 + i0);
    uint32_t phase = inc * t;                /* 相位 = 频率 × 时间，对 2^32 取模（无符号乘法自动回绕） */
    float env = envelope_at(ins, e, t);
    float gl = ins->gain * ins->pan_l, gr = ins->gain * ins->pan_r;
    for (int i = i0; i < i1; i++, t++) {
        float a = env;
        if (t < ATTACK_FRAMES)
            a *= (float)t / (float)ATTACK_FRAMES;
        float x = (float)ins->wave[phase >> (32 - WAVE_BITS)] * a;
        mix[2 * i] += (int32_t)(x * gl);
        mix[2 * i + 1] += (int32_t)(x * gr);
        phase += inc;
        env *= t < e->len ? ins->k_decay : ins->k_decay * ins->k_release;
    }
}

/* ================================================================ WAV 文件 */

/*
 * WAV 是微软 RIFF 格式的一种：文件由一个个“块”(chunk) 组成，每块 = 4 字节名字 + 4 字节长度 + 内容。
 *   "RIFF" <文件长度-8> "WAVE"
 *   "fmt " <16 或更多>  格式码(1=PCM) 声道数 采样率 每秒字节数 每帧字节数 每采样位数 ……
 *   "data" <长度>       PCM 采样（多声道交错存放，小端序）
 * 中间可能还夹着 "LIST"（歌名、作者）之类的块，不认识的块按长度跳过即可；块的长度是奇数时后面补一个字节。
 */
typedef struct {
    int      channels, bits, rate, align;
    uint64_t data_off, frames;     /* 采样数据的位置、源文件里一共有多少帧 */
} wav_fmt;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(int fd, void *buf, size_t n, uint64_t off)
{
    ssize_t r = pread(fd, buf, n, (off_t)off);
    return r == (ssize_t)n ? 0 : -1;
}

static int wav_probe(int fd, wav_fmt *f, char *err, size_t errlen)
{
    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(err, errlen, "读不了文件信息：%s", sk_strerror(errno));
        return -1;
    }
    uint64_t size = (uint64_t)st.st_size;
    uint8_t h[40];
    if (size < 12 || read_at(fd, h, 12, 0) != 0 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        snprintf(err, errlen, "不是 WAV 文件（开头没有 RIFF/WAVE 标记）");
        return -1;
    }
    memset(f, 0, sizeof *f);
    int have_fmt = 0;
    uint64_t off = 12;
    while (off + 8 <= size) {
        if (read_at(fd, h, 8, off) != 0)
            break;
        uint32_t len = le32(h + 4);
        uint64_t body = off + 8;
        if (memcmp(h, "fmt ", 4) == 0) {
            uint8_t b[40] = { 0 };
            size_t want = len < sizeof b ? len : sizeof b;
            if (len < 16 || read_at(fd, b, want, body) != 0) {
                snprintf(err, errlen, "WAV 文件的 fmt 块不完整");
                return -1;
            }
            int tag = le16(b);
            if (tag == 0xFFFE && len >= 26)       /* WAVE_FORMAT_EXTENSIBLE：真正的格式码在子格式 GUID 的开头 */
                tag = le16(b + 24);
            f->channels = le16(b + 2);
            f->rate = (int)le32(b + 4);
            f->align = le16(b + 12);
            f->bits = le16(b + 14);
            if (tag != 1) {
                snprintf(err, errlen, "这个 WAV 是压缩过的（格式码 %d），只支持未压缩的 PCM", tag);
                return -1;
            }
            if (f->channels < 1 || f->channels > 8 || f->rate < 1000 || f->rate > 384000 ||
                (f->bits != 8 && f->bits != 16 && f->bits != 24 && f->bits != 32) ||
                f->align != f->channels * f->bits / 8) {
                snprintf(err, errlen, "不支持的 WAV 格式：%d 声道 %d Hz %d 位", f->channels, f->rate, f->bits);
                return -1;
            }
            have_fmt = 1;
        } else if (memcmp(h, "data", 4) == 0) {
            if (!have_fmt) {
                snprintf(err, errlen, "WAV 文件的 data 块出现在 fmt 块前面");
                return -1;
            }
            uint64_t avail = size - body;
            uint64_t n = len;
            /* 录音软件中途崩溃时，长度字段常常是 0 或 0xFFFFFFFF：以文件的实际大小为准 */
            if (n == 0 || n > avail)
                n = avail;
            f->data_off = body;
            f->frames = n / (uint64_t)f->align;
            return 0;
        }
        off = body + len + (len & 1);
    }
    snprintf(err, errlen, have_fmt ? "WAV 文件里没有声音数据（data 块）" : "WAV 文件里没有格式说明（fmt 块）");
    return -1;
}

/* ================================================================ 曲目 */

static int song_notes(const song_def *s, note_ev *ev, int max, char *err, size_t errlen)
{
    int n = 0;
    if (parse_voice(s->melody, s->key, s->bpm, 0, ev, &n, max, err, errlen) != 0)
        return -1;
    if (s->bass && parse_voice(s->bass, s->key, s->bpm, 1, ev, &n, max, err, errlen) != 0)
        return -1;
    qsort(ev, (size_t)n, sizeof *ev, cmp_note);
    return n;
}

int track_init_song(track *t, const song_def *s)
{
    memset(t, 0, sizeof *t);
    t->kind = TRACK_SYNTH;
    t->song = s;
    sk_strlcpy(t->title, s->title, sizeof t->title);
    sk_strlcpy(t->source, "内置", sizeof t->source);
    snprintf(t->format, sizeof t->format, "简谱合成 · 每分钟 %d 拍", s->bpm);
    init_instruments();
    note_ev *ev = malloc(MAX_NOTES * sizeof *ev);
    char err[128];
    int n = ev ? song_notes(s, ev, MAX_NOTES, err, sizeof err) : -1;
    if (n < 0) {
        free(ev);
        return -1;
    }
    uint64_t end = 0;
    for (int i = 0; i < n; i++)
        if (note_end(&ev[i]) > end)
            end = note_end(&ev[i]);
    free(ev);
    t->frames = end + MUSIC_RATE / 4;     /* 最后留一点安静的尾巴，再切到下一首 */
    return 0;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

int track_init_wav(track *t, const char *path, char *err, size_t errlen)
{
    memset(t, 0, sizeof *t);
    t->kind = TRACK_WAV;
    sk_strlcpy(t->path, path, sizeof t->path);
    sk_strlcpy(t->title, base_name(path), sizeof t->title);
    char *dot = strrchr(t->title, '.');
    if (dot && dot != t->title)
        *dot = 0;
    sk_strlcpy(t->source, "WAV 文件", sizeof t->source);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errlen, "打不开：%s", sk_strerror(errno));
        return -1;
    }
    wav_fmt f;
    int rc = wav_probe(fd, &f, err, errlen);
    close(fd);
    if (rc != 0)
        return -1;
    t->frames = f.frames * MUSIC_RATE / (uint64_t)f.rate;
    snprintf(t->format, sizeof t->format, "%d Hz · %d 位 · %s", f.rate, f.bits,
             f.channels == 1 ? "单声道" : f.channels == 2 ? "立体声" : "多声道");
    return 0;
}

/* ================================================================ 解码器 */

#define WAV_BUF_FRAMES 2048
#define MIX_CHUNK      512

struct decoder {
    const track *t;
    uint64_t pos;               /* 下一个要输出的帧 */
    /* 内置曲子 */
    note_ev *ev;
    int      nev, first;        /* first 之前的音符都已经响完了 */
    /* WAV */
    int      fd;
    wav_fmt  fmt;
    uint64_t src_idx, src_rem;  /* 当前输出帧对应源文件里的位置 = src_idx + src_rem / MUSIC_RATE */
    int16_t *buf;               /* 源文件 [buf_start, buf_start + buf_n) 这几帧，已换成 16 位立体声 */
    uint64_t buf_start;
    int      buf_n;
    uint8_t *raw;
};

decoder *decoder_open(const track *t, uint64_t pos, char *err, size_t errlen)
{
    decoder *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(err, errlen, "内存不足");
        return NULL;
    }
    d->t = t;
    d->fd = -1;
    d->pos = pos > t->frames ? t->frames : pos;
    if (t->kind == TRACK_SYNTH) {
        init_instruments();
        d->ev = malloc(MAX_NOTES * sizeof *d->ev);
        d->nev = d->ev ? song_notes(t->song, d->ev, MAX_NOTES, err, errlen) : -1;
        if (d->nev < 0) {
            if (!d->ev)
                snprintf(err, errlen, "内存不足");
            decoder_close(d);
            return NULL;
        }
        return d;
    }
    d->fd = open(t->path, O_RDONLY | O_CLOEXEC);
    if (d->fd < 0) {
        snprintf(err, errlen, "打不开“%s”：%s", t->path, sk_strerror(errno));
        decoder_close(d);
        return NULL;
    }
    if (wav_probe(d->fd, &d->fmt, err, errlen) != 0) {
        decoder_close(d);
        return NULL;
    }
    d->buf = malloc((size_t)WAV_BUF_FRAMES * 2 * sizeof *d->buf);
    d->raw = malloc((size_t)WAV_BUF_FRAMES * (size_t)d->fmt.align);
    if (!d->buf || !d->raw) {
        snprintf(err, errlen, "内存不足");
        decoder_close(d);
        return NULL;
    }
    /* 输出的第 pos 帧 = 源文件的第 pos × 源采样率 / MUSIC_RATE 帧。整数部分和余数分开存，
     * 以后每输出一帧，余数加上源采样率、满 MUSIC_RATE 进一位——全是整数运算，放多久都不会有误差 */
    uint64_t s = d->pos * (uint64_t)d->fmt.rate;
    d->src_idx = s / MUSIC_RATE;
    d->src_rem = s % MUSIC_RATE;
    return d;
}

void decoder_close(decoder *d)
{
    if (!d)
        return;
    if (d->fd >= 0)
        close(d->fd);
    free(d->ev);
    free(d->buf);
    free(d->raw);
    free(d);
}

static int synth_read(decoder *d, int16_t *out, int frames)
{
    int total = 0;
    int32_t mix[MIX_CHUNK * 2];
    while (total < frames && d->pos < d->t->frames) {
        int n = frames - total;
        if (n > MIX_CHUNK)
            n = MIX_CHUNK;
        if ((uint64_t)n > d->t->frames - d->pos)
            n = (int)(d->t->frames - d->pos);
        memset(mix, 0, (size_t)n * 2 * sizeof *mix);
        while (d->first < d->nev && note_end(&d->ev[d->first]) <= d->pos)
            d->first++;
        for (int i = d->first; i < d->nev && d->ev[i].start < d->pos + (uint64_t)n; i++)
            if (note_end(&d->ev[i]) > d->pos)
                render_note(&d->ev[i], d->pos, n, mix);
        for (int i = 0; i < 2 * n; i++)
            out[2 * total + i] = (int16_t)SK_CLAMP(mix[i], -32768, 32767);
        total += n;
        d->pos += (uint64_t)n;
    }
    return total;
}

/* 把源文件从第 idx 帧开始的一段读进缓冲区，换算成 16 位立体声 */
static int wav_fill(decoder *d, uint64_t idx)
{
    uint64_t left = d->fmt.frames - idx;
    int n = left < WAV_BUF_FRAMES ? (int)left : WAV_BUF_FRAMES;
    ssize_t got = pread(d->fd, d->raw, (size_t)n * (size_t)d->fmt.align,
                        (off_t)(d->fmt.data_off + idx * (uint64_t)d->fmt.align));
    if (got < 0)
        return -1;
    n = (int)(got / d->fmt.align);
    int bytes = d->fmt.bits / 8;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = d->raw + (size_t)i * (size_t)d->fmt.align;
        int16_t ch[2];
        for (int c = 0; c < 2; c++) {
            /* 单声道：左右声道放同一个采样 */
            const uint8_t *q = p + (c < d->fmt.channels ? c : 0) * bytes;
            switch (d->fmt.bits) {
            case 8:  ch[c] = (int16_t)(((int)q[0] - 128) << 8); break;   /* 8 位 WAV 是无符号的，128 是“静音” */
            case 16: ch[c] = (int16_t)le16(q); break;
            default: ch[c] = (int16_t)le16(q + bytes - 2); break;       /* 24/32 位：只要最高的 16 位 */
            }
        }
        d->buf[2 * i] = ch[0];
        d->buf[2 * i + 1] = ch[1];
    }
    d->buf_start = idx;
    d->buf_n = n;
    return n;
}

static int wav_frame(decoder *d, uint64_t idx, int16_t *l, int16_t *r)
{
    if (idx >= d->fmt.frames)
        idx = d->fmt.frames - 1;
    if (idx < d->buf_start || idx >= d->buf_start + (uint64_t)d->buf_n)
        if (wav_fill(d, idx) <= 0)
            return -1;
    *l = d->buf[2 * (idx - d->buf_start)];
    *r = d->buf[2 * (idx - d->buf_start) + 1];
    return 0;
}

static int wav_read(decoder *d, int16_t *out, int frames)
{
    int total = 0;
    int rate = d->fmt.rate;
    while (total < frames && d->pos < d->t->frames && d->src_idx < d->fmt.frames) {
        int16_t al, ar, bl, br;
        if (wav_frame(d, d->src_idx, &al, &ar) != 0)
            break;
        if (d->src_rem == 0) {
            out[2 * total] = al;
            out[2 * total + 1] = ar;
        } else {
            /* 线性插值：输出时刻落在源文件两帧之间，按距离把两边的值加权平均 */
            if (wav_frame(d, d->src_idx + 1, &bl, &br) != 0)
                break;
            int w = (int)d->src_rem;
            out[2 * total] = (int16_t)(al + (int)((int64_t)(bl - al) * w / MUSIC_RATE));
            out[2 * total + 1] = (int16_t)(ar + (int)((int64_t)(br - ar) * w / MUSIC_RATE));
        }
        total++;
        d->pos++;
        d->src_rem += (uint64_t)rate;
        d->src_idx += d->src_rem / MUSIC_RATE;
        d->src_rem %= MUSIC_RATE;
    }
    return total;
}

int decoder_read(decoder *d, int16_t *out, int frames)
{
    return d->t->kind == TRACK_SYNTH ? synth_read(d, out, frames) : wav_read(d, out, frames);
}

/* ================================================================ 导出 WAV */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int track_export_wav(const track *t, const char *path, char *err, size_t errlen)
{
    decoder *d = decoder_open(t, 0, err, errlen);
    if (!d)
        return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(err, errlen, "无法创建“%s”：%s", path, sk_strerror(errno));
        decoder_close(d);
        return -1;
    }
    uint32_t data_len = (uint32_t)(t->frames * 4);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_len);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);                         /* PCM */
    put16(h + 22, MUSIC_CHANNELS);
    put32(h + 24, MUSIC_RATE);
    put32(h + 28, MUSIC_RATE * 4);            /* 每秒字节数 */
    put16(h + 32, 4);                         /* 每帧字节数 */
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_len);
    int rc = write_all(fd, h, sizeof h);
    int16_t buf[1024 * 2];
    int n;
    while (rc == 0 && (n = decoder_read(d, buf, 1024)) > 0)
        rc = write_all(fd, buf, (size_t)n * 4);   /* x86 本来就是小端序，和 WAV 的字节序一样，可以直接写 */
    if (rc != 0)
        snprintf(err, errlen, "写入“%s”失败：%s", path, sk_strerror(errno));
    if (close(fd) != 0 && rc == 0) {
        snprintf(err, errlen, "写入“%s”失败：%s", path, sk_strerror(errno));
        rc = -1;
    }
    decoder_close(d);
    if (rc != 0)
        unlink(path);
    return rc;
}
