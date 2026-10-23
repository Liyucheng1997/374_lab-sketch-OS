/*
 * skmusic.h — 音乐播放器内部接口
 *
 *   track.c   曲目：内置曲子（简谱 → 实时合成）和 WAV 文件（解析 RIFF 头、换算采样率）
 *   songs.c   内置曲子的简谱
 *   player.c  播放引擎：把解码出来的采样写进声卡 /dev/dsp，算出“现在播到哪儿了”
 *   main.c    界面
 *
 * 全程只用一种“内部格式”：MUSIC_RATE 赫兹、双声道、16 位有符号整数（交错存放：左 右 左 右……）。
 * 一个“帧”(frame) = 同一时刻左右两个声道的采样 = 4 字节。WAV 文件不管原来是什么格式，
 * 解码时都换算成这种格式，后面的代码就不用操心了。
 */
#ifndef SKMUSIC_H
#define SKMUSIC_H

#include <limits.h>
#include <stdint.h>

#include <sketch/client.h>

/* 22050 Hz：CD 音质 (44100 Hz) 的一半。能还原 11 kHz 以下的声音（奈奎斯特定理：采样率至少是
 * 最高频率的两倍），音乐听起来已经很完整；数据量只有 CD 的一半，网页模拟器里也跑得动 */
#define MUSIC_RATE      22050
#define MUSIC_CHANNELS  2

/* ================================================================ 曲目 (track.c) */

enum { TRACK_SYNTH = 0, TRACK_WAV };

/* 内置曲子：简谱写法见 track.c 开头 */
typedef struct {
    const char *title;
    const char *about;       /* 一句话介绍（显示在“正在播放”下面） */
    int         bpm;         /* 每分钟多少拍 */
    int         key;         /* 简谱“1”的音高（MIDI 音符号，60 = 中央 C） */
    const char *melody;      /* 旋律声部 */
    const char *bass;        /* 低音声部，可以为 NULL */
} song_def;

extern const song_def music_songs[];
extern const int music_nsongs;

typedef struct {
    int      kind;
    char     title[128];
    char     source[64];     /* “内置”“WAV 文件” */
    char     path[PATH_MAX]; /* WAV 文件的路径 */
    const song_def *song;    /* 内置曲子 */
    uint64_t frames;         /* 总长度（按 MUSIC_RATE 计的帧数） */
    char     format[64];     /* “22050 Hz 16 位 立体声”之类，显示用 */
} track;

/* 解码器：从任意位置开始，把一首曲子变成内部格式的采样 */
typedef struct decoder decoder;

/* 填好 t（WAV 要读文件头）。失败返回 -1，err 里是给人看的原因 */
int  track_init_song(track *t, const song_def *s);
int  track_init_wav(track *t, const char *path, char *err, size_t errlen);

/* 从第 pos 帧开始解码。失败返回 NULL（err 同上） */
decoder *decoder_open(const track *t, uint64_t pos, char *err, size_t errlen);
/* 解码 frames 帧到 out（交错的左右声道）。返回实际帧数，比 frames 少说明曲子结束了 */
int  decoder_read(decoder *d, int16_t *out, int frames);
void decoder_close(decoder *d);

/* 把整首曲子写成一个 WAV 文件（内置曲子“导出”用）。成功返回 0 */
int  track_export_wav(const track *t, const char *path, char *err, size_t errlen);

/* ================================================================ 播放引擎 (player.c) */

enum { PLAYER_STOPPED = 0, PLAYER_PLAYING, PLAYER_PAUSED };

/* 声音最后去了哪里 */
enum {
    OUT_NONE = 0,   /* 没有声卡：照样“播放”（进度条照走），只是听不见 */
    OUT_DSP,        /* 声卡 /dev/dsp */
    OUT_FILE,       /* 普通文件（自动化测试用：SKMUSIC_DSP=某个文件，检查写进去的采样） */
};

#define VIZ_BANDS 16

typedef struct player player;
/* 曲子放完了（声卡里的最后一个采样也播出去了） */
typedef void (*player_end_fn)(player *p, void *user);

player *player_new(sk_app *app, const char *dsp_path, player_end_fn on_end, void *user);
void    player_free(player *p);
/* 开始播放 t（从 pos_ms 毫秒处）。失败返回 -1，原因用 player_error 取 */
int     player_play(player *p, const track *t, uint64_t pos_ms);
void    player_pause(player *p);
void    player_resume(player *p);
void    player_stop(player *p);
void    player_seek(player *p, uint64_t pos_ms);
void    player_set_volume(player *p, int volume);   /* 0..100 */

int          player_state(const player *p);
const track *player_track(const player *p);
uint64_t     player_position_ms(player *p);          /* 听众此刻听到的位置 */
int          player_output(const player *p);         /* OUT_* */
const char  *player_output_desc(const player *p);    /* 给人看的设备说明 */
const char  *player_error(const player *p);
/* 频谱：此刻听到的声音在 VIZ_BANDS 个频段上的强度（0..1000） */
void         player_spectrum(player *p, int levels[VIZ_BANDS]);

#endif /* SKMUSIC_H */
