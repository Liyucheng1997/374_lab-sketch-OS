#!/usr/bin/env python3
"""
SketchOS 自动化测试工具：在无界面的 QEMU 里启动系统，通过串口和 QMP 控制它。

可以作为库使用：
    from qemu_harness import VM
    with VM(initrd="out/initramfs.cpio.gz") as vm:
        vm.wait_serial(r"sksh.*#", timeout=60)      # 等待串口出现 shell 提示符
        print(vm.run_serial("uname -a"))            # 在串口 shell 里执行命令并取回输出
        vm.wait_serial("sketchd: ready")
        vm.click(500, 300)                          # 屏幕坐标点击（通过 vmmouse 绝对坐标）
        vm.key("ctrl-alt-t")                        # 组合键（QEMU qcode 名称，用 - 连接）
        vm.type_text("ls\\n")
        vm.screenshot("/tmp/shot.png")

也可以用脚本文件驱动：
    python3 tools/qemu_harness.py --initrd out/initramfs.cpio.gz --script test.txt --log serial.log
脚本每行一个命令（# 开头是注释）：
    wait <正则> [超时秒]        等待串口输出匹配
    sleep <秒>
    serial <文本>               向串口写入（\\n 表示回车）
    run <命令>                  在串口 shell 执行并打印输出
    shot <文件.png>             截屏
    move <x> <y>                鼠标移动到屏幕坐标
    click <x> <y> [left|right|middle]
    dclick <x> <y>              双击
    drag <x1> <y1> <x2> <y2>
    wheel <x> <y> <格数>        正数向上
    key <组合键>                例如 ret、ctrl-alt-t、alt-f4、shift-a
    type <文本>                 逐字符输入（支持 ASCII，\\n=回车）
    expect_shot_differs <a.png> <b.png>   两张截图必须不同（检查画面是否有变化）

需要 WSL 里装好 qemu-system-i386（scripts/toolchain.sh 会装）。
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_KERNEL = os.path.join(ROOT, "out", "bzImage")
DEFAULT_INITRD = os.path.join(ROOT, "out", "initramfs.cpio.gz")
DEFAULT_CMDLINE = "console=tty1 console=ttyS0 tsc=reliable mitigations=off random.trust_cpu=on loglevel=4"

# 字符 → QEMU qcode（美式键盘）
_SHIFTED = {
    '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
    '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right',
    '|': 'backslash', ':': 'semicolon', '"': 'apostrophe', '<': 'comma', '>': 'dot',
    '?': 'slash', '~': 'grave_accent',
}
_PLAIN = {
    ' ': 'spc', '\n': 'ret', '\t': 'tab', '-': 'minus', '=': 'equal', '[': 'bracket_left',
    ']': 'bracket_right', '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe', ',': 'comma',
    '.': 'dot', '/': 'slash', '`': 'grave_accent',
}


def char_to_keys(ch):
    """返回 (qcode, 需要shift)"""
    if 'a' <= ch <= 'z' or '0' <= ch <= '9':
        return ch, False
    if 'A' <= ch <= 'Z':
        return ch.lower(), True
    if ch in _PLAIN:
        return _PLAIN[ch], False
    if ch in _SHIFTED:
        return _SHIFTED[ch], True
    raise ValueError("无法用键盘输入字符 %r" % ch)


class VM:
    def __init__(self, kernel=DEFAULT_KERNEL, initrd=DEFAULT_INITRD, cmdline=DEFAULT_CMDLINE,
                 extra_cmdline="", mem=256, workdir=None, log_path=None, extra_args=None,
                 screen_w=1024, screen_h=768, audio_wav=None):
        """audio_wav：把声卡放出的声音录进这个 WAV 文件（默认丢掉）"""
        self.audio_wav = audio_wav
        self.kernel = kernel
        self.initrd = initrd
        self.cmdline = (cmdline + " " + extra_cmdline).strip()
        self.mem = mem
        self.own_workdir = workdir is None
        self.workdir = workdir or tempfile.mkdtemp(prefix="skvm-")
        os.makedirs(self.workdir, exist_ok=True)
        self.log_path = log_path or os.path.join(self.workdir, "serial.log")
        self.extra_args = extra_args or []
        self.screen_w, self.screen_h = screen_w, screen_h
        self.proc = None
        self.serial = None
        self.qmp = None
        self._buf = ""
        self._buf_lock = threading.Lock()
        self._reader = None
        self._closed = False

    # ---------- 生命周期 ----------
    def start(self):
        ser_path = os.path.join(self.workdir, "serial.sock")
        qmp_path = os.path.join(self.workdir, "qmp.sock")
        for p in (ser_path, qmp_path):
            if os.path.exists(p):
                os.unlink(p)
        args = [
            "qemu-system-i386", "-m", str(self.mem),
            "-kernel", self.kernel, "-initrd", self.initrd, "-append", self.cmdline,
            "-vga", "std", "-display", "none", "-no-reboot", "-S",   # -S: 先暂停，连上串口后再开机，避免丢失早期输出
            "-chardev", "socket,id=ser0,path=%s,server=on,wait=off" % ser_path,
            "-serial", "chardev:ser0",
            "-qmp", "unix:%s,server=on,wait=off" % qmp_path,
            "-rtc", "base=utc",
            # 和网页版（v86）一样插一块 SB16 声卡；声音默认丢掉（none），audio_wav 时录进 WAV 文件
            "-audiodev", ("wav,id=snd0,path=%s" % self.audio_wav) if self.audio_wav else "none,id=snd0",
            "-device", "sb16,audiodev=snd0",
        ] + self.extra_args
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        deadline = time.time() + 15
        while not (os.path.exists(ser_path) and os.path.exists(qmp_path)):
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU 启动失败:\n" + self.proc.stdout.read().decode(errors="replace"))
            if time.time() > deadline:
                raise RuntimeError("等待 QEMU 套接字超时")
            time.sleep(0.05)
        self.serial = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.serial.connect(ser_path)
        self.qmp = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.qmp.connect(qmp_path)
        self._qmp_file = self.qmp.makefile("rwb")
        self._qmp_read()                      # 欢迎信息
        self._qmp_cmd("qmp_capabilities")
        self._log = open(self.log_path, "w", encoding="utf-8", errors="replace")
        self._reader = threading.Thread(target=self._read_serial, daemon=True)
        self._reader.start()
        self._qmp_cmd("cont")
        return self

    def stop(self):
        if self._closed:
            return
        self._closed = True
        try:
            if self.qmp:
                self._qmp_cmd("quit")
        except Exception:
            pass
        if self.proc:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        for s in (self.serial, self.qmp):
            try:
                s and s.close()
            except Exception:
                pass
        try:
            self._log.close()
        except Exception:
            pass

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
        if self.own_workdir and not os.environ.get("SKVM_KEEP"):
            shutil.rmtree(self.workdir, ignore_errors=True)

    # ---------- 串口 ----------
    def _read_serial(self):
        while not self._closed:
            try:
                data = self.serial.recv(4096)
            except OSError:
                break
            if not data:
                break
            text = data.decode("utf-8", errors="replace")
            with self._buf_lock:
                self._buf += text
            try:
                self._log.write(text)
                self._log.flush()
            except Exception:
                pass

    def serial_output(self):
        with self._buf_lock:
            return self._buf

    def wait_serial(self, pattern, timeout=60, start=0):
        """等待串口输出（从偏移 start 开始）匹配正则，返回 match 对象；超时抛异常"""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while True:
            with self._buf_lock:
                m = rx.search(self._buf, start)
            if m:
                return m
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU 已退出，串口输出结尾:\n" + self.serial_output()[-3000:])
            if time.time() > deadline:
                raise TimeoutError("等待 %r 超时，串口输出结尾:\n%s" % (pattern, self.serial_output()[-3000:]))
            time.sleep(0.05)

    def serial_write(self, text):
        self.serial.sendall(text.encode("utf-8"))

    def run_serial(self, command, timeout=30):
        """在串口上的 shell 里执行命令，返回它的输出（依靠唯一的结束标记）"""
        marker = "__SKDONE_%d__" % int(time.time() * 1000)
        start = len(self.serial_output())
        self.serial_write(command + "; echo " + marker[:6] + "''" + marker[6:] + "\n")
        m = self.wait_serial(re.escape(marker), timeout=timeout, start=start)
        out = self.serial_output()[start:m.start()]
        lines = out.replace("\r", "").split("\n")
        if lines and command in lines[0]:
            lines = lines[1:]               # 去掉回显的命令行
        return "\n".join(lines).strip("\n")

    # ---------- QMP ----------
    def _qmp_read(self):
        while True:
            line = self._qmp_file.readline()
            if not line:
                raise RuntimeError("QMP 连接断开")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def _qmp_cmd(self, cmd, **arguments):
        req = {"execute": cmd}
        if arguments:
            req["arguments"] = arguments
        self._qmp_file.write((json.dumps(req) + "\n").encode())
        self._qmp_file.flush()
        resp = self._qmp_read()
        if "error" in resp:
            raise RuntimeError("QMP %s 失败: %s" % (cmd, resp["error"]))
        return resp.get("return")

    def screenshot(self, path):
        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self._qmp_cmd("screendump", filename=path, format="png")
        # screendump 是异步写文件的，等它写完
        for _ in range(100):
            if os.path.exists(path) and os.path.getsize(path) > 0:
                break
            time.sleep(0.05)
        time.sleep(0.1)
        return path

    def _abs(self, x, y):
        # QEMU 输入层的绝对坐标范围是 0..0x7FFF
        ax = int(max(0, min(self.screen_w - 1, x)) * 0x7FFF / max(1, self.screen_w - 1))
        ay = int(max(0, min(self.screen_h - 1, y)) * 0x7FFF / max(1, self.screen_h - 1))
        return [{"type": "abs", "data": {"axis": "x", "value": ax}},
                {"type": "abs", "data": {"axis": "y", "value": ay}}]

    def mouse_move(self, x, y):
        self._qmp_cmd("input-send-event", events=self._abs(x, y))
        time.sleep(0.03)

    def mouse_button(self, button="left", down=True):
        self._qmp_cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": button}}])
        time.sleep(0.03)

    def click(self, x, y, button="left"):
        self.mouse_move(x, y)
        time.sleep(0.05)
        self.mouse_button(button, True)
        time.sleep(0.05)
        self.mouse_button(button, False)
        time.sleep(0.05)

    def double_click(self, x, y):
        self.mouse_move(x, y)
        for _ in range(2):
            self.mouse_button("left", True)
            time.sleep(0.03)
            self.mouse_button("left", False)
            time.sleep(0.06)

    def drag(self, x1, y1, x2, y2, steps=12):
        self.mouse_move(x1, y1)
        self.mouse_button("left", True)
        for i in range(1, steps + 1):
            self.mouse_move(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps)
            time.sleep(0.02)
        self.mouse_button("left", False)

    def wheel(self, x, y, clicks):
        self.mouse_move(x, y)
        btn = "wheel-up" if clicks > 0 else "wheel-down"
        for _ in range(abs(int(clicks))):
            self.mouse_button(btn, True)
            self.mouse_button(btn, False)

    def key(self, combo, hold=0.05):
        """combo 例如 'ctrl-alt-t'、'ret'、'shift-a'（QEMU qcode 名称）"""
        keys = [{"type": "qcode", "data": k} for k in combo.split("-") if k]
        if combo.endswith("--"):          # 'ctrl--' 表示 ctrl + minus
            keys = keys[:-1] + [{"type": "qcode", "data": "minus"}]
        self._qmp_cmd("send-key", keys=keys, **{"hold-time": int(hold * 1000)})
        time.sleep(0.04)

    def type_text(self, text, delay=0.02):
        for ch in text:
            code, shift = char_to_keys(ch)
            self._qmp_cmd("send-key", keys=([{"type": "qcode", "data": "shift"}] if shift else []) +
                          [{"type": "qcode", "data": code}], **{"hold-time": 30})
            time.sleep(delay)


def _unescape(s):
    return s.encode("utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")


def run_script(vm, lines):
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cmd, _, rest = line.partition(" ")
        a = rest.split()
        print(">>", line, flush=True)
        if cmd == "wait":
            timeout = 60
            pat = rest
            m = re.match(r"^(.*)\s+(\d+(?:\.\d+)?)$", rest)
            if m:
                pat, timeout = m.group(1), float(m.group(2))
            vm.wait_serial(pat, timeout)
        elif cmd == "sleep":
            time.sleep(float(a[0]))
        elif cmd == "serial":
            vm.serial_write(_unescape(rest))
        elif cmd == "run":
            print(vm.run_serial(rest))
        elif cmd == "shot":
            print("   截图:", vm.screenshot(rest))
        elif cmd == "move":
            vm.mouse_move(float(a[0]), float(a[1]))
        elif cmd == "click":
            vm.click(float(a[0]), float(a[1]), a[2] if len(a) > 2 else "left")
        elif cmd == "dclick":
            vm.double_click(float(a[0]), float(a[1]))
        elif cmd == "drag":
            vm.drag(*[float(v) for v in a[:4]])
        elif cmd == "wheel":
            vm.wheel(float(a[0]), float(a[1]), int(a[2]))
        elif cmd == "key":
            vm.key(a[0])
        elif cmd == "type":
            vm.type_text(_unescape(rest))
        elif cmd == "expect_shot_differs":
            if open(a[0], "rb").read() == open(a[1], "rb").read():
                raise AssertionError("截图 %s 与 %s 完全相同" % (a[0], a[1]))
        else:
            raise ValueError("未知脚本命令: " + cmd)


def main():
    ap = argparse.ArgumentParser(description="在 QEMU 里自动测试 SketchOS")
    ap.add_argument("--kernel", default=DEFAULT_KERNEL)
    ap.add_argument("--initrd", default=DEFAULT_INITRD)
    ap.add_argument("--append", default="", help="追加的内核命令行参数")
    ap.add_argument("--mem", type=int, default=256)
    ap.add_argument("--script", help="测试脚本文件")
    ap.add_argument("--log", help="串口日志保存路径")
    ap.add_argument("--timeout", type=float, default=0, help="脚本跑完后再保持运行的秒数")
    args = ap.parse_args()
    vm = VM(kernel=args.kernel, initrd=args.initrd, extra_cmdline=args.append, mem=args.mem,
            log_path=args.log)
    with vm:
        try:
            if args.script:
                with open(args.script, encoding="utf-8") as f:
                    run_script(vm, f.readlines())
            if args.timeout:
                time.sleep(args.timeout)
        except Exception as e:
            print("测试失败:", e, file=sys.stderr)
            print("---- 串口输出结尾 ----\n" + vm.serial_output()[-4000:], file=sys.stderr)
            sys.exit(1)
    print("测试脚本执行完毕")


if __name__ == "__main__":
    main()
