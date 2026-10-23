#!/usr/bin/env python3
"""
test_ui.py — libsketch 客户端库与控件核心的自动化测试（在 WSL 里运行，不需要 QEMU）

用 tools/mock_display.py 模拟显示服务器，运行 tests/ui/ui_test_app.c 编译出的 32 位程序，
注入鼠标/键盘/改变大小/关闭事件，检查：
  - 回调是否按预期触发（程序把事件打印到 stdout）；
  - damage 是否只覆盖变化的区域（例如鼠标悬停按钮只重画这个按钮）；
  - 光标形状只在变化时发送；剪贴板、弹出菜单、焦点、改变大小、关闭；
  - 事件循环（定时器精度、fd、嵌套循环、quit）和误用场景不崩溃；
并把各种状态的窗口截图保存成 PNG，供人工检查外观。

用法：
    python3 tests/ui/test_ui.py --bin <ui_test_app 路径> --out <截图目录> [--only form,loop]
"""
import argparse
import os
import queue
import re
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mock_display as md  # noqa: E402
from mock_display import MockDisplay  # noqa: E402

BLEED = 4
CURSOR_ARROW, CURSOR_TEXT, CURSOR_HAND, CURSOR_PENCIL = 0, 1, 2, 4

results = []


def check(cond, what):
    results.append((bool(cond), what))
    print(("  PASS  " if cond else "  FAIL  ") + what, flush=True)
    return cond


class App:
    """被测程序：后台线程逐行读取 stdout"""

    def __init__(self, d, argv):
        self.p = d.spawn(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.lines = []
        self.q = queue.Queue()
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        for raw in self.p.stdout:
            line = raw.decode("utf-8", "replace").rstrip("\n")
            self.lines.append(line)
            self.q.put(line)
            print("    app> " + line, flush=True)

    def expect(self, pattern, timeout=3.0):
        """等待匹配 pattern 的新一行（之前读过的行不算）"""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.q.get(timeout=max(0.01, deadline - time.time()))
            except queue.Empty:
                break
            if rx.search(line):
                return line
        return None

    def drain(self, wait=0.15):
        time.sleep(wait)
        out = []
        while True:
            try:
                out.append(self.q.get_nowait())
            except queue.Empty:
                return out

    def wait_exit(self, timeout=5):
        try:
            return self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None


# ---------------------------------------------------------------- damage 记录工具

DAMAGE_RX = re.compile(r"win=(\d+) \((-?\d+),(-?\d+) (\d+)x(\d+)\)")


def mark(d):
    return len(d.requests)


def reqs_since(d, m, name=None):
    out = d.requests[m:]
    return [r for r in out if name is None or r[2] == name]


def damages_since(d, m):
    rects = []
    for r in reqs_since(d, m, "DAMAGE"):
        g = DAMAGE_RX.search(r[3])
        if g:
            rects.append(tuple(int(v) for v in g.groups()[1:]))
    return rects


def within(rect, box):
    x, y, w, h = rect
    bx, by, bw, bh = box
    return x >= bx and y >= by and x + w <= bx + bw and y + h <= by + bh


def inflate(r, n=BLEED):
    return (r[0] - n, r[1] - n, r[2] + 2 * n, r[3] + 2 * n)


def settle(d, win, quiet=0.25):
    """等窗口静止（没有新的请求）"""
    deadline = time.time() + 5
    last = len(d.requests)
    t = time.time()
    while time.time() < deadline:
        time.sleep(0.03)
        if len(d.requests) != last:
            last = len(d.requests)
            t = time.time()
        elif time.time() - t >= quiet:
            return


def mods_key(d, win, keycode, mods=0, cp=0):
    d.raw_key(win, keycode, 1, mods, cp)
    time.sleep(0.01)
    d.raw_key(win, keycode, 0, mods, cp)
    time.sleep(0.02)


# ---------------------------------------------------------------- 各项测试

def test_gallery(d, args):
    print("== gallery: 控件各状态截图")
    app = App(d, [args.bin, "gallery"])
    w = d.wait_window(timeout=10)
    d.wait_idle(w, quiet=0.4)
    path = d.save_window(w, os.path.join(args.out, "gallery.png"))
    check(os.path.getsize(path) > 0, "陈列室截图 " + path)
    check(not d.errors, "没有协议错误 %s" % d.errors)
    d.close(w)
    check(app.wait_exit(5) == 0, "关闭窗口后进程正常退出")


def test_form(d, args):
    print("== form: 交互、局部重绘、光标、剪贴板、菜单、焦点、改变大小、关闭")
    app = App(d, [args.bin, "form"])
    w = d.wait_window(timeout=10)
    check(app.expect("READY", 5), "程序启动")
    check(app.expect(r"LAYOUT 560 460", 2) or "LAYOUT 560 460" in app.lines, "set_layout 立即调用一次")
    d.wait_idle(w, quiet=0.4)
    d.save_window(w, os.path.join(args.out, "form-initial.png"))
    settle(d, w)

    greet = (440, 20, 100, 36)
    name_box = (90, 21, 330, 34)

    # ---- 悬停：只重画按钮本身
    d.move(w, 5, 440)       # 先放到空白处
    settle(d, w)
    m = mark(d)
    d.move(w, 470, 35)
    settle(d, w)
    dmg = damages_since(d, m)
    check(dmg and all(within(r, inflate(greet)) for r in dmg),
          "悬停按钮只 damage 按钮区域 %s" % dmg)
    check(w.cursor == CURSOR_HAND, "按钮上的光标是小手 (cursor=%d)" % w.cursor)
    d.save_window(w, os.path.join(args.out, "form-hover-button.png"))

    # ---- 在同一个按钮里移动：没有任何重绘，也不重复发 SET_CURSOR
    m = mark(d)
    for x in (480, 490, 500, 510):
        d.move(w, x, 36)
    settle(d, w)
    check(not damages_since(d, m), "按钮内部移动不触发重绘 %s" % damages_since(d, m))
    check(not reqs_since(d, m, "SET_CURSOR"), "光标没变就不发 SET_CURSOR")

    # ---- 移开：只重画按钮
    m = mark(d)
    d.move(w, 5, 440)
    settle(d, w)
    dmg = damages_since(d, m)
    check(dmg and all(within(r, inflate(greet)) for r in dmg), "移开按钮只 damage 按钮 %s" % dmg)
    check(w.cursor == CURSOR_ARROW, "空白处光标是箭头")

    # ---- 输入框：点击、输入中文和英文
    d.click(w, 200, 38)
    check(w.cursor == CURSOR_TEXT, "输入框上的光标是 I 形")
    settle(d, w)
    m = mark(d)
    d.type_text(w, "小明 abc")
    check(app.expect(r"CHANGE name=小明 abc", 3), "输入中文+英文触发 on_change")
    settle(d, w)
    dmg = damages_since(d, m)
    check(dmg and all(within(r, inflate(name_box)) for r in dmg), "打字只 damage 输入框 %s" % dmg[:6])
    d.save_window(w, os.path.join(args.out, "form-typed.png"))

    # ---- 光标闪烁：只 damage 光标那一小条
    settle(d, w, 0.1)
    m = mark(d)
    time.sleep(1.3)
    dmg = damages_since(d, m)
    check(len(dmg) >= 2 and all(r[2] <= 8 and within(r, inflate(name_box)) for r in dmg),
          "光标闪烁只重画光标 (%d 次 %s)" % (len(dmg), dmg[:3]))

    # ---- 左移 + Backspace（UTF-8 正确：删掉一个汉字）
    d.key(w, "home")
    d.key(w, "right")         # 光标在“小”之后
    d.key(w, "backspace")
    check(app.expect(r"CHANGE name=明 abc", 2), "Backspace 删除一个完整汉字")
    d.key(w, "end")
    d.key(w, "shift+left")
    d.key(w, "shift+left")
    d.key(w, "delete")
    check(app.expect(r"CHANGE name=明 a$", 2), "Shift+← 选择后 Delete 删除选区")

    # ---- 剪贴板：全选复制、粘贴
    d.key(w, "ctrl+a")
    d.key(w, "ctrl+c")
    time.sleep(0.2)
    check(d.clipboard == "明 a".encode(), "Ctrl+A Ctrl+C 复制到系统剪贴板 %r" % d.clipboard)
    d.key(w, "end")
    d.key(w, "ctrl+v")
    check(app.expect(r"CHANGE name=明 a明 a", 2), "Ctrl+V 从剪贴板粘贴")
    d.key(w, "ctrl+a")
    d.key(w, "ctrl+x")
    check(app.expect(r"CHANGE name=$", 2), "Ctrl+X 剪切")
    d.save_window(w, os.path.join(args.out, "form-empty-focused.png"))
    d.type_text(w, "Ada")
    d.key(w, "ctrl+s")
    check(app.expect(r"APPKEY 31 mods=2", 2), "输入框不处理的 Ctrl+S 交给应用回调")
    d.key(w, "enter")
    check(app.expect(r"CLICK enter text=Ada", 2), "回车触发 on_click")

    # ---- 长文字：水平滚动，保存截图
    d.type_text(w, " Lovelace 是世界上第一位程序员，她为分析机写了算法")
    settle(d, w)
    d.save_window(w, os.path.join(args.out, "form-long-text.png"))
    d.key(w, "home")
    settle(d, w)
    d.save_window(w, os.path.join(args.out, "form-long-text-home.png"))
    d.drag(w, 100, 38, 260, 38)
    settle(d, w)
    d.save_window(w, os.path.join(args.out, "form-drag-select.png"))

    # ---- 按钮点击 → 标签更新（只 damage 标签和按钮）。先把鼠标移到按钮上
    # （离开输入框本身会让输入框重画悬停状态，那是正确的，不算在这一步里）
    d.move(w, 480, 38)
    settle(d, w)
    m = mark(d)
    d.click(w, 480, 38)
    check(app.expect(r"CLICK greet text=Ada Lovelace", 2), "点击按钮触发 on_click")
    settle(d, w)
    hello = (20, 66, 400, 26)
    dmg = damages_since(d, m)
    caret = lambda r: r[2] <= 8 and within(r, inflate(name_box))      # 闪烁的光标
    check(dmg and all(within(r, inflate(greet)) or within(r, inflate(hello)) or caret(r) for r in dmg),
          "点击后只重画按钮和问候语（以及闪烁的光标）%s" % dmg)

    # ---- 按下后移出再松开：不算点击
    d.move(w, 480, 38)
    d.mouse(w, md.MOUSE_DOWN, 480, 38, md.BTN_LEFT, md.BTN_LEFT, clicks=1)
    time.sleep(0.03)
    d.move(w, 300, 300, buttons=md.BTN_LEFT)
    d.mouse(w, md.MOUSE_UP, 300, 300, 0, md.BTN_LEFT)
    check(not app.expect(r"CLICK greet", 0.4), "按下后拖出按钮再松开不触发点击")

    # ---- 复选框、滑块、标签页、色块、禁用按钮
    d.click(w, 40, 119)
    check(app.expect(r"CHANGE check=1", 2), "点击复选框切换")
    d.drag(w, 240, 119, 330, 119)
    line = app.expect(r"CHANGE slider=\d+", 2)
    check(line, "拖动滑块触发 on_change")
    app.drain()
    d.key(w, "right")
    line = app.expect(r"CHANGE slider=(\d+)", 2)
    check(line, "滑块获得焦点后方向键调节 %s" % line)
    d.click(w, 110, 163)           # 第二个标签“高级”
    check(app.expect(r"CHANGE tabs=1", 2), "点击标签页切换")
    d.click(w, 40 + 44 * 2, 216)
    check(app.expect(r"CLICK swatch=4a7bd0", 2), "点击色块")
    d.click(w, 360, 216)
    check(not app.expect(r"CLICK save", 0.4), "禁用的按钮不响应点击")
    d.click(w, 255, 216)
    check(app.expect(r"CLICK save", 2), "图标按钮点击")
    d.move(w, 380, 216)
    settle(d, w)
    d.save_window(w, os.path.join(args.out, "form-widgets.png"))

    # ---- Tab 切换焦点（焦点框）
    m = mark(d)
    for _ in range(3):
        d.key(w, "tab")
    settle(d, w)
    check(not app.expect(r"APPKEY 15", 0.2), "Tab 被控件库用来切换焦点，不交给应用")
    check(damages_since(d, m), "Tab 切换焦点会重画焦点框")
    d.save_window(w, os.path.join(args.out, "form-focus-ring.png"))
    d.key(w, "shift+tab")
    settle(d, w)

    # ---- 弹出菜单：右键 → 选第 2 项（“复制”，序号 1）
    area_pt = (100, 302)           # 区域靠上的位置：菜单向下展开放得下
    d.move(w, *area_pt)
    time.sleep(0.05)
    check(w.cursor == CURSOR_PENCIL, "自定义区域可以由应用设置光标 (cursor=%d)" % w.cursor)
    d.mouse(w, md.MOUSE_DOWN, area_pt[0], area_pt[1], md.BTN_RIGHT, md.BTN_RIGHT, clicks=1)
    time.sleep(0.03)
    d.mouse(w, md.MOUSE_UP, area_pt[0], area_pt[1], 0, md.BTN_RIGHT)
    settle(d, w)
    check(not app.expect(r"MENU", 0.3), "弹出菜单的那次松开不会误选")
    item1 = (area_pt[0] + 40, area_pt[1] + 50)    # 第 2 项（行高 28..34 时都落在这一项上）
    d.move(w, *item1)
    settle(d, w)
    d.save_window(w, os.path.join(args.out, "form-menu.png"))
    d.click(w, *item1)
    check(app.expect(r"MENU 1", 2), "点击菜单项回调 on_pick(1)")
    settle(d, w)
    # 再弹出，按 Esc 关闭
    d.mouse(w, md.MOUSE_DOWN, 200, 330, md.BTN_RIGHT, md.BTN_RIGHT, clicks=1)
    d.mouse(w, md.MOUSE_UP, 200, 330, 0, md.BTN_RIGHT)
    time.sleep(0.1)
    d.key(w, "down")
    d.key(w, "down")
    d.key(w, "esc")
    check(not app.expect(r"MENU", 0.3), "Esc 关闭菜单，不回调")
    # 再弹出，点外面关闭
    d.mouse(w, md.MOUSE_DOWN, 200, 330, md.BTN_RIGHT, md.BTN_RIGHT, clicks=1)
    d.mouse(w, md.MOUSE_UP, 200, 330, 0, md.BTN_RIGHT)
    time.sleep(0.1)
    d.click(w, 20, 440)
    check(not app.expect(r"MENU", 0.3), "点菜单外面关闭，不回调")
    # 键盘选择
    d.mouse(w, md.MOUSE_DOWN, 200, 330, md.BTN_RIGHT, md.BTN_RIGHT, clicks=1)
    d.mouse(w, md.MOUSE_UP, 200, 330, 0, md.BTN_RIGHT)
    time.sleep(0.1)
    d.key(w, "down")
    d.key(w, "down")
    d.key(w, "down")      # 跳过分隔线 → “粘贴”（序号 3）
    d.key(w, "enter")
    check(app.expect(r"MENU 3", 2), "键盘上下（跳过分隔线）+ 回车选择菜单项")
    settle(d, w)

    # ---- 窗口失去/获得焦点
    d.focus(w, False)
    check(app.expect(r"FOCUS 0", 2), "FOCUS 事件交给应用")
    d.focus(w, True)
    check(app.expect(r"FOCUS 1", 2), "重新获得焦点")

    # ---- 改变大小
    d.resize(w, 720, 540)
    check(app.expect(r"LAYOUT 720 540", 2), "改变大小时调用 layout")
    check(app.expect(r"RESIZE 720 540 canvas=720x540", 2) or any("RESIZE 720 540" in l for l in app.lines),
          "RESIZE 事件，画布已经换成新尺寸")
    d.wait_idle(w, quiet=0.3)
    d.save_window(w, os.path.join(args.out, "form-resized.png"))
    m = mark(d)
    d.move(w, 640, 35)       # 按钮跟着右边走了
    settle(d, w)
    dmg = damages_since(d, m)
    check(dmg and all(within(r, inflate((600, 20, 100, 36))) for r in dmg), "改变大小后按钮在新位置 %s" % dmg)

    # ---- 小于最小尺寸：客户端按最小尺寸分配缓冲区
    old_fd = w.fd
    d._client_send(w, md.EV_CONFIGURE, struct.pack("<Iii", w.id, 300, 200))
    deadline = time.time() + 3
    while time.time() < deadline and w.fd == old_fd:
        time.sleep(0.02)
    check((w.w, w.h) == (400, 380), "最小尺寸 400x380 生效（实际 %dx%d）" % (w.w, w.h))

    # ---- 连续多次 CONFIGURE：只处理最后一次
    m = mark(d)
    for sz in ((500, 400), (520, 410), (540, 420), (600, 450)):
        d._client_send(w, md.EV_CONFIGURE, struct.pack("<Iii", w.id, *sz))
    deadline = time.time() + 3
    while time.time() < deadline and (w.w, w.h) != (600, 450):
        time.sleep(0.02)
    settle(d, w)
    attaches = reqs_since(d, m, "ATTACH_BUFFER")
    check((w.w, w.h) == (600, 450) and 1 <= len(attaches) <= 4,
          "连续 CONFIGURE 最终尺寸正确（ATTACH %d 次）" % len(attaches))
    check(not d.errors, "没有协议错误 %s" % d.errors)

    # ---- 关闭
    d.close(w)
    check(app.expect(r"CLOSE", 2), "CLOSE 交给应用")
    rc = app.wait_exit(5)
    check(rc == 0, "关闭最后一个窗口后 sk_app_run 返回 0（退出码 %s）" % rc)


def test_loop(d, args):
    print("== loop: 定时器、fd、空闲回调、嵌套循环、quit")
    app = App(d, [args.bin, "loop"])
    rc = app.wait_exit(10)
    check(rc == 0, "loop 测试程序退出码 0（%s）" % rc)
    check(any("LOOP PASS" in l for l in app.lines), "所有事件循环检查通过")
    check(not d.errors, "没有协议错误 %s" % d.errors)


def test_misuse(d, args):
    print("== misuse: 误用不崩溃")
    app = App(d, [args.bin, "misuse"])
    check(app.expect("READY", 5), "启动（NULL 参数、窗口销毁后操作 ui 都没有崩溃）")
    w1 = d.wait_window(timeout=5, title="误用测试")
    w2 = d.wait_window(timeout=5, title="第二个窗口")
    d.wait_idle(w1, quiet=0.3)
    d.click(w1, 60, 38)
    check(app.expect("SELFDESTROY ok", 2), "按钮在自己的回调里销毁自己")
    d.click(w1, 200, 38)
    time.sleep(0.15)
    d.click(w1, 190, 56 + 6 + 14)
    check(app.expect(r"MENU 0", 2), "菜单回调里销毁打开菜单的按钮")
    d.click(w1, 200, 38)      # 按钮已经没了：什么都不发生
    time.sleep(0.1)
    d.click(w2, 80, 38)
    check(app.expect("UIFREE ok", 2), "在 sk_ui 自己的回调里 sk_ui_free + 销毁窗口")
    time.sleep(0.2)
    check(app.p.poll() is None, "程序仍在运行")
    d.close(w1)
    check(app.expect("CLOSE", 2), "CLOSE 回调里销毁自己的窗口")
    rc = app.wait_exit(5)
    check(rc == 0, "最后一个窗口销毁后正常退出（%s）" % rc)
    check(not d.errors, "没有协议错误 %s" % d.errors)


def test_disconnect(d, args):
    print("== disconnect: 服务器断开连接")
    app = App(d, [args.bin, "form"])
    w = d.wait_window(timeout=10)
    check(app.expect("READY", 5), "程序启动")
    d.wait_idle(w, quiet=0.3)
    d.click(w, 200, 38)              # 让输入框获得焦点：光标闪烁会不停地发 DAMAGE
    time.sleep(0.3)
    with d._lock:
        c = w.client
    d._drop(c)                       # 服务器这边直接关掉连接（相当于 sketchd 崩溃）
    rc = app.wait_exit(5)
    check(rc == 1, "连接断开后 sk_app_run 返回 1（退出码 %s；被 SIGPIPE 杀死会是 -13）" % rc)
    check(any("EXIT 1" in l for l in app.lines), "程序正常走到 sk_app_run 之后")
    check(any("连接已断开" in l for l in app.lines), "stderr 说明了原因")


def test_skhello(d, args):
    print("== skhello: 入门教程应用")
    if not args.skhello:
        print("  （跳过：没有给 --skhello）")
        return
    app = App(d, [args.skhello])
    w = d.wait_window(timeout=10)
    d.wait_idle(w, quiet=0.4)
    d.save_window(w, os.path.join(args.out, "skhello-initial.png"))
    check(w.title == "你好", "窗口标题「你好」（%s）" % w.title)
    d.type_text(w, "小明")             # 窗口一打开输入框就有焦点
    d.key(w, "enter")
    time.sleep(0.3)
    d.wait_idle(w, quiet=0.3)
    d.save_window(w, os.path.join(args.out, "skhello-greeted.png"))
    d.type_text(w, "x")
    d.click(w, 400, 158)              # “打招呼”按钮
    time.sleep(0.2)
    d.resize(w, 640, 400)
    d.wait_idle(w, quiet=0.3)
    d.save_window(w, os.path.join(args.out, "skhello-resized.png"))
    check(not d.errors, "没有协议错误 %s" % d.errors)
    d.close(w)
    check(app.wait_exit(5) == 0, "关闭窗口后 skhello 退出码 0")


def run_cli(d, sketch, *argv, stdin=None, env_extra=None):
    env = dict(os.environ)
    env["SKETCH_SOCKET"] = d.socket_path
    if env_extra:
        env.update(env_extra)
    p = subprocess.run([sketch] + list(argv), input=stdin, capture_output=True, env=env, timeout=10)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def test_cli(d, args):
    print("== sketch 命令行工具")
    if not args.sketch:
        print("  （跳过：没有给 --sketch）")
        return
    s = args.sketch
    rc, out, err = run_cli(d, s, "--help")
    check(rc == 0 and "用法" in out and "launch" in out, "sketch --help 显示中文帮助")
    rc, out, err = run_cli(d, s, "info")
    check(rc == 0 and "1024x768" in out, "sketch info 显示屏幕尺寸 %r" % out)
    rc, out, err = run_cli(d, s, "clip", "set", "你好", "世界")
    time.sleep(0.2)
    check(rc == 0 and d.clipboard == "你好 世界".encode(), "sketch clip set 设置剪贴板 %r" % d.clipboard)
    rc, out, err = run_cli(d, s, "clip", "get")
    check(rc == 0 and out == "你好 世界", "sketch clip get 读出剪贴板 %r" % out)
    rc, out, err = run_cli(d, s, "clip", "set", stdin="来自管道\n第二行".encode())
    time.sleep(0.2)
    check(rc == 0 and d.clipboard == "来自管道\n第二行".encode(), "从标准输入设置剪贴板 %r" % d.clipboard)
    rc, out, err = run_cli(d, s, "clip", "set", "x" * 5000)
    time.sleep(0.2)
    check(rc == 0 and len(d.clipboard) == 4096 and "截掉" in err, "超长文字截断到 4096 字节并提示")
    rc, out, err = run_cli(d, s, "launch", "notepad", "/root/a.txt")
    time.sleep(0.2)
    check(rc == 0 and ("notepad", "/root/a.txt") in d.launches, "sketch launch 发送启动请求 %s" % d.launches)
    rc, out, err = run_cli(d, s, "launch")
    check(rc == 2, "launch 缺参数时退出码 2")
    rc, out, err = run_cli(d, s, "bogus")
    check(rc == 2 and "用法" in err, "未知命令退出码 2 并显示帮助")
    rc, out, err = run_cli(d, s, "info", env_extra={"SKETCH_SOCKET": "/tmp/no-such-sketch.sock"})
    check(rc == 1 and "无法连接" in err, "连不上服务器时给出清楚的错误 %r" % err.strip()[:80])
    check(not d.errors, "没有协议错误 %s" % d.errors)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--skhello", default="")
    ap.add_argument("--sketch", default="")
    ap.add_argument("--out", default="/tmp/ui-test-shots")
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    tests = [("gallery", test_gallery), ("form", test_form), ("loop", test_loop), ("misuse", test_misuse),
             ("disconnect", test_disconnect), ("skhello", test_skhello), ("cli", test_cli)]
    only = set(filter(None, args.only.split(",")))
    for name, fn in tests:
        if only and name not in only:
            continue
        sock = "/tmp/skui-%s-%d.sock" % (name, os.getpid())
        with MockDisplay(sock, 1024, 768) as d:
            try:
                fn(d, args)
            except Exception as e:  # 测试本身出错也记为失败
                check(False, "%s 测试异常: %r" % (name, e))
    bad = [w for ok, w in results if not ok]
    print("\n%d 项检查，%d 项失败" % (len(results), len(bad)))
    for w in bad:
        print("  失败: " + w)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
