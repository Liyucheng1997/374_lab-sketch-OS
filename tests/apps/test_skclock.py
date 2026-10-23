#!/usr/bin/env python3
"""
tests/apps/test_skclock.py — 时钟 skclock 的自动化测试（不需要 QEMU：tools/mock_display.py 扮演 sketchd）

    bash tests/apps/run.sh skclock
    python3 tests/apps/test_skclock.py --bin <用户态 bin 目录> --out <截图目录> [--only time,damage]

程序用 SKCLOCK_DEBUG=1 启动，它会把状态打印到标准输出（格式见 userspace/gui/apps/skclock/main.c 开头）：
    skclock: layout face=16,71,368,368 digital=... date=... tab0=... tab1=... display=... start=... lap=... reset=...
    skclock: tick 2026-10-23 14:05:09 星期五
    skclock: page 1
    skclock: stopwatch running=1 elapsed=0 laps=0
SKCLOCK_FREEZE=10:08:30 让表盘停在一个固定时刻，用来比较“重画前后是否一模一样”。
"""
import datetime
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apptest import check, App, settle, shot, run_tests  # noqa: E402

WEEK = "一二三四五六日"




def start(d, args, env=None):
    e = {"SKCLOCK_DEBUG": "1"}
    e.update(env or {})
    app = App(d, [os.path.join(args.bin, "skclock")], env=e)
    win = d.wait_window(timeout=10, title="时钟")
    settle(d, win)
    return app, win


def wait_line(app, since, pattern, timeout=5.0):
    rx = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in list(app.lines[since:]):
            m = rx.search(line)
            if m:
                return m
        time.sleep(0.02)
    return None


def layout(app, since=0, timeout=5.0):
    """最近一次 layout 行 → {名字: (x, y, w, h)}"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = [l for l in list(app.lines[since:]) if l.startswith("skclock: layout ")]
        if found:
            out = {}
            for k, v in re.findall(r"(\w+)=(-?\d+,-?\d+,-?\d+,-?\d+)", found[-1]):
                out[k] = tuple(int(x) for x in v.split(","))
            return out
        time.sleep(0.02)
    return None


def center(r):
    return r[0] + r[2] // 2, r[1] + r[3] // 2


def damages_since(d, start, win):
    """[(时间, (x, y, w, h)), ...]"""
    out = []
    for req in list(d.requests[start:]):
        if req[2] != "DAMAGE":
            continue
        m = re.match(r"win=(\d+) \((-?\d+),(-?\d+) (\d+)x(\d+)\)", req[3])
        if m and int(m.group(1)) == win.id:
            out.append((req[0], tuple(int(v) for v in m.groups()[1:])))
    return out


def inside(outer, r, slack=4):
    return (r[0] >= outer[0] - slack and r[1] >= outer[1] - slack and
            r[0] + r[2] <= outer[0] + outer[2] + slack and r[1] + r[3] <= outer[1] + outer[3] + slack)


def union(a, b):
    x0, y0 = min(a[0], b[0]), min(a[1], b[1])
    x1, y1 = max(a[0] + a[2], b[0] + b[2]), max(a[1] + a[3], b[1] + b[3])
    return (x0, y0, x1 - x0, y1 - y0)


def pixels(win):
    return win.pixels()[2]


def diff_box(a, b, w):
    """两张同尺寸截图不同像素的外接矩形（没有不同返回 None）"""
    xs, ys = [], []
    for i in range(0, min(len(a), len(b)), 4):
        if a[i:i + 3] != b[i:i + 3]:
            p = i // 4
            xs.append(p % w)
            ys.append(p // w)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1, len(xs))


# ---------------------------------------------------------------- 测试

def test_render(d, args):
    app, win = start(d, args, {"SKCLOCK_FREEZE": "10:08:30"})
    check(win.title == "时钟", "窗口标题“时钟”：%s" % win.title)
    check(win.flags & 1, "窗口可以改变大小（SK_WIN_RESIZABLE）")
    lay = layout(app)
    check(lay and lay["face"][2] == lay["face"][3] and lay["face"][2] >= 300, "表盘是正方形、足够大：%s" %
          (lay and lay["face"],))
    m = wait_line(app, 0, r"^skclock: tick (\S+) (\d\d:\d\d:\d\d) 星期(\S)$")
    check(m is not None and m.group(2) == "10:08:30", "SKCLOCK_FREEZE 生效：%s" % (m and m.group(2)))
    time.sleep(0.4)
    shot(d, win, args, "skclock-face")
    # 表盘中心附近应该是指针轴（墨色），表圈上应该有蓝绿色排线
    w, h, px = win.pixels()
    fx, fy, fw, fh = lay["face"]
    cx, cy = fx + fw // 2, fy + fh // 2

    def rgb(x, y):
        o = (y * w + x) * 4
        return px[o + 2], px[o + 1], px[o]
    dark = [rgb(cx + dx, cy + dy) for dx in range(-2, 3) for dy in range(-2, 3)]
    check(any(sum(c) < 300 for c in dark), "表盘中心画了指针轴")
    teal = 0
    R = fw / 2 - 10
    for i in range(360):
        import math
        a = math.radians(i)
        rr = R * 0.925
        c = rgb(int(cx + rr * math.cos(a)), int(cy + rr * math.sin(a)))
        if c[1] > c[0] + 20 and c[2] > c[0] + 20:      # 偏蓝绿
            teal += 1
    check(teal > 25, "表圈上有蓝绿色排线（%d 个采样点）" % teal)
    # 冻结时间：每秒的心跳发现指针没动，就什么也不重画
    start_i = len(d.requests)
    time.sleep(2.3)
    check(not damages_since(d, start_i, win), "时间不变时没有任何重画")
    app.stop()


def test_time(d, args):
    app, win = start(d, args)
    m = wait_line(app, 0, r"^skclock: tick (\d+)-(\d+)-(\d+) (\d\d):(\d\d):(\d\d) 星期(\S)$")
    check(m is not None, "打印了当前时间")
    if m:
        y, mo, da, hh, mi, ss = (int(v) for v in m.groups()[:6])
        shown = datetime.datetime(y, mo, da, hh, mi, ss)
        host = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).replace(tzinfo=None)
        delta = abs((host - shown).total_seconds())
        check(delta <= 2.0, "显示的时间（%s）和宿主机的北京时间（%s）相差 %.1f 秒" % (shown, host.strftime("%H:%M:%S"),
                                                                     delta))
        check(m.group(7) == WEEK[shown.weekday()], "星期%s 正确" % m.group(7))
    # 3 秒内每秒跳一次，而且新的一行比上一行正好多 1 秒
    mark = len(app.lines)
    time.sleep(3.2)
    ticks = [l for l in app.lines[mark:] if l.startswith("skclock: tick ")]
    secs = [datetime.datetime.strptime(l.split()[2] + " " + l.split()[3], "%Y-%m-%d %H:%M:%S") for l in ticks]
    check(3 <= len(ticks) <= 4, "3.2 秒里跳了 %d 次" % len(ticks))
    check(all((b - a).total_seconds() == 1 for a, b in zip(secs, secs[1:])), "每次正好走 1 秒：%s" %
          [s.strftime("%H:%M:%S") for s in secs])
    shot(d, win, args, "skclock-now")
    app.stop()
    # 时区来自环境变量 TZ：换成 UTC0，小时数就是 UTC 时间
    app, win = start(d, args, {"TZ": "UTC0"})
    m = wait_line(app, 0, r"^skclock: tick \S+ (\d\d):(\d\d):\d\d")
    utc = datetime.datetime.now(datetime.timezone.utc)
    check(m is not None and int(m.group(1)) == utc.hour or (m and int(m.group(2)) == 59),
          "TZ=UTC0 时显示 UTC 时间：%s（UTC %02d 点）" % (m and m.group(0), utc.hour))
    app.stop()


def test_damage(d, args):
    app, win = start(d, args)
    lay = layout(app)
    face, digital, date = lay["face"], lay["digital"], lay["date"]
    settle(d, win, quiet=0.3)
    start_i = len(d.requests)
    time.sleep(4.0)
    dm = damages_since(d, start_i, win)
    # 按时间把 damage 分组：每一组是一次“心跳”刷新
    groups = []
    for t, r in dm:
        if groups and t - groups[-1][0] < 0.25:
            groups[-1][1].append(r)
        else:
            groups.append((t, [r]))
    total = win.w * win.h
    print("    4 秒内 %d 次刷新：" % len(groups))
    for t, rs in groups:
        print("      %.3f %s 面积 %d" % (t % 60, rs, sum(r[2] * r[3] for r in rs)))
    check(3 <= len(groups) <= 5, "大约每秒刷新一次（4 秒 %d 次）" % len(groups))
    allowed = union(face, digital)
    check(all(inside(allowed, r) for _, rs in groups for r in rs), "只重画表盘和数字时间的范围")
    check(all(not (r[2] >= face[2] and r[3] >= face[3]) for _, rs in groups for r in rs), "从不整块重画表盘")
    check(all(r[2] * r[3] < total * 0.6 for _, rs in groups for r in rs), "从不整窗重画")
    areas = [sum(r[2] * r[3] for r in rs) for _, rs in groups]
    check(areas and sum(areas) / len(areas) < total * 0.2, "每秒平均只重画窗口的 %.1f%%" %
          (100.0 * sum(areas) / max(1, len(areas)) / total))
    # 每次都要重画数字时间（秒数变了），并且只重画那一行字
    check(all(any(inside(digital, r, 6) for r in rs) for _, rs in groups), "每秒都刷新数字时间")
    check(all(not inside(date, r, 0) for _, rs in groups for r in rs), "日期没变就不重画")
    # 刷新发生在整秒刚过的时候（定时器对齐到秒的边界）
    phases = [t - int(t) for t, _ in groups]
    check(all(p < 0.3 for p in phases), "刷新紧跟在整秒之后：%s" % ["%.3f" % p for p in phases])
    app.stop()


def test_identical(d, args):
    """冻结时间后，各种整窗重画（切换标签页、改变大小再改回来）得到的画面必须一模一样"""
    app, win = start(d, args, {"SKCLOCK_FREEZE": "03:41:15"})
    lay = layout(app)
    time.sleep(0.3)
    d.move(win, win.w // 2, win.h - 5)
    settle(d, win)
    a = pixels(win)
    d.click(win, *center(lay["tab1"]))
    check(wait_line(app, 0, r"^skclock: page 1$") is not None, "切换到秒表页")
    settle(d, win)
    d.click(win, *center(lay["tab0"]))
    check(wait_line(app, 0, r"^skclock: page 0$", 3) is not None, "切换回时钟页")
    d.move(win, win.w // 2, win.h - 5)
    time.sleep(0.2)
    settle(d, win)
    b = pixels(win)
    db = diff_box(a, b, win.w)
    check(db is None, "切换标签页再回来：画面一模一样（不同的像素 %s）" % (db,))
    mark = len(app.lines)
    d.resize(win, 560, 700)
    settle(d, win)
    big = layout(app, mark)
    check(big and big["face"][2] > lay["face"][2], "窗口变大，表盘也变大：%s" % (big and big["face"],))
    shot(d, win, args, "skclock-large")
    d.resize(win, 400, 540)
    settle(d, win)
    c = pixels(win)
    dc = diff_box(a, c, win.w)
    check(win.w == 400 and dc is None, "改变大小再改回来：画面一模一样（不同的像素 %s）" % (dc,))
    d.resize(win, 300, 420)
    settle(d, win)
    shot(d, win, args, "skclock-small")
    app.stop()


def test_stopwatch(d, args):
    app, win = start(d, args)
    lay = layout(app)
    d.click(win, *center(lay["tab1"]))
    check(wait_line(app, 0, r"^skclock: page 1$") is not None, "点“秒表”标签")
    settle(d, win)
    shot(d, win, args, "skclock-stopwatch")
    # 秒表页看不见表盘：时钟的心跳停了，没有任何重画
    start_i = len(d.requests)
    time.sleep(1.5)
    check(not damages_since(d, start_i, win), "秒表停着时完全不重画（时钟的定时器也停了）")

    mark = len(app.lines)
    d.click(win, *center(lay["start"]))
    check(wait_line(app, mark, r"^skclock: stopwatch running=1 ") is not None, "点“开始”")
    time.sleep(0.3)
    start_i = len(d.requests)
    time.sleep(1.0)
    dm = damages_since(d, start_i, win)
    disp = lay["display"]
    print("    计时中 1 秒内重画 %d 次，例如 %s" % (len(dm), [r for _, r in dm[:3]]))
    check(6 <= len(dm) <= 12, "计时中大约每 100 毫秒刷新一次（1 秒 %d 次）" % len(dm))
    check(all(inside(disp, r, 0) and r[2] < disp[2] for _, r in dm), "只重画秒表数字那一块")
    d.click(win, *center(lay["lap"]))
    check(wait_line(app, mark, r"laps=1$") is not None, "点“计次”记录一圈")
    time.sleep(0.4)
    mark = len(app.lines)
    d.click(win, *center(lay["start"]))
    m = wait_line(app, mark, r"^skclock: stopwatch running=0 elapsed=(\d+) laps=1$")
    check(m is not None and 1500 <= int(m.group(1)) <= 3500, "点“暂停”：累计 %s 毫秒" % (m and m.group(1)))
    settle(d, win)
    shot(d, win, args, "skclock-stopwatch-laps")
    start_i = len(d.requests)
    time.sleep(1.0)
    check(not damages_since(d, start_i, win), "暂停后不再刷新（100 毫秒的定时器停了）")
    mark = len(app.lines)
    d.click(win, *center(lay["reset"]))
    check(wait_line(app, mark, r"^skclock: stopwatch running=0 elapsed=0 laps=0$") is not None, "点“复位”清零")
    # 键盘：空格 开始/暂停，回车 计次，退格 复位
    mark = len(app.lines)
    d.key(win, "space")
    check(wait_line(app, mark, r"running=1") is not None, "空格 = 开始")
    time.sleep(0.3)
    d.key(win, "enter")
    check(wait_line(app, mark, r"laps=1$") is not None, "回车 = 计次")
    # 计时中切到时钟页：秒表数字不再刷新（看不见），时钟重新开始走
    mark = len(app.lines)
    d.click(win, *center(lay["tab0"]))
    check(wait_line(app, mark, r"^skclock: tick ") is not None, "切回时钟页立刻对表")
    settle(d, win, quiet=0.3)
    start_i = len(d.requests)
    time.sleep(1.2)
    dm = damages_since(d, start_i, win)
    check(dm and not any(inside(disp, r, 0) and r[1] < lay["face"][1] for _, r in dm), "计时中的秒表在后台不刷新")
    mark = len(app.lines)
    d.click(win, *center(lay["tab1"]))
    wait_line(app, mark, r"^skclock: page 1$")
    d.key(win, "space")
    m = wait_line(app, mark, r"^skclock: stopwatch running=0 elapsed=(\d+)")
    check(m is not None and int(m.group(1)) >= 1300, "后台时也一直在计时：%s 毫秒" % (m and m.group(1)))
    d.key(win, "backspace")
    check(wait_line(app, mark, r"elapsed=0 laps=0$") is not None, "退格 = 复位")
    app.stop()


if __name__ == "__main__":
    run_tests([
        ("render", test_render),
        ("time", test_time),
        ("damage", test_damage),
        ("identical", test_identical),
        ("stopwatch", test_stopwatch),
    ])
