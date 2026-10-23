#!/usr/bin/env python3
"""
sketchd 的 QEMU 自动化测试（在 WSL 里运行）

    bash tests/sketchd/build.sh --testinit          # 先编译并打包测试用 initramfs
    python3 tests/sketchd/run_tests.py --out /tmp/skd-shots [--only main,fbdev,...]

每一组测试启动一次虚拟机，用 tools/qemu_harness.py 的 VM 类：
  - 通过 QMP 发送鼠标（vmmouse 绝对坐标）和键盘事件；
  - 等待串口上的日志：sketchd 的里程碑、测试客户端 skdtest/skdbad 打印的事件、testinit 的消息；
  - 在关键时刻截图（PNG），供人工检查画面。
最后打印每一项检查的 PASS/FAIL 汇总，有失败时退出码为 1。

坐标都是按 sketchd 的布局规则算出来的（见各函数旁的注释），改了布局要同步改这里。
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from qemu_harness import VM  # noqa: E402

W, H = 1024, 768
TITLE_H, BORDER, TASKBAR_H = 34, 6, 44

# ---------- 布局（与 sketchd 的 wm.c / taskbar.c / menu.c / desktop.c 一致） ----------


def icon_center(i):
    """desktop.c：图标格子 (14, 14 + i*88, 92, 88)，48px 图标在格子顶部 +6 处"""
    return 14 + 46, 14 + i * 88 + 6 + 24


APPS = ["terminal", "t1", "dlg", "hang", "dragdie", "notepad", "about"]   # 测试 apps.conf 的顺序


def cascade(k, w, h):
    """wm.c place_window：层叠摆放第 k 个普通窗口，返回内容区左上角"""
    k %= 8
    x = 132 + 28 * k + BORDER
    y = 24 + 28 * k + TITLE_H
    bw, bh = w + 2 * BORDER, h + TITLE_H + BORDER
    wa_w, wa_h = W, H - TASKBAR_H
    if x - BORDER + bw > wa_w:
        x = max(BORDER, wa_w - bw + BORDER)
    if y - TITLE_H + bh > wa_h:
        y = max(TITLE_H, wa_h - bh + TITLE_H)
    return x, y


def title_point(x, y, w):
    """标题栏上一个不在按钮上的点"""
    return x + 60, y - TITLE_H // 2


def button_center(x, y, w, which):
    """theme.c：关闭按钮中心 = 外框右边 - 7 - 11，相邻按钮间隔 28（0 关闭 1 最大化 2 最小化）"""
    bx, by, bw = x - BORDER, y - TITLE_H, w + 2 * BORDER
    return bx + bw - 7 - 11 - which * 28, by + TITLE_H // 2


def corner(x, y, w, h):
    """右下角缩放抓手（内容区外、边框内）"""
    return x + w + 3, y + h + 3


START_BTN = (56, 746)      # taskbar.c：开始按钮 (8, 730, 96, 32)
CLOCK = (984, 746)         # 时钟 (952, 730, 64, 32)


def task_button(i):
    return 116 + i * (176 + 8) + 88, 746


def start_item(i, napps=7):
    """menu.c：开始菜单高 = 70 + 应用数*36 + 14(分隔线) + 2*36 + 16 + 6，底边在开始按钮上方 10px"""
    h = 70 + napps * 36 + 14 + 2 * 36 + 16 + 6
    top = 730 - 10 - h
    y = 70
    for k in range(i):
        y += 36 if k != napps else 14
    return 6 + 120, top + y + 18


CONFIRM_OK = (504, 403)      # menu.c：确认框 (332,276,360,172)，按钮 (118,108,108,38) / (238,108,96,38)
CONFIRM_CANCEL = (618, 403)


# ---------- 小测试框架 ----------

class Runner:
    def __init__(self, out):
        self.out = out
        self.results = []
        os.makedirs(out, exist_ok=True)

    def check(self, name, ok, detail=""):
        self.results.append((name, bool(ok), detail))
        print(("  PASS " if ok else "  FAIL ") + name + (("  — " + detail) if detail else ""), flush=True)
        return ok

    def summary(self):
        bad = [r for r in self.results if not r[1]]
        print("\n==== %d 项检查，%d 项失败 ====" % (len(self.results), len(bad)))
        for n, _, d in bad:
            print("  FAIL", n, d)
        return not bad


class Session:
    def __init__(self, runner, vm, tag):
        self.r, self.vm, self.tag = runner, vm, tag
        self.pos = 0

    def mark(self):
        self.pos = len(self.vm.serial_output())

    def expect(self, name, pattern, timeout=10, start=None):
        try:
            m = self.vm.wait_serial(pattern, timeout, self.pos if start is None else start)
            self.r.check("[%s] %s" % (self.tag, name), True)
            return m
        except Exception as e:  # noqa: BLE001
            self.r.check("[%s] %s" % (self.tag, name), False, "没有等到 %r" % pattern)
            return None

    def absent(self, name, pattern, start=None):
        text = self.vm.serial_output()[self.pos if start is None else start:]
        ok = re.search(pattern, text) is None
        return self.r.check("[%s] %s" % (self.tag, name), ok, "" if ok else "出现了 %r" % pattern)

    def shot(self, name):
        path = os.path.join(self.r.out, "%s-%s.png" % (self.tag, name))
        self.vm.screenshot(path)
        print("   截图", path)
        return path

    def created(self, app, n=1, timeout=15):
        """等待 n 个窗口创建，返回窗口 id 列表"""
        ids = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = self.vm.serial_output()[self.pos:]
            ids = [int(x) for x in re.findall(r"sketchd: window (\d+) created app=%s " % re.escape(app), text)]
            if len(ids) >= n:
                break
            time.sleep(0.1)
        self.r.check("[%s] 创建 %d 个 %s 窗口" % (self.tag, n, app), len(ids) >= n, "ids=%s" % ids)
        time.sleep(0.6)       # 等客户端画完第一帧
        return ids


def boot(runner, tag, extra="", timeout_ready=60):
    # 套接字放在短路径的临时目录（Unix 套接字路径不能超过 108 字节），串口日志放进输出目录
    vm = VM(initrd=runner.initrd, extra_cmdline=("testinit.noreboot " + extra).strip(),
            log_path=os.path.join(runner.out, "serial-%s.log" % tag))
    vm.start()
    s = Session(runner, vm, tag)
    return vm, s


# ---------- 测试组 ----------

def test_main(runner):
    vm, s = boot(runner, "main", "testinit.respawn=6000 sketch.debug")
    try:
        s.expect("testinit 启动 sketchd", r"testinit: started sketchd pid=(\d+)", 60)
        time.sleep(0.5)
        s.shot("01-splash")
        m = s.expect("ready 里程碑（kms + vmmouse）", r"sketchd: ready 1024x768 backend=kms mouse=vmmouse", 30)
        if not m:
            return
        time.sleep(0.4)
        vm.mouse_move(500, 400)
        s.shot("02-desktop")

        # --- 单击选中图标 / 双击启动 ---
        s.mark()
        vm.click(*icon_center(1))
        time.sleep(0.3)
        s.shot("03-icon-selected")
        vm.double_click(*icon_center(1))
        s.expect("双击图标启动 t1", r"sketchd: launch t1 pid=\d+")
        ids = s.created("t1", 2)
        if len(ids) < 2:
            return
        w1, w2 = ids[0], ids[1]
        s.expect("WINDOW_CREATED 之后收到 FOCUS", r"skdtest\[t1\]: focus win=%d on=1" % w2)
        # 第 1 个窗口 420x260 层叠位置 0，第 2 个 390x240 位置 1（在最上面）
        x1, y1 = cascade(0, 420, 260)
        x2, y2 = cascade(1, 390, 240)
        vm.mouse_move(x2 + 200, y2 + 150)
        time.sleep(0.3)
        s.shot("04-two-windows")

        # --- 内容区点击：坐标是内容区坐标 ---
        s.mark()
        vm.click(x2 + 50, y2 + 40)
        s.expect("内容区按下事件坐标正确", r"skdtest\[t1\]: mouse win=%d kind=down x=50 y=40 buttons=1 button=1 clicks=1" % w2)
        s.expect("松开事件", r"skdtest\[t1\]: mouse win=%d kind=up x=50 y=40 buttons=0 button=1" % w2)
        vm.double_click(x2 + 60, y2 + 60)
        s.expect("双击 clicks=2", r"skdtest\[t1\]: mouse win=%d kind=down x=60 y=60 buttons=1 button=1 clicks=2" % w2)
        vm.click(x2 + 70, y2 + 70, "right")
        s.expect("右键 button=2", r"skdtest\[t1\]: mouse win=%d kind=down x=70 y=70 buttons=2 button=2" % w2)
        vm.wheel(x2 + 80, y2 + 80, 2)
        s.expect("滚轮向上 wheel=1", r"skdtest\[t1\]: mouse win=%d kind=wheel .*wheel=1" % w2)

        # --- 键盘 ---
        s.mark()
        vm.type_text("Hi")
        s.expect("按键 Shift+h → 'H'", r"skdtest\[t1\]: key win=%d code=35 state=1 cp=72 mods=1" % w2)
        s.expect("按键 i → 'i'", r"skdtest\[t1\]: key win=%d code=23 state=1 cp=105 mods=0" % w2)
        s.expect("按键松开 state=0", r"skdtest\[t1\]: key win=%d code=23 state=0" % w2)
        vm.key("ret")
        s.expect("回车 cp=13", r"skdtest\[t1\]: key win=%d code=28 state=1 cp=13" % w2)
        vm.key("ctrl-c")
        s.expect("Ctrl+C 仍给出字母 c", r"skdtest\[t1\]: key win=%d code=46 state=1 cp=99 mods=2" % w2)
        vm.key("caps_lock")
        vm.type_text("a")
        s.expect("Caps Lock 后 a → 'A'（mods 含 CAPS）", r"skdtest\[t1\]: key win=%d code=30 state=1 cp=65 mods=16" % w2)
        vm.key("caps_lock")
        vm.key("kp_7")
        s.expect("小键盘 7（NumLock 默认开）", r"skdtest\[t1\]: key win=%d code=71 state=1 cp=55" % w2)
        s.shot("05-typed")

        # --- 点击另一个窗口：焦点切换 + 置顶 ---
        s.mark()
        vm.click(x1 + 15, y1 + 200)
        s.expect("点击下面的窗口获得焦点", r"skdtest\[t1\]: focus win=%d on=1" % w1)
        s.expect("原窗口失去焦点", r"skdtest\[t1\]: focus win=%d on=0" % w2)
        time.sleep(0.3)
        s.shot("06-raised")

        # --- 拖动标题栏移动窗口 ---
        s.mark()
        tx, ty = title_point(x1, y1, 420)
        vm.drag(tx, ty, tx + 300, ty + 200, steps=15)
        time.sleep(0.5)
        nx1, ny1 = x1 + 300, y1 + 200
        vm.click(nx1 + 30, ny1 + 30)
        s.expect("移动后内容区坐标仍正确", r"skdtest\[t1\]: mouse win=%d kind=down x=30 y=30 " % w1)
        s.shot("07-moved")

        # --- 右下角拖动缩放 ---
        s.mark()
        cx, cy = corner(nx1, ny1, 420, 260)
        vm.mouse_move(cx, cy)
        vm.mouse_button("left", True)
        for i in range(1, 9):
            vm.mouse_move(cx - 15 * i, cy - 8 * i)
            time.sleep(0.03)
        time.sleep(0.2)
        s.shot("08-resize-outline")
        vm.mouse_button("left", False)
        s.expect("缩放后收到 CONFIGURE 300x196", r"skdtest\[t1\]: configure win=%d 300x196" % w1)
        time.sleep(0.6)
        s.shot("09-resized")
        # 缩到最小尺寸以下：被限制在 --min 200x120
        s.mark()
        cx, cy = corner(nx1, ny1, 300, 196)
        vm.drag(cx, cy, cx - 400, cy - 400, steps=6)
        s.expect("最小尺寸限制 200x120", r"skdtest\[t1\]: configure win=%d 200x120" % w1)
        time.sleep(0.5)

        # --- 最大化 / 还原 ---
        s.mark()
        bx, by = button_center(nx1, ny1, 200, 1)
        vm.click(bx, by)
        s.expect("最大化：CONFIGURE 为工作区大小", r"skdtest\[t1\]: configure win=%d 1012x684" % w1)
        time.sleep(0.8)
        s.shot("10-maximized")
        s.mark()
        mx, my = BORDER, TITLE_H
        vm.double_click(mx + 300, my - 17)
        s.expect("双击标题栏还原", r"skdtest\[t1\]: configure win=%d 200x120" % w1)
        time.sleep(0.6)

        # --- 最小化 + 任务栏恢复 ---
        s.mark()
        bx, by = button_center(nx1, ny1, 200, 2)
        vm.click(bx, by)
        time.sleep(0.4)
        s.expect("最小化后焦点交给另一个窗口", r"skdtest\[t1\]: focus win=%d on=1" % w2)
        s.shot("11-minimized")
        s.mark()
        vm.click(*task_button(0))
        s.expect("点任务栏按钮恢复并获得焦点", r"skdtest\[t1\]: focus win=%d on=1" % w1)
        time.sleep(0.3)
        vm.click(*task_button(0))
        time.sleep(0.3)
        s.shot("12-taskbar-toggle")
        vm.click(*task_button(0))
        time.sleep(0.3)

        # --- Alt+Tab ---
        s.mark()
        vm.key("alt-tab")
        s.expect("Alt+Tab 切换焦点", r"skdtest\[t1\]: focus win=\d+ on=1")
        s.absent("Tab 不会发给应用", r"skdtest\[t1\]: key win=\d+ code=15 ")
        # 按住 Alt 截图（QMP send-key 的 hold-time 里截图）
        vm._qmp_cmd("input-send-event", events=[{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "alt"}}}])
        vm._qmp_cmd("input-send-event", events=[{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "tab"}}}])
        vm._qmp_cmd("input-send-event", events=[{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "tab"}}}])
        time.sleep(0.4)
        s.shot("13-alt-tab-overlay")
        vm._qmp_cmd("input-send-event", events=[{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "alt"}}}])
        time.sleep(0.3)

        # --- 关闭按钮 / Alt+F4 ---
        s.mark()
        vm.key("alt-f4")
        s.expect("Alt+F4 → CLOSE", r"skdtest\[t1\]: close win=(\d+)")
        s.expect("窗口销毁里程碑", r"sketchd: window \d+ destroyed")
        time.sleep(0.4)
        s.mark()
        # 剩下的那个窗口：点它的关闭按钮（位置可能是 w2 的原位置，也可能是 w1 的）
        vm.key("alt-f4")
        s.expect("第二个窗口也关闭", r"skdtest\[t1\]: all windows closed")

        # --- Ctrl+Alt+T 启动终端 ---
        s.mark()
        vm.key("ctrl-alt-t")
        s.expect("Ctrl+Alt+T 启动 terminal", r"sketchd: launch terminal pid=\d+")
        tids = s.created("term", 1)

        # --- 开始菜单 ---
        s.mark()
        vm.key("meta_l")
        time.sleep(0.5)
        s.shot("14-start-menu-super")
        vm.key("down")
        vm.key("down")
        time.sleep(0.3)
        s.shot("15-start-menu-keyboard")
        vm.key("esc")
        time.sleep(0.3)
        vm.click(*START_BTN)
        time.sleep(0.5)
        vm.mouse_move(*start_item(6))
        time.sleep(0.3)
        s.shot("16-start-menu-hover")
        vm.click(*start_item(6))
        s.expect("开始菜单启动“关于本机”", r"sketchd: launch about pid=\d+")
        s.created("about", 1)
        s.shot("17-about-centered")
        vm.key("alt-f4")
        time.sleep(0.3)

        # --- 桌面右键菜单 + 换壁纸 ---
        vm.click(620, 520, "right")
        time.sleep(0.4)
        s.shot("18-context-menu")
        vm.click(620 + 8 + 80, 520 + 10 + 34 + 17)
        time.sleep(1.0)
        s.shot("19-wallpaper-ruled")
        for i in range(2):
            vm.click(620, 520, "right")
            time.sleep(0.3)
            vm.click(620 + 8 + 80, 520 + 10 + 34 + 17)
            time.sleep(0.8)
        s.shot("20-wallpaper-kraft")
        vm.click(620, 520, "right")
        time.sleep(0.3)
        vm.click(620 + 8 + 80, 520 + 10 + 34 + 17)
        time.sleep(0.6)
        vm.click(CLOCK[0], CLOCK[1])
        time.sleep(0.3)
        s.shot("21-clock-toast")

        # --- PrintScreen ---
        s.mark()
        vm.key("print")
        time.sleep(1.5)
        s.shot("22-screenshot-toast")
        out = vm.run_serial("ls /root/图片")
        s.r.check("[main] PrintScreen 保存了 截图-*.ppm", re.search(r"截图-\d{8}-\d{6}\.ppm \d{7,}", out) is not None, out)

        # --- 光标形状 ---
        s.mark()
        vm.run_serial("run /bin/skdtest --name pen --cursor 4 --size 300x200 --center")
        s.expect("SET_CURSOR", r"skdtest\[pen\]: set cursor 4")
        time.sleep(0.5)
        vm.mouse_move(W // 2, (H - TASKBAR_H) // 2 + 10)
        time.sleep(0.4)
        s.shot("23-pencil-cursor")
        vm.key("alt-f4")
        time.sleep(0.3)

        # --- 对话框：不上任务栏，居中在父窗口上，总在父窗口之上 ---
        s.mark()
        vm.run_serial("run /bin/skdtest --name parent --windows 2 --dialog2 --size 460x300")
        ids = s.created("parent", 2)
        time.sleep(0.4)
        s.shot("24-dialog")
        vm.key("alt-f4")
        s.expect("Alt+F4 关闭对话框", r"skdtest\[parent\]: close win=%d" % (ids[1] if len(ids) > 1 else -1))
        vm.key("alt-f4")
        time.sleep(0.5)

        # --- 剪贴板 + 客户端请求启动应用 ---
        s.mark()
        vm.run_serial("run /bin/skdtest --name clip --clip 你好剪贴板 --launch dlg --size 240x140")
        s.expect("剪贴板往返", r"skdtest\[clip\]: clipboard len=15 text=你好剪贴板")
        s.expect("LAUNCH 请求", r"sketchd: launch dlg pid=\d+")
        s.created("dlg", 1)
        vm.key("alt-f4")
        time.sleep(0.3)
        vm.run_serial("kill 9 skdtest")
        time.sleep(0.8)

        # --- 强制关闭卡死的程序 ---
        s.mark()
        vm.double_click(*icon_center(3))
        hid = s.created("skdbad-hang", 1)
        s.expect("hang 窗口就绪", r"skdbad\[hang\]: window ready")
        # 卡死的程序是最上面的窗口：用 Alt+F4 请求关闭两次（第二次在 3 秒之后 → SIGKILL）
        vm.key("alt-f4")
        time.sleep(1.0)
        s.absent("第一次关闭不会立刻杀进程", r"sketchd: window %d destroyed" % (hid[0] if hid else -1))
        time.sleep(2.5)
        vm.key("alt-f4")
        s.expect("3 秒后再关 → 强制关闭", r"sketchd: window %d destroyed" % (hid[0] if hid else -1))
        time.sleep(0.3)
        s.shot("25-force-close-toast")

        # --- 拖动中途客户端崩溃 ---
        s.mark()
        vm.double_click(*icon_center(4))
        did = s.created("skdbad-dragdie", 1)
        s.expect("dragdie 窗口就绪", r"skdbad\[dragdie\]: window ready")
        cx_, cy_ = BAD_X + 100, BAD_Y + 80     # skdbad 把窗口移到了内容区 (320,180)
        vm.mouse_move(cx_, cy_)
        vm.mouse_button("left", True)
        s.expect("客户端在按下时退出", r"skdbad\[dragdie\]: got mouse down, dying mid-drag")
        for i in range(10):
            vm.mouse_move(cx_ + 10 * i, cy_ + 5 * i)
            time.sleep(0.03)
        vm.mouse_button("left", False)
        s.expect("它的窗口被销毁", r"sketchd: window %d destroyed" % (did[0] if did else -1))

        # 标题栏拖动过程中客户端崩溃
        s.mark()
        vm.run_serial("run /bin/skdbad crash")
        cid = s.created("skdbad-crash", 1)
        tx_, ty_ = BAD_X + 60, BAD_Y - TITLE_H // 2
        vm.mouse_move(tx_, ty_)
        vm.mouse_button("left", True)
        for i in range(60):
            vm.mouse_move(tx_ + (i % 20) * 6, ty_ + (i % 10) * 4)
            time.sleep(0.05)
        vm.mouse_button("left", False)
        s.expect("拖动中崩溃的窗口被清理", r"sketchd: window %d destroyed" % (cid[0] if cid else -1), 10)

        # --- 不守规矩的客户端 ---
        for mode in ["garbage", "nohello", "badver", "unsealed", "small", "notmemfd", "nofd", "fdspam",
                     "badsize", "toobig", "wrongwin", "oversize"]:
            s.mark()
            vm.run_serial("run /bin/skdbad " + mode)
            s.expect("坏客户端 %s 被断开" % mode, r"skdbad\[%s\]: (disconnected by server|result: disconnected)" % mode, 10)
        s.mark()
        vm.run_serial("run /bin/skdbad manywin")
        s.expect("窗口太多 → 非致命 ERROR", r"skdbad\[manywin\]: got ERROR code=24 req=2 msg=窗口太多")
        s.expect("manywin 结束", r"skdbad\[manywin\]: done", 15)
        s.mark()
        vm.run_serial("run /bin/skdbad shrink")
        s.expect("F_SEAL_SHRINK 阻止缩小", r"skdbad\[shrink\]: ftruncate refused")
        s.expect("shrink 之后服务器正常", r"skdbad\[shrink\]: still alive")
        s.mark()
        vm.run_serial("run /bin/skdbad flood")
        s.expect("DAMAGE 洪水", r"skdbad\[flood\]: done", 30)
        time.sleep(0.5)
        s.shot("26-after-bad-clients")

        # 卡死的客户端 + 大量鼠标移动 → 事件积压 → 10 秒后被断开
        s.mark()
        vm.run_serial("run /bin/skdbad hang")
        hid2 = s.created("skdbad-hang", 1)
        hx_, hy_ = BAD_X + 180, BAD_Y + 110
        # 不加延时地狂发鼠标移动，填满它的套接字接收缓冲区（约几百条报文），
        # 之后移动事件在 sketchd 的队列里合并成一条；再按几个键（按键绝不丢弃，只能排队）
        for i in range(1500):
            vm._qmp_cmd("input-send-event", events=vm._abs(hx_ - 100 + (i % 2) * 200, hy_ + (i % 7) * 10 - 30))
        vm.mouse_move(hx_, hy_)
        vm.type_text("abc")
        s.expect("不读事件的客户端被断开", r"sketchd: window %d destroyed" % (hid2[0] if hid2 else -1), 25)
        vm.run_serial("kill 9 skdbad")

        s.absent("整个过程中 sketchd 没有崩溃重启", r"testinit: sketchd exited", start=0)
        out = vm.run_serial("ps")
        s.r.check("[main] sketchd 进程仍在", "sketchd" in out, "")

        # --- 关机确认框 ---
        s.mark()
        vm.click(*START_BTN)
        time.sleep(0.4)
        vm.click(*start_item(9))     # 7 个应用 + 分隔线 + 重新启动 + 关机 → 第 9 项（分隔线占一个序号）
        time.sleep(0.6)
        s.shot("27-confirm-poweroff")
        vm.click(*CONFIRM_CANCEL)
        time.sleep(0.5)
        s.absent("取消不会关机", r"testinit: got signal 12")
        vm.click(*START_BTN)
        time.sleep(0.4)
        vm.click(*start_item(9))
        time.sleep(0.5)
        vm.click(*CONFIRM_OK)
        s.expect("确认关机 → kill(1, SIGUSR2)", r"testinit: got signal 12 from pid \d+: poweroff request")
        time.sleep(0.3)
        vm.click(*START_BTN)
        time.sleep(0.4)
        vm.click(*start_item(8))
        time.sleep(0.5)
        s.shot("28-confirm-reboot")
        vm.key("ret")
        s.expect("确认重启 → kill(1, SIGTERM)", r"testinit: got signal 15 from pid \d+: reboot request")
        time.sleep(0.4)
        vm.key("ctrl-alt-delete")
        s.expect("Ctrl+Alt+Delete → kill(1, SIGINT)", r"testinit: got signal 2 from pid \d+: reboot \(ctrl-alt-del\)")
        time.sleep(0.5)

        # --- Ctrl+Alt+Backspace：退出并恢复文字控制台，sinit/testinit 重新拉起 ---
        s.mark()
        vm.key("ctrl-alt-backspace")
        s.expect("Ctrl+Alt+Backspace 退出（状态 0）", r"testinit: sketchd exited \(status 0\)")
        time.sleep(1.0)
        out = vm.run_serial("vt")
        s.r.check("[main] 退出后 VT 回到文字模式", "vt mode=text" in out, out.strip())
        s.r.check("[main] 键盘模式恢复（不是 K_OFF=4）", re.search(r"kbmode=[0-3]\b", out) is not None, out.strip())
        s.shot("29-text-console")
        s.expect("被重新拉起", r"sketchd: ready 1024x768 backend=kms", 30)
        time.sleep(0.5)
        vm.mouse_move(400, 300)
        time.sleep(0.3)
        s.shot("30-respawned")
        log = vm.run_serial("log")
        m = re.findall(r"comp: (\d+) 帧，平均 (\d+) us，最长 (\d+) us", log)
        s.r.check("[main] 调试模式记录了合成耗时", bool(m), str(m[-3:]))
    finally:
        vm.stop()


BAD_X, BAD_Y = 320, 180     # skdbad 的 hang/dragdie/crash 窗口用 SK_REQ_MOVE 移到这里


def test_fbdev(runner):
    vm, s = boot(runner, "fbdev", "sketch.backend=fbdev sketch.nosplash sketch.autostart=t1,about")
    try:
        s.expect("fbdev 后端 ready", r"sketchd: ready 1024x768 backend=fbdev mouse=vmmouse", 60)
        s.expect("自动启动 t1", r"sketchd: launch t1 pid=\d+")
        s.expect("自动启动 about", r"sketchd: launch about pid=\d+")
        s.created("t1", 2)
        s.created("about", 1)
        vm.mouse_move(600, 500)
        time.sleep(0.5)
        s.shot("01-fbdev-desktop")
        s.mark()
        vm.key("alt-f4")
        s.expect("fbdev 下键盘正常", r"skdtest\[about\]: close win=\d+")
        vm.key("ctrl-alt-backspace")
        s.expect("fbdev 退出", r"testinit: sketchd exited \(status 0\)")
        time.sleep(0.8)
        out = vm.run_serial("vt")
        s.r.check("[fbdev] 退出后 VT 回到文字模式", "vt mode=text" in out, out.strip())
        s.shot("02-fbdev-text-console")
    finally:
        vm.stop()


def test_fb16(runner):
    vm, s = boot(runner, "fb16", "sketch.backend=fbdev sketch.nosplash video=1024x768-16 sketch.autostart=t1")
    try:
        m = s.expect("16 位 fbdev ready", r"sketchd: ready \d+x\d+ backend=fbdev", 60)
        log = vm.run_serial("log")
        bpp = re.search(r"fbdev: \d+x\d+ (\d+)bpp", log)
        print("   fbdev 色深:", bpp.group(1) if bpp else "?")
        s.r.check("[fb16] 帧缓冲是 16 位 RGB565", bpp is not None and bpp.group(1) == "16",
                  bpp.group(0) if bpp else "没有 fbdev 日志")
        s.created("t1", 2)
        s.shot("01-fb16-desktop")
    finally:
        vm.stop()


def test_mode(runner):
    vm, s = boot(runner, "mode", "sketch.mode=800x600 sketch.nosplash sketch.autostart=t1")
    try:
        vm.screen_w, vm.screen_h = 800, 600
        s.expect("sketch.mode=800x600", r"sketchd: ready 800x600 backend=kms mouse=vmmouse", 60)
        s.created("t1", 2)
        s.mark()
        vm.click(200, 300)
        time.sleep(0.4)
        s.shot("01-800x600")
    finally:
        vm.stop()


def test_splash(runner):
    vm, s = boot(runner, "splash", "")
    try:
        s.expect("启动", r"testinit: started sketchd", 60)
        t0 = time.time()
        shots = []
        for i in range(5):
            time.sleep(0.25)
            shots.append(s.shot("%02d-splash" % i))
        m = s.expect("动画结束后 ready", r"sketchd: ready", 20)
        s.r.check("[splash] 动画大约 1.3 秒内结束", time.time() - t0 < 6, "%.2fs" % (time.time() - t0))
        same = len(set(open(p, "rb").read() for p in shots))
        s.r.check("[splash] 动画画面在变化", same >= 3, "%d 张不同的截图" % same)
    finally:
        vm.stop()


GROUPS = {"main": test_main, "fbdev": test_fbdev, "fb16": test_fb16, "mode": test_mode, "splash": test_splash}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--initrd", default=os.path.expanduser("~/sketchos-work/agents/sketchd/test.cpio.gz"))
    ap.add_argument("--out", default="/tmp/skd-shots")
    ap.add_argument("--only", default=",".join(GROUPS))
    a = ap.parse_args()
    r = Runner(a.out)
    r.initrd = a.initrd
    for g in a.only.split(","):
        print("\n######## %s ########" % g, flush=True)
        try:
            GROUPS[g](r)
        except Exception as e:  # noqa: BLE001
            r.check("[%s] 测试组异常" % g, False, repr(e))
    sys.exit(0 if r.summary() else 1)


if __name__ == "__main__":
    main()
