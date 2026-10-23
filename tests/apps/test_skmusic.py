#!/usr/bin/env python3
"""
tests/apps/test_skmusic.py — 音乐播放器 skmusic 的自动化测试（不需要 QEMU：tools/mock_display.py 扮演 sketchd）

    bash tests/apps/run.sh skmusic
    python3 tests/apps/test_skmusic.py --bin <用户态 bin 目录> --out <截图目录> [--only play,wav]

宿主机上没有 SB16 声卡，所以用 SKMUSIC_DSP=<普通文件> 让播放器把 PCM 采样“录”进文件
（22050 Hz、16 位、立体声、小端），再用 Goertzel 算法检查里面的声音频率对不对。
程序用 SKMUSIC_DEBUG=1 启动，打印的格式见 userspace/gui/apps/skmusic/main.c 开头。
"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apptest import check, App, settle, shot, run_tests, temp_home, shutil  # noqa: E402

RATE = 22050


def start(d, args, home, dsp="none", argv=(), env=None, wait_idle=True):
    """wait_idle=False：命令行打开文件时一启动就在播放，画面不会静止下来"""
    e = {"SKMUSIC_DEBUG": "1", "HOME": home, "SKMUSIC_DSP": dsp}
    e.update(env or {})
    app = App(d, [os.path.join(args.bin, "skmusic")] + list(argv), env=e)
    win = d.wait_window(timeout=10, title="音乐")
    if wait_idle:
        settle(d, win)
    return app, win


def layout(app, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = [l for l in list(app.lines) if l.startswith("skmusic: layout ")]
        if found:
            return {k: tuple(int(x) for x in v.split(","))
                    for k, v in re.findall(r"(\w+)=(-?\d+,-?\d+,-?\d+,-?\d+)", found[-1])}
        time.sleep(0.02)
    return None


def center(r):
    return r[0] + r[2] // 2, r[1] + r[3] // 2


def wait_line(app, pattern, since=0, timeout=5.0):
    rx = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in list(app.lines[since:]):
            m = rx.search(line)
            if m:
                return m
        time.sleep(0.02)
    return None


def pcm_mono(path, skip_frames=0, n=None):
    """读录下来的 PCM，返回左右平均后的单声道采样列表"""
    with open(path, "rb") as f:
        data = f.read()
    frames = len(data) // 4
    vals = struct.unpack("<%dh" % (frames * 2), data[:frames * 4])
    end = frames if n is None else min(frames, skip_frames + n)
    return [(vals[2 * i] + vals[2 * i + 1]) / 2.0 for i in range(skip_frames, end)]


def goertzel(x, freq, rate=RATE):
    k = 2.0 * math.cos(2.0 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + k * s1 - s2, s1
    return s1 * s1 + s2 * s2 - k * s1 * s2


def dominant(x, candidates):
    return max(candidates, key=lambda f: goertzel(x, f))


def write_wav(path, rate, channels, bits, samples):
    """samples: 每帧一个元组（各声道的值，按 bits 的范围）"""
    body = bytearray()
    for fr in samples:
        for v in fr:
            if bits == 8:
                body += struct.pack("<B", v)
            else:
                body += struct.pack("<h", v)
    align = channels * bits // 8
    hdr = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * align, align, bits)
    # 中间夹一个 LIST 块：播放器应该跳过不认识的块
    hdr += b"LIST" + struct.pack("<I", 5) + b"INFOx" + b"\0"
    with open(path, "wb") as f:
        f.write(hdr + b"data" + struct.pack("<I", len(body)) + body)


def sine(rate, freq, seconds, amp):
    return [int(amp * math.sin(2 * math.pi * freq * i / rate)) for i in range(int(rate * seconds))]


# ---------------------------------------------------------------- 测试

def test_start(d, args):
    home = temp_home()
    try:
        app, win = start(d, args, home)
        tracks = [l for l in app.lines if l.startswith("skmusic: track ")]
        check(len(tracks) == 6 and all(" synth " in l for l in tracks), "内置 6 首曲子：%d" % len(tracks))
        check(any("小星星" in l for l in tracks), "列表里有《小星星》")
        check(wait_line(app, r"output none 静音模式") is not None, "SKMUSIC_DSP=none 时是静音模式")
        lay = layout(app)
        check(lay is not None and all(k in lay for k in ("play", "seek", "list", "vol")), "layout 行包含各控件")
        shot(d, win, args, "skmusic-start")
        app.stop()
    finally:
        shutil.rmtree(home, ignore_errors=True)


def test_play(d, args):
    """播放 → 录下的声音是对的 → 暂停 → 继续；同时检查播放时只重画小块区域"""
    home = temp_home()
    rec = os.path.join(home, "rec.pcm")
    open(rec, "wb").close()
    try:
        app, win = start(d, args, home, dsp=rec)
        check(wait_line(app, r"output file ") is not None, "SKMUSIC_DSP=普通文件 时写入文件")
        lay = layout(app)
        n0 = len(app.lines)
        req0 = len(d.requests)
        d.click(win, *center(lay["play"]))                 # 默认选中第一首：音阶
        check(wait_line(app, r"state playing track=0", n0) is not None, "点“播放”开始播放第一首")
        check(wait_line(app, r"time 0:01/0:0\d", n0, timeout=4) is not None, "一秒后时间显示 0:01")
        time.sleep(0.6)
        shot(d, win, args, "skmusic-playing")
        m = wait_line(app, r"time 0:0[12]/\S+ peak=(\d+)", n0, timeout=3)
        check(m is not None and int(m.group(1)) > 300, "频谱有起伏（peak=%s）" % (m.group(1) if m else None))

        n1 = len(app.lines)
        d.key(win, "space")
        m = wait_line(app, r"state paused track=0 pos=(\d+)", n1)
        check(m is not None and 1500 <= int(m.group(1)) <= 3500, "空格暂停，位置约 2 秒：%s" % (m.group(1) if m else None))
        size_paused = os.path.getsize(rec)
        time.sleep(0.5)
        check(os.path.getsize(rec) == size_paused, "暂停后不再输出声音")
        # 音阶第一拍是 do（C4 = 261.6 Hz），第二拍是 re（D4 = 293.7 Hz）；每拍 0.5 秒
        cands = [261.63, 293.66, 329.63, 349.23, 392.0, 440.0]
        first = pcm_mono(rec, int(0.05 * RATE), 4096)
        second = pcm_mono(rec, int(0.55 * RATE), 4096)
        check(max(abs(v) for v in first) > 3000, "录下来的不是静音")
        check(dominant(first, cands) == 261.63, "第一拍是 do (261.6 Hz)")
        check(dominant(second, cands) == 293.66, "第二拍是 re (293.7 Hz)")
        frames = size_paused // 4
        check(abs(frames / RATE - int(m.group(1)) / 1000.0) < 0.5 if m else False,
              "写出的采样数和暂停位置一致：%.2f 秒" % (frames / RATE))

        # 播放期间的重画都是小块（频谱、进度条、时间），不会整窗重画
        big = []
        for req in list(d.requests[req0:]):
            if req[2] != "DAMAGE":
                continue
            mm = re.match(r"win=(\d+) \((-?\d+),(-?\d+) (\d+)x(\d+)\)", req[3])
            if mm and int(mm.group(1)) == win.id and int(mm.group(4)) * int(mm.group(5)) > win.w * win.h * 0.5:
                big.append(req[3])
        check(len(big) <= 1, "播放时没有反复整窗重画（大块 damage %d 次）" % len(big))

        n2 = len(app.lines)
        d.click(win, *center(lay["play"]))
        check(wait_line(app, r"state playing track=0 pos=(1[5-9]\d\d|2\d\d\d|3[0-4]\d\d)", n2) is not None,
              "继续：从暂停的位置接着放")
        time.sleep(0.5)
        check(os.path.getsize(rec) > size_paused, "继续后接着输出声音")
        d.click(win, *center(lay["stop"]))
        check(wait_line(app, r"state stopped", n2) is not None, "停止")
        app.stop()
    finally:
        shutil.rmtree(home, ignore_errors=True)


def test_end(d, args):
    """拖到快结束 → 放完自动切到下一首（默认列表循环）；单曲循环；顺序播放到最后一首停下"""
    home = temp_home()
    try:
        app, win = start(d, args, home)
        lay = layout(app)
        n0 = len(app.lines)
        d.click(win, *center(lay["play"]))
        check(wait_line(app, r"state playing track=0", n0) is not None, "开始播放")
        sx, sy, sw, sh = lay["seek"]
        n1 = len(app.lines)
        d.click(win, sx + sw - 6, sy + sh // 2)          # 进度条最右边
        check(wait_line(app, r"state playing track=0 pos=([7-9]\d\d\d)", n1) is not None, "点进度条末尾：跳到快结束的地方")
        check(wait_line(app, r"end track=0", n1, timeout=4) is not None, "放完了")
        check(wait_line(app, r"state playing track=1 pos=0", n1, timeout=2) is not None, "自动播放下一首")

        d.click(win, *center(lay["mode"]))               # 列表循环 → 单曲循环
        check(wait_line(app, r"mode 2", n1) is not None, "切换到单曲循环")
        n2 = len(app.lines)
        d.click(win, sx + sw - 6, sy + sh // 2)
        check(wait_line(app, r"end track=1", n2, timeout=5) is not None, "第二首放完了")
        check(wait_line(app, r"state playing track=1 pos=0", n2, timeout=2) is not None, "单曲循环：再放一遍同一首")

        d.click(win, *center(lay["mode"]))               # 单曲循环 → 顺序播放
        check(wait_line(app, r"mode 0", n2) is not None, "切换到顺序播放")
        for _ in range(4):                               # 跳到最后一首（6 首里的第 6 首，序号 5）
            d.key(win, "ctrl+right")
        check(wait_line(app, r"state playing track=5", n2) is not None, "Ctrl+→ 切到最后一首")
        n3 = len(app.lines)
        d.click(win, sx + sw - 6, sy + sh // 2)
        check(wait_line(app, r"end track=5", n3, timeout=5) is not None, "最后一首放完了")
        check(wait_line(app, r"state stopped track=5", n3, timeout=2) is not None, "顺序播放：放完最后一首就停下")
        app.stop()
    finally:
        shutil.rmtree(home, ignore_errors=True)


def test_wav(d, args):
    """WAV 文件：命令行打开就开始放；44100 Hz/8000 Hz 换算到 22050 Hz 后音高不变；坏文件不影响别的"""
    home = temp_home()
    music = os.path.join(home, "音乐")
    os.makedirs(music)
    rec = os.path.join(home, "rec.pcm")
    open(rec, "wb").close()
    try:
        a440 = os.path.join(home, "a440.wav")
        write_wav(a440, 44100, 1, 16, [(v,) for v in sine(44100, 440.0, 1.5, 12000)])
        # 8 位、立体声、8000 Hz：左声道 330 Hz，右声道同一个
        low = [(128 + v, 128 + v) for v in sine(8000, 330.0, 1.0, 90)]
        write_wav(os.path.join(music, "低保真.wav"), 8000, 2, 8, low)
        with open(os.path.join(music, "坏掉的.wav"), "wb") as f:
            f.write(b"this is not a wav file at all")
        app, win = start(d, args, home, dsp=rec, argv=[a440], wait_idle=False)
        check(wait_line(app, r"state playing track=7") is not None, "命令行打开的文件直接开始播放")
        tracks = [l for l in app.lines if l.startswith("skmusic: track ")]
        check(any(" wav ms=1000 低保真" in l for l in tracks), "扫描 ~/音乐：8 位 8000 Hz 的 WAV，时长 1 秒")
        check(not any("坏掉的" in l for l in tracks), "不是 WAV 的文件被跳过")
        check(any(" wav ms=1500 a440" in l for l in tracks), "命令行参数里的 WAV 加入列表")
        time.sleep(0.6)
        shot(d, win, args, "skmusic-wav")
        check(wait_line(app, r"end track=7", timeout=4) is not None, "放完")
        x = pcm_mono(rec, int(0.2 * RATE), 8192)
        check(dominant(x, [330.0, 392.0, 440.0, 494.0, 880.0]) == 440.0, "44100 → 22050 Hz 换算后还是 440 Hz")

        # 列表循环：a440 是最后一首，放完回到第一首；这里只检查切过去了，然后停下
        check(wait_line(app, r"state playing track=0", timeout=2) is not None, "最后一首放完回到第一首（列表循环）")
        lay = layout(app)
        d.click(win, *center(lay["stop"]))
        app.stop()

        # 只有这个 8 位文件：检查它的音高
        open(rec, "wb").close()
        app, win = start(d, args, home, dsp=rec, argv=[os.path.join(music, "低保真.wav")], wait_idle=False)
        check(wait_line(app, r"state playing track=6") is not None, "8 位立体声 WAV 开始播放")
        check(wait_line(app, r"end track=6", timeout=4) is not None, "放完")
        x = pcm_mono(rec, int(0.2 * RATE), 8192)
        check(dominant(x, [262.0, 294.0, 330.0, 392.0, 440.0]) == 330.0, "8000 → 22050 Hz 换算后还是 330 Hz")
        app.stop()

        # 打不开的参数：提示原因，不崩溃
        app, win = start(d, args, home, argv=[os.path.join(music, "坏掉的.wav")])
        time.sleep(0.3)
        check(app.alive(), "参数是坏文件：程序还活着")
        check(not any(l.startswith("skmusic: state playing") for l in app.lines), "坏文件不会开始播放")
        shot(d, win, args, "skmusic-bad-arg")
        app.stop()
    finally:
        shutil.rmtree(home, ignore_errors=True)


def test_export(d, args):
    """内置曲子导出成 WAV：文件头正确、长度对、导出后出现在列表里"""
    home = temp_home()
    try:
        app, win = start(d, args, home)
        lay = layout(app)
        lx, ly, lw, lh = lay["list"]
        n0 = len(app.lines)
        # 选中第二行（小星星）：表头之下第二行
        d.click(win, lx + 60, ly + 28 + 28 + 14 + 4)
        d.click(win, *center(lay["export"]))
        m = wait_line(app, r"export ok (.+)$", n0, timeout=15)
        check(m is not None, "导出成功")
        path = m.group(1) if m else ""
        check(path.endswith("/音乐/小星星.wav"), "导出到 ~/音乐/小星星.wav：%s" % path)
        if os.path.exists(path):
            with open(path, "rb") as f:
                h = f.read(44)
            ch, rate, bits = struct.unpack("<H", h[22:24])[0], struct.unpack("<I", h[24:28])[0], struct.unpack("<H", h[34:36])[0]
            check(h[:4] == b"RIFF" and h[8:16] == b"WAVEfmt " and h[36:40] == b"data", "WAV 文件头")
            check((ch, rate, bits) == (2, 22050, 16), "格式 22050 Hz 16 位立体声：%s" % ((ch, rate, bits),))
            data_len = struct.unpack("<I", h[40:44])[0]
            check(os.path.getsize(path) == 44 + data_len, "data 块长度和文件大小一致")
            track = [l for l in app.lines if re.search(r"track 1 synth ms=\d+ 小星星", l)]
            ms = int(re.search(r"ms=(\d+)", track[0]).group(1)) if track else 0
            check(abs(data_len / 4 / RATE * 1000 - ms) < 2, "导出的时长和播放器里的一样：%d ms" % ms)
        check(wait_line(app, r"track 6 wav ms=\d+ 小星星", n0) is not None, "导出的文件出现在播放列表里")
        settle(d, win)
        shot(d, win, args, "skmusic-export")
        app.stop()
    finally:
        shutil.rmtree(home, ignore_errors=True)


def test_volume(d, args):
    """音量：键盘 Ctrl+↓，音量 0 时录下来的是静音"""
    home = temp_home()
    rec = os.path.join(home, "rec.pcm")
    open(rec, "wb").close()
    try:
        app, win = start(d, args, home, dsp=rec)
        n0 = len(app.lines)
        d.key(win, "ctrl+down")
        check(wait_line(app, r"volume 75", n0) is not None, "Ctrl+↓ 音量 80 → 75")
        lay = layout(app)
        vx, vy, vw, vh = lay["vol"]
        d.click(win, vx + 1, vy + vh // 2)               # 拖到最左边：静音
        check(wait_line(app, r"volume 0", n0) is not None, "点音量条最左边：音量 0")
        d.key(win, "space")
        check(wait_line(app, r"state playing", n0) is not None, "开始播放")
        time.sleep(1.0)
        d.key(win, "space")
        check(wait_line(app, r"state paused", n0) is not None, "暂停")
        x = pcm_mono(rec)
        check(len(x) > RATE // 2 and max(abs(v) for v in x) == 0, "音量 0：写出的全是 0（%d 帧）" % len(x))
        app.stop()
    finally:
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    run_tests([("start", test_start), ("play", test_play), ("end", test_end), ("wav", test_wav),
               ("export", test_export), ("volume", test_volume)])
