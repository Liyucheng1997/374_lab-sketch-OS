#!/usr/bin/env python3
"""
test_skpaint.py — 画板 skpaint 的自动化测试（tests/apps/run.sh skpaint 调用）

    python3 tests/apps/test_skpaint.py --bin <用户态 bin 目录> --out <截图目录> [--only pencil,undo]

画板用环境变量 SKPAINT_DEBUG=1 打开调试输出（见 userspace/gui/apps/skpaint/main.c 开头）：
    skpaint: ready {JSON}   启动完成：画布原点 ox/oy、图片尺寸、各控件的矩形……
    skpaint: state {JSON}   按 F12 时打印（用来和程序“对时”：它打印出来时，之前的事件都处理完了）
    skpaint: saved <路径> / opened <路径> / error <原因>
测试根据 ox/oy 把“图片坐标”换算成窗口坐标，再读窗口像素检查画出来的东西。
对话框（消息框、文件对话框）是单独的窗口：d.wait_window(count=2) 等它出现。
"""
import json
import queue
import struct

from apptest import App, check, md, os, run_tests, settle, shot, shutil, temp_home, time

# 调色盘（和 main.c 的 palette[] 一致）
BLACK, INK_SOFT, SHADOW, WHITE = (0, 0, 0), (0x5C, 0x5C, 0x5C), (0xB4, 0xB4, 0xB4), (255, 255, 255)
RED, ORANGE, YELLOW, GREEN = (0xE0, 0x60, 0x5A), (0xF2, 0x9E, 0x4C), (0xF2, 0xC9, 0x4C), (0x5B, 0xAA, 0x62)
TEAL, BLUE, PURPLE, PINK = (0x3F, 0xB0, 0xA8), (0x4A, 0x7B, 0xD0), (0x9B, 0x7B, 0xD4), (0xF2, 0x8D, 0xB2)
PALETTE = [BLACK, INK_SOFT, SHADOW, WHITE, RED, ORANGE, YELLOW, GREEN, TEAL, BLUE, PURPLE, PINK]
TOOLS = ["pencil", "brush", "eraser", "line", "rect", "ellipse", "fill"]
CURSOR_HAND, CURSOR_CROSS, CURSOR_PENCIL = 2, 3, 4


# ---------------------------------------------------------------- 像素快照
class Snap:
    """窗口内容的一次快照（一次读出整块缓冲区，逐点读太慢）"""

    def __init__(self, win):
        self.w, self.h, self.px = win.pixels()

    def get(self, x, y):
        if not (0 <= x < self.w and 0 <= y < self.h):
            return None
        o = (y * self.w + x) * 4
        return (self.px[o + 2], self.px[o + 1], self.px[o])

    def near(self, x, y, color, r=1, tol=0):
        """(x,y) 周围 (2r+1)² 个像素里有没有 color（tol = 每个分量允许的误差）"""
        for yy in range(y - r, y + r + 1):
            for xx in range(x - r, x + r + 1):
                p = self.get(xx, yy)
                if p and all(abs(p[k] - color[k]) <= tol for k in range(3)):
                    return True
        return False


def close(a, b, tol=0):
    return a is not None and all(abs(a[k] - b[k]) <= tol for k in range(3))


# ---------------------------------------------------------------- 被测程序
class Paint:
    def __init__(self, d, args, extra=(), home=None, expect_ready=True):
        self.d = d
        self.home = home or temp_home()
        os.makedirs(os.path.join(self.home, "图片"), exist_ok=True)
        self.app = App(d, [os.path.join(args.bin, "skpaint")] + list(extra),
                       env={"SKPAINT_DEBUG": "1", "HOME": self.home})
        self.win = d.wait_window(timeout=10, title="画板")
        line = self.app.expect(r"^skpaint: ready ", 10)
        if not line:
            raise RuntimeError("没有收到 ready")
        self.st = json.loads(line.split(" ", 2)[2])
        settle(d, self.win)

    def drain(self):
        while True:
            try:
                self.app.q.get_nowait()
            except queue.Empty:
                return

    def state(self):
        """按 F12 取最新状态；收到时之前的所有事件都已经处理完"""
        self.drain()
        self.d.key(self.win, "f12")
        line = self.app.expect(r"^skpaint: state ", 5)
        if not line:
            raise RuntimeError("没有收到 state")
        self.st = json.loads(line.split(" ", 2)[2])
        time.sleep(0.05)
        settle(self.d, self.win, quiet=0.15)
        return self.st

    def snap(self):
        return Snap(self.win)

    def at(self, ix, iy):
        """图片坐标 → 窗口坐标"""
        return self.st["ox"] + ix, self.st["oy"] + iy

    def img_px(self, snap, ix, iy):
        return snap.get(*self.at(ix, iy))

    def click_rect(self, r):
        self.d.click(self.win, r[0] + r[2] // 2, r[1] + r[3] // 2)

    def tool(self, name):
        self.click_rect(self.st["tools"][TOOLS.index(name)])

    def color(self, i):
        self.click_rect(self.st["swatches"][i])

    def drag(self, x1, y1, x2, y2, steps=10):
        a, b = self.at(x1, y1), self.at(x2, y2)
        self.d.drag(self.win, a[0], a[1], b[0], b[1], steps=steps)

    def set_size(self, n):
        cur = self.state()["size"]
        key = "rightbrace" if n > cur else "leftbrace"
        for _ in range(abs(n - cur)):
            self.d.key(self.win, key)
        return self.state()["size"]

    def set_rough(self, on):
        if self.state()["rough"] != on:
            self.click_rect(self.st["rough_box"])
        return self.state()["rough"]

    def stop(self, rm_home=True):
        self.app.stop()
        # mock_display.wait_window 发现任何一个启动过的程序非 0 退出就报错；
        # 被 SIGTERM 结束的程序是我们自己停的，从列表里拿掉，再等它的窗口消失
        if self.app.p in self.d.procs:
            self.d.procs.remove(self.app.p)
        t0 = time.time()
        while self.win.id in self.d.windows and time.time() - t0 < 5:
            time.sleep(0.02)
        if rm_home:
            shutil.rmtree(self.home, ignore_errors=True)


def line_points(x1, y1, x2, y2, n):
    return [(round(x1 + (x2 - x1) * i / n), round(y1 + (y2 - y1) * i / n)) for i in range(n + 1)]


def damages_since(d, start, win):
    out = []
    for req in list(d.requests[start:]):
        if req[2] != "DAMAGE":
            continue
        parts = req[3].replace("(", " ").replace(")", " ").replace(",", " ").replace("x", " ").split()
        # "win=1 (x,y wxh)" → ["win=1", x, y, w, h]
        if parts and parts[0] == "win=%d" % win.id:
            out.append(tuple(int(v) for v in parts[1:5]))
    return out


def parse_pnm(data):
    """最小的 P6 解析器（允许注释），返回 (w, h, maxval, 像素字节)"""
    assert data[:2] == b"P6", data[:2]
    pos, vals = 2, []
    while len(vals) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while data[pos:pos + 1].isdigit():
            pos += 1
        vals.append(int(data[start:pos]))
    pos += 1                       # 头部最后一个数后面恰好一个空白
    w, h, maxval = vals
    return w, h, maxval, data[pos:]


def compare_window_with_image(p, snap, w, h, raw):
    """窗口里画布区域的每个像素都和图片数据一致吗？返回不一致的像素数"""
    bad = 0
    ox, oy = p.st["ox"], p.st["oy"]
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 3
            if snap.get(ox + x, oy + y) != (raw[o], raw[o + 1], raw[o + 2]):
                bad += 1
    return bad


# ================================================================ 测试

def test_start(d, args):
    p = Paint(d, args)
    st = p.st
    check(p.win.title == "未命名 - 画板", "初始标题是“未命名 - 画板”: %s" % p.win.title)
    check(st["w"] == 640 and st["h"] == 420, "默认画布 640×420: %dx%d" % (st["w"], st["h"]))
    check(st["vis"] == [st["ox"], st["oy"], 640, 420], "默认窗口里整张画布都可见: %s" % st["vis"])
    check(st["tool"] == "pencil" and st["color"] == "#000000", "默认铅笔 + 黑色")
    s = p.snap()
    whites = sum(1 for y in range(0, 420, 20) for x in range(0, 640, 20) if p.img_px(s, x, y) == WHITE)
    check(whites == 32 * 21, "新画布是白的（%d/%d 个采样点）" % (whites, 32 * 21))
    # 画布外面是纸色（不是白色），画布左上角外有铅笔边框
    out = s.get(st["ox"] - 10, st["oy"] + 100)
    check(out is not None and out != WHITE and out[0] > 0xF0, "画布外面是纸: %s" % (out,))
    check(any(s.get(st["ox"] - k, st["oy"] + 200)[0] < 0xC0 for k in range(1, 5)), "画布有手绘边框")
    shot(d, p.win, args, "start")
    p.stop()


def test_pencil(d, args):
    p = Paint(d, args)
    p.color(4)                                   # 红
    st = p.state()
    check(st["color"] == "#E0605A", "点色块换成红色: %s" % st["color"])
    p.drag(40, 40, 240, 140, steps=20)
    st = p.state()
    s = p.snap()
    pts = line_points(40, 40, 240, 140, 200)
    hits = sum(1 for (x, y) in pts if s.near(*p.at(x, y), RED, r=1))
    check(hits == len(pts), "慢速拖动：线上每一点都是红色（%d/%d）" % (hits, len(pts)))
    check(p.img_px(s, 140, 120) == WHITE and p.img_px(s, 60, 120) == WHITE, "线外面还是白的")
    # 铅笔是 1 像素硬边：线附近只有纯红和纯白两种颜色
    colors = {p.img_px(s, x, y) for x in range(100, 140) for y in range(60, 100)}
    check(colors <= {RED, WHITE}, "铅笔线没有抗锯齿的中间色: %s" % (colors - {RED, WHITE}))
    check(st["modified"] == 1 and p.win.title.startswith("*"), "画过之后标题带 *: %s" % p.win.title)
    check(st["undo"] == 1, "一笔 = 一步撤销: %d" % st["undo"])

    # 快速拖动：两次移动事件就跨过 560 像素，中间不能有缺口
    p.drag(30, 400, 600, 250, steps=2)
    p.state()
    s = p.snap()
    pts = line_points(30, 400, 600, 250, 600)
    gaps = [(x, y) for (x, y) in pts if not s.near(*p.at(x, y), RED, r=1)]
    check(not gaps, "快速拖动没有缺口（缺 %d 处，例如 %s）" % (len(gaps), gaps[:3]))
    shot(d, p.win, args, "pencil")
    p.stop()


def test_brush_eraser(d, args):
    p = Paint(d, args)
    p.tool("brush")
    p.color(9)                                   # 蓝
    size = p.set_size(10)
    check(size == 10, "[ ] 键调粗细: %d" % size)
    p.drag(100, 100, 400, 100, steps=15)
    p.state()
    s = p.snap()
    check(p.img_px(s, 250, 100) == BLUE and p.img_px(s, 250, 103) == BLUE, "刷子中间是纯蓝色")
    check(p.img_px(s, 250, 109) == WHITE and p.img_px(s, 250, 91) == WHITE, "刷子宽度约 10 像素")
    edge = p.img_px(s, 250, 105)
    check(edge != WHITE, "刷子边缘有颜色: %s" % (edge,))
    check(p.img_px(s, 96, 100) == BLUE or p.img_px(s, 97, 100) == BLUE, "刷子是圆头（起点前也有颜色）")
    # 一个点：只点一下也留下一个圆点
    p.d.click(p.win, *p.at(500, 300))
    p.state()
    s = p.snap()
    check(p.img_px(s, 500, 300) == BLUE, "单击留下一个圆点")
    # 橡皮
    p.tool("eraser")
    p.drag(250, 60, 250, 140, steps=8)
    p.state()
    s = p.snap()
    check(p.img_px(s, 250, 100) == WHITE, "橡皮擦过的地方变白")
    check(p.img_px(s, 200, 100) == BLUE, "橡皮旁边的蓝色还在")
    shot(d, p.win, args, "brush-eraser")
    p.stop()


def test_shapes(d, args):
    p = Paint(d, args)
    # ---- 矩形（规整），拖动时先预览，松开才画进去
    p.tool("rect")
    check(p.set_rough(0) == 0, "关掉“手绘风格”")
    check(p.set_size(3) == 3, "粗细 3")
    p.color(7)                                   # 绿
    a, b = p.at(100, 80), p.at(300, 200)
    p.d.move(p.win, *a)
    p.d.mouse(p.win, md.MOUSE_DOWN, a[0], a[1], md.BTN_LEFT, md.BTN_LEFT, clicks=1)
    for i in range(1, 6):
        p.d.move(p.win, a[0] + (b[0] - a[0]) * i // 5, a[1] + (b[1] - a[1]) * i // 5, buttons=md.BTN_LEFT)
    st = p.state()
    s = p.snap()
    check(st["drag"] == 2 and st["undo"] == 0, "拖动中：还没有画进图片（undo=%d）" % st["undo"])
    check(p.img_px(s, 200, 80) == GREEN and p.img_px(s, 100, 140) == GREEN, "拖动中能看到矩形预览")
    shot(d, p.win, args, "rect-preview")
    p.d.mouse(p.win, md.MOUSE_UP, b[0], b[1], 0, md.BTN_LEFT)
    st = p.state()
    s = p.snap()
    check(st["undo"] == 1 and st["drag"] == 0, "松开后画进图片")
    for (x, y) in [(200, 80), (200, 200), (100, 140), (300, 140), (100, 80), (300, 200)]:
        check(p.img_px(s, x, y) == GREEN, "矩形边上 (%d,%d) 是绿色: %s" % (x, y, p.img_px(s, x, y)))
    check(p.img_px(s, 200, 140) == WHITE and p.img_px(s, 200, 70) == WHITE, "矩形里外都是白的")

    # ---- 油漆桶：填满矩形内部，不漏到外面
    p.tool("fill")
    p.color(6)                                   # 黄
    p.d.click(p.win, *p.at(200, 140))
    st = p.state()
    s = p.snap()
    inside = [p.img_px(s, x, y) for x in range(103, 298, 13) for y in range(83, 198, 11)]
    check(all(c == YELLOW for c in inside), "矩形内部全部填成黄色")
    check(p.img_px(s, 200, 80) == GREEN, "边框还是绿色")
    check(p.img_px(s, 50, 50) == WHITE and p.img_px(s, 400, 300) == WHITE, "填充没有漏到矩形外面")
    check(st["undo"] == 2, "填充是一步: %d" % st["undo"])
    # 在同样的颜色上再点一次：什么都不做，不产生撤销记录
    p.d.click(p.win, *p.at(200, 140))
    check(p.state()["undo"] == 2, "同色区域再填一次不算一步")
    # 大面积填充：外面的白色区域（绕过矩形，扫描线算法要处理“凹”进去的地方）
    p.color(11)                                  # 粉
    p.d.click(p.win, *p.at(10, 10))
    p.state()
    s = p.snap()
    check(p.img_px(s, 630, 410) == PINK and p.img_px(s, 200, 300) == PINK and p.img_px(s, 200, 30) == PINK,
          "外面的白色区域全部填成粉色")
    check(p.img_px(s, 200, 140) == YELLOW, "矩形里面的黄色不受影响")

    # ---- 椭圆（规整）
    p.tool("ellipse")
    p.color(10)                                  # 紫
    p.drag(350, 250, 550, 390, steps=8)
    p.state()
    s = p.snap()
    for (x, y) in [(350, 320), (550, 320), (450, 250), (450, 390)]:
        check(s.near(*p.at(x, y), PURPLE, r=2, tol=8), "椭圆经过 (%d,%d)" % (x, y))
    check(p.img_px(s, 450, 320) == PINK, "椭圆中间没有被画到")
    check(s.near(*p.at(379, 270), PURPLE, r=3, tol=8), "椭圆的斜向部分也画出来了")

    # ---- 手绘风格的直线和矩形
    p.tool("line")
    check(p.set_rough(1) == 1, "打开“手绘风格”")
    p.color(0)
    p.drag(40, 300, 300, 400, steps=6)
    p.state()
    s = p.snap()
    mid_ok = sum(1 for (x, y) in line_points(40, 300, 300, 400, 20)[2:-2] if s.near(*p.at(x, y), BLACK, r=4, tol=60))
    check(mid_ok >= 15, "手绘直线大致沿着拖动的方向（%d/17）" % mid_ok)
    p.tool("rect")
    p.color(9)
    p.drag(420, 40, 600, 200, steps=6)
    p.state()
    s = p.snap()
    edge_hits = sum(1 for x in range(440, 590, 10) if any(s.near(*p.at(x, y), BLUE, r=1, tol=60) for y in range(34, 47)))
    check(edge_hits >= 13, "手绘矩形的上边（%d/15）" % edge_hits)
    # Shift 约束：正方形
    p.tool("rect")
    p.set_rough(0)
    a, b = p.at(20, 20), p.at(80, 60)
    p.d.move(p.win, *a)
    p.d.mouse(p.win, md.MOUSE_DOWN, a[0], a[1], md.BTN_LEFT, md.BTN_LEFT, clicks=1, modifiers=md.MOD_SHIFT)
    p.d.mouse(p.win, md.MOUSE_MOVE, b[0], b[1], md.BTN_LEFT, modifiers=md.MOD_SHIFT)
    time.sleep(0.02)
    p.d.mouse(p.win, md.MOUSE_UP, b[0], b[1], 0, md.BTN_LEFT, modifiers=md.MOD_SHIFT)
    p.state()
    s = p.snap()
    check(p.img_px(s, 80, 50) == BLUE and p.img_px(s, 50, 80) == BLUE, "按住 Shift 画出正方形（60×60）")
    # Esc 取消正在拖的图形
    a, b = p.at(150, 300), p.at(250, 380)
    p.d.move(p.win, *a)
    p.d.mouse(p.win, md.MOUSE_DOWN, a[0], a[1], md.BTN_LEFT, md.BTN_LEFT, clicks=1)
    p.d.move(p.win, b[0], b[1], buttons=md.BTN_LEFT)
    undo_before = p.state()["undo"]
    p.d.key(p.win, "esc")
    p.d.mouse(p.win, md.MOUSE_UP, b[0], b[1], 0, md.BTN_LEFT)
    st = p.state()
    s = p.snap()
    check(st["undo"] == undo_before and p.img_px(s, 200, 300) == PINK, "Esc 取消了图形，预览也擦掉了")
    shot(d, p.win, args, "shapes")
    p.stop()


def test_undo(d, args):
    p = Paint(d, args)
    # 在白纸上用橡皮擦：像素没有变化，不算一步，也不算“改过”
    p.tool("eraser")
    p.drag(100, 100, 300, 200, steps=5)
    st = p.state()
    check(st["undo"] == 0 and st["modified"] == 0 and not p.win.title.startswith("*"),
          "在白纸上擦不产生撤销记录、不算修改: undo=%d %s" % (st["undo"], p.win.title))
    p.tool("pencil")
    p.color(0)
    for i in range(12):
        p.drag(50, 20 + i * 30, 590, 20 + i * 30, steps=4)
    st = p.state()
    check(st["undo"] == 12, "12 笔 = 12 步撤销: %d" % st["undo"])
    s = p.snap()
    check(all(p.img_px(s, 300, 20 + i * 30) == BLACK for i in range(12)), "12 条线都画上了")
    p.d.key(p.win, "ctrl+z")
    st = p.state()
    s = p.snap()
    check(p.img_px(s, 300, 20 + 11 * 30) == WHITE and p.img_px(s, 300, 20 + 10 * 30) == BLACK,
          "Ctrl+Z 撤销了最后一笔")
    check(st["undo"] == 11 and st["redo"] == 1, "撤销后 undo=11 redo=1: %d %d" % (st["undo"], st["redo"]))
    p.d.key(p.win, "ctrl+y")
    p.state()
    s = p.snap()
    check(p.img_px(s, 300, 20 + 11 * 30) == BLACK, "Ctrl+Y 重做")
    for _ in range(12):
        p.d.key(p.win, "ctrl+z")
    st = p.state()
    s = p.snap()
    left = sum(1 for i in range(12) if p.img_px(s, 300, 20 + i * 30) != WHITE)
    check(left == 0, "连续撤销 12 步，画布恢复全白（剩 %d 条）" % left)
    check(st["modified"] == 0 and not p.win.title.startswith("*"), "撤销回原样后不算修改: %s" % p.win.title)
    check(st["redo"] == 12, "可以重做 12 步: %d" % st["redo"])
    # “重做”按钮
    p.click_rect(st["redo_btn"])
    p.click_rect(st["redo_btn"])
    st = p.state()
    s = p.snap()
    check(p.img_px(s, 300, 20) == BLACK and p.img_px(s, 300, 50) == BLACK and p.img_px(s, 300, 80) == WHITE,
          "点“重做”按钮两次")
    # 新的一笔之后不能再重做
    p.drag(100, 400, 200, 400, steps=2)
    st = p.state()
    check(st["redo"] == 0 and st["undo"] == 3, "画了新的一笔，重做记录清空: undo=%d redo=%d" % (st["undo"], st["redo"]))
    p.click_rect(st["undo_btn"])
    p.state()
    s = p.snap()
    check(p.img_px(s, 150, 400) == WHITE, "点“撤销”按钮")
    # 油漆桶填满整张画布再撤销（大块撤销记录）
    p.tool("fill")
    p.color(5)
    p.d.click(p.win, *p.at(320, 200))
    p.state()
    s = p.snap()
    check(p.img_px(s, 600, 400) == ORANGE, "整张画布填成橙色")
    p.d.key(p.win, "ctrl+z")
    p.state()
    s = p.snap()
    check(p.img_px(s, 600, 400) == WHITE and p.img_px(s, 300, 20) == BLACK, "撤销整张填充")
    shot(d, p.win, args, "undo")
    p.stop()


def draw_sample(p):
    """画一张有几种颜色的小画（保存/打开测试用）"""
    p.color(4)
    p.drag(30, 30, 300, 200, steps=12)
    p.tool("brush")
    p.color(9)
    p.drag(320, 60, 600, 380, steps=12)
    p.tool("ellipse")
    p.color(7)
    p.drag(80, 250, 280, 390, steps=6)
    p.tool("fill")
    p.color(6)
    p.d.click(p.win, *p.at(180, 320))
    p.tool("pencil")
    return p.state()


def test_save_open(d, args):
    p = Paint(d, args)
    draw_sample(p)
    p.drain()
    p.d.key(p.win, "ctrl+s")
    dlg = d.wait_window(10, count=2)
    settle(d, dlg)
    check(dlg.title == "保存图片", "Ctrl+S 弹出保存对话框: %s" % dlg.title)
    shot(d, dlg, args, "save-dialog")
    d.key(dlg, "enter")                          # 默认位置 ~/图片，默认名字 画.ppm
    line = p.app.expect(r"^skpaint: saved ", 10)
    path = os.path.join(p.home, "图片", "画.ppm")
    check(line and line.endswith(path), "保存到 ~/图片/画.ppm: %s" % line)
    st = p.state()
    check(p.win.title == "画.ppm - 画板" and st["modified"] == 0, "保存后标题: %s" % p.win.title)
    leftovers = [f for f in os.listdir(os.path.dirname(path)) if ".tmp" in f]
    check(not leftovers, "没有留下临时文件: %s" % leftovers)
    data = open(path, "rb").read()
    w, h, maxval, raw = parse_pnm(data)
    check((w, h, maxval) == (640, 420, 255) and len(raw) == 640 * 420 * 3,
          "文件是 P6 640×420 maxval 255: %s 数据 %d 字节" % ((w, h, maxval), len(raw)))
    s = p.snap()
    bad = compare_window_with_image(p, s, w, h, raw)
    check(bad == 0, "文件里的每个像素都和屏幕上的画一致（不一致 %d 个）" % bad)
    colors = {(raw[o], raw[o + 1], raw[o + 2]) for o in range(0, len(raw), 3 * 97)}
    check({RED, BLUE, GREEN, YELLOW, WHITE} <= colors, "文件里有画上去的几种颜色")
    # 再画一笔，Ctrl+S 直接保存（不再弹对话框）
    p.color(0)
    p.drag(10, 410, 630, 410, steps=3)
    p.d.key(p.win, "ctrl+s")
    line = p.app.expect(r"^skpaint: saved ", 5)
    check(line is not None and len(d.windows) == 1, "已有文件名时 Ctrl+S 直接保存")
    raw2 = parse_pnm(open(path, "rb").read())[3]
    o = (410 * 640 + 320) * 3
    check(tuple(raw2[o:o + 3]) == BLACK, "第二次保存写进了新的一笔")
    p.stop(rm_home=False)

    # 用这个文件作为参数启动：画面一模一样
    q = Paint(d, args, extra=[path], home=p.home)
    st = q.st
    check(q.win.title == "画.ppm - 画板", "打开文件后的标题: %s" % q.win.title)
    check(st["w"] == 640 and st["h"] == 420 and st["modified"] == 0 and st["path"] == path, "打开的状态: %s" % st)
    s = q.snap()
    bad = compare_window_with_image(q, s, 640, 420, raw2)
    check(bad == 0, "重新打开后屏幕上的每个像素和文件一致（不一致 %d 个）" % bad)
    shot(d, q.win, args, "reopened")

    # Ctrl+O 用文件对话框打开另一张图（P3 文字格式、带注释、maxval 15）
    other = os.path.join(q.home, "图片", "小图.ppm")
    with open(other, "w") as f:
        f.write("P3\n# 一张 3x2 的小图\n3 2\n15\n15 0 0   0 15 0   0 0 15\n# 第二行\n15 15 15 0 0 0 7 7 7\n")
    q.drain()
    q.d.key(q.win, "ctrl+o")
    dlg = d.wait_window(10, count=2)
    settle(d, dlg)
    check(dlg.title == "打开图片", "Ctrl+O 弹出打开对话框: %s" % dlg.title)
    d.click(dlg, 300, 16 + 17)                   # 路径框
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, other)
    d.key(dlg, "enter")
    line = q.app.expect(r"^skpaint: opened ", 10)
    check(line is not None and line.endswith(other), "打开了小图: %s" % line)
    st = q.state()
    s = q.snap()
    got = [q.img_px(s, x, y) for y in range(2) for x in range(3)]
    want = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255), (0, 0, 0), (119, 119, 119)]
    check(got == want, "P3 + 注释 + maxval 15 按比例换算: %s" % got)
    check(st["w"] == 3 and st["h"] == 2 and q.win.title == "小图.ppm - 画板", "打开后画布变成 3×2")
    q.stop()


def make_bad_files(dirpath):
    files = {
        "截断.ppm": b"P6\n64 48\n255\n" + b"\x80" * 1000,
        "不是图片.ppm": "你好，这是一个文本文件。\n".encode(),
        "巨大.ppm": b"P6\n100000 100000\n255\n",
        "零宽.ppm": b"P6 0 10 255\n",
        "坏头.ppm": b"P6\n64 abc\n255\n",
        "空文件.ppm": b"",
        "坏像素.ppm": b"P3\n2 1\n255\n1 2 3 4 x 6\n",
        "超长数字.ppm": b"P6 99999999999999999999 1 255\n",
    }
    paths = []
    for name, data in files.items():
        path = os.path.join(dirpath, name)
        with open(path, "wb") as f:
            f.write(data)
        paths.append(path)
    os.makedirs(os.path.join(dirpath, "文件夹.ppm"), exist_ok=True)
    paths.append(os.path.join(dirpath, "文件夹.ppm"))
    return paths


def test_bad_files(d, args):
    home = temp_home()
    pics = os.path.join(home, "图片")
    os.makedirs(pics)
    for i, path in enumerate(make_bad_files(pics)):
        p = Paint(d, args, extra=[path], home=home)
        err = p.app.expect(r"^skpaint: error ", 10)
        dlg = d.wait_window(10, count=2)
        settle(d, dlg)
        check(dlg.title == "打开失败" and err is not None,
              "%s：弹出错误对话框（%s）" % (os.path.basename(path), err and err[15:]))
        if i == 0:
            shot(d, dlg, args, "bad-file-dialog")
            shot(d, p.win, args, "bad-file-main")
        time.sleep(0.2)
        check(p.app.alive(), "%s：程序没有崩溃" % os.path.basename(path))
        d.key(dlg, "enter")
        t0 = time.time()
        while len(d.windows) > 1 and time.time() - t0 < 5:
            time.sleep(0.05)
        st = p.state()
        check(p.app.alive() and len(d.windows) == 1 and st["w"] == 640 and st["path"] == "",
              "%s：关掉对话框后照常使用空白画布" % os.path.basename(path))
        p.stop(rm_home=False)
    # 运行中通过 Ctrl+O 打开坏文件：原来的画保留
    p = Paint(d, args, home=home)
    p.drag(100, 100, 200, 100, steps=2)
    p.drain()
    p.d.key(p.win, "ctrl+o")
    dlg = d.wait_window(10, count=2)
    check(dlg.title == "画板", "有未保存的改动：先问要不要保存: %s" % dlg.title)
    d.key(dlg, "tab")
    d.key(dlg, "tab")
    d.key(dlg, "enter")                          # 不保存
    dlg = d.wait_window(10, title="打开图片")
    settle(d, dlg)
    d.click(dlg, 300, 16 + 17)
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, os.path.join(pics, "截断.ppm"))
    d.key(dlg, "enter")
    err = p.app.expect(r"^skpaint: error ", 10)
    check(err is not None and "不完整" in err, "截断的文件：说明“数据不完整”: %s" % err)
    dlg = d.wait_window(10, title="打开失败")
    d.key(dlg, "enter")
    st = p.state()
    s = p.snap()
    check(p.img_px(s, 150, 100) == BLACK and st["modified"] == 1, "打开失败后原来的画还在")
    p.stop()


def test_formats(d, args):
    """P1/P2/P4/P5、16 位 P6 都能读"""
    home = temp_home()
    pics = os.path.join(home, "图片")
    os.makedirs(pics)
    cases = [
        ("a.pbm", b"P1\n# pbm\n4 2\n0101\n1 0 1 0\n", [WHITE, BLACK, WHITE, BLACK, BLACK, WHITE, BLACK, WHITE]),
        ("b.pbm", b"P4\n4 2\n" + bytes([0b10010000, 0b01100000]), [BLACK, WHITE, WHITE, BLACK, WHITE, BLACK, BLACK, WHITE]),
        ("c.pgm", b"P2 4 2 100 0 50 100 25 100 100 0 0", [(0,) * 3, (128,) * 3, (255,) * 3, (64,) * 3,
                                                           (255,) * 3, (255,) * 3, (0,) * 3, (0,) * 3]),
        ("d.pgm", b"P5\n4 2\n255\n" + bytes([0, 10, 20, 30, 40, 50, 60, 255]),
         [(v, v, v) for v in (0, 10, 20, 30, 40, 50, 60, 255)]),
        ("e.ppm", b"P6\n2 1\n65535\n" + struct.pack(">6H", 65535, 0, 32768, 0, 65535, 257),
         [(255, 0, 128), (0, 255, 1)]),
    ]
    for name, data, want in cases:
        path = os.path.join(pics, name)
        with open(path, "wb") as f:
            f.write(data)
        p = Paint(d, args, extra=[path], home=home)
        st = p.st
        s = p.snap()
        got = [p.img_px(s, x, y) for y in range(st["h"]) for x in range(st["w"])]
        check(got == want, "%s 读对了: %s" % (name, got))
        p.stop(rm_home=False)
    shutil.rmtree(home, ignore_errors=True)


def test_close_prompt(d, args):
    p = Paint(d, args)
    # 没改过：直接关
    p.d.close(p.win)
    check(p.app.wait(5) == 0, "没改过的画直接关闭，退出码 0")
    p.stop()

    p = Paint(d, args)
    p.drag(100, 100, 300, 300, steps=5)
    p.state()
    d.close(p.win)
    dlg = d.wait_window(10, count=2)
    settle(d, dlg)
    check(dlg.title == "画板", "关闭时问要不要保存: %s" % dlg.title)
    shot(d, dlg, args, "close-prompt")
    d.key(dlg, "esc")                            # 取消
    time.sleep(0.3)
    check(p.app.alive() and len(d.windows) == 1, "取消 → 不关闭")
    d.close(p.win)
    dlg = d.wait_window(10, count=2)
    d.key(dlg, "tab")
    d.key(dlg, "tab")
    d.key(dlg, "enter")                          # 第二个按钮“不保存”
    check(p.app.wait(5) == 0, "不保存 → 关闭，退出码 0")
    check(not os.listdir(os.path.join(p.home, "图片")), "不保存就真的没写文件")
    p.stop()

    # 选“保存”：弹出文件对话框，保存后关闭
    p = Paint(d, args)
    p.drag(100, 100, 300, 300, steps=5)
    p.state()
    d.close(p.win)
    dlg = d.wait_window(10, count=2)
    d.key(dlg, "enter")                          # 保存
    fdlg = d.wait_window(10, title="保存图片")
    settle(d, fdlg)
    d.key(fdlg, "ctrl+a")
    d.type_text(fdlg, "关闭前")                   # 不写扩展名：自动加 .ppm
    d.key(fdlg, "enter")
    check(p.app.wait(5) == 0, "保存之后关闭")
    path = os.path.join(p.home, "图片", "关闭前.ppm")
    check(os.path.exists(path) and parse_pnm(open(path, "rb").read())[:2] == (640, 420),
          "自动加上 .ppm 并保存: %s" % os.listdir(os.path.join(p.home, "图片")))
    p.stop()


def test_new(d, args):
    p = Paint(d, args)
    p.drag(100, 100, 300, 300, steps=5)
    st = p.state()
    p.drain()
    p.click_rect(st["new"])
    dlg = d.wait_window(10, count=2)
    d.key(dlg, "tab")
    d.key(dlg, "tab")
    d.key(dlg, "enter")                          # 不保存
    t0 = time.time()
    while len(d.windows) > 1 and time.time() - t0 < 5:
        time.sleep(0.05)
    st = p.state()
    s = p.snap()
    check(p.img_px(s, 200, 200) == WHITE and st["modified"] == 0 and st["undo"] == 0,
          "新建：空白画布、没有撤销记录")
    check(p.win.title == "未命名 - 画板", "新建后的标题: %s" % p.win.title)
    p.stop()


def test_damage(d, args):
    p = Paint(d, args)
    win = p.win
    st = p.state()
    a = p.at(200, 200)
    d.move(win, *a)
    time.sleep(0.2)
    start = len(d.requests)
    d.mouse(win, md.MOUSE_DOWN, a[0], a[1], md.BTN_LEFT, md.BTN_LEFT, clicks=1)
    for i in range(1, 21):
        d.move(win, a[0] + i * 3, a[1] + i, buttons=md.BTN_LEFT)
    time.sleep(0.3)
    rects = damages_since(d, start, win)
    check(rects, "画的时候有 damage")
    biggest = max((r[2] * r[3] for r in rects), default=0)
    check(biggest <= 70 * 30, "铅笔画线时每次只 damage 笔画附近（最大 %d 像素）: %s" % (biggest, rects[:6]))
    vis = st["vis"]
    check(all(vis[0] <= r[0] and r[0] + r[2] <= vis[0] + vis[2] and vis[1] <= r[1] and r[1] + r[3] <= vis[1] + vis[3]
              for r in rects), "damage 都在画布范围内")
    d.mouse(win, md.MOUSE_UP, a[0] + 60, a[1] + 20, 0, md.BTN_LEFT)
    # 粗刷子：damage 跟着笔画走，不是整个窗口
    p.tool("brush")
    p.set_size(20)
    a = p.at(100, 300)
    d.move(win, *a)
    time.sleep(0.2)
    start = len(d.requests)
    d.mouse(win, md.MOUSE_DOWN, a[0], a[1], md.BTN_LEFT, md.BTN_LEFT, clicks=1)
    for i in range(1, 11):
        d.move(win, a[0] + i * 5, a[1], buttons=md.BTN_LEFT)
    time.sleep(0.3)
    rects = damages_since(d, start, win)
    total = sum(r[2] * r[3] for r in rects)
    check(rects and max(r[3] for r in rects) <= 30 and total < 40 * 100 * 4,
          "刷子 damage 只覆盖笔画（%d 块，合计 %d 像素）: %s" % (len(rects), total, rects[:4]))
    d.mouse(win, md.MOUSE_UP, a[0] + 50, a[1], 0, md.BTN_LEFT)
    # 拖动图形预览：damage 只是图形的包围盒
    p.tool("rect")
    p.set_rough(0)
    p.set_size(2)
    a = p.at(300, 100)
    d.move(win, *a)
    time.sleep(0.2)
    start = len(d.requests)
    d.mouse(win, md.MOUSE_DOWN, a[0], a[1], md.BTN_LEFT, md.BTN_LEFT, clicks=1)
    for i in range(1, 6):
        d.move(win, a[0] + i * 10, a[1] + i * 8, buttons=md.BTN_LEFT)
    time.sleep(0.3)
    rects = damages_since(d, start, win)
    check(rects and all(r[2] <= 70 and r[3] <= 60 for r in rects), "矩形预览只重画包围盒: %s" % rects[:5])
    d.mouse(win, md.MOUSE_UP, a[0] + 50, a[1] + 40, 0, md.BTN_LEFT)
    # 鼠标在画布上移动（不按键）：不重画任何东西
    time.sleep(0.3)
    start = len(d.requests)
    for i in range(10):
        d.move(win, 300 + i * 7, 300 + i * 3)
    time.sleep(0.3)
    rects = damages_since(d, start, win)
    check(not rects, "只移动鼠标不产生 damage: %s" % rects[:3])
    p.stop()


def test_cursor(d, args):
    p = Paint(d, args)
    win = p.win
    d.move(win, *p.at(300, 200))
    time.sleep(0.3)
    check(win.cursor == CURSOR_PENCIL, "鼠标在画布上显示铅笔光标: %d" % win.cursor)
    p.drag(300, 200, 350, 220, steps=3)
    time.sleep(0.3)
    check(win.cursor == CURSOR_PENCIL, "画完一笔光标还是铅笔: %d" % win.cursor)
    r = p.st["new"]
    d.move(win, r[0] + r[2] // 2, r[1] + r[3] // 2)
    time.sleep(0.3)
    check(win.cursor == CURSOR_HAND, "移到按钮上变成小手: %d" % win.cursor)
    p.tool("ellipse")
    d.move(win, *p.at(100, 100))
    time.sleep(0.3)
    check(win.cursor == CURSOR_CROSS, "画图形时是十字光标: %d" % win.cursor)
    p.stop()


def test_big_image(d, args):
    """比窗口大的图片：窗口不超过屏幕，出现滚动条，滚动后画的位置仍然正确"""
    home = temp_home()
    os.makedirs(os.path.join(home, "图片"))
    path = os.path.join(home, "图片", "大图.ppm")
    W, H = 1200, 900
    rows = bytearray()
    for y in range(H):
        for x in range(W):
            rows += bytes(((x * 255) // W, (y * 255) // H, 160))
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H) + bytes(rows))
    p = Paint(d, args, extra=[path], home=home)
    st = p.st
    check(p.win.w <= 1024 - 60 and p.win.h <= 768 - 100, "窗口不超过屏幕: %dx%d" % (p.win.w, p.win.h))
    check(st["bars"] == [1, 1], "横竖都有滚动条: %s" % st["bars"])
    port, vis = st["port"], st["vis"]
    check(vis[0] >= port[0] and vis[1] >= port[1] and vis[0] + vis[2] <= port[0] + port[2]
          and vis[1] + vis[3] <= port[1] + port[3], "图片被裁剪在显示区域里: vis=%s port=%s" % (vis, port))
    s = p.snap()
    check(close(p.img_px(s, 100, 100), (21, 28, 160), 1), "左上角的像素: %s" % (p.img_px(s, 100, 100),))
    shot(d, p.win, args, "big-image")
    # 滚轮向下滚
    d.wheel(p.win, port[0] + 200, port[1] + 200, -3)
    st2 = p.state()
    check(st2["scroll"][1] == 144 and st2["oy"] == st["oy"] - 144, "滚轮滚动 3 格: %s" % st2["scroll"])
    d.mouse(p.win, md.MOUSE_WHEEL, port[0] + 200, port[1] + 200, wheel=-2, modifiers=md.MOD_SHIFT)
    st2 = p.state()
    check(st2["scroll"][0] == 96, "Shift+滚轮横向滚动: %s" % st2["scroll"])
    s = p.snap()
    check(close(p.img_px(s, 300, 400), ((300 * 255) // W, (400 * 255) // H, 160), 0), "滚动后画面跟着移动")
    # 拖动竖滚动条到底
    vx = port[0] + port[2] + 7
    d.drag(p.win, vx, port[1] + 30, vx, port[1] + port[3] + 50, steps=5)
    st2 = p.state()
    check(st2["scroll"][1] == H + 40 - port[3], "拖滚动条到底: %s" % st2["scroll"])
    # 滚动之后画一笔：画在鼠标所在的位置
    wx, wy = port[0] + 150, port[1] + 120
    p.color(0)
    d.click(p.win, wx, wy)
    st2 = p.state()
    s = p.snap()
    check(s.get(wx, wy) == BLACK, "滚动后在鼠标位置画点")
    ix, iy = wx - st2["ox"], wy - st2["oy"]
    want = (150 + st2["scroll"][0] - 20, 120 + st2["scroll"][1] - 20)     # 20 = 画布四周的留白
    check((ix, iy) == want and iy > H // 2, "点画在图片的 %s（应为 %s）" % ((ix, iy), want))
    shot(d, p.win, args, "big-image-scrolled")
    # 把窗口缩小
    d.resize(p.win, 620, 500)
    st3 = p.state()
    s = p.snap()
    check(st3["port"][2] == 620 - 112 - 14 and s.get(wx - st2["ox"] + st3["ox"], wy - st2["oy"] + st3["oy"]) in (BLACK, None),
          "缩小窗口后重新布局: port=%s" % st3["port"])
    shot(d, p.win, args, "big-image-small-window")
    p.stop()


def test_demo(d, args):
    """用默认的“手绘风格”画一幅小画（房子、太阳、草地），截图看效果"""
    p = Paint(d, args)
    check(p.st["rough"] == 1, "默认打开手绘风格")
    p.tool("rect")
    p.set_size(4)
    p.color(1)
    p.drag(160, 190, 360, 350, steps=6)          # 房子
    p.tool("line")
    p.color(4)
    p.drag(140, 200, 260, 110, steps=4)          # 屋顶
    p.drag(260, 110, 380, 200, steps=4)
    p.tool("rect")
    p.color(9)
    p.drag(230, 270, 290, 350, steps=4)          # 门
    p.tool("ellipse")
    p.color(5)
    p.set_size(5)
    # 太阳：拖动中截一张，松开后再截一张——手绘图形的预览和定稿必须一个像素都不差
    a, b = p.at(470, 40), p.at(580, 150)
    p.d.move(p.win, *a)
    p.d.mouse(p.win, md.MOUSE_DOWN, a[0], a[1], md.BTN_LEFT, md.BTN_LEFT, clicks=1)
    for i in range(1, 5):
        p.d.move(p.win, a[0] + (b[0] - a[0]) * i // 4, a[1] + (b[1] - a[1]) * i // 4, buttons=md.BTN_LEFT)
    p.state()
    pre = p.snap()
    p.d.mouse(p.win, md.MOUSE_UP, b[0], b[1], 0, md.BTN_LEFT)
    p.state()
    post = p.snap()
    diff = sum(1 for y in range(20, 170) for x in range(450, 600) if p.img_px(pre, x, y) != p.img_px(post, x, y))
    check(diff == 0, "手绘椭圆：预览和画进去的结果完全一样（不同 %d 个像素）" % diff)
    p.tool("fill")
    p.color(6)
    p.d.click(p.win, *p.at(525, 95))
    p.tool("fill")
    p.color(8)
    p.d.click(p.win, *p.at(260, 310))            # 门涂成青色
    p.tool("brush")
    p.color(7)
    p.set_size(12)
    p.drag(20, 375, 620, 380, steps=12)          # 草地
    p.tool("pencil")
    p.color(0)
    for i in range(5):                           # 几只小鸟
        x = 80 + i * 40
        p.drag(x, 80 + (i % 2) * 15, x + 8, 74 + (i % 2) * 15, steps=2)
        p.drag(x + 8, 74 + (i % 2) * 15, x + 16, 80 + (i % 2) * 15, steps=2)
    st = p.state()
    s = p.snap()
    check(p.img_px(s, 525, 95) == YELLOW, "手绘椭圆里面填成黄色")
    check(p.img_px(s, 470, 200) == WHITE and p.img_px(s, 620, 20) == WHITE, "填充没有从手绘椭圆漏出去")
    check(p.img_px(s, 260, 310) == TEAL and p.img_px(s, 200, 300) == WHITE, "门填成青色，没漏到房子里")
    check(st["undo"] >= 10, "每个图形都是一步撤销: %d" % st["undo"])
    shot(d, p.win, args, "demo")
    p.stop()


def test_new_file_arg(d, args):
    """参数是一个还不存在的文件：空白画布，保存时直接写到这个路径"""
    home = temp_home()
    os.makedirs(os.path.join(home, "图片"))
    path = os.path.join(home, "图片", "新画.ppm")
    p = Paint(d, args, extra=[path], home=home)
    check(p.win.title == "新画.ppm - 画板", "标题用参数里的文件名: %s" % p.win.title)
    p.drag(10, 10, 100, 100, steps=3)
    p.d.key(p.win, "ctrl+s")
    line = p.app.expect(r"^skpaint: saved ", 5)
    check(line is not None and os.path.exists(path), "Ctrl+S 直接保存到参数给的路径")
    p.stop()


if __name__ == "__main__":
    run_tests([
        ("start", test_start),
        ("pencil", test_pencil),
        ("brush", test_brush_eraser),
        ("shapes", test_shapes),
        ("undo", test_undo),
        ("save", test_save_open),
        ("badfile", test_bad_files),
        ("formats", test_formats),
        ("close", test_close_prompt),
        ("new", test_new),
        ("damage", test_damage),
        ("cursor", test_cursor),
        ("big", test_big_image),
        ("newfile", test_new_file_arg),
        ("demo", test_demo),
    ])
