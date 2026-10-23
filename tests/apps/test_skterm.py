#!/usr/bin/env python3
"""
tests/apps/test_skterm.py — 终端 skterm 的自动化测试

    bash tests/apps/run.sh skterm                     （编译并运行全部测试）
    bash tests/apps/run.sh skterm -- --only echo,top  （只跑几项）

不需要 QEMU：skterm、sksh、skbox 都是 32 位静态程序，宿主机直接运行；tools/mock_display.py
扮演显示服务器。skterm 里运行的是真正的 sksh（SHELL=$BIN/sksh），PATH 指向一个临时目录，
里面是 `skbox --install` 建好的 ls、top、sleep、seq……；HOME 是临时目录，~/.skshrc 把提示符
设成固定的 “sk> ”。

怎么“看到”终端里的字：skterm 的调试钩子（见 userspace/gui/apps/skterm/main.c 开头）——
环境变量 SKTERM_DEBUG=1 时，给它发 SIGUSR1，它就把当前画面打印成一行 JSON
（每行文字、每格是宽字符还是普通字符、颜色属性）。测试反复取画面，直到满足条件。
颜色、光标、选区、滚动条等视觉效果还会用像素检查和截图来确认（截图在 --out 目录，请打开看看）。
"""
import json
import signal
import struct
import subprocess
import sys
import tempfile

from apptest import check, App, settle, shot, pixel, temp_home, run_tests, md, shutil, time, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PROMPT = "sk>"                 # ~/.skshrc 里设置的提示符（"sk> "，取画面时行尾空格被去掉）

# 版面常量：与 userspace/gui/apps/skterm/skterm.h 一致
PAD_L, PAD_T, PAD_R, PAD_B = 16, 6, 20, 6


def font_metrics():
    """mono-16.skf 的格子宽和行高（与 libsketch 读到的完全一样）"""
    data = open(os.path.join(ROOT, "assets", "fonts", "mono-16.skf"), "rb").read()
    _m, _size, ascent, descent, line_h, _n, _i, _b, _flags, cell_w = struct.unpack_from("<4sHhhHIIIHH", data, 0)
    return cell_w, max(line_h, ascent + descent)


CW, LH = font_metrics()


def win_size(cols, rows):
    return PAD_L + cols * CW + PAD_R, PAD_T + rows * LH + PAD_B


def cell_xy(col, row):
    """第 row 行第 col 格的中心（窗口坐标）"""
    return PAD_L + col * CW + CW // 2, PAD_T + row * LH + LH // 2


DAMAGE_RX = re.compile(r"win=(\d+) \((-?\d+),(-?\d+) (\d+)x(\d+)\)")


def damages_since(d, start, win):
    out = []
    for req in list(d.requests[start:]):
        if req[2] != "DAMAGE":
            continue
        m = DAMAGE_RX.search(req[3])
        if m and int(m.group(1)) == win.id:
            out.append(tuple(int(v) for v in m.groups()[1:]))
    return out


def ensure_tools(bindir):
    """skterm 的测试还需要 sksh 和 skbox；run.sh 只编译了 skterm 时在这里补编"""
    need = [p for p in ("sksh", "skbox") if not os.path.exists(os.path.join(bindir, p))]
    if not need:
        return True
    cc = os.environ.get("SKCC") or os.path.join(os.environ.get("SKETCHOS_WORK", os.path.expanduser("~/sketchos-work")),
                                                "toolchain", "bin", "sk-cc")
    build = os.path.dirname(bindir)
    print("  （编译测试需要的 %s → %s）" % (" ".join(need), build), flush=True)
    r = subprocess.run(["make", "-C", os.path.join(ROOT, "userspace"), "CC=" + cc, "BUILD=" + build] + need,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    return r.returncode == 0


class QuietApp(App):
    """和 App 一样，只是不把很长的画面 JSON 打印出来"""

    def _read(self):
        for raw in self.p.stdout:
            line = raw.decode("utf-8", "replace").rstrip("\n")
            self.lines.append(line)
            self.q.put(line)
            if not line.startswith("SKTERM-DUMP "):
                print("    app> " + line, flush=True)


class Term:
    """一个运行中的 skterm（带临时 HOME、PATH）和取画面、打字的辅助函数"""

    def __init__(self, d, args, argv=()):
        self.d = d
        self.args = args
        self.home = temp_home()
        with open(os.path.join(self.home, ".skshrc"), "w") as f:
            f.write("PS1='sk> '\n")
        self.bindir = tempfile.mkdtemp(prefix="skterm-bin.")
        subprocess.run([os.path.join(args.bin, "skbox"), "--install", self.bindir],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        env = {"SHELL": os.path.join(args.bin, "sksh"), "HOME": self.home, "PATH": self.bindir,
               "SKTERM_DEBUG": "1"}
        for k in ("COLUMNS", "LINES"):
            os.environ.pop(k, None)
        self.app = QuietApp(d, [os.path.join(args.bin, "skterm")] + list(argv), env=env)
        self.win = d.wait_window(timeout=10)
        line = self.app.expect(r"^skterm: child pid=", timeout=5)
        m = re.search(r"pid=(\d+)", line or "")
        self.child = int(m.group(1)) if m else 0

    # ---------- 画面 ----------
    def dump(self, timeout=3.0):
        if self.app.p.poll() is not None:
            return None
        self.app.p.send_signal(signal.SIGUSR1)
        line = self.app.expect(r"^SKTERM-DUMP ", timeout=timeout)
        if not line:
            return None
        return json.loads(line[len("SKTERM-DUMP "):])

    def wait(self, pred, timeout=8.0, what="画面"):
        """反复取画面直到 pred(画面) 为真；超时打印最后的画面并返回 None"""
        deadline = time.time() + timeout
        s = None
        while time.time() < deadline:
            s = self.dump()
            if s is not None:
                try:
                    if pred(s):
                        return s
                except (IndexError, KeyError):
                    pass
            time.sleep(0.08)
        print("  （等待%s超时，最后的画面：）" % what)
        for ln in (s or {}).get("lines", []):
            if ln:
                print("    | " + ln)
        return None

    def wait_text(self, text, timeout=8.0):
        return self.wait(lambda s: any(text in ln for ln in s["lines"]), timeout, "“%s”" % text)

    def wait_line(self, text, timeout=8.0):
        return self.wait(lambda s: text in s["lines"], timeout, "整行“%s”" % text)

    def at_prompt(self, s):
        """光标停在一个空的提示符后面（提示符前面可以有别的字：上一条输出没有换行时）"""
        ln = s["lines"][s["cy"]]
        return ln.endswith(PROMPT) and s["cx"] == len(ln) + 1

    def wait_prompt(self, timeout=10.0, after=None):
        """等到光标停在一个空的提示符后面（after：提示符所在行或它上面必须有包含它的行）"""
        def ok(s):
            if not self.at_prompt(s):
                return False
            return after is None or any(after in ln for ln in s["lines"][:s["cy"] + 1])
        return self.wait(ok, timeout, "提示符")

    def row_of(self, s, text, last=True):
        rows = [i for i, ln in enumerate(s["lines"]) if ln == text]
        return (rows[-1] if last else rows[0]) if rows else -1

    # ---------- 输入 ----------
    def type(self, text):
        self.d.type_text(self.win, text)

    def run(self, cmd):
        self.type(cmd + "\n")

    def key(self, combo):
        self.d.key(self.win, combo)

    def shot(self, name):
        settle(self.d, self.win, quiet=0.25)
        return shot(self.d, self.win, self.args, name)

    def close(self):
        self.app.stop()
        shutil.rmtree(self.home, ignore_errors=True)
        shutil.rmtree(self.bindir, ignore_errors=True)


def start(d, args, argv=(), clear=True):
    """启动 skterm，等到提示符；clear=True 时先清屏（宿主机的 /etc/profile 可能打印杂讯）"""
    t = Term(d, args, argv)
    s = t.wait_prompt(timeout=15)
    check(s is not None, "出现提示符 “sk> ”")
    if clear and s is not None:
        t.run("clear")
        t.wait(lambda s: s["cy"] == 0 and t.at_prompt(s), what="清屏")
    return t


def color_close(c, ref, tol=10):
    return all(abs(a - b) <= tol for a, b in zip(c, ref))


def has_color(win, rect, ref, tol=10):
    x0, y0, w, h = rect
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            if color_close(pixel(win, x, y), ref, tol):
                return True
    return False


def sleepers(pattern, states="RSDT"):
    """还活着（不是僵尸）的、命令行等于 pattern 的进程；states 限定进程状态"""
    out = []
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            if open("/proc/%s/cmdline" % p, "rb").read() == pattern:
                if open("/proc/%s/stat" % p).read().rsplit(")", 1)[1].split()[0] in states:
                    out.append(p)
        except OSError:
            pass
    return out


def wait_proc(pattern, states, timeout=4.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sleepers(pattern, states):
            return True
        time.sleep(0.05)
    return False


PAPER = (0xFF, 0xFD, 0xF7)
INK = (0x2B, 0x2B, 0x2B)
ANSI_RED = (0xC4, 0x3E, 0x37)
ANSI_BLUE12 = (0x4A, 0x7B, 0xD0)


# ====================================================================== 测试

def test_prompt(d, args):
    t = Term(d, args)
    try:
        s = t.wait_prompt(timeout=15)
        check(s is not None, "启动后出现 sksh 的提示符")
        if not s:
            return
        check(s["cols"] == 80 and s["rows"] == 24, "默认 80×24（实际 %d×%d）" % (s["cols"], s["rows"]))
        w, h = win_size(80, 24)
        check((t.win.w, t.win.h) == (w, h), "窗口尺寸 %dx%d（期望 %dx%d）" % (t.win.w, t.win.h, w, h))
        check(t.win.title == "终端", "窗口标题是“终端”")
        check(t.win.flags & 1, "窗口可以改变大小")
        t.run("echo TERM=$TERM")
        check(t.wait_line("TERM=xterm-256color") is not None, "子进程的 TERM=xterm-256color")
        t.run("clear")
        s = t.wait(lambda s: s["cy"] == 0 and t.at_prompt(s), what="清屏")
        check(s is not None, "clear 之后提示符在第一行")
        # 光标：实心方块（墨色）
        x, y = cell_xy(4, 0)
        check(has_color(t.win, (x - 3, y - 6, 6, 12), INK, 30), "光标是墨色的实心方块")
        t.shot("skterm-prompt")
        # DECSCUSR：ESC[5 q 换成竖线光标，ESC[0 q 恢复方块
        t.run(r"printf '\033[5 q'; clear")
        s = t.wait(lambda s: s["cy"] == 0 and t.at_prompt(s), what="竖线光标")
        time.sleep(0.3)
        x, y = cell_xy(4, 0)
        check(has_color(t.win, (x - 4, y - 4, 3, 8), INK, 40) and
              not has_color(t.win, (x, y - 4, 3, 8), INK, 40), "ESC[5 q 之后光标是一条竖线")
        t.shot("skterm-bar-cursor")
        t.run(r"printf '\033[0 q'; clear")
        t.wait(lambda s: s["cy"] == 0 and t.at_prompt(s), what="方块光标")
        time.sleep(0.3)
        check(has_color(t.win, (x - 1, y - 4, 3, 8), INK, 30), "ESC[0 q 恢复方块光标")
    finally:
        t.close()


def test_echo(d, args):
    t = start(d, args)
    try:
        t.run("echo 你好 hello")
        s = t.wait_line("你好 hello")
        check(s is not None, "echo 你好 hello 的输出出现在下一行")
        if s:
            r = t.row_of(s, "你好 hello")
            check(s["cells"][r] == "W-W-.aaaaa", "汉字各占 2 格（格子图 %r）" % s["cells"][r])
            check(t.at_prompt(s) and s["cy"] == r + 1, "输出之后回到新的提示符")
        # 宽字符在最后一列放不下：挪到下一行，最后一格留空
        t.run("printf '%79s汉字\\n' x")
        s = t.wait(lambda s: any(ln.startswith("汉字") for ln in s["lines"]), what="折行的汉字")
        check(s is not None, "第 80 列放不下的汉字折到下一行")
        if s:
            r = [i for i, ln in enumerate(s["lines"]) if ln.startswith("汉字")][-1]
            check(len(s["cells"][r - 1]) == 79 and s["cells"][r - 1].endswith("a"),
                  "上一行只用了 79 格（%d）" % len(s["cells"][r - 1]))
            check(s["cells"][r].startswith("W-W-"), "下一行从汉字开始")
        # 正好写满 80 列再换行：不应多出空行（延迟折行）
        t.run("printf '%80s\\n' Z; echo after")
        s = t.wait_line("after")
        if s:
            r = t.row_of(s, "after")
            check(s["lines"][r - 1].endswith("Z") and len(s["lines"][r - 1]) == 80,
                  "写满 80 列后的换行不产生空行（延迟折行）")
        # 行编辑器：命令行超过一行时自动折行，左右移动、插入
        t.type("echo " + "a" * 75)
        t.key("home")
        for _ in range(5):
            t.key("right")
        t.type("B")
        t.key("end")
        t.type("C\n")
        s = t.wait_line("B" + "a" * 75 + "C")
        check(s is not None, "长命令行折行后在中间插入字符（sksh 行编辑器）")
        t.shot("skterm-echo")
    finally:
        t.close()


def test_colors(d, args):
    t = start(d, args)
    try:
        t.run(r"printf '\033[31mred\033[0m \033[1;94mbold\033[0m \033[7mrev\033[0m \033[4mul\033[0m"
              r" \033[38;5;196mX\033[48;2;10;20;30mY\033[0m\n'")
        s = t.wait(lambda s: any(ln.startswith("red bold rev") for ln in s["lines"]), what="彩色输出")
        check(s is not None, "printf 输出彩色文字")
        if not s:
            return
        r = [i for i, ln in enumerate(s["lines"]) if ln.startswith("red bold")][-1]
        attrs = {(a[0], a[1]): a for a in s["attrs"]}
        check(attrs.get((r, 0), [0, 0, 0, None])[2:5] == [3, "1", "default"], "red 是前景色 1（红）")
        check(attrs.get((r, 4), [0] * 6)[3] == "12" and attrs[(r, 4)][5] & 1, "bold 是亮蓝色 + 粗体")
        check(attrs.get((r, 9), [0] * 6)[5] & 32, "rev 是反显")
        check(attrs.get((r, 13), [0] * 6)[5] & 8, "ul 有下划线")
        check(attrs.get((r, 16), [0] * 6)[3] == "196", "256 色：38;5;196")
        check(attrs.get((r, 17), [0] * 6)[4] == "#0a141e", "真彩色背景：48;2;10;20;30")
        time.sleep(0.3)
        x, y = PAD_L, PAD_T + r * LH
        check(has_color(t.win, (x, y, 3 * CW, LH), ANSI_RED), "“red” 的像素是主题里的红色")
        check(has_color(t.win, (x + 4 * CW, y, 4 * CW, LH), ANSI_BLUE12), "“bold” 的像素是亮蓝色")
        check(has_color(t.win, (x + 9 * CW, y + 2, 3 * CW, LH - 4), INK, 12), "“rev” 的背景是墨色")
        # ls 的颜色
        t.run("mkdir adir; touch afile; ls --color=always")
        s = t.wait(lambda s: any(ln.startswith("adir") for ln in s["lines"]) and t.at_prompt(s), what="ls 输出")
        if s:
            r = [i for i, ln in enumerate(s["lines"]) if ln.startswith("adir")][-1]
            col = s["lines"][r].index("adir")
            a = [a for a in s["attrs"] if a[0] == r and a[1] == col]
            check(a and a[0][3] == "4" and a[0][5] & 1, "ls 把目录显示成粗体蓝色（01;34）")
        t.shot("skterm-colors")
    finally:
        t.close()


def test_vt(d, args):
    """直接检查各种转义序列：用“运行命令”模式跑一段 printf，结束后看画面"""
    seq = (r"\033[2J\033[H"                       # 清屏、回到左上角
           r"\033[3;5HA"                           # 光标定位
           r"\033[5;1Habcdef\033[5;3H\033[2P"      # 删除 2 个字符 → abef
           r"\033[6;1Habcdef\033[6;2H\033[2@"      # 插入 2 个空格 → a  bcdef
           r"\033[7;1Hxxxxxx\033[7;2H\033[3X"      # 擦除 3 个字符 → x   xx
           r"\033[8;1Hkeep\0337\033[9;1Hother\0338!"   # 保存/恢复光标 → keep!
           r"\033[10;1Ha\tb"                       # 制表位 → b 在第 9 列
           r"\033[11;1H123456789\033[11;4H\033[K"  # 擦到行尾 → 123
           r"\033[12;1H123456789\033[12;4H\033[1K" # 擦到行首 → 4 个空格 + 56789
           r"\033[13;1H\033[?7l" + "W" * 85 + r"\033[?7h"   # 关闭自动折行：停在最后一列
           r"\033[15;17r\033[17;1HL1\nL2\nL3\033[r"          # 滚动区域：只在 15~17 行里滚
           r"\033[14;1Hhead"
           r"\033[18;1H\033[1;2;3m\033[0mplain"
           r"\033]2;我的标题\007"
           r"\033[20;1Hline-a\033[21;1Hline-b\033[20;1H\033[L"   # IL 插入一行
           r"\033[23;1H")
    t = Term(d, args, ["printf '%s'" % seq])
    try:
        s = t.wait(lambda s: s["finished"] == 1, what="命令结束")
        check(s is not None, "printf 命令运行结束")
        if not s:
            return
        L = s["lines"]
        check(L[2] == "    A", "CUP：A 在第 3 行第 5 列（%r）" % L[2])
        check(L[4] == "abef", "DCH 删除字符（%r）" % L[4])
        check(L[5] == "a  bcdef", "ICH 插入空格（%r）" % L[5])
        check(L[6] == "x   xx", "ECH 擦除字符（%r）" % L[6])
        check(L[7] == "keep!" and L[8] == "other", "ESC 7 / ESC 8 保存恢复光标（%r）" % L[7])
        check(L[9] == "a       b", "制表位每 8 列（%r）" % L[9])
        check(L[10] == "123", "EL 0 擦到行尾（%r）" % L[10])
        check(L[11] == "    56789", "EL 1 擦到行首（%r）" % L[11])
        check(L[12] == "W" * 80 and L[13] == "head", "?7l 关闭自动折行时不换行（%d 个 W）" % len(L[12]))
        check(L[14] == "L1" and L[15] == "L2" and L[16] == "L3", "滚动区域内换行（%r）" % L[14:17])
        check(L[17] == "plain", "SGR 0 之后恢复默认（%r）" % L[17])
        check(L[19] == "" and L[20] == "line-a" and L[21] == "line-b", "IL 插入一行（%r）" % L[19:22])
        found_title = any(r[2] == "SET_TITLE" and "我的标题" in r[3] for r in d.requests)
        check(found_title, "OSC 2 设置窗口标题")
        check(any("程序已结束，退出码 0" in ln for ln in L), "显示“程序已结束，退出码 0”")
        t.shot("skterm-vt")
    finally:
        t.close()


def test_utf8_split(d, args):
    """UTF-8 和转义序列被 read() 切成两半也能正确拼起来（用 sleep 把输出拆成两次 write）"""
    cmd = r"printf '\344\275'; sleep 0.3; printf '\240\345\245\275 \033['; sleep 0.3; printf '31mR\033[0m\n'"
    t = Term(d, args, [cmd])
    try:
        s = t.wait(lambda s: s["finished"] == 1, what="命令结束")
        check(s is not None and s["lines"][0] == "你好 R", "跨两次 read 的 UTF-8 和 CSI 序列（%r）" %
              (s["lines"][0] if s else None))
        if s:
            check(any(a[0] == 0 and a[1] == 5 and a[3] == "1" for a in s["attrs"]), "被切开的 ESC[31m 仍然生效")
    finally:
        t.close()


def test_clear(d, args):
    t = start(d, args, clear=False)
    try:
        t.run("seq 1 40")
        t.wait_prompt(after="40")
        s = t.dump()
        check(s and s["sb"] > 0, "输出 40 行后有滚动历史（%s 行）" % (s and s["sb"]))
        t.run("clear")
        s = t.wait(lambda s: s["cy"] == 0 and t.at_prompt(s), what="清屏")
        check(s is not None, "clear 清屏，提示符回到第一行")
        if s:
            check(all(ln == "" for ln in s["lines"][1:]), "其余各行都是空的")
            check(s["sb"] == 0, "clear（ESC[3J）也清掉了滚动历史")
    finally:
        t.close()


def test_top(d, args):
    t = start(d, args)
    try:
        t.run("echo marker-before-top")
        t.wait_prompt(after="marker-before-top")
        t.run("top")
        s = t.wait(lambda s: s["alt"] == 1 and any("PID" in ln for ln in s["lines"]), timeout=10, what="top")
        check(s is not None, "top 进入备用屏幕并显示进程表")
        if s:
            check(s["cursor"] == 0, "top 隐藏了光标（?25l）")
            check(not any("marker-before-top" in ln for ln in s["lines"]), "备用屏幕上看不到原来的内容")
            hdr = [a for a in s["attrs"] if a[5] & 32]
            check(len(hdr) > 0, "表头是反显的")
            t.shot("skterm-top")
        # 改变窗口大小：内核给前台进程组（top）发 SIGWINCH，top 按新尺寸重画。
        # （top 每屏输出 rows 行、每行都以 \n 结尾，最后一个换行会让屏幕上滚一行，
        #  所以最后一行总是空的；看倒数第二行有没有内容就知道它用的是不是新行数）
        w, h = win_size(100, 30)
        d.resize(t.win, w, h)
        s = t.wait(lambda s: s["rows"] == 30 and s["alt"] == 1 and s["lines"][28] != "",
                   timeout=10, what="top 按新尺寸重画")
        check(s is not None, "top 收到 SIGWINCH，按 100×30 重画")
        t.type("q")
        s = t.wait(lambda s: s["alt"] == 0 and t.at_prompt(s), what="退出 top")
        check(s is not None, "按 q 退出 top，回到主屏幕的提示符")
        if s:
            check(any(ln == "marker-before-top" for ln in s["lines"]), "原来屏幕上的内容恢复了")
            check(s["cursor"] == 1, "光标重新显示")
    finally:
        t.close()


def test_more(d, args):
    t = start(d, args)
    try:
        t.run("seq 1 100 | more")
        s = t.wait_text("--更多--")
        check(s is not None, "more 显示“--更多--”提示")
        if s:
            r = [i for i, ln in enumerate(s["lines"]) if "--更多--" in ln][-1]
            check(any(a[0] == r and a[5] & 32 for a in s["attrs"]), "提示是反显的")
            t.shot("skterm-more")
        t.type(" ")
        s = t.wait(lambda s: "46" in s["lines"] and "23" not in s["lines"], what="下一页")
        check(s is not None, "空格翻到下一页")
        t.type("q")
        s = t.wait(lambda s: t.at_prompt(s) and not any("--更多--" in ln for ln in s["lines"]), what="退出 more")
        check(s is not None, "按 q 退出 more，提示被 \\r ESC[K 擦掉")
    finally:
        t.close()


def test_resize(d, args):
    t = start(d, args)
    try:
        t.run("mkdir many; cd many; for i in 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24;"
              " do touch file_$i; done; clear; ls")
        s = t.wait(lambda s: t.at_prompt(s) and any("file_24" in ln for ln in s["lines"]), what="ls")
        check(s is not None, "ls 列出 24 个文件")
        rows80 = len([ln for ln in s["lines"] if "file_" in ln]) if s else 0
        w, h = win_size(34, 20)
        d.resize(t.win, w, h)
        s = t.wait(lambda s: s["cols"] == 34 and s["rows"] == 20, what="新尺寸")
        check(s is not None, "窗口变小后终端是 34×20")
        if s:
            check(all(len(ln) <= 34 for ln in s["lines"]), "所有行都截到 34 列以内")
            check(t.at_prompt(s), "提示符（光标所在行）仍然可见")
        t.run("clear; ls")
        s = t.wait(lambda s: t.at_prompt(s) and any("file_24" in ln for ln in s["lines"]), what="ls（窄）")
        if s:
            rows34 = len([ln for ln in s["lines"] if "file_" in ln])
            check(rows34 > rows80 and all(len(ln) <= 34 for ln in s["lines"]),
                  "ls 按新宽度分栏（80 列 %d 行 → 34 列 %d 行）：程序看到了新尺寸" % (rows80, rows34))
        t.shot("skterm-resize-small")
        w, h = win_size(100, 30)
        d.resize(t.win, w, h)
        s = t.wait(lambda s: s["cols"] == 100 and s["rows"] == 30, what="变大")
        check(s is not None, "窗口变大后终端是 100×30")
        t.run("echo $((1+1))wide")
        s = t.wait_line("2wide")
        check(s is not None, "变大之后还能正常使用")
        # 变高、光标在最后一行：从滚动历史里拉回几行（就像把纸往下拉）
        t.run("clear; seq 1 60")
        s0 = t.wait_prompt(after="60")
        w, h = win_size(100, 36)
        d.resize(t.win, w, h)
        s = t.wait(lambda s: s["rows"] == 36, what="变高")
        if s0 and s:
            check(s0["cy"] == 29 and s["cy"] == 35 and t.at_prompt(s), "光标仍在最后一行的提示符上")
            check(s["lines"][0] == str(60 - 34), "变高时从历史区拉回旧的行（第一行 %r）" % s["lines"][0])
            check(s["sb"] == s0["sb"] - 6, "历史区少了 6 行（%d → %d）" % (s0["sb"], s["sb"]))
        # 太小的窗口：库按最小尺寸分配缓冲区（直接发 CONFIGURE，因为 d.resize 要等精确尺寸）
        fd0 = t.win.fd
        d._client_send(t.win, md.EV_CONFIGURE, struct.pack("<Iii", t.win.id, 50, 40))
        deadline = time.time() + 5
        while t.win.fd == fd0 and time.time() < deadline:
            time.sleep(0.05)
        s = t.wait(lambda s: s["cols"] < 100, what="最小尺寸")
        check(s and s["cols"] == 24 and s["rows"] == 6, "最小 24×6（%r）" % ((s["cols"], s["rows"]) if s else None,))
        check((t.win.w, t.win.h) == win_size(24, 6), "窗口不小于最小尺寸（%dx%d）" % (t.win.w, t.win.h))
        if s:
            check(t.at_prompt(s), "缩到最小时提示符仍然可见")
        t.shot("skterm-resize-min")
    finally:
        t.close()


def test_ctrl_c(d, args):
    t = start(d, args)
    try:
        t.run("sleep 100")
        t.wait(lambda s: s["lines"][s["cy"] - 1] == PROMPT + " sleep 100" and s["cx"] == 0, what="sleep 运行")
        time.sleep(0.4)
        t0 = time.time()
        t.key("ctrl+c")
        s = t.wait_prompt(timeout=4, after="sleep 100")
        dt = time.time() - t0
        check(s is not None and dt < 4, "Ctrl+C 打断 sleep 100（%.1f 秒）" % dt)
        if s:
            r = [i for i, ln in enumerate(s["lines"]) if ln.endswith("sleep 100")][-1]
            check("^C" in "".join(s["lines"][r:]), "线路规程回显了 ^C")
        check(not sleepers(b"sleep\x00100\x00"), "sleep 进程已经结束")
        t.run("echo still-alive")
        check(t.wait_line("still-alive") is not None, "shell 还活着")
    finally:
        t.close()


def test_job_control(d, args):
    """Ctrl+Z：线路规程给前台进程组发 SIGTSTP，shell 拿回终端；fg 再把作业放回前台"""
    t = start(d, args)
    try:
        t.run("sleep 300")
        t.wait(lambda s: s["lines"][s["cy"] - 1] == PROMPT + " sleep 300", what="sleep 运行")
        time.sleep(0.3)
        t.key("ctrl+z")
        s = t.wait_prompt(timeout=4, after="sleep 300")
        check(s is not None, "Ctrl+Z 暂停 sleep，回到提示符")
        check(wait_proc(b"sleep\x00300\x00", "T"), "sleep 处于暂停状态（T）")
        t.run("fg")
        check(wait_proc(b"sleep\x00300\x00", "S"), "fg 之后 sleep 继续运行（S）")
        time.sleep(0.3)               # 等 shell 把终端交给它（tcsetpgrp）
        t.key("ctrl+c")
        s = t.wait_prompt(timeout=4, after="fg")
        check(s is not None, "fg 之后 Ctrl+C 结束它")
        check(not sleepers(b"sleep\x00300\x00"), "sleep 300 已经结束")
    finally:
        t.close()


def test_replies(d, args):
    """终端回答程序的询问：DSR 光标位置报告（ESC[6n → ESC[行;列R），回答被线路规程回显成 ^[[行;列R"""
    t = start(d, args)
    try:
        t.run(r"printf '\033[6n'; sleep 1")
        s = t.wait_text("^[[2;1R", timeout=5)
        check(s is not None, "DSR 6 的回答 ESC[2;1R 写回了 pty")
        t.wait_prompt(after="^[[2;1R")
        t.run(r"printf '\033[c'; sleep 1")
        s = t.wait_text("^[[?62;22c", timeout=5)
        check(s is not None, "DA 的回答 ESC[?62;22c")
        t.wait_prompt(after="^[[?62;22c")
        # reset（ESC c）：清屏、恢复默认
        t.run(r"printf '\033[31m'; reset")
        s = t.wait(lambda s: s["cy"] == 0 and t.at_prompt(s), what="reset")
        check(s is not None, "reset 清屏，提示符回到第一行")
        t.run("echo plain-after-reset")
        s = t.wait_line("plain-after-reset")
        if s:
            r = t.row_of(s, "plain-after-reset")
            check(not any(a[0] == r for a in s["attrs"]), "reset 之后颜色恢复默认")
    finally:
        t.close()


def test_keys(d, args):
    """按键编码：在 cat（规范模式，ECHOCTL 把 ESC 回显成 ^[）里按各种键，看终端发出的字节"""
    t = start(d, args)
    try:
        t.run("cat")
        time.sleep(0.4)
        for k in ("up", "f5", "alt+x", "ctrl+left", "delete", "home", "shift+tab"):
            t.key(k)
        s = t.wait_text("^[[A^[[15~^[x^[[1;5D^[[3~^[[H^[[Z", timeout=4)
        check(s is not None, "↑ F5 Alt+x Ctrl+← Delete Home Shift+Tab 的 xterm 序列")
        t.key("ctrl+c")
        t.wait_prompt(after="^[[A")
        # 应用光标键模式（DECCKM）
        t.run(r"printf '\033[?1h'; cat")
        time.sleep(0.4)
        t.key("up")
        t.key("end")
        s = t.wait_text("^[OA^[OF", timeout=4)
        check(s is not None, "?1h 之后方向键发 ESC O A")
        if s:
            check(s["app_cursor"] == 1, "DECCKM 已打开")
        t.key("ctrl+c")
        t.wait_prompt(after="^[OA")
        t.run(r"printf '\033[?1l'")
        # 退格：0x7F，规范模式下删掉一个汉字（IUTF8）
        t.run("cat")
        time.sleep(0.3)
        t.type("汉字x")
        t.key("backspace")
        t.key("backspace")
        t.type("y\n")
        check(t.wait_line("汉y") is not None, "规范模式下退格删除一个完整的汉字（IUTF8）")
        t.key("ctrl+d")
        check(t.wait_prompt(after="汉y") is not None, "Ctrl+D 结束 cat 的输入")
    finally:
        t.close()


def test_exit(d, args):
    t = Term(d, args)
    try:
        t.wait_prompt(timeout=15)
        t.run("exit")
        rc = t.app.wait(5)
        check(rc == 0, "输入 exit 后 skterm 退出，退出码 0（%r）" % rc)
        check(t.win.id not in d.windows, "窗口关闭了")
        check(not os.path.exists("/proc/%d" % t.child), "shell 子进程被回收（没有僵尸）")
    finally:
        t.close()


def test_close(d, args):
    t = start(d, args)
    try:
        t.run("sleep 1234")
        t.wait(lambda s: s["lines"][s["cy"] - 1] == PROMPT + " sleep 1234", what="sleep 运行")
        time.sleep(0.3)
        check(sleepers(b"sleep\x001234\x00"), "sleep 1234 正在运行")
        d.close(t.win)
        rc = t.app.wait(5)
        check(rc == 0, "点关闭按钮后 skterm 退出，退出码 0（%r）" % rc)
        deadline = time.time() + 3
        while sleepers(b"sleep\x001234\x00") and time.time() < deadline:
            time.sleep(0.1)
        check(not sleepers(b"sleep\x001234\x00"), "终端里正在运行的 sleep 收到 SIGHUP 结束了")
    finally:
        t.close()


def test_dir_arg(d, args):
    tmp = os.path.realpath(tempfile.mkdtemp(prefix="skterm-dir 空格."))
    t = start(d, args, [tmp])
    try:
        t.run("pwd")
        s = t.wait_line(tmp)
        check(s is not None, "参数是目录：shell 在这个目录里启动（pwd = %s）" % tmp)
    finally:
        t.close()
        shutil.rmtree(tmp, ignore_errors=True)


def test_command(d, args):
    t = Term(d, args, ["echo 命令输出; exit 3"])
    try:
        s = t.wait(lambda s: s["finished"] == 1, what="命令结束")
        check(s is not None, "命令结束后窗口保留")
        if not s:
            return
        check("命令输出" in s["lines"], "显示命令的输出")
        msg = "[程序已结束，退出码 3，按任意键关闭窗口]"
        check(msg in s["lines"], "显示 %s" % msg)
        check(t.app.alive(), "skterm 还在运行")
        time.sleep(0.2)
        check(not os.path.exists("/proc/%d" % t.child), "子进程已被回收（/proc/%d 不存在）" % t.child)
        check(t.win.title == "终端（已结束）", "标题变成“终端（已结束）”（%s）" % t.win.title)
        t.shot("skterm-command-done")
        t.key("leftshift")             # 单独的修饰键不算“按键”
        time.sleep(0.3)
        check(t.app.alive(), "只按 Shift 不关窗口")
        t.key("space")
        rc = t.app.wait(5)
        check(rc == 0, "按任意键关闭窗口，退出码 0（%r）" % rc)
    finally:
        t.close()
    # 被信号杀死
    t = Term(d, args, ["kill -9 $$"])
    try:
        s = t.wait(lambda s: s["finished"] == 1, what="命令结束")
        check(s is not None and "[程序被信号 9（KILL）终止，按任意键关闭窗口]" in s["lines"],
              "被信号终止时显示信号编号和名字")
        t.key("enter")
        check(t.app.wait(5) == 0, "回车也能关闭")
    finally:
        t.close()
    # 找不到程序
    t = Term(d, args, ["/no/such/program"])
    try:
        s = t.wait(lambda s: s["finished"] == 1, what="命令结束")
        check(s is not None and any("退出码 127" in ln for ln in s["lines"]), "找不到程序：退出码 127")
    finally:
        t.close()


def test_clipboard(d, args):
    t = start(d, args)
    try:
        t.run("echo copyme-text; printf 'line1\\nline2\\n'")
        s = t.wait(lambda s: "line2" in s["lines"] and t.at_prompt(s), what="输出")
        if not s:
            check(False, "输出没出现")
            return
        r = t.row_of(s, "copyme-text")
        x0, y = PAD_L + 1, PAD_T + r * LH + LH // 2
        x1 = PAD_L + 11 * CW - 1
        d.drag(t.win, x0, y, x1, y, steps=6)
        time.sleep(0.3)
        check(has_color(t.win, (PAD_L + 2, PAD_T + r * LH, 11 * CW - 4, LH), (0xFA, 0xEF, 0xA5), 24),
              "拖动选择的文字用荧光笔（黄色）高亮")
        t.shot("skterm-selection")
        t.key("ctrl+shift+c")
        time.sleep(0.3)
        check(d.clipboard == b"copyme-text", "Ctrl+Shift+C 复制到剪贴板（%r）" % d.clipboard)
        # 跨两行的选区：两行之间是换行
        r1 = t.row_of(s, "line1")
        d.drag(t.win, PAD_L + 1, PAD_T + r1 * LH + 9, PAD_L + 5 * CW, PAD_T + (r1 + 1) * LH + 9, steps=6)
        time.sleep(0.2)
        d.click(t.win, PAD_L + 3 * CW, PAD_T + (r1 + 1) * LH + 9, button=md.BTN_RIGHT)
        time.sleep(0.3)
        check(d.clipboard == b"line1\nline2", "右键复制多行选区（%r）" % d.clipboard)
        # 粘贴
        d.clipboard = "echo pasted-ok 粘贴".encode()
        t.key("ctrl+shift+v")
        time.sleep(0.3)
        t.key("enter")
        check(t.wait_line("pasted-ok 粘贴") is not None, "Ctrl+Shift+V 粘贴并执行")
        d.clipboard = b"echo right-click-paste"
        d.click(t.win, PAD_L + 100, PAD_T + 5, button=md.BTN_RIGHT)
        time.sleep(0.3)
        t.key("enter")
        check(t.wait_line("right-click-paste") is not None, "没有选区时右键粘贴")
        # 双击选词
        s = t.dump()
        r = t.row_of(s, "copyme-text")
        if r >= 0:
            x, y = cell_xy(3, r)
            d.double_click(t.win, x, y)
            t.key("ctrl+shift+c")
            time.sleep(0.3)
            check(d.clipboard == b"copyme-text", "双击选中一个词（%r）" % d.clipboard)
        # 选中的文字被程序改掉了：选区作废
        t.run("clear")
        t.wait(lambda s: s["cy"] == 0 and t.at_prompt(s), what="清屏")
        time.sleep(0.3)
        check(not has_color(t.win, (PAD_L, PAD_T, 20 * CW, 6 * LH), (0xFA, 0xEF, 0xA5), 16),
              "清屏后选区的高亮消失了")
    finally:
        t.close()


def test_scrollback(d, args):
    t = start(d, args)
    try:
        t.run("seq 1 200")
        s0 = t.wait_prompt(after="200")
        check(s0 is not None, "seq 1 200 输出完")
        if not s0:
            return
        check(s0["sb"] > 150, "超出屏幕的行进入滚动历史（%d 行）" % s0["sb"])
        x, y = cell_xy(40, 10)
        d.wheel(t.win, x, y, 2)
        s = t.wait(lambda s: s["view"] == 6, what="滚轮")
        check(s is not None, "滚轮向上滚 2 格 = 往回翻 6 行")
        if s:
            check(s["lines"][23] == s0["lines"][17], "画面显示更早的内容")
        t.key("shift+pageup")
        s = t.wait(lambda s: s["view"] == 6 + 23, what="Shift+PageUp")
        check(s is not None, "Shift+PageUp 再往回翻一页")
        t.shot("skterm-scrollback")
        before = s["lines"] if s else None
        # 拖动滚动条到最上面
        d.drag(t.win, t.win.w - 10, 40, t.win.w - 10, 0, steps=4)
        s = t.wait(lambda s: s["view"] == s["sb"], what="滚动条拖到顶")
        check(s is not None, "拖动滚动条翻到最早的历史")
        if s and before:
            check(s["lines"] != before, "画面变了")
        t.key("shift+pagedown")
        s = t.dump()
        check(s and 0 < s["view"] < s["sb"], "Shift+PageDown 往后翻")
        t.type("e")
        s = t.wait(lambda s: s["view"] == 0, what="回到底部")
        check(s is not None, "打字时回到最新的屏幕")
        t.key("backspace")
    finally:
        t.close()


def test_scroll_pixels(d, args):
    """翻看历史时 skterm 直接搬像素、只画新露出来的行；结果必须和整屏重画一模一样"""
    t = start(d, args)
    try:
        # 输出一行一行地滚屏（走“搬像素”的路）
        t.run("seq 1 200; for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28; "
              "do echo 第$i行; sleep 0.03; done")
        t.wait_prompt(after="第28行")
        settle(d, t.win, quiet=0.3)
        p0 = t.win.pixels()[2]
        x, y = cell_xy(40, 10)
        # 路线 A：滚轮一格一格往回翻（每次 3 行，走“搬像素”的路）
        for _ in range(10):
            d.wheel(t.win, x, y, 1)
            time.sleep(0.12)
        s = t.wait(lambda s: s["view"] == 30, what="翻到 30 行前")
        settle(d, t.win, quiet=0.3)
        a = t.win.pixels()[2]
        # 路线 B：回到底部，再一次翻 30 行（超过一屏，整屏重画）
        d.wheel(t.win, x, y, -100)
        t.wait(lambda s: s["view"] == 0, what="回到底部")
        d.wheel(t.win, x, y, 10)
        s2 = t.wait(lambda s: s["view"] == 30, what="一次翻到 30 行前")
        settle(d, t.win, quiet=0.3)
        b = t.win.pixels()[2]
        check(s and s2 and s["lines"] == s2["lines"], "两种方式看到的是同样的内容")
        diff = sum(1 for i in range(0, len(a), 4) if a[i:i + 3] != b[i:i + 3])
        check(diff == 0, "往回翻：搬像素和整屏重画的结果逐像素相同（%d 个像素不同）" % diff)
        # 回到底部（整屏重画），和最开始逐行滚屏得到的画面比较
        d.wheel(t.win, x, y, -100)
        t.wait(lambda s: s["view"] == 0, what="回到底部")
        settle(d, t.win, quiet=0.3)
        p1 = t.win.pixels()[2]
        diff = sum(1 for i in range(0, len(p0), 4) if p0[i:i + 3] != p1[i:i + 3])
        check(diff == 0, "输出滚屏：搬像素和整屏重画的结果逐像素相同（%d 个像素不同）" % diff)
    finally:
        t.close()


def test_scroll_while_viewing(d, args):
    """往回翻看历史时，后台不断有输出：画面停在正在看的内容上"""
    t = start(d, args)
    try:
        t.run("seq 1 100; sleep 1; seq 101 110")
        t.wait_line("100")
        x, y = cell_xy(40, 10)
        d.wheel(t.win, x, y, 10)
        s1 = t.wait(lambda s: s["view"] >= 30, what="往回翻")
        s2 = t.wait(lambda s: s["view"] > (s1["view"] if s1 else 0), timeout=5, what="新输出")
        check(s1 and s2 and s2["lines"] == s1["lines"], "新输出到来时画面不动（view %s → %s）" %
              (s1 and s1["view"], s2 and s2["view"]))
    finally:
        t.close()


def test_big_output(d, args):
    t = start(d, args)
    try:
        t0 = time.time()
        start_req = len(d.requests)
        t.run("seq 1 5000")
        s = t.wait_prompt(timeout=30, after="5000")
        dt = time.time() - t0
        check(s is not None, "seq 1 5000 输出完（%.1f 秒）" % dt)
        if s:
            check(s["sb"] == 1000, "滚动历史最多 1000 行（%d）" % s["sb"])
        n = len([r for r in d.requests[start_req:] if r[2] == "DAMAGE"])
        print("  （5000 行输出共发出 %d 个 DAMAGE 请求）" % n)
        check(n < 2000, "大量输出时合并重画（%d 个 DAMAGE）" % n)
        x, y = cell_xy(10, 10)
        d.wheel(t.win, x, y, 400)
        s = t.wait(lambda s: s["view"] == 1000, what="翻到最早")
        check(s is not None and s["lines"][0] == str(5000 - 1000 - 22),
              "最早的历史行是 %d（%r）" % (5000 - 1000 - 22, s and s["lines"][0]))
    finally:
        t.close()


def test_damage(d, args):
    t = start(d, args)
    try:
        settle(d, t.win, quiet=0.4)
        n0 = len(d.requests)
        t.type("x")
        time.sleep(0.6)
        rects = damages_since(d, n0, t.win)
        area = sum(w * h for _x, _y, w, h in rects)
        print("  （打一个字的 damage：%s）" % rects)
        check(rects, "打字后有重画")
        check(area <= 6 * CW * LH and all(h <= LH for _x, _y, _w, h in rects),
              "打一个字只重画几个格子（%d 像素，整窗 %d）" % (area, t.win.w * t.win.h))
        # 一行输出导致整屏上滚：搬像素，只有新出现的行需要画字
        t.key("backspace")
        t.run("seq 1 30")
        t.wait_prompt(after="30")
        settle(d, t.win, quiet=0.3)
        # 失去焦点：只重画光标那一格，光标变成空心框
        n0 = len(d.requests)
        d.focus(t.win, False)
        time.sleep(0.5)
        rects = damages_since(d, n0, t.win)
        area = sum(w * h for _x, _y, w, h in rects)
        check(rects and area <= 4 * CW * LH, "失去焦点只重画光标附近（%s）" % rects)
        s = t.dump()
        check(s and s["focused"] == 0, "记录了失去焦点")
        x, y = cell_xy(s["cx"], s["cy"]) if s else (0, 0)
        check(not has_color(t.win, (x - 1, y - 3, 3, 6), INK, 30), "失去焦点时光标是空心的")
        t.shot("skterm-unfocused")
        d.focus(t.win, True)
        time.sleep(0.3)
        check(has_color(t.win, (x - 3, y - 6, 6, 12), INK, 30), "重新获得焦点：实心光标")
        # 闲着的时候不重画
        n0 = len(d.requests)
        time.sleep(1.0)
        check(not damages_since(d, n0, t.win), "没有输入时不重画（没有闪烁定时器）")
    finally:
        t.close()


def test_bell(d, args):
    t = start(d, args)
    try:
        n0 = len(d.requests)
        t.run(r"printf '\a'")
        t.wait_prompt(after="printf")
        time.sleep(0.4)
        rects = damages_since(d, n0, t.win)
        margin = [r for r in rects if r[0] == 0 or r[1] == 0]
        check(margin, "响铃时边距闪一下（%d 块边距 damage）" % len(margin))
        check(color_close(pixel(t.win, 2, t.win.h // 2), PAPER, 8), "150 毫秒后闪光擦掉了")
        check(t.app.alive(), "响铃后 skterm 正常")
    finally:
        t.close()


if __name__ == "__main__":
    # 先确认 sksh、skbox 已编译（run.sh 只编译 skterm 本身）
    for i, a in enumerate(sys.argv):
        if a == "--bin" and i + 1 < len(sys.argv) and not ensure_tools(os.path.abspath(sys.argv[i + 1])):
            print("编译 sksh/skbox 失败")
            sys.exit(1)
    run_tests([
        ("prompt", test_prompt),
        ("echo", test_echo),
        ("colors", test_colors),
        ("vt", test_vt),
        ("utf8_split", test_utf8_split),
        ("clear", test_clear),
        ("top", test_top),
        ("more", test_more),
        ("resize", test_resize),
        ("ctrl_c", test_ctrl_c),
        ("keys", test_keys),
        ("job_control", test_job_control),
        ("replies", test_replies),
        ("exit", test_exit),
        ("close", test_close),
        ("dir_arg", test_dir_arg),
        ("command", test_command),
        ("clipboard", test_clipboard),
        ("scrollback", test_scrollback),
        ("scroll_pixels", test_scroll_pixels),
        ("scroll_while_viewing", test_scroll_while_viewing),
        ("big_output", test_big_output),
        ("damage", test_damage),
        ("bell", test_bell),
    ])
