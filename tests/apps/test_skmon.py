#!/usr/bin/env python3
"""
test_skmon.py — 系统监视器 skmon 的自动化测试（不需要 QEMU）

skmon 是 32 位静态程序，直接在宿主机上运行，所以它看到的 /proc 是宿主机的：
进程和内核线程很多，正好检验界面能不能应付。tools/mock_display.py 扮演显示服务器。

被测程序在环境变量 SKMON_DEBUG=1 时，每次刷新后打印一行 “SKMON {JSON}”（当前页、CPU 千分比、
内存、表格各行的进程号、选中的进程、滚动位置、主要控件的位置……），这里据此找到要点击的位置、
核对结果。各项检查：
    overview   概览页的 CPU / 内存 / 进程数与 /proc 一致，曲线图在动
    damage     每秒刷新只重画小块区域（检查 DAMAGE 请求），两个页面分别检查
    procs      点标签页切到“进程”页，自己的进程出现在表格里，内容正确
    sort       点表头排序：第一次升序、再点一次降序，换一列从升序开始
    keep       刷新时选中行和滚动位置保持不变；隐藏/显示内核线程
    kill       选中一个 sleep 300，点“结束进程”，在确认对话框里回车 → 它收到 SIGTERM；
               “强制结束”→ SIGKILL；Esc 取消；进程已经不在时显示 ESRCH 的中文错误
    focus      窗口失去焦点后刷新变慢（两秒一次），回来后恢复
    resize     改变窗口大小后布局跟着变，截图检查
    python3 tests/apps/test_skmon.py --bin <bin 目录> --out <截图目录>
"""
import json
import subprocess

from apptest import check, App, settle, shot, run_tests, time, os, re

DEBUG_ENV = {"SKMON_DEBUG": "1"}


class Mon(App):
    """SKMON 调试行不回显（每秒一行，很长），其它输出照常打印"""

    def _read(self):
        for raw in self.p.stdout:
            line = raw.decode("utf-8", "replace").rstrip("\n")
            self.lines.append(line)
            self.q.put(line)
            if not line.startswith("SKMON "):
                print("    app> " + line, flush=True)

    def state(self, pred=None, why=None, timeout=6.0):
        """等待下一行满足条件的 SKMON 状态，返回解析后的 dict"""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            line = self.expect(r"^SKMON ", timeout=max(0.05, deadline - time.time()))
            if not line:
                break
            obj = json.loads(line[6:])
            if "why" not in obj:
                continue
            last = obj
            if why and obj["why"] != why:
                continue
            if pred is None or pred(obj):
                return obj
        raise AssertionError("等待 skmon 状态超时（why=%s），最后一行: %s" % (why, str(last)[:400]))

    def event(self, name, timeout=6.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.expect(r'^SKMON \{"event"', timeout=max(0.05, deadline - time.time()))
            if not line:
                break
            obj = json.loads(line[6:])
            if obj.get("event") == name:
                return obj
        raise AssertionError("等待事件 %s 超时" % name)


def start(d, args, extra=()):
    app = Mon(d, [os.path.join(args.bin, "skmon")] + list(extra), env=DEBUG_ENV)
    win = d.wait_window(timeout=10, title="系统监视器")
    st = app.state(why="refresh", timeout=8)
    settle(d, win)
    return app, win, st


def center(r):
    return r[0] + r[2] // 2, r[1] + r[3] // 2


def goto_procs(d, win, app):
    st = app.state(why="refresh")
    d.click(win, *center(st["ui"]["tab1"]))
    st = app.state(lambda s: s["page"] == 1 and s["count"] > 0, timeout=6)
    settle(d, win)
    return st


def header_point(st, col):
    lx, ly = st["ui"]["list"][0], st["ui"]["list"][1]
    x = lx + 3 + 4 + sum(st["ui"]["widths"][:col]) + 24     # 列宽由 skmon 报告（窄窗口会换一套）
    return x, ly + 3 + st["ui"]["header_h"] // 2


def row_point(st, idx):
    lx, ly = st["ui"]["list"][0], st["ui"]["list"][1]
    rh = st["ui"]["row_h"]
    return lx + 60, ly + 3 + st["ui"]["header_h"] + (idx - st["scroll"]) * rh + rh // 2


def click_row(d, win, app, pid, tries=4):
    """点击进程 pid 所在的行，直到它被选中。数据是活的：读到状态和点击之间表格可能刚好刷新、
    行挪了位置（宿主机上随时有进程出生和退出），所以没点中就按新的状态再点一次"""
    st = app.state(lambda s: pid in s["rows"])
    for _ in range(tries):
        idx = st["rows"].index(pid)
        vis = visible_rows(st)
        if not (st["scroll"] <= idx < st["scroll"] + vis):
            # 不在可见范围：滚轮滚过去（一格 3 行）
            d.wheel(win, *row_point(st, st["scroll"]), -((idx - st["scroll"] - vis // 2) // 3) or 1)
            st = app.state(lambda s: pid in s["rows"])
            continue
        d.click(win, *row_point(st, idx))
        st = app.state(why="select")
        if st["sel"] == pid:
            return st
        st = app.state(lambda s: pid in s["rows"])
    return st


def visible_rows(st):
    return (st["ui"]["list"][3] - 6 - st["ui"]["header_h"]) // st["ui"]["row_h"]


def read_meminfo():
    out = {}
    for line in open("/proc/meminfo"):
        k, v = line.split(":", 1)
        out[k] = int(v.split()[0])
    return out


def count_pids():
    return len([n for n in os.listdir("/proc") if n.isdigit()])


def comm_of(pid):
    try:
        return open("/proc/%d/comm" % pid).read().strip()
    except OSError:
        return None


# ================================================================ 测试

def test_overview(d, args):
    app, win, st = start(d, args)
    check(st["page"] == 0, "默认显示概览页")
    st = app.state(why="refresh")
    st = app.state(why="refresh")
    mi = read_meminfo()
    check(0 <= st["cpu"] <= 1000, "CPU 千分比在 0..1000 之间: %d" % st["cpu"])
    check(st["mem_total_kb"] == mi["MemTotal"], "内存总量与 /proc/meminfo 一致: %d vs %d" % (st["mem_total_kb"], mi["MemTotal"]))
    check(abs(st["mem_avail_kb"] - mi["MemAvailable"]) < mi["MemTotal"] * 0.05,
          "可用内存与 MemAvailable 接近: %d vs %d" % (st["mem_avail_kb"], mi["MemAvailable"]))
    n = count_pids()
    check(abs(st["nproc"] - n) <= max(10, n // 10), "进程数与 /proc 一致: %d vs %d" % (st["nproc"], n))
    threads = int(open("/proc/loadavg").read().split()[3].split("/")[1])
    check(abs(st["nthread"] - threads) <= max(20, threads // 5), "线程数与 /proc/loadavg 一致: %d vs %d" % (st["nthread"], threads))
    up = float(open("/proc/uptime").read().split()[0])
    check(abs(st["uptime"] - up) < 5, "开机时长与 /proc/uptime 一致: %.0f vs %.0f" % (st["uptime"], up))
    load = [float(v) for v in open("/proc/loadavg").read().split()[:3]]
    check(all(abs(a - b) < 1.5 for a, b in zip(st["load"], load)), "负载均值与 /proc/loadavg 接近: %s vs %s" % (st["load"], load))
    # 做一点 CPU 负载，让曲线有起伏（同时检查曲线历史在增长）
    h0 = st["hist"]
    busy = subprocess.Popen(["sh", "-c", "end=$(($(date +%s)+3)); while [ $(date +%s) -lt $end ]; do :; done"])
    st = app.state(lambda s: s["hist"] >= h0 + 3, why="refresh", timeout=8)
    check(st["hist"] >= h0 + 3, "每秒往曲线里加一个样本: %d → %d" % (h0, st["hist"]))
    busy.wait()
    app.state(why="refresh")
    settle(d, win)
    shot(d, win, args, "skmon-overview")
    app.stop()


def bursts_since(d, start, win):
    """把 DAMAGE 请求按时间分组：一次刷新的重画在几毫秒内发完，两次刷新相隔一秒"""
    groups, last_t = [], None
    for req in list(d.requests[start:]):
        if req[2] != "DAMAGE":
            continue
        m = re.match(r"win=(\d+) \((-?\d+),(-?\d+) (\d+)x(\d+)\)", req[3])
        if not m or int(m.group(1)) != win.id:
            continue
        if last_t is None or req[0] - last_t > 0.3:
            groups.append([])
        groups[-1].append(tuple(int(v) for v in m.groups()[1:]))
        last_t = req[0]
    return groups


def test_damage(d, args):
    app, win, st = start(d, args)
    W, H = win.w, win.h
    area = W * H
    for page in (0, 1):
        if page == 1:
            st = goto_procs(d, win, app)
            # 显示内核线程、按 PID 升序、停在最上面：看得见的都是开机就有的内核线程，
            # 不会因为宿主机上别的程序（并行的测试……）不停地创建/结束进程而整行挪动
            d.click(win, *center(st["ui"]["hide"]))
            app.state(why="filter")
            app.state(why="refresh")
        st = app.state(why="refresh")
        time.sleep(0.3)
        start_idx = len(d.requests)
        for _ in range(4):
            app.state(why="refresh")
        time.sleep(0.3)
        groups = [g for g in bursts_since(d, start_idx, win) if g]
        sizes = sorted(sum(r[2] * r[3] for r in g) * 100.0 / area for g in groups)
        rects = [r for g in groups for r in g]
        name = ["概览", "进程"][page]
        print("    %s页：%d 次刷新，每次重画面积占窗口 %s%%" % (name, len(groups), ", ".join("%.1f" % v for v in sizes)))
        if page == 0:
            check(len(groups) >= 3, "概览页：曲线每秒都在动（4 秒内 %d 次重画）" % len(groups))
        else:
            check(len(groups) <= 5, "进程页：4 次刷新，%d 次有内容变化需要重画（没变就不画）" % len(groups))
        check(all(not (r[2] >= W - 4 and r[3] >= H - 4) for r in rects), "%s页：刷新从不整窗重画" % name)
        median = sizes[len(sizes) // 2] if sizes else 0
        if page == 0:
            # 曲线每秒整体左移一格，绘图区（约占窗口的 1/4）每秒都必须重画；除此之外只有几个数字在变
            check(max(sizes or [0]) <= 50, "概览页：每次刷新重画 ≤ 50%% 窗口（最多 %.1f%%）" % max(sizes or [0]))
            g = st["ui"]["graph"]
            others = [r for r in rects if not (r[0] < g[0] + g[2] and g[0] < r[0] + r[2] and
                                               r[1] < g[1] + g[3] and g[1] < r[1] + r[3])]
            o = sum(r[2] * r[3] for r in others) * 100.0 / area / max(1, len(groups))
            check(o <= 15, "概览页：曲线图以外只有几个数字标签在变（平均每次 %.1f%%）" % o)
        else:
            # 只重画变了的单元格。宿主机上随时有进程出生/退出（并行跑别的测试时更多），下面的行要
            # 跟着挪，那几次重画得多；所以看最小的一次——进程没有增减时只重画几个单元格
            best = sizes[0] if sizes else 0
            check(best <= 6, "进程页：进程没有增减的那次刷新只重画几个单元格（最小 %.1f%%，中位数 %.1f%%）"
                  % (best, median))
            check(max(sizes or [0]) <= 45, "进程页：即使有进程出生/退出也只重画变了的行（最多 %.1f%%）" % max(sizes or [0]))
    app.stop()


def test_procs(d, args):
    app, win, st = start(d, args)
    st = goto_procs(d, win, app)
    me = app.p.pid
    check(me in st["rows"], "自己的进程 %d 出现在表格里" % me)
    check(st["self"] and st["self"][0] == "skmon" and st["self"][1] == str(me),
          "自己那一行：名称 skmon、PID 正确: %s" % st["self"])
    check(st["self"][3][0] in "RS", "自己的状态是运行或睡眠: %s" % st["self"][3])
    check(st["self"][5].endswith(("K", "M")), "内存列有数值: %s" % st["self"][5])
    check("skmon" in st["self"][6], "命令行列: %s" % st["self"][6])
    check(st["hide_kernel"] == 1 and 2 not in st["rows"], "默认隐藏内核线程（kthreadd 不在表格里）")
    check(st["count"] == len(st["rows"]) and st["procs"] >= st["count"], "表格行数一致: %d" % st["count"])
    check(st["rows"] == sorted(st["rows"]), "默认按 PID 升序")
    check(st["term_enabled"] == 0, "没有选中时“结束进程”按钮禁用")
    shot(d, win, args, "skmon-procs")
    # 切回概览页
    d.click(win, *center(st["ui"]["tab0"]))
    st = app.state(lambda s: s["page"] == 0, timeout=5)
    check(st["page"] == 0, "点标签页切回概览")
    app.stop()


def test_sort(d, args):
    app, win, st = start(d, args, ["--tab", "1"])
    st = app.state(lambda s: s["count"] > 0)
    check(st["page"] == 1, "--tab 1 直接显示进程页")
    d.click(win, *header_point(st, 1))
    ev = app.event("sort")
    st = app.state(why="sort")
    check(ev["col"] == 1 and ev["desc"] == 0, "第一次点 PID 列：升序 %s" % ev)
    check(st["rows"] == sorted(st["rows"]), "PID 升序")
    d.click(win, *header_point(st, 1))
    ev = app.event("sort")
    st = app.state(why="sort")
    check(ev["col"] == 1 and ev["desc"] == 1, "再点一次：降序 %s" % ev)
    check(st["rows"] == sorted(st["rows"], reverse=True), "PID 降序")
    st = app.state(why="refresh")
    check(st["rows"] == sorted(st["rows"], reverse=True), "刷新后仍然按 PID 降序")
    shot(d, win, args, "skmon-sorted-pid-desc")
    d.click(win, *header_point(st, 0))
    ev = app.event("sort")
    st = app.state(why="sort")
    check(ev["col"] == 0 and ev["desc"] == 0, "换到名称列：从升序开始 %s" % ev)
    names = [n.lower() for n in st["names"]]
    check(names == sorted(names), "按名称升序: %s" % st["names"][:8])
    d.click(win, *header_point(st, 4))
    app.event("sort")
    d.click(win, *header_point(st, 4))
    ev = app.event("sort")
    check(ev["col"] == 4 and ev["desc"] == 1, "CPU%% 列降序 %s" % ev)
    settle(d, win)
    shot(d, win, args, "skmon-sorted-cpu")
    app.stop()


def test_keep(d, args):
    app, win, st = start(d, args, ["--tab", "1"])
    st = app.state(lambda s: s["count"] > 0)
    # 显示内核线程：行数变多，kthreadd（PID 2）出现
    n0 = st["count"]
    d.click(win, *center(st["ui"]["hide"]))
    st = app.state(why="filter")
    check(st["hide_kernel"] == 0 and st["count"] > n0, "取消“隐藏内核线程”后行数变多: %d → %d" % (n0, st["count"]))
    check(2 in st["rows"], "kthreadd（PID 2）出现在表格里")
    # 选中自己那一行
    me = app.p.pid
    vis = visible_rows(st)
    st = click_row(d, win, app, me)
    check(st["sel"] == me and st["term_enabled"] == 1, "选中自己（PID %d），按钮可用: sel=%d" % (me, st["sel"]))
    # 滚动到别处，然后等两次刷新：选中和滚动位置都不变
    if st["count"] > vis:
        target = 0 if st["scroll"] > 0 else min(st["count"] - vis, 6)
        d.wheel(win, *row_point(st, st["scroll"]), 3 if target == 0 else -2)
        time.sleep(0.2)
    s1 = app.state(why="refresh")
    s2 = app.state(why="refresh")
    s3 = app.state(why="refresh")
    check(s3["sel"] == me and s3["rows"][s3["sel_row"]] == me, "刷新后选中的仍是同一个进程: %s" % s3["sel"])
    check(s1["scroll"] == s2["scroll"] == s3["scroll"], "刷新后滚动位置不变: %d %d %d" % (s1["scroll"], s2["scroll"], s3["scroll"]))
    if st["count"] > vis:
        check(s3["scroll"] != st["scroll"] or st["scroll"] == 0, "滚轮确实滚动了")
    settle(d, win)
    shot(d, win, args, "skmon-all-threads")
    # 重新隐藏内核线程
    d.click(win, *center(st["ui"]["hide"]))
    st = app.state(why="filter")
    check(st["hide_kernel"] == 1 and 2 not in st["rows"], "再次隐藏内核线程")
    check(st["sel"] == me, "过滤后选中仍然保持")
    app.stop()


def select_pid(d, win, app, pid):
    """按 PID 降序排列（新进程在最上面），然后点击这个进程那一行"""
    st = app.state(lambda s: pid in s["rows"])
    if st["sort"] != 1 or not st["desc"]:
        d.click(win, *header_point(st, 1))
        app.event("sort")
        d.click(win, *header_point(st, 1))
        app.event("sort")
    app.state(lambda s: pid in s["rows"] and s["desc"] == 1)
    return click_row(d, win, app, pid)


def test_kill(d, args):
    victim = subprocess.Popen(["sleep", "300"])
    victim2 = subprocess.Popen(["sleep", "300"])
    try:
        app, win, st = start(d, args, ["--tab", "1"])
        # 1. 结束进程（SIGTERM），在确认对话框里按回车
        st = select_pid(d, win, app, victim.pid)
        check(st["sel"] == victim.pid and st["term_enabled"], "选中 sleep 300（PID %d）" % victim.pid)
        shot(d, win, args, "skmon-selected")
        d.click(win, *center(st["ui"]["term"]))
        dlg = d.wait_window(timeout=5, count=2)
        settle(d, dlg)
        check(dlg.flags & 0x8, "确认对话框是对话框窗口")
        shot(d, dlg, args, "skmon-dialog-term")
        d.key(dlg, "enter")
        ev = app.event("kill")
        check(ev["pid"] == victim.pid and ev["sig"] == 15 and ev["ok"] == 1, "发送了 SIGTERM: %s" % ev)
        try:
            rc = victim.wait(timeout=5)
        except subprocess.TimeoutExpired:
            rc = None
        check(rc == -15, "sleep 被 SIGTERM 结束（返回 %s）" % rc)
        st = app.state(lambda s: victim.pid not in s["rows"], timeout=6)
        check(victim.pid not in st["rows"], "进程从表格里消失")
        check("SIGTERM" in st["msg"], "状态栏显示结果: %s" % st["msg"])
        settle(d, win)
        shot(d, win, args, "skmon-after-kill")

        # 2. 取消：Esc 关掉对话框，进程还活着
        st = select_pid(d, win, app, victim2.pid)
        d.click(win, *center(st["ui"]["kill"]))
        dlg = d.wait_window(timeout=5, count=2)
        settle(d, dlg)
        shot(d, dlg, args, "skmon-dialog-kill")
        d.key(dlg, "esc")
        st = app.state(lambda s: s["msg"] == "已取消", timeout=5)
        check(victim2.poll() is None, "取消后进程还活着")

        # 3. 强制结束（SIGKILL）
        d.click(win, *center(st["ui"]["kill"]))
        dlg = d.wait_window(timeout=5, count=2)
        settle(d, dlg)
        d.key(dlg, "enter")
        ev = app.event("kill")
        check(ev["pid"] == victim2.pid and ev["sig"] == 9 and ev["ok"] == 1, "发送了 SIGKILL: %s" % ev)
        try:
            rc = victim2.wait(timeout=5)
        except subprocess.TimeoutExpired:
            rc = None
        check(rc == -9, "sleep 被 SIGKILL 结束（返回 %s）" % rc)

        # 4. 进程在确认期间已经退出（并被回收）：kill 失败，显示中文的 ESRCH 错误
        victim3 = subprocess.Popen(["sleep", "300"])
        st = select_pid(d, win, app, victim3.pid)
        d.click(win, *center(st["ui"]["term"]))
        dlg = d.wait_window(timeout=5, count=2)
        settle(d, dlg)
        victim3.kill()
        victim3.wait()
        d.key(dlg, "enter")
        ev = app.event("kill")
        check(ev["ok"] == 0 and ev["errno"] == 3, "进程已不存在：kill 返回 ESRCH: %s" % ev)
        err = d.wait_window(timeout=5, title="无法结束进程")
        settle(d, err)
        shot(d, err, args, "skmon-error")
        d.key(err, "enter")
        st = app.state(why="refresh")
        check("无法结束" in st["msg"] and "没有那个进程" in st["msg"],
              "状态栏显示中文错误: %s" % st["msg"])
        settle(d, win)
        shot(d, win, args, "skmon-after-error")
        app.stop()
    finally:
        for v in (victim, victim2):
            if v.poll() is None:
                v.kill()
                v.wait()


def test_focus(d, args):
    app, win, st = start(d, args)
    d.focus(win, False)
    st = app.state(lambda s: s["rate"] == 2000, timeout=6)
    check(st["focused"] == 0 and st["rate"] == 2000, "失去焦点后两秒刷新一次")
    t0 = time.time()
    app.state(why="refresh", timeout=6)
    app.state(why="refresh", timeout=6)
    dt = time.time() - t0
    check(dt > 2.5, "两次刷新之间约两秒（实际两次共 %.1f 秒）" % dt)
    d.focus(win, True)
    st = app.state(lambda s: s["rate"] == 1000, timeout=6)
    check(st["focused"] == 1, "获得焦点后恢复每秒一次")
    app.stop()


def test_resize(d, args):
    app, win, st = start(d, args)
    d.resize(win, 600, 440)
    st = app.state(why="layout", timeout=5)
    check(st["ui"]["list"][2] == 600 - 32, "缩小后表格宽度跟着变: %s" % st["ui"]["list"])
    app.state(why="refresh")
    settle(d, win)
    shot(d, win, args, "skmon-small-overview")
    d.click(win, *center(st["ui"]["tab1"]))
    app.state(lambda s: s["page"] == 1 and s["count"] > 0)
    settle(d, win)
    shot(d, win, args, "skmon-small-procs")
    d.resize(win, 1000, 700)
    st = app.state(why="layout", timeout=5)
    check(st["ui"]["list"][2] == 1000 - 32 and st["ui"]["list"][3] > 500, "放大后表格变大: %s" % st["ui"]["list"])
    app.state(why="refresh")
    settle(d, win)
    shot(d, win, args, "skmon-large-procs")
    d.click(win, *center(st["ui"]["tab0"]))
    app.state(lambda s: s["page"] == 0)
    settle(d, win)
    shot(d, win, args, "skmon-large-overview")
    check(app.alive(), "改变大小后程序仍在运行")
    app.stop()


if __name__ == "__main__":
    run_tests([("overview", test_overview), ("damage", test_damage), ("procs", test_procs),
               ("sort", test_sort), ("keep", test_keep), ("kill", test_kill), ("focus", test_focus),
               ("resize", test_resize)])
