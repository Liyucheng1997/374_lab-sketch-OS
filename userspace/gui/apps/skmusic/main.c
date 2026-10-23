/*
 * skmusic — SketchOS 音乐播放器
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ ♫  小星星                                                 │ 正在播放
 *   │    法国童谣，莫扎特为它写过十二段变奏                      │
 *   │    内置 · 简谱合成 · 每分钟 100 拍                         │
 *   │  ▂ ▅ █ ▇ ▅ ▃ ▂ ▁ ……                                       │ 频谱（16 个频段）
 *   │ 0:12 ──────●──────────────────────────────────── 0:31     │ 进度（可以拖）
 *   │ [上一首] [▶ 播放] [■ 停止] [下一首] [列表循环]  音量 ──●── │
 *   │ 播放列表（7 首）                     [打开文件…] [导出 WAV]│
 *   │  ♪ 曲名                          时长     来源            │
 *   │ ……                                                        │
 *   │ Sound Blaster 16（/dev/dsp）· 22050 Hz · 16 位立体声       │ 状态栏
 *   └──────────────────────────────────────────────────────────┘
 *
 * 曲子从哪儿来：
 *   - 内置曲子：用简谱写在 songs.c 里，播放时实时合成（track.c），系统里不需要任何音频文件；
 *   - WAV 文件：~/音乐 文件夹里的 *.wav，“打开文件…”选的，或者命令行参数（文件管理器双击 .wav 就是这样打开的）。
 *   内置曲子可以“导出 WAV”存进 ~/音乐，然后在文件管理器里看到、双击播放——顺便看看 WAV 文件有多大：
 *   22050 帧/秒 × 2 声道 × 2 字节 ≈ 每秒 86 KB，一分钟 5 MB。这就是为什么后来有了 MP3 这样的压缩格式。
 *
 * 声音怎么到喇叭里去的见 player.c；内置曲子怎么“算”出声音见 track.c。
 *
 * 界面刷新：播放时每 150 毫秒醒来一次，只重画变了的东西——频谱那一块、进度条、时间文字
 * （时间一秒才变一次）。暂停/停止以后定时器停掉，进程睡在 poll() 里。
 *
 * 用法：skmusic [文件.wav | 文件夹]
 * 键盘：空格 播放/暂停　Ctrl+→ 下一首　Ctrl+← 上一首　Ctrl+↑/↓ 音量
 *
 * 调试（自动化测试 tests/apps/test_skmusic.py 用）：
 *   SKMUSIC_DEBUG=1       向标准输出打印
 *                           skmusic: layout now=x,y,w,h viz=... seek=... prev=... play=... stop=... next=...
 *                                    mode=... vol=... open=... export=... list=...       （每次布局）
 *                           skmusic: track <序号> <synth|wav> ms=<长度> <曲名>             （启动、加入曲子时）
 *                           skmusic: output <none|dsp|file> <说明>
 *                           skmusic: state <stopped|playing|paused> track=<序号> pos=<毫秒>
 *                           skmusic: time <m:ss>/<m:ss> peak=<0..1000>                      （播放中，每秒一次）
 *                           skmusic: end track=<序号>
 *                           skmusic: mode <0 顺序|1 列表循环|2 单曲循环>   skmusic: volume <0..100>
 *                           skmusic: export <ok|fail> <路径或原因>
 *   SKMUSIC_DSP=路径      声卡设备（默认 /dev/dsp）；none = 静音；普通文件 = 把采样写进这个文件
 *   SKMUSIC_DIR=文件夹    音乐文件夹（默认 $HOME/音乐）
 */
#include "skmusic.h"

#include <sketch/client.h>
#include <sketch/ui.h>
#include <sketch/theme.h>
#include <sketch/rough.h>
#include <skutil.h>

#include <dirent.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MARGIN       14
#define NOW_H        86
#define VIZ_H        70
#define ROW_H        30
#define BTN_H        36
#define BG_SEED      0x5EC0u
#define PAPER_GRAIN  3
#define UI_TICK_MS   150
#define SEEK_DELAY   200            /* 拖动进度条停下这么久才真正跳过去（拖动过程中不反复重开声卡） */

enum { MODE_ORDER = 0, MODE_REPEAT_ALL, MODE_REPEAT_ONE, NMODES };
static const char *const mode_labels[NMODES] = { "顺序播放", "列表循环", "单曲循环" };

typedef struct {
    sk_app    *app;
    sk_window *win;
    sk_ui     *ui;
    player    *pl;
    int        debug;
    char       music_dir[PATH_MAX];

    track     *tracks;
    int        ntracks, cap;
    int        cur;                 /* 当前（正在播 / 暂停 / 刚播过）的曲子，-1 = 没有 */
    int        mode;

    sk_widget *now, *viz, *seek, *t_pos, *t_len;
    sk_widget *btn_prev, *btn_play, *btn_stop, *btn_next, *btn_mode, *vol_label, *vol;
    sk_widget *list_title, *btn_open, *btn_export, *list, *status;

    int        ui_timer, seek_timer;
    int        levels[VIZ_BANDS];
    int        last_sec;            /* 上次显示的秒数（-1 = 要刷新） */
    int        last_state;
} music_app;

/* ================================================================ 小工具 */

static void dbg(music_app *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dbg(music_app *m, const char *fmt, ...)
{
    if (!m->debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("skmusic: ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    fflush(stdout);
}

static void set_status(music_app *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(music_app *m, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sk_widget_set_text(m->status, buf);
}

static void fmt_time(char *buf, size_t n, uint64_t ms)
{
    uint64_t s = ms / 1000;
    snprintf(buf, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static const char *state_name(int st)
{
    return st == PLAYER_PLAYING ? "playing" : st == PLAYER_PAUSED ? "paused" : "stopped";
}

static const char *output_name(int out)
{
    return out == OUT_DSP ? "dsp" : out == OUT_FILE ? "file" : "none";
}

static uint64_t track_ms(const track *t) { return t->frames * 1000 / MUSIC_RATE; }

/* ================================================================ 曲目列表 */

static void list_row_cells(music_app *m, int i, char *title, size_t tn, char *len, size_t ln)
{
    const track *t = &m->tracks[i];
    /* 正在播放（或暂停）的那一首前面加个音符 */
    snprintf(title, tn, "%s%s", i == m->cur && player_state(m->pl) != PLAYER_STOPPED ? "♪ " : "", t->title);
    fmt_time(len, ln, track_ms(t));
}

static void refresh_list_title(music_app *m)
{
    char buf[64];
    snprintf(buf, sizeof buf, "播放列表（%d 首）", m->ntracks);
    sk_widget_set_text(m->list_title, buf);
}

static void refresh_row(music_app *m, int i)
{
    if (i < 0 || i >= m->ntracks)
        return;
    char title[160], len[16];
    list_row_cells(m, i, title, sizeof title, len, sizeof len);
    sk_list_set_cell(m->list, i, 0, title);
}

static int add_track(music_app *m, const track *t)
{
    if (m->ntracks == m->cap) {
        int nc = m->cap ? m->cap * 2 : 16;
        track *nt = realloc(m->tracks, (size_t)nc * sizeof *nt);
        if (!nt)
            return -1;
        m->tracks = nt;
        m->cap = nc;
    }
    int i = m->ntracks++;
    m->tracks[i] = *t;
    char title[160], len[16];
    list_row_cells(m, i, title, sizeof title, len, sizeof len);
    const char *cells[3] = { title, len, t->source };
    sk_list_add(m->list, SK_ICON_MUSIC, cells, i);
    refresh_list_title(m);
    dbg(m, "track %d %s ms=%llu %s", i, t->kind == TRACK_SYNTH ? "synth" : "wav",
        (unsigned long long)track_ms(t), t->title);
    return i;
}

static int find_wav(music_app *m, const char *path)
{
    char a[PATH_MAX], b[PATH_MAX];
    const char *pa = realpath(path, a) ? a : path;
    for (int i = 0; i < m->ntracks; i++) {
        if (m->tracks[i].kind != TRACK_WAV)
            continue;
        const char *pb = realpath(m->tracks[i].path, b) ? b : m->tracks[i].path;
        if (strcmp(pa, pb) == 0)
            return i;
    }
    return -1;
}

/* 加入一个 WAV 文件（已经在列表里就直接返回它的序号）。失败返回 -1，err 是原因 */
static int add_wav(music_app *m, const char *path, char *err, size_t errlen)
{
    int i = find_wav(m, path);
    if (i >= 0)
        return i;
    track t;
    if (track_init_wav(&t, path, err, errlen) != 0)
        return -1;
    i = add_track(m, &t);
    if (i < 0)
        snprintf(err, errlen, "内存不足");
    return i;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* 扫描音乐文件夹里的 .wav（按名字排序） */
static void scan_dir(music_app *m, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;                                  /* 文件夹不存在很正常：还没导出过任何曲子 */
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len < 5 || strcasecmp(e->d_name + len - 4, ".wav") != 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **nn = realloc(names, (size_t)cap * sizeof *nn);
            if (!nn)
                break;
            names = nn;
        }
        if (!(names[n] = strdup(e->d_name)))
            break;
        n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_str);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX], err[256];
        sk_path_join(path, sizeof path, dir, names[i]);
        if (add_wav(m, path, err, sizeof err) < 0)
            dbg(m, "skip %s: %s", path, err);
        free(names[i]);
    }
    free(names);
}

/* ================================================================ 播放控制 */

static void ui_tick(sk_app *app, void *user);

static void update_buttons(music_app *m)
{
    int st = player_state(m->pl);
    sk_widget_set_text(m->btn_play, st == PLAYER_PLAYING ? "暂停" : st == PLAYER_PAUSED ? "▶ 继续" : "▶ 播放");
    sk_widget_set_enabled(m->btn_stop, st != PLAYER_STOPPED);
    sk_widget_set_enabled(m->seek, m->cur >= 0);
}

/* 播放状态变了：按钮、列表里的音符、卡片、定时器都跟着变 */
static void state_changed(music_app *m)
{
    int st = player_state(m->pl);
    update_buttons(m);
    for (int i = 0; i < m->ntracks; i++)
        refresh_row(m, i);
    sk_widget_invalidate(m->now);
    m->last_sec = -1;
    if (st == PLAYER_PLAYING && !m->ui_timer)
        m->ui_timer = sk_app_add_timer(m->app, UI_TICK_MS, 1, ui_tick, m);
    ui_tick(m->app, m);                          /* 马上刷新一次时间和进度条 */
    if (st != m->last_state || st == PLAYER_PLAYING) {
        const char *err = player_error(m->pl);
        if (err[0])
            set_status(m, "%s", err);
        else
            set_status(m, "%s", player_output_desc(m->pl));
        dbg(m, "output %s %s", output_name(player_output(m->pl)), player_output_desc(m->pl));
    }
    m->last_state = st;
    dbg(m, "state %s track=%d pos=%llu", state_name(st), m->cur,
        (unsigned long long)player_position_ms(m->pl));
}

static void play_index(music_app *m, int i, uint64_t pos_ms)
{
    if (i < 0 || i >= m->ntracks)
        return;
    int old = m->cur;
    m->cur = i;
    if (old != i)
        refresh_row(m, old);
    if (player_play(m->pl, &m->tracks[i], pos_ms) != 0) {
        state_changed(m);
        set_status(m, "无法播放“%s”：%s", m->tracks[i].title, player_error(m->pl));
        return;
    }
    sk_list_select(m->list, i);
    state_changed(m);
}

static void on_track_end(player *p, void *user)
{
    (void)p;
    music_app *m = user;
    dbg(m, "end track=%d", m->cur);
    int next = -1;
    if (m->mode == MODE_REPEAT_ONE)
        next = m->cur;
    else if (m->cur + 1 < m->ntracks)
        next = m->cur + 1;
    else if (m->mode == MODE_REPEAT_ALL)
        next = 0;
    if (next >= 0)
        play_index(m, next, 0);
    else
        state_changed(m);                        /* 顺序播放到最后一首：停下 */
}

static void on_play(sk_widget *w, void *user)
{
    (void)w;
    music_app *m = user;
    switch (player_state(m->pl)) {
    case PLAYER_PLAYING:
        player_pause(m->pl);
        break;
    case PLAYER_PAUSED:
        player_resume(m->pl);
        break;
    default: {
        /* 停着：播放列表里选中的那首（没选就从第一首开始） */
        int sel = sk_list_selected(m->list);
        play_index(m, sel >= 0 ? sel : m->cur >= 0 ? m->cur : 0, 0);
        return;
    }
    }
    state_changed(m);
}

static void on_stop(sk_widget *w, void *user)
{
    (void)w;
    music_app *m = user;
    player_stop(m->pl);
    state_changed(m);
}

static void step_track(music_app *m, int dir)
{
    if (m->ntracks == 0)
        return;
    /* “上一首”在一首歌开头 3 秒以后按，是“从头再听一遍”（大多数播放器都这样） */
    if (dir < 0 && m->cur >= 0 && player_state(m->pl) == PLAYER_PLAYING && player_position_ms(m->pl) > 3000) {
        play_index(m, m->cur, 0);
        return;
    }
    int i = m->cur < 0 ? 0 : (m->cur + dir + m->ntracks) % m->ntracks;
    play_index(m, i, 0);
}

static void on_prev(sk_widget *w, void *user) { (void)w; step_track(user, -1); }
static void on_next(sk_widget *w, void *user) { (void)w; step_track(user, 1); }

static void on_mode(sk_widget *w, void *user)
{
    music_app *m = user;
    m->mode = (m->mode + 1) % NMODES;
    sk_widget_set_text(w, mode_labels[m->mode]);
    dbg(m, "mode %d", m->mode);
}

static void show_volume(music_app *m)
{
    char buf[32];
    snprintf(buf, sizeof buf, "音量 %d", sk_slider_value(m->vol));
    sk_widget_set_text(m->vol_label, buf);
}

static void on_volume(sk_widget *w, void *user)
{
    music_app *m = user;
    player_set_volume(m->pl, sk_slider_value(w));
    show_volume(m);
    dbg(m, "volume %d", sk_slider_value(w));
}

static void change_volume(music_app *m, int delta)
{
    sk_slider_set_value(m->vol, sk_slider_value(m->vol) + delta);
    on_volume(m->vol, m);
}

/* 双击 / 回车 */
static void on_activate(sk_widget *w, void *user)
{
    music_app *m = user;
    int row = sk_list_selected(w);
    if (row >= 0)
        play_index(m, (int)sk_list_data(w, row), 0);
}

/* ---- 进度条 ---- */

static uint64_t seek_target_ms(music_app *m)
{
    if (m->cur < 0)
        return 0;
    return track_ms(&m->tracks[m->cur]) * (uint64_t)sk_slider_value(m->seek) / 1000;
}

static void do_seek(sk_app *app, void *user)
{
    (void)app;
    music_app *m = user;
    m->seek_timer = 0;
    if (m->cur < 0)
        return;
    player_seek(m->pl, seek_target_ms(m));
    state_changed(m);
}

static void on_seek(sk_widget *w, void *user)
{
    (void)w;
    music_app *m = user;
    if (m->cur < 0)
        return;
    /* 拖动时只更新时间文字；停下 SEEK_DELAY 毫秒后再真正跳过去 */
    char buf[16];
    fmt_time(buf, sizeof buf, seek_target_ms(m));
    sk_widget_set_text(m->t_pos, buf);
    if (m->seek_timer)
        sk_app_remove_timer(m->app, m->seek_timer);
    m->seek_timer = sk_app_add_timer(m->app, SEEK_DELAY, 0, do_seek, m);
}

/* ---- 打开、导出 ---- */

static void on_open(sk_widget *w, void *user)
{
    (void)w;
    music_app *m = user;
    char *path = sk_dialog_file(m->app, "打开 WAV 文件", m->music_dir, 0, NULL);
    if (!path)
        return;
    char err[256];
    int i = add_wav(m, path, err, sizeof err);
    if (i < 0) {
        char msg[PATH_MAX + 300];
        snprintf(msg, sizeof msg, "无法打开“%s”：\n%s", path, err);
        sk_dialog_message(m->app, "音乐", msg, "确定");
    } else {
        play_index(m, i, 0);
    }
    free(path);
}

static void on_export(sk_widget *w, void *user)
{
    (void)w;
    music_app *m = user;
    int row = sk_list_selected(m->list);
    int i = row >= 0 ? (int)sk_list_data(m->list, row) : m->cur;
    if (i < 0 || m->tracks[i].kind != TRACK_SYNTH) {
        sk_dialog_message(m->app, "导出 WAV", "请先在播放列表里选中一首“内置”曲子。\n"
                          "WAV 文件本来就是 WAV，不用再导出。", "确定");
        return;
    }
    const track *t = &m->tracks[i];
    char path[PATH_MAX], err[256];
    if (sk_mkdir_p(m->music_dir, 0755) != 0) {
        set_status(m, "无法创建文件夹“%s”：%s", m->music_dir, sk_strerror(errno));
        dbg(m, "export fail mkdir %s", m->music_dir);
        return;
    }
    char name[160];
    snprintf(name, sizeof name, "%s.wav", t->title);
    sk_path_join(path, sizeof path, m->music_dir, name);
    set_status(m, "正在导出“%s”……", t->title);
    sk_ui_redraw_now(m->ui);                     /* 导出要算一会儿：先让用户看见提示 */
    if (track_export_wav(t, path, err, sizeof err) != 0) {
        set_status(m, "导出失败：%s", err);
        dbg(m, "export fail %s", err);
        return;
    }
    struct stat st;
    char size[32] = "";
    if (stat(path, &st) == 0)
        sk_fmt_size(size, sizeof size, (uint64_t)st.st_size);
    int j = add_wav(m, path, err, sizeof err);
    if (j >= 0)
        sk_list_select(m->list, j);
    /* 主目录下的路径写成 ~/……，短一点 */
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && strncmp(path, home, hl) == 0 && path[hl] == '/')
        set_status(m, "已导出到 ~%s（%s）", path + hl, size);
    else
        set_status(m, "已导出到 %s（%s）", path, size);
    dbg(m, "export ok %s", path);
}

/* ================================================================ 画面 */

static void now_draw(sk_widget *w, sk_canvas *c, void *user)
{
    music_app *m = user;
    sk_rect r = w->r;
    sk_rough_style s;
    sk_style_pencil(&s, sk_hash_str("skmusic-now"));
    sk_rough_round_rect(c, (float)r.x + 1, (float)r.y + 1, (float)r.w - 2, (float)r.h - 2, 10.0f, &s);
    sk_draw_icon(c, SK_ICON_MUSIC, r.x + 14, r.y + (r.h - 56) / 2, 56, sk_hash_str("skmusic-icon"));

    int tx = r.x + 84, tw = r.w - 84 - 12;
    const char *title = "没有在播放", *about = "双击播放列表里的曲子，或者按“播放”", *fmt = "";
    char line3[256] = "";
    if (m->cur >= 0 && player_state(m->pl) != PLAYER_STOPPED) {
        const track *t = &m->tracks[m->cur];
        title = t->title;
        about = t->kind == TRACK_SYNTH ? t->song->about : t->path;
        snprintf(line3, sizeof line3, "%s · %s%s", t->source, t->format,
                 player_state(m->pl) == PLAYER_PAUSED ? " · 已暂停" : "");
        fmt = line3;
    }
    sk_draw_text_in_rect(c, sk_font_get(SK_FONT_TITLE), sk_rect_make(tx, r.y + 10, tw, 28), title, SK_INK, SK_ALIGN_LEFT);
    sk_draw_text_in_rect(c, sk_font_get(SK_FONT_UI), sk_rect_make(tx, r.y + 38, tw, 22), about, SK_INK_SOFT, SK_ALIGN_LEFT);
    sk_draw_text_in_rect(c, sk_font_get(SK_FONT_UI), sk_rect_make(tx, r.y + 58, tw, 22), fmt, SK_INK_FAINT, SK_ALIGN_LEFT);
}

/* 频谱：16 根“电平柱”，每根由一格格小方块堆起来；颜色从低音的蓝绿渐变到高音的红紫。
 * 每个小方块左右错开一两个像素（错开多少由频段和格子的序号决定，每次都一样），
 * 看起来像手画的格子，又不用每帧都画昂贵的手绘图元 */
static void viz_draw(sk_widget *w, sk_canvas *c, void *user)
{
    music_app *m = user;
    static const sk_color colors[8] = { SK_BLUE, SK_TEAL, SK_GREEN, SK_YELLOW, SK_ORANGE, SK_RED, SK_PINK, SK_PURPLE };
    sk_rect r = w->r;
    const int gap = 6, seg_h = 5, seg_gap = 2;
    int inner = r.w - 16;
    int bw = (inner - gap * (VIZ_BANDS - 1)) / VIZ_BANDS;
    if (bw < 2)
        return;
    int x0 = r.x + 8 + (inner - (bw * VIZ_BANDS + gap * (VIZ_BANDS - 1))) / 2;
    int bottom = r.y + r.h - 8;
    int nseg = (r.h - 12) / (seg_h + seg_gap);
    for (int b = 0; b < VIZ_BANDS; b++) {
        int lit = (m->levels[b] * nseg + 500) / 1000;
        sk_color col = colors[b * 8 / VIZ_BANDS];
        int x = x0 + b * (bw + gap);
        for (int sgi = 0; sgi < nseg; sgi++) {
            uint32_t h = sk_hash_u32((uint32_t)b, (uint32_t)sgi);
            int jx = (int)(h % 3) - 1, jw = (int)((h >> 3) % 2);
            sk_rect cell = sk_rect_make(x + jx, bottom - (sgi + 1) * (seg_h + seg_gap) + seg_gap, bw - jw, seg_h);
            if (sgi < lit)
                sk_fill_rect(c, cell, SK_WITH_ALPHA(col, 0xC8));
            else
                sk_fill_rect(c, cell, SK_WITH_ALPHA(SK_SHADOW, 0x28));
        }
    }
    sk_draw_separator(c, r.x + 4, r.x + r.w - 4, bottom + 2, sk_hash_str("skmusic-viz"));
}

static void ui_tick(sk_app *app, void *user)
{
    (void)app;
    music_app *m = user;
    int st = player_state(m->pl);

    /* 频谱：变了才重画那一块 */
    int lv[VIZ_BANDS], changed = 0, peak = 0, any = 0;
    player_spectrum(m->pl, lv);
    for (int b = 0; b < VIZ_BANDS; b++) {
        changed |= lv[b] != m->levels[b];
        any |= lv[b] != 0;
        if (lv[b] > peak)
            peak = lv[b];
        m->levels[b] = lv[b];
    }
    if (changed)
        sk_widget_invalidate(m->viz);

    /* 时间和进度条：拖动进度条的时候不去和用户抢 */
    uint64_t pos = player_position_ms(m->pl);
    uint64_t len = m->cur >= 0 ? track_ms(&m->tracks[m->cur]) : 0;
    int sec = (int)(pos / 1000);
    int dragging = (m->seek->state & SK_STATE_PRESSED) || m->seek_timer;
    if (!dragging) {
        if (sec != m->last_sec) {
            char a[16], b[16];
            fmt_time(a, sizeof a, pos);
            fmt_time(b, sizeof b, len);
            sk_widget_set_text(m->t_pos, a);
            sk_widget_set_text(m->t_len, b);
            if (st == PLAYER_PLAYING)
                dbg(m, "time %s/%s peak=%d", a, b, peak);
            m->last_sec = sec;
        }
        sk_slider_set_value(m->seek, len ? (int)(pos * 1000 / len) : 0);
    }

    /* 不在播放、频谱也落到底了：定时器停下来，进程去睡觉 */
    if (st != PLAYER_PLAYING && !any && m->ui_timer) {
        sk_app_remove_timer(m->app, m->ui_timer);
        m->ui_timer = 0;
    }
}

/* ================================================================ 布局、事件 */

static void dbg_rect(char *buf, size_t n, const char *name, sk_rect r)
{
    size_t len = strlen(buf);
    snprintf(buf + len, n - len, " %s=%d,%d,%d,%d", name, r.x, r.y, r.w, r.h);
}

static void layout(sk_ui *ui, int W, int H, void *user)
{
    (void)ui;
    music_app *m = user;
    int y = 10;
    sk_widget_set_rect(m->now, sk_rect_make(MARGIN, y, W - 2 * MARGIN, NOW_H));
    y += NOW_H + 4;
    sk_widget_set_rect(m->viz, sk_rect_make(MARGIN, y, W - 2 * MARGIN, VIZ_H));
    y += VIZ_H + 6;

    sk_widget_set_rect(m->t_pos, sk_rect_make(MARGIN, y, 52, ROW_H));
    sk_widget_set_rect(m->seek, sk_rect_make(MARGIN + 56, y, W - 2 * MARGIN - 112, ROW_H));
    sk_widget_set_rect(m->t_len, sk_rect_make(W - MARGIN - 52, y, 52, ROW_H));
    y += ROW_H + 8;

    int x = MARGIN;
    struct { sk_widget *w; int width; } row[] = {
        { m->btn_prev, 76 }, { m->btn_play, 92 }, { m->btn_stop, 80 }, { m->btn_next, 76 }, { m->btn_mode, 100 },
    };
    for (size_t i = 0; i < SK_ARRAY_LEN(row); i++) {
        sk_widget_set_rect(row[i].w, sk_rect_make(x, y, row[i].width, BTN_H));
        x += row[i].width + 8;
    }
    x += 4;
    sk_widget_set_rect(m->vol_label, sk_rect_make(x, y, 66, BTN_H));
    x += 68;
    int vw = W - MARGIN - x;
    sk_widget_set_rect(m->vol, sk_rect_make(x, y, vw > 50 ? vw : 50, BTN_H));
    y += BTN_H + 12;

    sk_widget_set_rect(m->btn_export, sk_rect_make(W - MARGIN - 116, y, 116, 34));
    sk_widget_set_rect(m->btn_open, sk_rect_make(W - MARGIN - 116 - 8 - 116, y, 116, 34));
    sk_widget_set_rect(m->list_title, sk_rect_make(MARGIN, y, W - 2 * MARGIN - 248, 34));
    y += 34 + 6;
    int lh = H - y - 34;
    sk_widget_set_rect(m->list, sk_rect_make(MARGIN, y, W - 2 * MARGIN, lh > 60 ? lh : 60));
    sk_widget_set_rect(m->status, sk_rect_make(MARGIN, H - 30, W - 2 * MARGIN, 24));

    if (m->debug) {
        char buf[768] = "layout";
        dbg_rect(buf, sizeof buf, "now", m->now->r);
        dbg_rect(buf, sizeof buf, "viz", m->viz->r);
        dbg_rect(buf, sizeof buf, "seek", m->seek->r);
        dbg_rect(buf, sizeof buf, "prev", m->btn_prev->r);
        dbg_rect(buf, sizeof buf, "play", m->btn_play->r);
        dbg_rect(buf, sizeof buf, "stop", m->btn_stop->r);
        dbg_rect(buf, sizeof buf, "next", m->btn_next->r);
        dbg_rect(buf, sizeof buf, "mode", m->btn_mode->r);
        dbg_rect(buf, sizeof buf, "vol", m->vol->r);
        dbg_rect(buf, sizeof buf, "open", m->btn_open->r);
        dbg_rect(buf, sizeof buf, "export", m->btn_export->r);
        dbg_rect(buf, sizeof buf, "list", m->list->r);
        dbg(m, "%s", buf);
    }
}

static void background(sk_ui *ui, sk_canvas *c, sk_rect dirty, void *user)
{
    (void)ui;
    (void)user;
    sk_paper_fill(c, dirty, SK_PAPER, PAPER_GRAIN, BG_SEED);
}

/* 快捷键。这里只收到控件没处理的按键：播放列表有焦点时会把字母当作“按首字母跳转”吃掉，
 * 所以除了空格，其它快捷键都带 Ctrl（控件不处理带 Ctrl 的方向键） */
static int on_event(sk_window *win, const sk_event *e, void *user)
{
    (void)win;
    music_app *m = user;
    if (e->type != SK_EVENT_KEY || e->key.state == 0 || (e->key.modifiers & (SK_MOD_ALT | SK_MOD_SUPER)))
        return 0;
    uint32_t kc = e->key.keycode, cp = e->key.codepoint;
    int ctrl = (e->key.modifiers & SK_MOD_CTRL) != 0;
    int first = e->key.state == 1;               /* 2 = 按住不放的自动重复：切歌不重复，音量可以连续调 */
    if (!ctrl && (kc == KEY_SPACE || cp == ' ')) {
        if (first)
            on_play(m->btn_play, m);
        return 1;
    }
    if (!ctrl)
        return 0;
    switch (kc) {
    case KEY_RIGHT: if (first) step_track(m, 1); return 1;
    case KEY_LEFT:  if (first) step_track(m, -1); return 1;
    case KEY_UP:    change_volume(m, 5); return 1;
    case KEY_DOWN:  change_volume(m, -5); return 1;
    }
    return 0;
}

/* ================================================================ main */

int main(int argc, char **argv)
{
    music_app m;
    memset(&m, 0, sizeof m);
    m.cur = -1;
    m.last_sec = -1;
    m.mode = MODE_REPEAT_ALL;
    if (argc > 2) {
        fprintf(stderr, "用法: skmusic [文件.wav | 文件夹]\n");
        return 2;
    }
    const char *dv = getenv("SKMUSIC_DEBUG");
    m.debug = dv && *dv && strcmp(dv, "0") != 0;
    const char *dir = getenv("SKMUSIC_DIR");
    const char *home = getenv("HOME");
    if (dir && *dir)
        sk_strlcpy(m.music_dir, dir, sizeof m.music_dir);
    else
        sk_path_join(m.music_dir, sizeof m.music_dir, home && *home ? home : "/root", "音乐");

    const char *arg = argc > 1 ? argv[1] : NULL;
    struct stat ast;
    if (arg && stat(arg, &ast) == 0 && S_ISDIR(ast.st_mode)) {
        sk_strlcpy(m.music_dir, arg, sizeof m.music_dir);   /* 参数是文件夹：把它当作音乐文件夹 */
        arg = NULL;
    }

    m.app = sk_app_connect("skmusic");
    if (!m.app)
        return 1;
    m.win = sk_window_create(m.app, "音乐", 680, 580, SK_WIN_RESIZABLE, SK_PINK);
    if (!m.win) {
        sk_app_disconnect(m.app);
        return 1;
    }
    sk_window_set_min_size(m.win, 640, 470);
    m.ui = sk_ui_new(m.win);
    m.pl = player_new(m.app, getenv("SKMUSIC_DSP"), on_track_end, &m);
    if (!m.ui || !m.pl) {
        fprintf(stderr, "skmusic: 内存不足\n");
        return 1;
    }

    sk_rect z = sk_rect_make(0, 0, 10, 10);
    m.now = sk_custom_new(m.ui, z, now_draw, NULL, 0, &m);
    m.viz = sk_custom_new(m.ui, z, viz_draw, NULL, 0, &m);
    m.t_pos = sk_label_new(m.ui, z, "0:00", SK_FONT_UI, SK_ALIGN_LEFT);
    m.seek = sk_slider_new(m.ui, z, 0, 1000, 0, on_seek, &m);
    m.t_len = sk_label_new(m.ui, z, "0:00", SK_FONT_UI, SK_ALIGN_RIGHT);
    m.btn_prev = sk_button_new(m.ui, z, "上一首", on_prev, &m);
    m.btn_play = sk_button_new(m.ui, z, "▶ 播放", on_play, &m);
    m.btn_stop = sk_button_new(m.ui, z, "■ 停止", on_stop, &m);
    m.btn_next = sk_button_new(m.ui, z, "下一首", on_next, &m);
    m.btn_mode = sk_button_new(m.ui, z, mode_labels[m.mode], on_mode, &m);
    m.vol_label = sk_label_new(m.ui, z, "", SK_FONT_UI, SK_ALIGN_RIGHT);
    m.vol = sk_slider_new(m.ui, z, 0, 100, 80, on_volume, &m);
    m.list_title = sk_label_new(m.ui, z, "", SK_FONT_TITLE, SK_ALIGN_LEFT);
    m.btn_open = sk_button_new(m.ui, z, "打开文件…", on_open, &m);
    m.btn_export = sk_button_new(m.ui, z, "导出 WAV", on_export, &m);
    m.list = sk_list_new(m.ui, z, NULL, on_activate, &m);
    m.status = sk_label_new(m.ui, z, "", SK_FONT_UI, SK_ALIGN_LEFT);
    if (!m.now || !m.viz || !m.t_pos || !m.seek || !m.t_len || !m.btn_prev || !m.btn_play || !m.btn_stop ||
        !m.btn_next || !m.btn_mode || !m.vol_label || !m.vol || !m.list_title || !m.btn_open || !m.btn_export ||
        !m.list || !m.status) {
        fprintf(stderr, "skmusic: 内存不足\n");
        return 1;
    }
    static const char *const cols[3] = { "曲名", "时长", "来源" };
    static const int widths[3] = { 0, -64, 110 };
    sk_list_set_columns(m.list, cols, widths, 3);
    sk_list_set_row_height(m.list, 28);
    sk_label_set_color(m.status, SK_INK_SOFT);
    sk_label_set_color(m.t_pos, SK_INK_SOFT);
    sk_label_set_color(m.t_len, SK_INK_SOFT);
    sk_widget_set_accent(m.seek, SK_PINK);
    sk_widget_set_accent(m.vol, SK_PINK);
    show_volume(&m);

    for (int i = 0; i < music_nsongs; i++) {
        track t;
        if (track_init_song(&t, &music_songs[i]) == 0)
            add_track(&m, &t);
        else
            fprintf(stderr, "skmusic: 内置曲子“%s”的简谱写错了\n", music_songs[i].title);
    }
    scan_dir(&m, m.music_dir);
    int start = -1;
    if (arg) {
        char err[256];
        start = add_wav(&m, arg, err, sizeof err);
        if (start < 0)
            set_status(&m, "无法打开“%s”：%s", arg, err);
    }
    if (m.ntracks > 0)
        sk_list_select(m.list, start >= 0 ? start : 0);

    sk_ui_set_background(m.ui, background, &m);
    sk_ui_set_layout(m.ui, layout, &m);
    sk_ui_set_handler(m.ui, on_event, &m);
    update_buttons(&m);
    m.last_state = -1;
    if (start >= 0) {
        play_index(&m, start, 0);                /* 从文件管理器双击打开的：直接开始放 */
    } else {
        if (!m.status->text || !m.status->text[0])
            set_status(&m, "%s", player_output_desc(m.pl));
        dbg(&m, "output %s %s", output_name(player_output(m.pl)), player_output_desc(m.pl));
        m.last_state = PLAYER_STOPPED;
    }
    sk_ui_focus(m.ui, m.list);
    sk_ui_invalidate_all(m.ui);

    int rc = sk_app_run(m.app);
    player_free(m.pl);
    sk_ui_free(m.ui);
    sk_app_disconnect(m.app);
    free(m.tracks);
    return rc;
}
