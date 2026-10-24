#!/usr/bin/env python3
"""
test_skabout.py — “关于本机” skabout 的自动化测试（不需要 QEMU）

skabout 在宿主机上直接运行，所以它显示的内核版本、主机名、处理器、内存都是宿主机的——
正好拿 Python 自己读到的值来核对。/etc/os-release 用环境变量 SKABOUT_OS_RELEASE 指向
仓库里的 rootfs/etc/os-release（宿主机的 os-release 是别的发行版）。

被测程序在 SKABOUT_DEBUG=1 时，布局、点击之后打印一行 “SKABOUT {JSON}”：系统信息、
布局模式（并排 / 标签页）、图里每个方块的位置、便签上显示的是什么。各项检查：
    render     系统信息与 uname / /proc 一致；四层都用各自的蜡笔颜色画了排线；截图
    click      点芯片、点一层、点箭头，便签跟着变；只重画小块区域
    idle       画完之后没有任何重画（没有不必要的定时器）
    resize     窄窗口变成两个标签页；放不下便签时点击改用消息框；再拉宽恢复并排
    fallback   没有 os-release 时用编译进程序的版本号
    python3 tests/apps/test_skabout.py --bin <bin 目录> --out <截图目录>
"""
import json

from apptest import check, App, settle, shot, pixel, run_tests, time, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OS_RELEASE = os.path.join(ROOT, "rootfs", "etc", "os-release")

# 四层的蜡笔颜色（theme.h：SK_YELLOW、SK_BLUE、SK_GREEN、SK_ORANGE）
LAYER_COLORS = [(0xF2, 0xC9, 0x4C), (0x4A, 0x7B, 0xD0), (0x5B, 0xAA, 0x62), (0xF2, 0x9E, 0x4C)]


class About(App):
    def state(self, pred=None, why=None, timeout=6.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            line = self.expect(r"^SKABOUT ", timeout=max(0.05, deadline - time.time()))
            if not line:
                break
            obj = json.loads(line[8:])
            last = obj
            if why and obj["why"] != why:
                continue
            if pred is None or pred(obj):
                return obj
        raise AssertionError("等待 skabout 状态超时（why=%s），最后一行: %s" % (why, str(last)[:300]))


def start(d, args, os_release=OS_RELEASE):
    app = About(d, [os.path.join(args.bin, "skabout")],
                env={"SKABOUT_DEBUG": "1", "SKABOUT_OS_RELEASE": os_release})
    win = d.wait_window(timeout=10, title="关于本机")
    st = app.state(why="layout")
    settle(d, win)
    return app, win, st


def center(r):
    return r[0] + r[2] // 2, r[1] + r[3] // 2


def damages_since(d, start_idx, win):
    out = []
    for req in list(d.requests[start_idx:]):
        if req[2] != "DAMAGE":
            continue
        m = re.match(r"win=(\d+) \((-?\d+),(-?\d+) (\d+)x(\d+)\)", req[3])
        if m and int(m.group(1)) == win.id:
            out.append(tuple(int(v) for v in m.groups()[1:]))
    return out


PAPER = (0xFF, 0xFD, 0xF7)


def tint_counts(win, rect):
    """rect 里被各层蜡笔颜色“染过”的像素个数（每隔 2 像素取样），返回 4 个数。
    排线是半透明、抗锯齿的细线，像素值是纸色和蜡笔色的混合：所以不比颜色本身，
    而是看“偏离纸色的方向”最像哪一种蜡笔（余弦 > 0.95），并且偏离得足够多；灰色的像素不算。"""
    refs = []
    for col in LAYER_COLORS:
        d = [p - c for p, c in zip(PAPER, col)]
        n = sum(v * v for v in d) ** 0.5
        refs.append([v / n for v in d])
    counts = [0] * len(refs)
    x0, y0, w, h = rect
    for y in range(y0 + 2, y0 + h - 2, 2):
        for x in range(x0 + 2, x0 + w - 2, 2):
            px = pixel(win, x, y)
            if max(px) - min(px) < 40:       # 灰色（铅笔线、文字、纸）不算
                continue
            d = [p - c for p, c in zip(PAPER, px)]
            nd = sum(v * v for v in d) ** 0.5
            if nd <= 25:
                continue
            cos = [sum(a * b for a, b in zip(d, r)) / nd for r in refs]
            best = max(range(len(refs)), key=lambda i: cos[i])
            if cos[best] > 0.95:
                counts[best] += 1
    return counts


def cpu_model():
    model, count = None, 0
    for line in open("/proc/cpuinfo"):
        if ":" not in line:
            continue
        k, v = (s.strip() for s in line.split(":", 1))
        if k == "processor":
            count += 1
        elif k == "model name" and model is None:
            model = re.sub(r" {2,}", " ", v)
    return model, count


def parse_size(text):
    m = re.match(r"([\d.]+) (B|KB|MB|GB)", text)
    if not m:
        return None
    return float(m.group(1)) * {"B": 1, "KB": 1024, "MB": 1024 ** 2, "GB": 1024 ** 3}[m.group(2)]


# ================================================================ 测试

def test_render(d, args):
    app, win, st = start(d, args)
    u = os.uname()
    info = st["info"]
    check(st["wide"] == 1 and st["diagram_visible"] and st["info_visible"], "默认大小：图和信息并排显示")
    check(st["release"] == u.release, "内核版本来自 uname(): %s" % st["release"])
    check(u.release in info["内核"] and info["内核"].startswith("Linux"), "“内核”一行: %s" % info["内核"])
    check(st["hostname"] == u.nodename and info["主机名"] == u.nodename, "主机名: %s" % info["主机名"])
    check(st["os_name"] == "SketchOS" and info["系统版本"] == "1.1 (铅笔)", "系统名称和版本来自 os-release: %s %s" %
          (st["os_name"], info["系统版本"]))
    check(st["os_pretty"] == "手绘操作系统 1.1", "副标题是去掉系统名的 PRETTY_NAME: %s" % st["os_pretty"])
    model, count = cpu_model()
    if model:
        check(model in info["处理器"], "处理器型号来自 /proc/cpuinfo: %s" % info["处理器"])
    if count > 1:
        check(info["处理器"].startswith("%d × " % count), "处理器个数: %s" % info["处理器"])
    total = int(open("/proc/meminfo").readline().split()[1]) * 1024
    size = parse_size(info["内存"])
    check(size and abs(size - total) / total < 0.01, "内存总量与 MemTotal 一致: %s" % info["内存"])
    up = float(open("/proc/uptime").read().split()[0])
    m = re.match(r"(?:(\d+) 天 )?(?:(\d+) 小时 )?(\d+) 分钟", info["已运行"])
    if up >= 60:
        mins = int(m.group(1) or 0) * 1440 + int(m.group(2) or 0) * 60 + int(m.group(3)) if m else -1
        check(abs(mins - up / 60) <= 1.5, "开机时长与 /proc/uptime 一致: %s" % info["已运行"])
    check(info["屏幕"] == "1024 × 768 像素", "屏幕大小来自显示服务器: %s" % info["屏幕"])
    check(st["note_on"] == 1 and st["selected"] == "点一点上面的方块", "便签显示提示")
    check(st["info_h"] <= st["infopanel"][3] and st["info_compact"] == 0,
          "信息栏内容（含来源说明）放得下: %d <= %d" % (st["info_h"], st["infopanel"][3]))
    # 四层都画了各自颜色的排线；彼此不重叠，都在图的范围里
    for i, r in enumerate(st["layers"]):
        counts = tint_counts(win, r)
        n, others = counts[i], max(c for k, c in enumerate(counts) if k != i)
        check(n > 150 and n > 2 * others, "第 %d 层用它自己的蜡笔颜色画了排线（%d 个取样点，其它颜色最多 %d）" %
              (i + 1, n, others))
    for a, b in zip(st["layers"], st["layers"][1:]):
        check(a[1] + a[3] <= b[1], "相邻两层不重叠")
    dg = st["diagram"]
    check(st["note"][1] + st["note"][3] <= dg[1] + dg[3] + 1, "便签在图的范围里")
    for name, r in st["chips"].items():
        layer = next(L for L in st["layers"] if L[1] <= r[1] < L[1] + L[3])
        check(r[0] + r[2] <= layer[0] + layer[2], "芯片 %s 在所属的层里面" % name)
    shot(d, win, args, "skabout")
    app.stop()


def test_click(d, args):
    app, win, st = start(d, args)
    W, H = win.w, win.h
    start_idx = len(d.requests)
    d.click(win, *center(st["chips"]["sinit"]))
    st = app.state(why="select")
    check(st["selected"] == "sinit · 1 号进程", "点 sinit：便签显示它的解释（%s）" % st["selected"])
    settle(d, win)
    rects = damages_since(d, start_idx, win)
    area = sum(r[2] * r[3] for r in rects)
    print("    点击后的重画：%s" % rects)
    check(rects and area < W * H * 0.25, "点击只重画芯片和便签（%.1f%% 窗口面积）" % (area * 100.0 / (W * H)))
    shot(d, win, args, "skabout-sinit")
    # 点一层的空白处（右下角，避开芯片）
    L = st["layers"][2]
    d.click(win, L[0] + L[2] - 20, L[1] + L[3] - 8)
    st = app.state(why="select")
    check(st["selected"] == "Linux 6.12 内核", "点内核这一层: %s" % st["selected"])
    settle(d, win)
    shot(d, win, args, "skabout-kernel")
    d.click(win, *center(st["links"][1]))
    st = app.state(why="select")
    check(st["selected"] == "系统调用", "点“系统调用”箭头: %s" % st["selected"])
    d.click(win, *center(st["chips"]["sketchd"]))
    st = app.state(why="select")
    check(st["selected"] == "sketchd · 显示服务器", "点 sketchd（芯片上只写程序名，便签里有中文名）: %s" % st["selected"])
    settle(d, win)
    shot(d, win, args, "skabout-sketchd")
    app.stop()


def test_idle(d, args):
    app, win, st = start(d, args)
    time.sleep(0.5)
    start_idx = len(d.requests)
    time.sleep(2.5)
    rects = damages_since(d, start_idx, win)
    check(not rects, "画完之后静止：2.5 秒内没有任何重画 %s" % rects[:3])
    check(app.alive(), "程序在运行")
    app.stop()


def test_resize(d, args):
    app, win, st = start(d, args)
    d.resize(win, 600, 520)
    st = app.state(why="layout")
    check(st["wide"] == 0 and st["info_visible"] and not st["diagram_visible"], "窄窗口：标签页模式，先显示本机信息")
    check(st["info_h"] <= st["infopanel"][3], "窄窗口里信息栏也放得下（%s来源说明）: %d <= %d" %
          ("省略了" if st["info_compact"] else "带", st["info_h"], st["infopanel"][3]))
    settle(d, win)
    shot(d, win, args, "skabout-narrow-info")
    d.click(win, *center(st["tabs"][1]))
    st = app.state(why="tab")
    check(st["tab"] == 1 and st["diagram_visible"] and not st["info_visible"], "点“系统结构”标签显示图")
    settle(d, win)
    shot(d, win, args, "skabout-narrow-diagram")
    for L in st["layers"]:
        check(L[1] + L[3] <= st["diagram"][1] + st["diagram"][3] + 1, "窄窗口里四层都放得下")
    if not st["note_on"]:
        # 便签放不下：点击改用消息框解释
        d.click(win, *center(st["chips"]["skmon"]))
        dlg = d.wait_window(timeout=5, count=2)
        settle(d, dlg)
        check("skmon" in dlg.title, "放不下便签时用消息框解释: %s" % dlg.title)
        shot(d, dlg, args, "skabout-narrow-dialog")
        d.key(dlg, "enter")
        time.sleep(0.3)
        check(len(d.windows) == 1, "消息框关掉了")
    else:
        d.click(win, *center(st["chips"]["skmon"]))
        st = app.state(why="select")
        check(st["selected"] == "skmon", "窄窗口里点芯片: %s" % st["selected"])
    d.resize(win, 1000, 660)
    st = app.state(why="layout")
    check(st["wide"] == 1 and st["diagram_visible"] and st["info_visible"], "拉宽后恢复并排")
    settle(d, win)
    shot(d, win, args, "skabout-wide")
    check(app.alive(), "改变大小后程序仍在运行")
    app.stop()


def test_fallback(d, args):
    app, win, st = start(d, args, os_release="/nonexistent/os-release")
    check(st["os_name"] == "SketchOS", "没有 os-release：名称用 SketchOS")
    check(re.match(r"^\d+(\.\d+)*", st["info"]["系统版本"]) is not None, "版本用编译时的 SKETCHOS_VERSION: %s" % st["info"]["系统版本"])
    check(st["os_pretty"].startswith("手绘操作系统"), "副标题: %s" % st["os_pretty"])
    app.stop()


if __name__ == "__main__":
    run_tests([("render", test_render), ("click", test_click), ("idle", test_idle), ("resize", test_resize),
               ("fallback", test_fallback)])
