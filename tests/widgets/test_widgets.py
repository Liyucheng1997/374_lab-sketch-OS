#!/usr/bin/env python3
"""
test_widgets.py — 复杂控件（多行编辑器、列表、对话框）和控件展示程序的自动化测试

在 WSL 里运行（不需要 QEMU）：SketchOS 的图形程序是 32 位静态程序，WSL 可以直接运行；
tools/mock_display.py 扮演显示服务器，接收窗口缓冲区、注入鼠标键盘事件、把窗口存成 PNG。

    python3 tests/widgets/test_widgets.py --bin <用户态 bin 目录> --twidgets <twidgets 程序> --out <截图目录>
    （通常由 tests/widgets/run.sh 调用）

被测程序：
    twidgets   tests/widgets/twidgets.c：编辑器 A/B、输入框 C、列表 L；按 F5 打印内部状态（JSON）
    skwidgets  控件展示程序；--demo 直接打开某种对话框并打印结果
每个测试都会保存截图，测试结束后请打开看看（视觉效果也是测试的一部分）。
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import threading
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from mock_display import MockDisplay  # noqa: E402

ARGS = None
SOCK = "/tmp/sketch-widget-test-%d.sock" % os.getpid()


# ---------------------------------------------------------------- 字体度量（算点击位置用）
class Font:
    """读 .skf 文件头和字形表，得到行高和每个字的前进宽度（与 libsketch 完全一致）"""

    def __init__(self, path):
        data = open(path, "rb").read()
        magic, self.size, self.ascent, self.descent, self.line_height, count, idx, _bmp = \
            struct.unpack_from("<4sHhhHIII", data, 0)
        assert magic == b"SKF1", path
        self.adv = {}
        for i in range(count):
            cp, _b, _w, _h, _l, _t, adv = struct.unpack_from("<IIBBbbB", data, idx + 16 * i)
            self.adv[cp] = adv

    def width(self, s):
        return sum(self.adv.get(ord(ch), self.adv.get(0xFFFD, 8)) for ch in s)


FONT = None


def row_h():
    return max(FONT.line_height, FONT.ascent + FONT.descent) + 6


# 编辑器 A 的几何（与 ui_textarea.c 的布局常量一致：外框 3 + 红线 12 + 留白 7；上边 3 + 3）
A_X, A_Y = 10, 10
TEXT_X, TEXT_Y = A_X + 3 + 12 + 7, A_Y + 3 + 3


def a_point(col_text, row, dx=0):
    """编辑器 A 第 row 个显示行、在 col_text 这段文字之后的屏幕位置"""
    return TEXT_X + FONT.width(col_text) + dx, TEXT_Y + row * row_h() + row_h() // 2


# ---------------------------------------------------------------- 程序包装
class App:
    """启动一个被测程序，后台线程收集它的标准输出（每行一个 JSON）"""

    def __init__(self, d, argv):
        self.d = d
        self.p = d.spawn(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.lines = []
        self.pos = 0
        self.err = []
        self.cv = threading.Condition()
        threading.Thread(target=self._read, args=(self.p.stdout, self.lines), daemon=True).start()
        threading.Thread(target=self._read, args=(self.p.stderr, self.err), daemon=True).start()

    def _read(self, f, out):
        for line in f:
            with self.cv:
                out.append(line.rstrip("\n"))
                self.cv.notify_all()

    def wait(self, pred, timeout=5.0, what="输出"):
        """等待一行满足 pred 的 JSON（之前的行被跳过）"""
        deadline = time.time() + timeout
        with self.cv:
            while True:
                while self.pos < len(self.lines):
                    line = self.lines[self.pos]
                    self.pos += 1
                    try:
                        obj = json.loads(line)
                    except ValueError:
                        obj = line
                    if pred(obj):
                        return obj
                left = deadline - time.time()
                if left <= 0:
                    raise AssertionError("等待%s超时；最近的输出: %s；stderr: %s" %
                                         (what, self.lines[-5:], self.err[-5:]))
                self.cv.wait(left)

    def wait_text(self, prefix, timeout=8.0):
        deadline = time.time() + timeout
        with self.cv:
            while True:
                for i in range(self.pos, len(self.lines)):
                    if self.lines[i].startswith(prefix):
                        self.pos = i + 1
                        return self.lines[i][len(prefix):].strip()
                left = deadline - time.time()
                if left <= 0:
                    raise AssertionError("等待 %r 超时；输出: %s；stderr: %s" % (prefix, self.lines[-5:], self.err[-5:]))
                self.cv.wait(left)

    def finish(self, timeout=5):
        try:
            self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
        return self.p.returncode


def state(app, w):
    app.d.key(w, "f5")
    return app.wait(lambda o: isinstance(o, dict) and "a" in o, what="状态")


def mark(app, w):
    app.d.key(w, "f12")
    return app.wait(lambda o: isinstance(o, dict) and "mark" in o, what="同步标记")


def damages_since(d, start, win=None):
    """从 d.requests[start:] 里取出 DAMAGE 矩形"""
    out = []
    for req in list(d.requests[start:]):
        if req[2] != "DAMAGE":
            continue
        m = re.match(r"win=(\d+) \((-?\d+),(-?\d+) (\d+)x(\d+)\)", req[3])
        if m and (win is None or int(m.group(1)) == win.id):
            out.append(tuple(int(v) for v in m.groups()[1:]))
    return out


def shot(d, w, name):
    path = os.path.join(ARGS.out, name + ".png")
    d.wait_idle(w, quiet=0.25)
    d.save_window(w, path)
    return path


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def start_twidgets(d, initial=""):
    argv = [ARGS.twidgets] + ([initial] if initial else [])
    app = App(d, argv)
    app.wait(lambda o: isinstance(o, dict) and o.get("ready"), what="程序启动")
    w = d.wait_window(10, title="控件测试")
    d.wait_idle(w, quiet=0.3)
    return app, w


# ================================================================ 编辑器测试

def test_typing_cjk(d):
    app, w = start_twidgets(d)
    d.type_text(w, "Hello 世界，SketchOS！\n第二行")
    s = state(app, w)
    check(s["a"] == "Hello 世界，SketchOS！\n第二行", "输入结果不对: %r" % s["a"])
    check(s["a_pos"] == [2, 4], "光标位置应为第 2 行第 4 列: %s" % s["a_pos"])
    check(s["a_mod"] == 1, "应标记为已修改")
    check(s["changes"] > 0, "on_change 应被调用")
    shot(d, w, "editor_typing")
    d.close(w)
    app.finish()


def test_editing_keys(d):
    app, w = start_twidgets(d)
    d.type_text(w, "abc")
    d.key(w, "enter")
    d.type_text(w, "def")
    d.key(w, "home")                  # 第 2 行行首
    d.key(w, "backspace")             # 与上一行合并
    s = state(app, w)
    check(s["a"] == "abcdef", "跨行退格: %r" % s["a"])
    d.key(w, "left")
    d.key(w, "enter")                 # "ab|c" -> 拆行
    d.key(w, "up")
    d.key(w, "end")
    d.key(w, "delete")                # 行尾 Delete 合并下一行
    s = state(app, w)
    check(s["a"] == "abcdef", "跨行 Delete: %r" % s["a"])
    d.key(w, "ctrl+home")
    d.key(w, "tab")
    s = state(app, w)
    check(s["a"] == "    abcdef", "Tab 应插入 4 个空格: %r" % s["a"])
    check(s["a_pos"] == [1, 5], "Tab 后光标: %s" % s["a_pos"])
    d.key(w, "home")
    d.key(w, "ctrl+right")            # 从行首跳过 4 个空格
    d.key(w, "shift+end")
    d.type_text(w, "XYZ")
    s = state(app, w)
    check(s["a"] == "    XYZ", "Shift+End 选中后输入替换: %r" % s["a"])
    d.close(w)
    app.finish()


def test_undo_redo(d):
    app, w = start_twidgets(d)
    d.type_text(w, "hello world")
    d.key(w, "ctrl+z")
    check(state(app, w)["a"] == "hello ", "第一次撤销应去掉一个词")
    d.key(w, "ctrl+z")
    s = state(app, w)
    check(s["a"] == "" and s["a_mod"] == 0, "撤销到底应为空且未修改: %r" % s)
    d.key(w, "ctrl+y")
    d.key(w, "ctrl+y")
    check(state(app, w)["a"] == "hello world", "重做两次")
    d.key(w, "ctrl+a")
    d.type_text(w, "新")
    check(state(app, w)["a"] == "新", "全选后输入替换")
    d.key(w, "ctrl+z")
    check(state(app, w)["a"] == "hello world", "撤销替换")
    # 连续退格合并成一步
    d.key(w, "ctrl+end")
    for _ in range(5):
        d.key(w, "backspace")
    check(state(app, w)["a"] == "hello ", "退格 5 次")
    d.key(w, "ctrl+z")
    check(state(app, w)["a"] == "hello world", "一次撤销恢复连续退格")
    d.close(w)
    app.finish()


def test_clipboard(d):
    app, w = start_twidgets(d, "第一行 first\n第二行 second\n第三行")
    d.key(w, "ctrl+home")
    d.key(w, "shift+down")
    d.key(w, "shift+end")
    d.key(w, "ctrl+c")
    mark(app, w)
    time.sleep(0.2)
    check(d.clipboard.decode() == "第一行 first\n第二行 second", "复制到剪贴板: %r" % d.clipboard)
    shot(d, w, "editor_selection")
    # 粘贴到编辑器 B
    d.click(w, 600, 100)
    d.key(w, "ctrl+v")
    time.sleep(0.3)
    s = state(app, w)
    check(s["b"] == "第一行 first\n第二行 second", "粘贴到编辑器 B: %r" % s["b"])
    # 剪切：A 里删掉，B 里再粘贴一次
    d.click(w, 200, 30)
    d.key(w, "ctrl+a")
    d.key(w, "ctrl+x")
    mark(app, w)
    time.sleep(0.2)
    s = state(app, w)
    check(s["a"] == "", "剪切后 A 为空: %r" % s["a"])
    check(d.clipboard.decode() == "第一行 first\n第二行 second\n第三行", "剪切内容: %r" % d.clipboard)
    d.click(w, 600, 217)               # 单行输入框 C
    d.key(w, "ctrl+v")
    time.sleep(0.3)
    s = state(app, w)
    check("第一行" in s["c"], "单行输入框粘贴: %r" % s["c"])
    # 撤销剪切
    d.click(w, 200, 30)
    d.key(w, "ctrl+z")
    check(state(app, w)["a"] == "第一行 first\n第二行 second\n第三行", "撤销剪切")
    shot(d, w, "editor_clipboard")
    d.close(w)
    app.finish()


def test_double_click_word(d):
    app, w = start_twidgets(d, "你好世界，hello world")
    d.wait_idle(w, quiet=0.3)
    x, y = a_point("你好", 0, 4)                     # “世”字中间
    d.double_click(w, x, y)
    d.key(w, "ctrl+c")
    mark(app, w)
    time.sleep(0.2)
    check(d.clipboard.decode() == "你好世界", "双击中文选中连续的汉字: %r" % d.clipboard)
    x, y = a_point("你好世界，he", 0, 2)
    d.double_click(w, x, y)
    d.key(w, "ctrl+c")
    mark(app, w)
    time.sleep(0.2)
    check(d.clipboard.decode() == "hello", "双击英文选中一个单词: %r" % d.clipboard)
    shot(d, w, "editor_double_click")
    # 鼠标拖动选择：从行首拖到 world 中间
    x0, y0 = a_point("", 0, 1)
    x1, y1 = a_point("你好世界，hello wo", 0, 1)
    d.drag(w, x0, y0, x1, y1, steps=6)
    d.key(w, "ctrl+c")
    mark(app, w)
    time.sleep(0.2)
    check(d.clipboard.decode() == "你好世界，hello wo", "拖动选择: %r" % d.clipboard)
    # 点击定位光标
    x, y = a_point("你好世界，hel", 0, 1)
    d.click(w, x, y)
    s = state(app, w)
    check(s["a_pos"] == [1, 9], "点击定位: %s" % s["a_pos"])
    shot(d, w, "editor_drag_select")
    d.close(w)
    app.finish()


def test_wrap_resize(d):
    long = ("自动换行测试：" + "SketchOS 是一个用来学习操作系统的小系统，它的界面全部是手绘风格。" * 4)
    app, w = start_twidgets(d, long)
    d.key(w, "ctrl+home")
    d.key(w, "down")
    s1 = state(app, w)
    check(s1["a_pos"][0] == 1 and s1["a_pos"][1] > 10, "自动换行时下移应停留在同一逻辑行: %s" % s1["a_pos"])
    shot(d, w, "wrap_wide")
    d.resize(w, 700, 520)              # 编辑器变窄 → 每个显示行字更少
    d.wait_idle(w, quiet=0.3)
    d.key(w, "ctrl+home")
    d.key(w, "down")
    s2 = state(app, w)
    check(s2["a_pos"][0] == 1 and 1 < s2["a_pos"][1] < s1["a_pos"][1],
          "窗口变窄后每行字数应变少: %s -> %s" % (s1["a_pos"], s2["a_pos"]))
    shot(d, w, "wrap_narrow")
    # 关闭自动换行：只有一个显示行，下移就到文末
    d.key(w, "f8")
    d.key(w, "ctrl+home")
    d.key(w, "end")
    s3 = state(app, w)
    check(s3["a_pos"] == [1, len(long) + 1], "不换行时 End 到整行末尾: %s" % s3["a_pos"])
    shot(d, w, "nowrap_hscroll")
    d.key(w, "f8")
    d.key(w, "f9")                     # 行号
    d.key(w, "ctrl+home")
    shot(d, w, "wrap_line_numbers")
    d.close(w)
    app.finish()


def test_scroll(d):
    app, w = start_twidgets(d)
    d.key(w, "f10")                    # 300 行
    d.key(w, "ctrl+home")
    before = shot(d, w, "scroll_top")
    d.key(w, "pagedown")
    s = state(app, w)
    vis = (600 - 20 - 12) // row_h()
    check(abs(s["a_pos"][0] - vis) <= 2, "PageDown 应下移约一页（%d 行）: %s" % (vis - 1, s["a_pos"]))
    d.key(w, "ctrl+end")
    s = state(app, w)
    check(s["a_pos"][0] == 301, "Ctrl+End 到最后: %s" % s["a_pos"])
    shot(d, w, "scroll_end")
    d.key(w, "ctrl+home")
    # 滚轮向下 5 格 = 15 行；然后点击第一行，光标应在第 16 行附近
    d.wheel(w, 200, 200, -5)
    d.wait_idle(w, quiet=0.3)
    d.click(w, TEXT_X + 30, TEXT_Y + row_h() // 2)
    s = state(app, w)
    check(s["a_pos"][0] == 16, "滚轮滚动 15 行后点击第一行: %s" % s["a_pos"])
    # 拖动滚动条滑块到底
    bar_x = 10 + (900 - 380) - 3 - 1 - 6     # 编辑器右边缘内侧的滚动条中线
    d.key(w, "ctrl+home")
    d.wait_idle(w, quiet=0.3)
    d.drag(w, bar_x, 30, bar_x, 590, steps=10)
    d.wait_idle(w, quiet=0.3)
    d.click(w, TEXT_X + 30, TEXT_Y + row_h() // 2)
    s = state(app, w)
    check(s["a_pos"][0] > 250, "拖动滚动条到底后点击第一行，应在文末附近: %s" % s["a_pos"])
    after = shot(d, w, "scroll_dragged")
    check(open(before, "rb").read() != open(after, "rb").read(), "滚动后画面应变化")
    d.close(w)
    app.finish()


def test_readonly(d):
    app, w = start_twidgets(d, "只读的文字")
    d.key(w, "f11")
    d.type_text(w, "改不了")
    d.key(w, "backspace")
    d.key(w, "ctrl+x")
    d.key(w, "end")
    s = state(app, w)
    check(s["a"] == "只读的文字" and s["a_pos"] == [1, 6], "只读时不能修改但能移动光标: %s" % s)
    d.close(w)
    app.finish()


def test_damage_typing(d):
    app, w = start_twidgets(d, "第一行\n第二行\n第三行\n" + "长段落" * 60)
    d.key(w, "ctrl+home")
    d.key(w, "end")
    mark(app, w)
    d.wait_idle(w, quiet=0.3)
    start = len(d.requests)
    d.type_text(w, "字")
    mark(app, w)
    d.wait_idle(w, quiet=0.2)
    rects = damages_since(d, start, w)
    check(rects, "打字后应有重画")
    maxh = max(r[3] for r in rects)
    area = sum(r[2] * r[3] for r in rects)
    print("    打一个字的重画：%d 个矩形 %s，最高 %d 像素（行高 %d），面积 %d 像素" %
          (len(rects), rects[:4], maxh, row_h(), area))
    check(maxh <= row_h() + 2, "打一个字只应重画一个显示行（最高 %d）" % maxh)
    # 回车：下面的行整体下移，重画从这一行到底
    start = len(d.requests)
    d.key(w, "enter")
    mark(app, w)
    d.wait_idle(w, quiet=0.2)
    rects = damages_since(d, start, w)
    print("    回车的重画：%s" % rects[:4])
    check(max(r[3] for r in rects) > 3 * row_h(), "回车后下面的行都要重画")
    # 光标移动（不改内容）：只重画新旧两个光标小矩形（跨行时 sk_ui 会把上下相邻的两个合并成一个）
    for keyname in ("right", "left", "up"):
        start = len(d.requests)
        d.key(w, keyname)
        mark(app, w)
        d.wait_idle(w, quiet=0.2)
        rects = damages_since(d, start, w)
        print("    光标 %s 的重画：%s" % (keyname, rects[:4]))
        check(rects and all(r[2] <= 16 and r[3] <= 2 * row_h() + 2 for r in rects), "光标移动只重画光标处")
    d.close(w)
    app.finish()


# ================================================================ 列表测试

LIST_X, LIST_Y = 900 - 355, 245


def list_row_y(i, scroll=0):
    header = max(26, FONT.line_height + 10)
    rh = max(26, FONT.line_height + 8)
    return LIST_Y + 3 + header + (i - scroll) * rh + rh // 2


def test_list_keyboard(d):
    app, w = start_twidgets(d)
    d.click(w, LIST_X + 60, list_row_y(0))
    ev = app.wait(lambda o: isinstance(o, dict) and o.get("event") == "list-change", what="选中事件")
    check(ev["sel"] == 0, "点击第一行: %s" % ev)
    for _ in range(3):
        d.key(w, "down")
    s = state(app, w)
    check(s["sel"] == 3 and s["sel_name"] == "项目 03", "下移 3 次: %s" % s)
    d.key(w, "end")
    s = state(app, w)
    check(s["sel"] == 49 and s["scroll"] > 30, "End 到最后一行并滚动: %s" % s)
    d.key(w, "home")
    d.key(w, "pagedown")
    s = state(app, w)
    check(s["sel"] > 5, "PageDown: %s" % s)
    d.key(w, "enter")
    ev = app.wait(lambda o: isinstance(o, dict) and o.get("event") == "list-activate", what="激活事件")
    check(ev["sel"] == s["sel"], "回车激活: %s" % ev)
    shot(d, w, "list_keyboard")
    d.close(w)
    app.finish()


def test_list_mouse(d):
    app, w = start_twidgets(d)
    d.double_click(w, LIST_X + 60, list_row_y(4))
    ev = app.wait(lambda o: isinstance(o, dict) and o.get("event") == "list-activate", what="双击激活")
    check(ev["sel"] == 4 and ev["name"] == "项目 04", "双击第 5 行: %s" % ev)
    d.wheel(w, LIST_X + 100, list_row_y(3), -3)
    s = state(app, w)
    check(s["scroll"] == 9, "滚轮 3 格 = 9 行: %s" % s)
    d.move(w, LIST_X + 100, list_row_y(12, 9))       # 悬停
    shot(d, w, "list_hover")
    # 拖动列表的滚动条滑块到底（先 Home 让滑块回到顶上；按在滑块上方的轨道上是“向上翻一页”）
    d.key(w, "home")
    bar_x = LIST_X + 345 - 3 - 1 - 6
    thumb_y = LIST_Y + 3 + max(26, FONT.line_height + 10) + 3 + 8
    s = state(app, w)
    check(s["scroll"] == 0, "Home 回到顶部: %s" % s)
    d.drag(w, bar_x, thumb_y, bar_x, 600 - 12, steps=10)
    s2 = state(app, w)
    check(s2["scroll"] > 30, "拖动滚动条到底: %s -> %s" % (s["scroll"], s2["scroll"]))
    shot(d, w, "list_scrolled")
    d.close(w)
    app.finish()


def test_list_sort(d):
    app, w = start_twidgets(d)
    header_y = LIST_Y + 3 + max(26, FONT.line_height + 10) // 2
    d.click(w, LIST_X + 160, header_y)              # “编号”列
    ev = app.wait(lambda o: isinstance(o, dict) and o.get("event") == "sort", what="排序回调")
    check(ev["col"] == 1 and ev["desc"] == 0 and ev["first"] == "项目 00", "按编号升序: %s" % ev)
    d.click(w, LIST_X + 160, header_y)
    ev = app.wait(lambda o: isinstance(o, dict) and o.get("event") == "sort", what="排序回调")
    check(ev["col"] == 1 and ev["desc"] == 1, "再点一次降序: %s" % ev)
    shot(d, w, "list_sorted_desc")
    d.click(w, LIST_X + 40, header_y)
    ev = app.wait(lambda o: isinstance(o, dict) and o.get("event") == "sort", what="排序回调")
    check(ev["col"] == 0 and ev["desc"] == 0, "换一列从升序开始: %s" % ev)
    d.close(w)
    app.finish()


def test_list_damage(d):
    app, w = start_twidgets(d)
    d.click(w, LIST_X + 60, list_row_y(5))
    mark(app, w)
    d.wait_idle(w, quiet=0.3)
    start = len(d.requests)
    d.key(w, "f6")                                  # set_cell 一个单元格
    mark(app, w)
    d.wait_idle(w, quiet=0.2)
    rects = damages_since(d, start, w)
    print("    sk_list_set_cell 的重画：%s" % rects)
    rh = max(26, FONT.line_height + 8)
    check(rects and max(r[3] for r in rects) <= rh + 2, "只重画一行（单元格）")
    check(state(app, w)["cell"] == "1001", "单元格内容")
    # 第一次重建：第 2 行的“编号”恢复成原来的值，只有这一行要重画
    start = len(d.requests)
    d.key(w, "f7")
    mark(app, w)
    time.sleep(0.3)
    rects = [r for r in damages_since(d, start, w) if r[2] > 16]   # 忽略光标闪烁
    print("    clear + add（只有一行变了）的重画：%s" % rects)
    check(len(rects) == 1 and rects[0][3] <= rh + 2, "只重画变了的那一行: %s" % rects)
    # 第二次原样重建：内容完全没变，不应该有任何重画
    start = len(d.requests)
    d.key(w, "f7")
    mark(app, w)
    time.sleep(0.3)
    rects = [r for r in damages_since(d, start, w) if r[2] > 16]
    print("    clear + 原样 add 的重画：%s" % rects)
    check(not rects, "内容没变的重建不应重画: %s" % rects)
    s = state(app, w)
    check(s["sel"] == 5 and s["count"] == 50, "重建后选中保持: %s" % s)
    d.close(w)
    app.finish()


# ================================================================ 对话框测试

def demo(d, kind, arg=None):
    argv = [os.path.join(ARGS.bin, "skwidgets"), "--demo", kind] + ([arg] if arg else [])
    app = App(d, argv)
    main = d.wait_window(10, title="控件展示")
    dlg = d.wait_window(10, count=2)
    d.wait_idle(dlg, quiet=0.3)
    check(dlg.flags & 0x8 and dlg.flags & 0x2, "对话框应带 SK_WIN_DIALOG|SK_WIN_CENTER: %#x" % dlg.flags)
    return app, main, dlg


def test_message_dialogs(d):
    for kind in ("message", "ask", "warn", "error"):
        app, _main, dlg = demo(d, kind)
        shot(d, dlg, "dialog_" + kind)
        d.key(dlg, "enter")
        r = app.wait_text("demo result:")
        check(r == "0", "%s: 回车 = 第一个按钮，得到 %s" % (kind, r))
        app.finish()
    app, _main, dlg = demo(d, "ask")
    d.key(dlg, "esc")
    check(app.wait_text("demo result:") == "-1", "Esc = -1")
    app.finish()
    app, _main, dlg = demo(d, "ask")
    d.close(dlg)
    check(app.wait_text("demo result:") == "-1", "关闭按钮 = -1")
    app.finish()
    # 点最右边的“取消”按钮（按钮右对齐在底部，最后一个按钮离右边 20 像素）
    app, _main, dlg = demo(d, "ask")
    d.click(dlg, dlg.w - 20 - 40, dlg.h - 20 - 13)
    check(app.wait_text("demo result:") == "2", "点击第三个按钮")
    app.finish()
    # Tab 切到第二个按钮再回车
    app, _main, dlg = demo(d, "ask")
    d.key(dlg, "tab")
    d.key(dlg, "tab")
    shot(d, dlg, "dialog_ask_focus")
    d.key(dlg, "enter")
    check(app.wait_text("demo result:") == "1", "Tab 两次 + 回车 = 第二个按钮")
    app.finish()


def test_input_dialog(d):
    app, _main, dlg = demo(d, "input", "旧名字.txt")
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, "新名字")
    shot(d, dlg, "dialog_input")
    d.key(dlg, "enter")
    check(app.wait_text("demo result:") == "新名字", "输入框返回输入的文字")
    app.finish()
    app, _main, dlg = demo(d, "input")
    d.type_text(dlg, "随便")
    d.key(dlg, "esc")
    check(app.wait_text("demo result:") == "(NULL)", "Esc 返回 NULL")
    app.finish()


def make_tree():
    base = "/tmp/sketch-widget-test-files"
    subprocess.run(["rm", "-rf", base])
    os.makedirs(base + "/文档/深一层")
    os.makedirs(base + "/bin")
    for name, data in (("readme.txt", "hello"), ("照片.ppm", "P6 1 1 255 abc"), ("笔记.md", "x" * 5000),
                       ("文档/计划.txt", "计划"), ("文档/深一层/秘密.txt", "42"), ("zz-last.c", "int x;")):
        with open(os.path.join(base, name), "w") as f:
            f.write(data)
    with open(base + "/bin/tool", "w") as f:
        f.write("#!/bin/sh\n")
    os.chmod(base + "/bin/tool", 0o755)
    return base


def test_file_dialog_open(d):
    base = make_tree()
    app, _main, dlg = demo(d, "open", base)
    shot(d, dlg, "file_open_root")
    # 列表有焦点：文件夹排在前面（bin, 文档），然后是文件
    d.key(dlg, "down")                 # bin
    d.key(dlg, "down")                 # 文档
    d.key(dlg, "enter")                # 进入“文档”
    d.wait_idle(dlg, quiet=0.3)
    shot(d, dlg, "file_open_sub")
    d.key(dlg, "down")                 # 深一层
    d.key(dlg, "down")                 # 计划.txt
    d.key(dlg, "enter")
    r = app.wait_text("demo result:")
    check(r == base + "/文档/计划.txt", "打开模式选中子文件夹里的文件: %s" % r)
    app.finish()
    # 向上按钮 + 双击文件
    app, _main, dlg = demo(d, "open", base + "/文档")
    d.click(dlg, 16 + 18, 16 + 17)     # ↑ 按钮
    d.wait_idle(dlg, quiet=0.3)
    shot(d, dlg, "file_open_up")
    rh = max(26, FONT.line_height + 8)
    first_y = 16 + 34 + 10 + 3 + max(26, FONT.line_height + 10) + rh // 2
    # 回到上一级后：bin, 文档, readme.txt, zz-last.c, 照片.ppm, 笔记.md（中文按码点排在后面）
    d.double_click(dlg, 120, first_y + 2 * rh)
    r = app.wait_text("demo result:")
    check(r == base + "/readme.txt", "双击 readme.txt: %s" % r)
    app.finish()
    # 在路径框里输入路径 + 回车 = 进入那个文件夹（焦点回到列表）
    app, _main, dlg = demo(d, "open", base)
    d.click(dlg, 300, 16 + 17)
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, base + "/文档/深一层")
    d.key(dlg, "enter")
    d.wait_idle(dlg, quiet=0.3)
    d.key(dlg, "down")
    d.key(dlg, "enter")
    r = app.wait_text("demo result:")
    check(r == base + "/文档/深一层/秘密.txt", "路径框跳转后选文件: %s" % r)
    app.finish()
    # 列表里按退格 = 上一级，并选中刚才所在的文件夹
    app, _main, dlg = demo(d, "open", base + "/文档/深一层")
    d.key(dlg, "backspace")
    d.wait_idle(dlg, quiet=0.3)
    d.key(dlg, "down")                 # 选中的是“深一层”，下一个是“计划.txt”
    d.key(dlg, "enter")
    r = app.wait_text("demo result:")
    check(r == base + "/文档/计划.txt", "退格回到上一级: %s" % r)
    app.finish()
    # 什么都没选时按“打开”：提示，不返回
    app, _main, dlg = demo(d, "open", base)
    d.click(dlg, dlg.w - 16 - 88 - 12 - 44, dlg.h - 16 - 17)
    time.sleep(0.3)
    check(app.p.poll() is None, "没选文件时不应关闭")
    shot(d, dlg, "file_open_nothing")
    d.key(dlg, "esc")
    check(app.wait_text("demo result:") == "(NULL)", "Esc 取消")
    app.finish()


def test_file_dialog_save(d):
    base = make_tree()
    app, _main, dlg = demo(d, "save", base)
    shot(d, dlg, "file_save")
    d.key(dlg, "enter")                # 默认文件名“我的笔记.txt”，不存在 → 直接返回
    r = app.wait_text("demo result:")
    check(r == base + "/我的笔记.txt", "保存新文件: %s" % r)
    app.finish()
    # 已存在的文件：先问一句
    app, _main, dlg = demo(d, "save", base)
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, "readme.txt")
    d.key(dlg, "enter")
    confirm = d.wait_window(5, count=3)
    d.wait_idle(confirm, quiet=0.3)
    shot(d, confirm, "file_save_overwrite")
    d.key(confirm, "esc")              # 不替换 → 回到文件对话框
    time.sleep(0.3)
    check(app.p.poll() is None, "取消替换后文件对话框还在")
    d.key(dlg, "enter")
    confirm = d.wait_window(5, count=3)
    d.wait_idle(confirm, quiet=0.3)
    d.key(confirm, "enter")            # 替换
    r = app.wait_text("demo result:")
    check(r == base + "/readme.txt", "确认替换: %s" % r)
    app.finish()
    # 输入子文件夹名 + 回车 = 进入文件夹
    app, _main, dlg = demo(d, "save", base)
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, "文档")
    d.key(dlg, "enter")
    d.wait_idle(dlg, quiet=0.3)
    d.type_text(dlg, "新计划.txt")
    d.key(dlg, "enter")
    r = app.wait_text("demo result:")
    check(r == base + "/文档/新计划.txt", "进入子文件夹后保存: %s" % r)
    app.finish()
    # “主目录”按钮：回到 $HOME
    app, _main, dlg = demo(d, "save", base)
    d.click(dlg, 16 + 42 + 18, 16 + 17)
    d.wait_idle(dlg, quiet=0.3)
    shot(d, dlg, "file_save_home")
    d.click(dlg, 300, dlg.h - 16 - 34 - 10 - 17)   # 文件名框
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, "home-test.txt")
    d.key(dlg, "enter")
    r = app.wait_text("demo result:")
    home = os.environ.get("HOME", "/root").rstrip("/") or "/"
    check(r == os.path.join(home, "home-test.txt"), "主目录按钮后保存: %s" % r)
    app.finish()


# ================================================================ 控件展示程序

def test_gallery(d):
    for tab in range(5):
        app = App(d, [os.path.join(ARGS.bin, "skwidgets"), "--tab", str(tab), "--log"])
        w = d.wait_window(10, title="控件展示")
        d.wait_idle(w, quiet=0.4)
        shot(d, w, "gallery_tab%d" % tab)
        if tab == 0:
            # 滑块拖动 → 占用率条跟着变；点按钮 → 状态栏
            d.click(w, 16 + 60, 90 + 228 + 34 + 18)
            app.wait_text("skwidgets: 按钮「普通按钮」被点击")
        if tab == 4:
            d.mouse(w, 2, 400, 400, 2, 2, clicks=1)     # 右键 → 弹出菜单
            time.sleep(0.05)
            d.mouse(w, 3, 400, 400, 0, 2)
            shot(d, w, "gallery_menu")
            d.key(w, "esc")
        d.close(w)
        app.finish()
    check(not d.errors, "协议错误: %s" % d.errors)


TESTS = [test_typing_cjk, test_editing_keys, test_undo_redo, test_clipboard, test_double_click_word,
         test_wrap_resize, test_scroll, test_readonly, test_damage_typing,
         test_list_keyboard, test_list_mouse, test_list_sort, test_list_damage,
         test_message_dialogs, test_input_dialog, test_file_dialog_open, test_file_dialog_save, test_gallery]


def main():
    global ARGS, FONT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="用户态程序目录（里面有 skwidgets）")
    ap.add_argument("--twidgets", required=True, help="tests/widgets/twidgets.c 编译出的程序")
    ap.add_argument("--out", default="/tmp/sketch-widget-shots", help="截图目录")
    ap.add_argument("-k", default="", help="只运行名字包含这个字符串的测试")
    ARGS = ap.parse_args()
    os.makedirs(ARGS.out, exist_ok=True)
    os.environ.setdefault("SKETCH_FONT_DIR", os.path.join(ROOT, "assets", "fonts"))
    os.environ.setdefault("HOME", "/root")
    FONT = Font(os.path.join(os.environ["SKETCH_FONT_DIR"], "sans-16.skf"))
    failed = []
    for t in TESTS:
        if ARGS.k and ARGS.k not in t.__name__:
            continue
        t0 = time.time()
        with MockDisplay(SOCK, 1024, 768) as d:
            try:
                t(d)
                if d.errors:
                    raise AssertionError("协议错误: %s" % d.errors)
                print("PASS %-24s %.1fs" % (t.__name__, time.time() - t0))
            except Exception as e:
                failed.append(t.__name__)
                print("FAIL %-24s %s" % (t.__name__, e))
                if not isinstance(e, AssertionError):
                    traceback.print_exc()
    print("\n截图保存在 %s" % ARGS.out)
    print("%d 个测试，%d 个失败%s" % (len([t for t in TESTS if not ARGS.k or ARGS.k in t.__name__]), len(failed),
                                 ("：" + ", ".join(failed)) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
