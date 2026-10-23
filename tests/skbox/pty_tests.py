#!/usr/bin/env python3
"""
tests/skbox/pty_tests.py — skbox 里需要终端的交互式工具测试（伪终端 pty）：more、top

由 tests/skbox/run_tests.sh 调用，参数是 skbox --install 建好的符号链接目录：

    python3 tests/skbox/pty_tests.py <applet 目录>

more 和 top 只有在“标准输出是终端”时才进入交互模式（分页停顿、全屏刷新、原始模式读按键），
所以这里用 pty.fork() 给它们一个真正的终端：子进程成为新会话的首领，pty 从设备是它的控制终端，
/dev/tty 也就指向它——这正是 “seq 100 | more” 能从键盘读按键的原因。
测试从主设备那一端读输出、写按键，并检查退出后终端设置（回显、规范模式）被恢复。

退出码 = 失败的检查数（0 表示全部通过）。
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

g_pass = 0
g_fail = 0


def check(cond, name, detail=""):
    global g_pass, g_fail
    if cond:
        g_pass += 1
    else:
        g_fail += 1
        print("  ✗ %s" % name)
        if detail:
            for line in str(detail)[-800:].splitlines()[-12:]:
                print("      " + line)


def strip_ansi(s):
    return ANSI.sub("", s)


class Pty:
    """在伪终端里运行一个程序：rows×cols 的窗口，从主设备读输出、写按键"""

    def __init__(self, argv, rows=24, cols=80, cwd=None, env=None):
        self.buf = b""
        pid, fd = pty.fork()
        if pid == 0:
            try:
                # 子进程：0/1/2 已经是 pty 从设备。先设好窗口大小，程序启动时用 TIOCGWINSZ 查到的就是它
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
                if cwd:
                    os.chdir(cwd)
                os.execve(argv[0], argv, env or os.environ)
            finally:
                os._exit(127)
        self.pid, self.fd = pid, fd
        self.status = None

    def read(self, timeout=0.2):
        """把 timeout 秒内能读到的输出都读进 buf"""
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                break
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                break
            try:
                data = os.read(self.fd, 65536)
            except OSError:          # 子进程退出后从设备关闭，读主设备得到 EIO
                break
            if not data:
                break
            self.buf += data

    def text(self):
        return self.buf.decode("utf-8", "replace")

    def expect(self, pattern, timeout=5.0):
        """等待输出（去掉颜色序列后）出现 pattern，返回是否等到"""
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, strip_ansi(self.text())):
                return True
            self.read(0.1)
        return bool(re.search(pattern, strip_ansi(self.text())))

    def clear(self):
        self.buf = b""

    def send(self, s):
        os.write(self.fd, s.encode() if isinstance(s, str) else s)

    def resize(self, rows, cols):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)

    def lflag(self):
        """从设备当前的本地模式（Linux 上对主设备 tcgetattr 得到的就是从设备的设置）"""
        return termios.tcgetattr(self.fd)[3]

    def wait(self, timeout=5.0):
        """等程序退出，返回退出码（被信号杀死时为 -信号）；超时返回 None"""
        end = time.time() + timeout
        while time.time() < end:
            self.read(0.05)
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = os.waitstatus_to_exitcode(st)
                self.read(0.1)
                return self.status
        return None

    def close(self):
        if self.status is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except OSError:
                pass
        try:
            os.close(self.fd)
        except OSError:
            pass


def shown_lines(text, prefix):
    """输出里出现过的 “prefix数字” 行号集合"""
    return {int(n) for n in re.findall(re.escape(prefix) + r"(\d+)", strip_ansi(text))}


# ---------------------------------------------------------------------------
def test_more(sb, tmp):
    print("== more")
    more = os.path.join(sb, "more")
    path = os.path.join(tmp, "lines.txt")
    with open(path, "w") as f:
        for i in range(1, 101):
            f.write("line%d\n" % i)

    # 24 行的终端：一页 23 行，最后一行留给提示
    p = Pty([more, path], rows=24, cols=80)
    try:
        ok = p.expect(r"--更多--\(\d+%\)")
        got = shown_lines(p.text(), "line")
        check(ok and got == set(range(1, 24)), "more 第一页显示 23 行后停下", "显示了 %s" % sorted(got))
        check(not (p.lflag() & termios.ECHO), "more 等待按键时关闭了回显（原始模式）")
        p.clear()
        p.send(" ")
        ok = p.expect(r"--更多--")
        got = shown_lines(p.text(), "line")
        check(ok and got == set(range(24, 47)), "空格翻一页", "显示了 %s" % sorted(got))
        p.clear()
        p.send("\r")
        p.expect(r"--更多--")
        got = shown_lines(p.text(), "line")
        check(got == {47}, "回车前进一行", "显示了 %s" % sorted(got))
        p.clear()
        p.send("d")
        p.expect(r"--更多--")
        got = shown_lines(p.text(), "line")
        check(got == set(range(48, 59)), "d 前进半页（11 行）", "显示了 %s" % sorted(got))
        p.clear()
        p.send("x")
        p.read(0.3)
        check("\a" in p.text(), "不认识的键响铃")
        p.clear()
        p.send("h")
        check(p.expect(r"空格=下一页"), "h 显示按键帮助")
        p.clear()
        p.send("q")
        rc = p.wait()
        check(rc == 0, "q 退出，退出码 0", "退出码 %s" % rc)
        got = shown_lines(p.text(), "line")
        check(not got, "q 之后不再输出内容", "又显示了 %s" % sorted(got))
        check(bool(p.lflag() & termios.ECHO) and bool(p.lflag() & termios.ICANON), "退出后恢复回显和规范模式")
    finally:
        p.close()

    # 读到文件末尾自己退出，不再停顿
    p = Pty([more, path], rows=60, cols=80)
    try:
        p.expect(r"--更多--")
        p.clear()
        p.send(" ")
        rc = p.wait()
        check(rc == 0 and 100 in shown_lines(p.text(), "line"), "翻到末尾后自动退出", "退出码 %s" % rc)
    finally:
        p.close()

    # 标准输入是管道：内容从管道来，按键从 /dev/tty（控制终端）读
    seq = os.path.join(sb, "seq")
    p = Pty(["/bin/sh", "-c", "'%s' -f 'n%%g' 1 100 | '%s'" % (seq, more)], rows=24, cols=80)
    try:
        ok = p.expect(r"--更多--")
        got = shown_lines(p.text(), "n")
        check(ok and got == set(range(1, 24)), "管道输入：第一页", "显示了 %s" % sorted(got))
        check("%" not in strip_ansi(p.text()).split("--更多--")[1][:4], "管道输入不知道总长度，不显示百分比")
        p.clear()
        p.send(" ")
        p.expect(r"--更多--")
        got = shown_lines(p.text(), "n")
        check(got == set(range(24, 47)), "管道输入：空格从 /dev/tty 读到", "显示了 %s" % sorted(got))
        p.send("q")
        rc = p.wait()
        check(rc == 0, "管道输入：q 退出", "退出码 %s" % rc)
    finally:
        p.close()

    # 汉字占两列：一行 3+45×2=93 列，在 80 列的终端里折成两行，一页 23 行只能放 11 行文字
    wide = os.path.join(tmp, "wide.txt")
    with open(wide, "w", encoding="utf-8") as f:
        for i in range(1, 41):
            f.write("w%02d%s\n" % (i, "汉" * 45))
    p = Pty([more, wide], rows=24, cols=80)
    try:
        ok = p.expect(r"--更多--")
        got = shown_lines(p.text(), "w")
        check(ok and got == set(range(1, 12)), "宽字符折行：第一页 11 行", "显示了 %s" % sorted(got))
        p.send("q")
        p.wait()
    finally:
        p.close()

    # 两个文件：先显示 3 行的文件名分隔栏；第一个文件正好在页尾结束时（3 + 20 = 23 行），
    # 提示的是下一个文件名而不是百分比
    short = os.path.join(tmp, "short.txt")
    with open(short, "w") as f:
        f.write("".join("s%d\n" % i for i in range(1, 21)))
    p = Pty([more, short, path], rows=24, cols=80)
    try:
        check(p.expect(r"::::::::::::::\s*\S*short\.txt"), "多个文件：显示文件名")
        check(p.expect(r"下一个文件: \S*lines\.txt"), "多个文件：提示下一个文件名", strip_ansi(p.text())[-200:])
        check(shown_lines(p.text(), "s") == set(range(1, 21)), "多个文件：第一个文件全部显示")
        p.clear()
        p.send(" ")
        check(p.expect(r"::::::::::::::\s*\S*lines\.txt"), "多个文件：空格后显示第二个文件")
        p.send("q")
        p.wait()
    finally:
        p.close()

    # 输出不是终端时就是 cat
    r = subprocess.run([more, path], stdout=subprocess.PIPE, stdin=subprocess.DEVNULL, timeout=10)
    check(r.returncode == 0 and r.stdout == open(path, "rb").read(), "输出不是终端时等同 cat")


# ---------------------------------------------------------------------------
def test_top(sb, tmp):
    print("== top")
    top = os.path.join(sb, "top")

    r = subprocess.run([top], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=10)
    check(r.returncode == 1 and "终端" in r.stderr.decode("utf-8", "replace"),
          "不在终端里运行时报错并提示 -b", r.stderr.decode("utf-8", "replace"))

    # 窗口要足够高：宿主机上可能有上百个内核线程，sleep 要能出现在列表里
    victim = subprocess.Popen(["sleep", "300"])
    p = Pty([top, "-d", "0.5"], rows=500, cols=120)
    try:
        ok = p.expect(r"PID USER")
        t = p.text()
        check(ok and "任务" in strip_ansi(t) and "内存" in strip_ansi(t), "top 显示概况和进程表头", strip_ansi(t)[:400])
        check("\x1b[?1049h" in t and "\x1b[?25l" in t, "进入备用屏幕并隐藏光标")
        check(not (p.lflag() & termios.ECHO) and not (p.lflag() & termios.ICANON), "原始模式：关闭回显和规范模式")
        check(p.expect(r"\b%d\b" % victim.pid, 3), "进程列表里有 sleep", strip_ansi(p.text())[-600:])

        # 定时刷新：不按键也会重画
        p.clear()
        p.read(1.3)
        check("PID USER" in strip_ansi(p.text()), "按刷新间隔自动重画")

        for key, msg in (("M", "按内存占用排序"), ("N", "按进程号排序"), ("T", "按累计 CPU 时间排序"),
                         ("P", "按 CPU 占用排序"), ("c", "显示完整命令行"), ("?", "按键: q 退出")):
            p.clear()
            p.send(key)
            check(p.expect(re.escape(msg), 3), "按键 %s：%s" % (key, msg), strip_ansi(p.text())[-300:])
        p.clear()
        check(p.expect(r"sleep 300", 3), "c 之后显示完整命令行 sleep 300", strip_ansi(p.text())[-600:])

        # 方向键的转义序列不能被当成命令（ESC [ A 里的 A 不是任何命令，但 ESC 后面的字节要整个吞掉）
        p.clear()
        p.send("\x1b[A")
        p.read(0.8)
        check(p.status is None and os.waitpid(p.pid, os.WNOHANG) == (0, 0), "方向键不会让 top 退出")

        # k：结束一个进程（先问进程号，再问信号）
        p.clear()
        p.send("k")
        check(p.expect(r"要结束的进程号", 3), "k 询问进程号")
        p.send("%d\r" % victim.pid)
        check(p.expect(r"发送的信号", 3), "输入进程号后询问信号")
        p.send("9\r")
        check(p.expect(r"已向进程 %d 发送信号 9 \(KILL\)" % victim.pid, 3), "k 发送信号并提示",
              strip_ansi(p.text())[-300:])
        try:
            rc = victim.wait(timeout=3)
        except subprocess.TimeoutExpired:
            rc = None
        check(rc == -signal.SIGKILL, "被结束的进程收到了 SIGKILL", "退出码 %s" % rc)

        # k 之后按 Esc 取消
        p.clear()
        p.send("k")
        p.expect(r"要结束的进程号", 3)
        p.send("\x1b")
        check(p.expect(r"已取消", 3), "Esc 取消结束进程")

        # 窗口变小：每行都截断到新的宽度（SIGWINCH）
        p.resize(20, 50)
        p.clear()
        p.read(1.2)
        frame = strip_ansi(p.text().split("\x1b[H")[-1])
        lines = [l for l in frame.replace("\r", "").split("\n") if l]
        widest = max((sum(2 if ord(ch) >= 0x1100 and ch not in "“”…—·" else 1 for ch in l) for l in lines),
                     default=0)
        check(lines and widest <= 50, "SIGWINCH 后按新宽度截断", "最宽 %d 列：%r" % (widest, lines[:3]))
        check(len(lines) <= 20, "SIGWINCH 后行数不超过新高度", "%d 行" % len(lines))

        p.clear()
        p.send("q")
        rc = p.wait()
        check(rc == 0, "q 退出，退出码 0", "退出码 %s" % rc)
        t = p.text()
        check("\x1b[?25h" in t and "\x1b[?1049l" in t, "退出时显示光标、离开备用屏幕")
        check(bool(p.lflag() & termios.ECHO) and bool(p.lflag() & termios.ICANON), "退出后恢复终端设置")
    finally:
        p.close()
        if victim.poll() is None:
            victim.kill()
            victim.wait()

    # Ctrl+C：原始模式保留了 ISIG，SIGINT 让 top 正常收尾退出
    p = Pty([top, "-d", "0.5"], rows=24, cols=80)
    try:
        p.expect(r"PID USER")
        p.send("\x03")
        rc = p.wait()
        check(rc == 0 and "\x1b[?1049l" in p.text(), "Ctrl+C 退出并恢复屏幕", "退出码 %s" % rc)
        check(bool(p.lflag() & termios.ECHO), "Ctrl+C 退出后回显已恢复")
    finally:
        p.close()

    # 一屏正好占满终端：最后一行后面不能再换行，否则终端上滚一行，第一行“top - …”就看不见了
    p = Pty([top, "-d", "0.3"], rows=12, cols=100)
    try:
        p.expect(r"PID USER")
        p.clear()
        p.read(0.8)
        frames = [f.split("\x1b[J")[0] for f in p.text().split("\x1b[H")[1:] if "\x1b[J" in f]
        check(frames, "收到完整的一屏", repr(p.text()[-300:]))
        if frames:
            f = frames[-1]
            check(f.count("\n") == 11, "12 行的终端里一屏有 11 个换行（最后一行后面没有换行）",
                  "%d 个换行" % f.count("\n"))
            check(strip_ansi(f).startswith("top - "), "第一行是“top - …”", strip_ansi(f)[:80])
        p.send("q")
        p.wait()
    finally:
        p.close()

    # -n：刷新指定次数后自己退出
    p = Pty([top, "-d", "0.2", "-n", "3"], rows=24, cols=80)
    try:
        rc = p.wait(10)
        frames = strip_ansi(p.text()).count("PID USER")
        check(rc == 0 and frames == 3, "-n 3 刷新 3 次后退出", "退出码 %s，%d 帧" % (rc, frames))
    finally:
        p.close()


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        print("用法: pty_tests.py <skbox --install 建好的符号链接目录>", file=sys.stderr)
        return 2
    sb = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="skbox-pty.") as tmp:
        for t in (test_more, test_top):
            try:
                t(sb, tmp)
            except Exception as e:   # 一组测试出异常不影响下一组
                check(False, "%s 抛出异常" % t.__name__, repr(e))
    print("pty 测试：通过 %d 项，失败 %d 项" % (g_pass, g_fail))
    return min(g_fail, 125)


if __name__ == "__main__":
    sys.exit(main())
