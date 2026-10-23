"""
tests/apps/apptest.py — 图形应用测试的公共部分（被 tests/apps/test_<应用>.py 导入）

图形应用是 32 位静态程序，Linux 宿主机（WSL、云端容器）可以直接运行；
tools/mock_display.py 扮演显示服务器 sketchd：接收窗口缓冲区、注入鼠标键盘事件、
把窗口存成 PNG。所以这里的测试不需要 QEMU，也不需要内核。

每个测试文件这样写：

    from apptest import check, App, settle, shot, run_tests

    def test_open(d, args):
        app = App(d, [args.bin + "/sknote", path])     # 启动应用（自动设置 SKETCH_SOCKET）
        win = d.wait_window(timeout=10)
        settle(d, win)                                  # 等它画完
        shot(d, win, args, "sknote-open")               # 截图存到 --out 目录
        check("你好" in app.text(), "……")              # 检查
        app.stop()

    if __name__ == "__main__":
        run_tests([("open", test_open), ...])

命令行（通常由 tests/apps/run.sh 调用）：
    python3 tests/apps/test_<应用>.py --bin <用户态 bin 目录> --out <截图目录> [--only a,b]
"""
import argparse
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mock_display as md  # noqa: E402,F401  （测试文件可以 from apptest import md 拿到协议常量）
from mock_display import MockDisplay  # noqa: E402

FONT_DIR = os.path.join(ROOT, "assets", "fonts")
results = []


def check(cond, what):
    """记录一项检查；返回 cond，方便 if not check(...): return"""
    results.append((bool(cond), what))
    print(("  PASS  " if cond else "  FAIL  ") + what, flush=True)
    return cond


class App:
    """被测程序：后台线程逐行读取 stdout+stderr，方便检查程序打印的内容"""

    def __init__(self, d, argv, env=None, cwd=None):
        e = dict(os.environ)
        e.update({"SKETCH_FONT_DIR": FONT_DIR, "LANG": "C.UTF-8", "TZ": "CST-8"})
        if env:
            e.update(env)
        self.d = d
        self.p = d.spawn(argv, env=e, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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

    def text(self):
        return "\n".join(self.lines)

    def expect(self, pattern, timeout=5.0):
        """等待匹配 pattern 的新一行（之前读过的行不算），返回匹配的行或 None"""
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

    def wait(self, timeout=5.0):
        """等程序退出，返回退出码；超时返回 None"""
        try:
            return self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def alive(self):
        return self.p.poll() is None

    def stop(self):
        """结束程序（SIGTERM，不行再 SIGKILL），并等模拟服务器删掉它的窗口"""
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            if self.wait(2) is None:
                self.p.kill()
                self.p.wait()
        # wait_window 发现有进程以非 0 退出码结束就会报错：被我们主动结束的进程不算
        if self.p in self.d.procs:
            self.d.procs.remove(self.p)
        # 服务器发现连接断开后才删窗口；不等的话，下一个 wait_window(title=...) 可能拿到旧窗口
        deadline = time.time() + 3
        while time.time() < deadline and \
                any(w.client.pid == self.p.pid for w in list(self.d.windows.values())):
            time.sleep(0.02)


def settle(d, win, quiet=0.3, timeout=10):
    """等窗口一段时间内没有新的 damage（画完了）"""
    d.wait_idle(win, quiet=quiet, timeout=timeout)


def shot(d, win, args, name):
    """把窗口内容存成 <--out>/<name>.png，返回路径"""
    path = os.path.join(args.out, name + ".png")
    d.save_window(win, path)
    return path


def pixel(win, x, y):
    """窗口内容区 (x, y) 处的像素，返回 (r, g, b)"""
    w, h, px = win.pixels()
    off = (y * w + x) * 4
    b, g, r = px[off], px[off + 1], px[off + 2]
    return (r, g, b)


def temp_home():
    """建一个临时 HOME 目录（测试结束后由调用者 shutil.rmtree）"""
    return tempfile.mkdtemp(prefix="sketch-app-home.")


def run_tests(tests):
    """解析命令行，依次运行 tests = [(名字, 函数(d, args)), ...]，每个测试用一个新的模拟显示服务器"""
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="用户态程序目录（里面有 sknote、skterm……）")
    ap.add_argument("--out", default="/tmp/app-test-shots", help="截图目录")
    ap.add_argument("--only", default="", help="只运行这些测试（逗号分隔）")
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)
    os.makedirs(args.out, exist_ok=True)
    only = set(filter(None, args.only.split(",")))
    for name, fn in tests:
        if only and name not in only:
            continue
        print("== %s" % name, flush=True)
        sock = "/tmp/skapp-%s-%d.sock" % (name, os.getpid())
        with MockDisplay(sock, 1024, 768) as d:
            try:
                fn(d, args)
            except Exception:  # 测试本身出错也记为失败
                traceback.print_exc()
                check(False, "%s 测试异常" % name)
            check(not d.errors, "%s：没有协议错误 %s" % (name, d.errors))
    bad = [w for ok, w in results if not ok]
    print("\n%d 项检查，%d 项失败" % (len(results), len(bad)))
    for w in bad:
        print("  失败: " + w)
    sys.exit(1 if bad else 0)


__all__ = ["check", "App", "settle", "shot", "pixel", "temp_home", "run_tests", "MockDisplay", "md",
           "shutil", "time", "os", "re"]
