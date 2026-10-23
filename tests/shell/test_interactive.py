#!/usr/bin/env python3
"""
tests/shell/test_interactive.py — sksh 交互模式测试（伪终端 pty）

用 pty.fork() 给 sksh 一个真正的终端：子进程成为新会话的首领，pty 从设备是它的控制终端，
于是 sksh 会进入交互模式、开启作业控制、使用行编辑器——和在串口/终端窗口里一样。

测试内容：提示符、光标移动和各种编辑键、UTF-8 汉字、折行、历史记录（含历史文件）、
Tab 补全、Ctrl-C/Ctrl-D、续行提示符、作业控制（Ctrl-Z、jobs、bg、fg、后台完成通知）。

为了检查“屏幕上显示的是不是对的”，这里带了一个极简的 VT100 终端模拟器 Screen：
把 sksh 输出的字节（包括 ESC [ n A 之类的光标控制序列）喂给它，再读出屏幕内容和光标位置。

用法：python3 tests/shell/test_interactive.py [--sksh 路径] [-k 关键字]
"""
import argparse
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import socket
import struct
import sys
import tempfile
import termios
import time
import unicodedata

DEFAULT_SKSH = os.path.expanduser("~/sketchos-work/agents/shell/build/bin/sksh")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07")
PROMPT_RE = r"# $"

LEFT, RIGHT, UP, DOWN = "\x1b[D", "\x1b[C", "\x1b[A", "\x1b[B"
HOME, END, DEL = "\x1b[H", "\x1b[F", "\x1b[3~"


def strip_ansi(s):
    return ANSI.sub("", s)


class Screen:
    """极简终端模拟器：自动折行（含“行尾等待折行”状态）、宽字符、常用 CSI 序列"""

    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.cells = [[" "] * cols for _ in range(rows)]
        self.r = self.c = 0
        self.pending = False

    def _newline(self):
        self.r += 1
        if self.r >= self.rows:
            self.cells.pop(0)
            self.cells.append([" "] * self.cols)
            self.r = self.rows - 1

    def _put(self, ch):
        w = 2 if unicodedata.east_asian_width(ch) in "WF" else 1
        if self.pending or self.c + w > self.cols:
            self.c = 0
            self._newline()
            self.pending = False
        self.cells[self.r][self.c] = ch
        if w == 2:
            self.cells[self.r][self.c + 1] = ""
        self.c += w
        if self.c >= self.cols:
            self.c = self.cols - 1
            self.pending = True

    def feed(self, text):
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "\x1b":
                m = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", text[i:])
                if m:
                    self._csi(m.group(1), m.group(2))
                    i += len(m.group(0))
                    continue
                i += 2
                continue
            if ch == "\r":
                self.c = 0
                self.pending = False
            elif ch == "\n":
                self._newline()
                self.pending = False
            elif ch == "\b":
                self.c = max(0, self.c - 1)
                self.pending = False
            elif ord(ch) >= 32:
                self._put(ch)
            i += 1

    def _csi(self, params, final):
        nums = [int(x) if x.isdigit() else 0 for x in params.split(";")] if params else []
        n = nums[0] if nums and nums[0] > 0 else 1
        self.pending = False
        if final == "A":
            self.r = max(0, self.r - n)
        elif final == "B":
            self.r = min(self.rows - 1, self.r + n)
        elif final == "C":
            self.c = min(self.cols - 1, self.c + n)
        elif final == "D":
            self.c = max(0, self.c - n)
        elif final == "H":
            self.r = (nums[0] - 1) if len(nums) > 0 and nums[0] else 0
            self.c = (nums[1] - 1) if len(nums) > 1 and nums[1] else 0
        elif final == "J":
            mode = nums[0] if nums else 0
            if mode == 2:
                self.cells = [[" "] * self.cols for _ in range(self.rows)]
            else:
                for c in range(self.c, self.cols):
                    self.cells[self.r][c] = " "
                for r in range(self.r + 1, self.rows):
                    self.cells[r] = [" "] * self.cols
        elif final == "K":
            for c in range(self.c, self.cols):
                self.cells[self.r][c] = " "

    def line(self, r):
        return "".join(self.cells[r]).rstrip()

    def text(self):
        return "\n".join(self.line(r) for r in range(self.rows)).rstrip("\n")


class Term:
    def __init__(self, sksh, home, cols=80, rows=24, env=None, args=(), cwd=None):
        self.cols, self.rows = cols, rows
        e = {
            "PATH": "/usr/local/bin:/usr/bin:/bin",
            "HOME": home,
            "TERM": "xterm",
            "USER": "root",
            "LANG": "C.UTF-8",
        }
        if env:
            e.update(env)
        pid, fd = pty.fork()
        if pid == 0:
            try:
                os.chdir(cwd or home)
                os.execve(sksh, [sksh] + list(args), e)
            finally:
                os._exit(127)
        self.pid, self.fd = pid, fd
        self.set_size(rows, cols)
        self.raw = b""
        self.mark = 0

    def set_size(self, rows, cols):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def _pump(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return False
        try:
            data = os.read(self.fd, 65536)
        except OSError:
            return None
        if not data:
            return None
        self.raw += data
        return True

    def text(self, start=None):
        return self.raw[self.mark if start is None else start:].decode("utf-8", "replace")

    def expect(self, pattern, timeout=5.0, clean=True):
        """等输出（从上次的标记开始）匹配正则；返回匹配的文本。clean=True 时先去掉 ANSI 序列"""
        deadline = time.time() + timeout
        while True:
            t = self.text()
            s = strip_ansi(t) if clean else t
            m = re.search(pattern, s, re.S)
            if m:
                return m
            left = deadline - time.time()
            if left <= 0:
                raise AssertionError("等待 %r 超时，输出是：\n%r" % (pattern, s[-1500:]))
            if self._pump(min(left, 0.2)) is None:
                t = self.text()
                s = strip_ansi(t) if clean else t
                m = re.search(pattern, s, re.S)
                if m:
                    return m
                raise AssertionError("sksh 已退出，等待 %r，输出是：\n%r" % (pattern, s[-1500:]))

    def prompt(self, timeout=5.0):
        self.expect(PROMPT_RE, timeout)
        self.mark = len(self.raw)

    def send(self, s, mark=True):
        if mark:
            self.mark = len(self.raw)
        os.write(self.fd, s.encode("utf-8"))

    def run(self, cmd, timeout=5.0):
        """输入一条命令并回车，等到下一个提示符，返回这段输出（去掉 ANSI）"""
        self.send(cmd + "\r")
        start = self.mark
        self.expect(r"\n.*" + PROMPT_RE, timeout)
        out = strip_ansi(self.text(start))
        self.mark = len(self.raw)
        return out

    def settle(self, quiet=0.3):
        """读完所有输出，直到安静 quiet 秒"""
        while self._pump(quiet):
            pass

    def screen(self):
        sc = Screen(self.rows, self.cols)
        sc.feed(self.raw.decode("utf-8", "replace"))
        return sc

    def wait_exit(self, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._pump(0.1)
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return st
        raise AssertionError("sksh 没有退出")

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def ok(cond, msg):
    if not cond:
        raise AssertionError(msg)


def lines_of(out):
    return [l.rstrip("\r") for l in out.replace("\r\n", "\n").split("\n")]


# ======================================================================

@test
def prompt_format(t, home):
    t.prompt()
    raw = t.raw.decode("utf-8", "replace")
    host = socket.gethostname().split(".")[0]
    want = "\x1b[1;32mroot@%s\x1b[0m:\x1b[1;34m~\x1b[0m# " % host
    ok(raw.endswith(want), "提示符格式不对：%r（期望结尾是 %r）" % (raw[-80:], want))
    os.mkdir(os.path.join(home, "dir"))
    t.run("cd dir")
    raw = t.text(0)
    ok(raw.endswith("\x1b[1;34m~/dir\x1b[0m# "), "cd 之后提示符不对：%r" % raw[-60:])
    t.run("cd /tmp")
    ok(t.text(0).endswith("\x1b[1;34m/tmp\x1b[0m# "), "绝对路径提示符不对")


@test
def simple_echo(t, home):
    t.prompt()
    out = t.run("echo hello; echo __SKDO''NE_1__")
    ls = lines_of(out)
    ok("hello" in ls and "__SKDONE_1__" in ls, "输出不对：%r" % out)


@test
def cursor_left_insert(t, home):
    t.prompt()
    out = t.run("echo abc" + LEFT + LEFT + "X")
    ok("aXbc" in lines_of(out), "插入不对：%r" % out)


@test
def ctrl_a_ctrl_e(t, home):
    t.prompt()
    out = t.run("cho hi" + "\x01" + "e" + "\x05" + "!")
    ok("hi!" in lines_of(out), "Ctrl-A/E 不对：%r" % out)


@test
def home_end_keys(t, home):
    t.prompt()
    out = t.run("cho x" + HOME + "e" + END + "y")
    ok("xy" in lines_of(out), "Home/End 不对：%r" % out)
    out = t.run("cho z" + "\x1bOH" + "e")
    ok("z" in lines_of(out), "ESC O H 不对：%r" % out)


@test
def ctrl_u_k_w_y(t, home):
    t.prompt()
    out = t.run("garbage text" + "\x15" + "echo clean")
    ok("clean" in lines_of(out), "Ctrl-U 不对：%r" % out)
    out = t.run("echo keep remove" + LEFT * 6 + "\x0b")
    ok("keep" in lines_of(out), "Ctrl-K 不对：%r" % out)
    out = t.run("echo one two" + "\x17" + "three")
    ok("one three" in lines_of(out), "Ctrl-W 不对：%r" % out)
    out = t.run("echo AAA" + "\x17" + "\x19" + "\x19")
    ok("AAAAAA" in lines_of(out), "Ctrl-Y 不对：%r" % out)


@test
def backspace_delete(t, home):
    t.prompt()
    out = t.run("echo abcd" + "\x7f" + LEFT + LEFT + DEL)
    ok("ac" in lines_of(out), "Backspace/Delete 不对：%r" % out)
    out = t.run("echo xy" + LEFT + "\x04")
    ok("x" in lines_of(out), "Ctrl-D 删除字符不对：%r" % out)


@test
def word_motion(t, home):
    t.prompt()
    out = t.run("echo aaa bbb" + "\x1bb" + "X" + "\x1bf" + "Y")
    ok("aaa XbbbY" in lines_of(out), "Alt-B/Alt-F 不对：%r" % out)


@test
def utf8_cjk(t, home):
    t.prompt()
    out = t.run("echo 你好" + LEFT + "X")
    ok("你X好" in lines_of(out), "汉字中间插入不对：%r" % out)
    out = t.run("echo 你好世" + "\x7f")
    ok("你好" in lines_of(out), "汉字退格不对：%r" % out)
    out = t.run("echo 一二三四" + LEFT * 2 + "\x7f")
    ok("一三四" in lines_of(out), "汉字中间退格不对：%r" % out)


@test
def wrap_and_cjk_screen(t, home):
    """窄终端（20 列）里输入跨行的长命令（含汉字），在中间编辑，检查屏幕显示和执行结果"""
    t.set_size(24, 20)
    t.cols = 20
    t.prompt()
    t.send("PS1='$ '\r")    # 换一个短提示符；下一次读行时 shell 也会重新读到窗口大小
    t.expect(r"\n\$ $")
    t.mark = len(t.raw)
    long_cmd = "echo 0123456789abcdefghij你好世界XYZ"
    t.send(long_cmd)
    t.settle()
    t.send(LEFT * 7 + "#")      # 光标移到“你”的前面插入 #
    t.settle()
    sc = t.screen()
    shown = "".join(sc.line(r) for r in range(sc.rows)).replace(" ", "")
    ok("$echo0123456789abcdefghij#你好世界XYZ" in shown, "屏幕内容不对：\n%s" % sc.text())
    # 光标应该紧跟在 # 后面：屏幕上光标处的字符是“你”
    ok(sc.cells[sc.r][sc.c] == "你", "光标位置不对：(%d,%d) 处是 %r\n%s" % (sc.r, sc.c, sc.cells[sc.r][sc.c], sc.text()))
    t.send("\r")
    t.expect(r"0123456789abcdefghij#你好世界XYZ\s*\n")
    t.expect(r"\$ $")
    # 一个汉字正好落在行尾只剩一列的位置：必须整体挪到下一行
    t.mark = len(t.raw)
    t.send("echo 123456789012汉")    # "$ " 2 列 + 17 列 = 19 列，只剩 1 列放不下汉字
    t.settle()
    sc = t.screen()
    ok(sc.cells[sc.r][0] == "汉" and sc.c == 2, "宽字符折行不对：光标 (%d,%d)\n%s" % (sc.r, sc.c, sc.text()))
    t.send(HOME)
    t.settle()
    sc2 = t.screen()
    ok(sc2.r == sc.r - 1 and sc2.c == 2, "Home 后光标不对：(%d,%d)" % (sc2.r, sc2.c))
    t.send(END + "\x15")
    t.settle()
    sc3 = t.screen()
    ok(sc3.line(sc3.r) == "$" and sc3.line(sc3.r + 1) == "" and sc3.c == 2,
       "Ctrl-U 后屏幕没有清干净：\n%s" % sc3.text())
    t.send("echo ok\r")
    t.expect(r"\nok\s*\n")


@test
def exact_line_fill(t, home):
    """输入正好写满一行时，光标要停在下一行开头（不能停在“等待折行”状态）"""
    t.set_size(24, 20)
    t.cols = 20
    t.prompt()
    t.run("PS1='> # '")
    t.send("echo 45678901234")    # "> # " 4 列 + 16 列 = 正好 20 列
    t.settle()
    sc = t.screen()
    ok(sc.c == 0 and not sc.pending, "写满一行后光标应在下一行开头：(%d,%d) pending=%s" % (sc.r, sc.c, sc.pending))
    t.send("6" + LEFT + LEFT + "X")
    t.settle()
    sc = t.screen()
    shown = sc.line(sc.r - 1) + sc.line(sc.r)
    ok(shown == "> # echo 4567890123X46", "编辑后屏幕不对：%r\n%s" % (shown, sc.text()))
    ok(sc.c == 0 and sc.cells[sc.r][0] == "4", "光标应该在第二行的 4 上：(%d,%d)\n%s" % (sc.r, sc.c, sc.text()))
    t.send("\r")
    t.expect(r"4567890123X46")


@test
def history_and_file(t, home):
    t.prompt()
    t.run("echo first")
    t.run("echo second")
    out = t.run(UP + UP)
    ok("first" in lines_of(out), "↑↑ 没有取回 first：%r" % out)
    out = t.run(UP + UP + DOWN)
    ok("second" in lines_of(out) or "first" in lines_of(out), "↓ 不对：%r" % out)
    out = t.run("history")
    ok(re.search(r"\d+\s+echo first", out) and re.search(r"\d+\s+history", out), "history 输出不对：%r" % out)
    hf = open(os.path.join(home, ".sksh_history"), encoding="utf-8").read()
    ok("echo first\necho second\n" in hf, "历史文件不对：%r" % hf)
    t.send("exit\r")
    t.wait_exit()


@test
def history_persists(t, home):
    with open(os.path.join(home, ".sksh_history"), "w") as f:
        f.write("echo from_last_session\n")
    t.prompt()
    out = t.run(UP)
    ok("from_last_session" in lines_of(out), "没有读到上次的历史：%r" % out)


@test
def history_draft(t, home):
    t.prompt()
    t.run("echo old")
    t.send("echo draft" + UP + DOWN)
    t.settle()
    out = t.run("")
    ok("draft" in lines_of(out), "翻历史后草稿丢了：%r" % out)


@test
def ctrl_c_line(t, home):
    t.prompt()
    t.send("echo should_not_run" + "\x03")
    t.expect(r"\^C\s*\n.*" + PROMPT_RE)
    t.mark = len(t.raw)
    out = t.run("echo $?; echo ok")
    ok("130" in lines_of(out) and "ok" in lines_of(out), "Ctrl-C 之后不对：%r" % out)
    ok("should_not_run" not in lines_of(strip_ansi(t.text(0)).replace("echo should_not_run", "")),
       "被 Ctrl-C 放弃的命令执行了")


@test
def ctrl_d_exit(t, home):
    t.prompt()
    t.send("\x04")
    st = t.wait_exit()
    ok(os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0, "Ctrl-D 退出状态不对：%r" % st)
    ok("exit" in strip_ansi(t.text(0)), "没有打印 exit")


@test
def continuation_prompt(t, home):
    t.prompt()
    t.send("if true\r")
    t.expect(r"> $")
    t.send("then echo yes\r", mark=False)
    t.expect(r"> $")
    t.send("fi\r", mark=False)
    t.expect(r"\nyes\s*\n.*" + PROMPT_RE)
    t.mark = len(t.raw)
    t.send("echo 'a\r")
    t.expect(r"> $")
    t.send("b'\r", mark=False)
    t.expect(r"\na\s*\nb\s*\n")
    t.prompt()
    t.send("for i in 1 2 # 注释\r")
    t.expect(r"> $")
    t.send("do echo n$i\r", mark=False)
    t.expect(r"> $")
    t.send("done\r", mark=False)
    t.expect(r"n1\s*\nn2\s*\n")
    t.prompt()
    out = t.run("history 4")
    ok("if true; then echo yes; fi" in out, "多行命令在历史里没有合成一行：%r" % out)
    ok("echo 'a'$'\\n''b'" in out, "引号里的换行在历史里不对：%r" % out)
    ok("for i in 1 2; do echo n$i; done" in out, "注释没有去掉：%r" % out)
    # 从历史里取回合成的那一行再执行，结果应该一样
    out = t.run(UP + UP + UP)
    ok(re.search(r"\na\s*\nb\s*\n", out), "取回多行引号命令执行结果不对：%r" % out)


@test
def typeahead_reaches_next_program(t, home):
    """回车之后紧跟着输入的内容属于下一个程序：行编辑器不能把它多读走"""
    t.prompt()
    t.send("tr a-z A-Z\rtypeahead-line\r")
    time.sleep(0.5)
    t.send("\x04", mark=False)
    t.prompt()
    raw = strip_ansi(t.text(0))
    ok("TYPEAHEAD-LINE" in raw, "tr 没有读到提前输入的内容：%r" % raw[-300:])
    t.send("echo one\recho two\r")
    t.expect(r"\none\s*\n.*\ntwo\s*\n")


@test
def tab_complete_file(t, home):
    for n, c in (("beta_file.txt", "BETA-CONTENT\n"), ("alpha1.txt", ""), ("alpha2.txt", ""), ("with space.txt", "SPACE\n")):
        with open(os.path.join(home, n), "w") as f:
            f.write(c)
    os.mkdir(os.path.join(home, "subdir"))
    t.prompt()
    out = t.run("cat bet\t")
    ok("BETA-CONTENT" in out, "单个候选补全不对：%r" % out)
    out = t.run("cat with\t")
    ok("SPACE" in out, "带空格的文件名补全不对：%r" % out)
    t.send("echo alp\t")
    t.settle()
    t.send("\t")
    t.expect(r"alpha1\.txt\s+alpha2\.txt")
    t.settle()
    out = t.run("")
    ok("alpha" in lines_of(out), "公共前缀补全不对：%r" % out)
    t.run("cd sub\t")
    out = t.run("pwd")
    ok(any(l.endswith("/subdir") for l in lines_of(out)), "目录补全不对：%r" % out)


@test
def tab_complete_command(t, home):
    t.prompt()
    out = t.run("ech\thi")
    ok("hi" in lines_of(out), "命令名补全不对：%r" % out)
    out = t.run("histo\t")
    ok(any(re.match(r"\s*\d+\s+", l) for l in lines_of(out)), "内建命令补全不对：%r" % out)
    out = t.run("echo $HOM\t")
    ok(home in out, "变量名补全不对：%r" % out)


@test
def job_control(t, home):
    t.prompt()
    t.send("sleep 30\r")
    time.sleep(0.5)
    t.send("\x1a", mark=False)
    t.expect(r"\[1\]\+\s+已停止\s+sleep 30\s*\n.*" + PROMPT_RE)
    t.mark = len(t.raw)
    out = t.run("jobs")
    ok(re.search(r"\[1\]\+\s+已停止\s+sleep 30", out), "jobs 输出不对：%r" % out)
    out = t.run("bg")
    ok("[1]+ sleep 30 &" in out, "bg 输出不对：%r" % out)
    out = t.run("jobs")
    ok(re.search(r"\[1\]\+\s+运行中\s+sleep 30 &", out), "bg 之后 jobs 不对：%r" % out)
    t.send("fg\r")
    t.expect(r"sleep 30\s*\n")
    time.sleep(0.3)
    t0 = time.time()
    t.send("\x03", mark=False)
    t.prompt(timeout=5)
    ok(time.time() - t0 < 3, "Ctrl-C 没有及时结束前台作业")
    out = t.run("echo $?")
    ok("130" in lines_of(out), "被 Ctrl-C 结束的作业状态应是 130：%r" % out)
    out = t.run("jobs")
    ok("sleep" not in out, "作业表没清空：%r" % out)


@test
def background_notify(t, home):
    t.prompt()
    out = t.run("sleep 0.3 &")
    ok(re.search(r"\[1\] \d+", out), "后台启动没有显示作业号：%r" % out)
    time.sleep(0.8)
    out = t.run("")
    ok(re.search(r"\[1\]\+\s+已完成\s+sleep 0\.3", out), "后台完成通知不对：%r" % out)
    out = t.run("(exit 3) &")
    time.sleep(0.4)
    out = t.run("")
    ok(re.search(r"\[1\]\+\s+退出 3", out), "后台非零退出通知不对：%r" % out)


@test
def fg_ctrl_c_and_pipeline(t, home):
    t.prompt()
    t0 = time.time()
    t.send("sleep 30 | cat\r")
    time.sleep(0.4)
    t.send("\x03", mark=False)
    t.prompt()
    ok(time.time() - t0 < 4, "Ctrl-C 没有结束整个管道")
    t.send("sleep 30 | cat\r")
    time.sleep(0.4)
    t.send("\x1a", mark=False)
    t.expect(r"已停止\s+sleep 30 \| cat")
    t.prompt()
    out = t.run("kill %1; sleep 0.2; jobs")
    t.settle()
    out = t.run("jobs")
    ok("sleep" not in out, "kill %%1 没有结束停止的管道：%r" % out)


@test
def ctrl_c_builtin_loop(t, home):
    t.prompt()
    t.send("while :; do :; done\r")
    time.sleep(0.3)
    t.send("\x03", mark=False)
    t.prompt(timeout=4)
    out = t.run("echo alive")
    ok("alive" in lines_of(out), "Ctrl-C 没有打断内建命令循环")


@test
def ctrl_z_stops_line(t, home):
    t.prompt()
    t.send("sleep 30; echo should_not_print\r")
    time.sleep(0.4)
    t.send("\x1a", mark=False)
    t.expect(r"已停止")
    t.prompt()
    t.settle(0.5)
    ok("should_not_print" not in strip_ansi(t.text(0)).replace("echo should_not_print", ""),
       "Ctrl-Z 之后同一行的后续命令不应执行")
    out = t.run("exit")
    ok("还有已停止的作业" in out, "退出时没有提醒停止的作业：%r" % out)
    t.send("exit\r")
    t.wait_exit()


@test
def read_builtin(t, home):
    t.prompt()
    t.send("read -p 'name? ' n\r")
    t.expect(r"name\? $")
    t.send("Tom\r", mark=False)
    t.prompt()
    out = t.run("echo hi $n")
    ok("hi Tom" in lines_of(out), "read 不对：%r" % out)


@test
def wait_interrupt(t, home):
    t.prompt()
    t.run("sleep 30 &")
    t.send("wait\r")
    time.sleep(0.3)
    t.send("\x03", mark=False)
    t.prompt(timeout=4)
    out = t.run("kill %1; echo killed")
    ok("killed" in out, "wait 没有被 Ctrl-C 打断")


@test
def ps1_custom(t, home):
    t.prompt()
    t.send("PS1='[$(echo dyn) \\W]# '\r")
    t.expect(r"\[dyn ~\]# $")
    t.mark = len(t.raw)
    t.send("cd /tmp\r")
    t.expect(r"\[dyn tmp\]# $")


@test
def ctrl_l_clear(t, home):
    t.prompt()
    t.send("echo abc" + "\x0c")
    t.expect(r"\x1b\[H\x1b\[2J.*# echo abc", clean=False)
    out = t.run("")
    ok("abc" in lines_of(out), "Ctrl-L 之后输入内容丢了：%r" % out)


@test
def login_shell_rc(t, home):
    t.prompt()
    out = t.run("alias")
    ok("rcalias" in out, "登录 shell 没有执行 ~/.skshrc：%r" % out)


def run_all(sksh, keyword=None):
    passed = failed = 0
    for fn in TESTS:
        name = fn.__name__
        if keyword and keyword not in name:
            continue
        home = tempfile.mkdtemp(prefix="sksh-pty-")
        args = ()
        if name == "login_shell_rc":
            with open(os.path.join(home, ".skshrc"), "w") as f:
                f.write("alias rcalias='echo rc'\n")
            args = ("-l",)
        t = Term(sksh, home, args=args)
        try:
            fn(t, home)
            passed += 1
            print("✓ %s" % name)
        except AssertionError as e:
            failed += 1
            print("✗ %s\n    %s" % (name, str(e).replace("\n", "\n    ")))
        except Exception as e:
            failed += 1
            print("✗ %s（异常）%r" % (name, e))
        finally:
            t.close()
            shutil.rmtree(home, ignore_errors=True)
    print("\n交互测试：通过 %d，失败 %d" % (passed, failed))
    return failed


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sksh", default=os.environ.get("SKSH", DEFAULT_SKSH))
    ap.add_argument("-k")
    a = ap.parse_args()
    sys.exit(1 if run_all(a.sksh, a.k) else 0)
