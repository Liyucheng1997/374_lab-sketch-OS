#!/usr/bin/env python3
"""
mock_display.py — 模拟显示服务器（用于在 WSL 里直接测试图形应用，不用启动 QEMU）

它实现了 userspace/gui/include/sketch/proto.h 定义的协议的服务器端：
接受应用连接、接收共享内存缓冲区 (memfd, SCM_RIGHTS)、记录 damage，
并且可以向应用发送鼠标/键盘/改变大小/关闭等事件，还能把窗口内容保存成 PNG。
因为 WSL 能直接运行 SketchOS 的 32 位静态程序，所以图形应用可以这样测试：

    from mock_display import MockDisplay
    with MockDisplay("/tmp/skmock.sock", 1024, 768) as d:
        p = d.spawn(["/path/to/build/bin/sknote", "/tmp/a.txt"])   # 自动设置 SKETCH_SOCKET
        w = d.wait_window(timeout=10)            # 等第一个窗口出现
        d.wait_idle(w)                           # 等应用画完（一段时间内没有新的 damage）
        d.save_window(w, "/tmp/note.png")        # 保存窗口内容
        d.click(w, 100, 50)                      # 窗口内容区坐标
        d.type_text(w, "你好 hello\\n")           # ASCII 用键码，非 ASCII 只发 codepoint
        d.key(w, "ctrl+s")
        d.resize(w, 800, 600)                    # 模拟用户拉大窗口
        d.close(w)                               # 模拟点击关闭按钮
        print(d.requests)                        # 收到的所有请求（调试用）

命令行：
    python3 tools/mock_display.py --size 1024x768 --out /tmp/shots -- /path/to/app args
    会启动应用，等它画完后把每个窗口保存成 PNG，然后退出。

服务器会像真正的 sketchd 一样严格检查报文（大小、缓冲区是否加了 F_SEAL_SHRINK 封印等），
发现客户端协议错误会打印出来并记入 d.errors。
"""
import argparse
import fcntl
import mmap
import os
import select
import socket
import struct
import subprocess
import sys
import threading
import time

try:
    from PIL import Image
except ImportError:  # 保存 PNG 需要 Pillow
    Image = None

# ---------------- 协议常量（与 proto.h 保持一致） ----------------
REQ_HELLO, REQ_CREATE_WINDOW, REQ_DESTROY_WINDOW, REQ_ATTACH_BUFFER, REQ_DAMAGE, REQ_SET_TITLE, \
    REQ_SHOW, REQ_MOVE, REQ_SET_CURSOR, REQ_SET_CLIPBOARD, REQ_GET_CLIPBOARD, REQ_LAUNCH = range(1, 13)
EV_WELCOME, EV_WINDOW_CREATED, EV_MOUSE, EV_KEY, EV_FOCUS, EV_CONFIGURE, EV_CLOSE, EV_CLIPBOARD, \
    EV_ERROR = range(100, 109)
REQ_NAMES = {1: "HELLO", 2: "CREATE_WINDOW", 3: "DESTROY_WINDOW", 4: "ATTACH_BUFFER", 5: "DAMAGE",
             6: "SET_TITLE", 7: "SHOW", 8: "MOVE", 9: "SET_CURSOR", 10: "SET_CLIPBOARD",
             11: "GET_CLIPBOARD", 12: "LAUNCH"}
PROTO_VERSION = 1
TITLE_MAX, APPNAME_MAX, CLIP_MAX, MAX_WIN = 64, 32, 4096, 4096
MOUSE_MOVE, MOUSE_DOWN, MOUSE_UP, MOUSE_WHEEL, MOUSE_ENTER, MOUSE_LEAVE = range(1, 7)
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 1, 2, 4
MOD_SHIFT, MOD_CTRL, MOD_ALT, MOD_SUPER, MOD_CAPS = 1, 2, 4, 8, 16
F_ADD_SEALS, F_GET_SEALS = 1033, 1034
F_SEAL_SEAL, F_SEAL_SHRINK, F_SEAL_GROW, F_SEAL_WRITE = 1, 2, 4, 8

# 请求的固定长度（字节）；剪贴板是变长的
REQ_SIZES = {REQ_HELLO: 44, REQ_CREATE_WINDOW: 96, REQ_DESTROY_WINDOW: 8, REQ_ATTACH_BUFFER: 16,
             REQ_DAMAGE: 24, REQ_SET_TITLE: 72, REQ_SHOW: 8, REQ_MOVE: 16, REQ_SET_CURSOR: 12,
             REQ_GET_CLIPBOARD: 4, REQ_LAUNCH: 292}

# Linux evdev 键码（input-event-codes.h）
KEY = {
    "esc": 1, "1": 2, "2": 3, "3": 4, "4": 5, "5": 6, "6": 7, "7": 8, "8": 9, "9": 10, "0": 11,
    "minus": 12, "equal": 13, "backspace": 14, "tab": 15, "q": 16, "w": 17, "e": 18, "r": 19,
    "t": 20, "y": 21, "u": 22, "i": 23, "o": 24, "p": 25, "leftbrace": 26, "rightbrace": 27,
    "enter": 28, "leftctrl": 29, "a": 30, "s": 31, "d": 32, "f": 33, "g": 34, "h": 35, "j": 36,
    "k": 37, "l": 38, "semicolon": 39, "apostrophe": 40, "grave": 41, "leftshift": 42,
    "backslash": 43, "z": 44, "x": 45, "c": 46, "v": 47, "b": 48, "n": 49, "m": 50, "comma": 51,
    "dot": 52, "slash": 53, "rightshift": 54, "leftalt": 56, "space": 57, "capslock": 58,
    "f1": 59, "f2": 60, "f3": 61, "f4": 62, "f5": 63, "f6": 64, "f7": 65, "f8": 66, "f9": 67,
    "f10": 68, "f11": 87, "f12": 88, "home": 102, "up": 103, "pageup": 104, "left": 105,
    "right": 106, "end": 107, "down": 108, "pagedown": 109, "insert": 110, "delete": 111,
    "leftmeta": 125,
}
_SHIFT_CHARS = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
                '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'leftbrace',
                '}': 'rightbrace', '|': 'backslash', ':': 'semicolon', '"': 'apostrophe',
                '<': 'comma', '>': 'dot', '?': 'slash', '~': 'grave'}
_PLAIN_CHARS = {' ': 'space', '-': 'minus', '=': 'equal', '[': 'leftbrace', ']': 'rightbrace',
                '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe', ',': 'comma',
                '.': 'dot', '/': 'slash', '`': 'grave', '\n': 'enter', '\t': 'tab'}
_SPECIAL_CP = {"enter": 0x0D, "backspace": 0x08, "tab": 0x09, "esc": 0x1B, "space": 0x20}


def _cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", errors="replace")


class Window:
    def __init__(self, wid, client, w, h, flags, accent, title, fd, min_w, min_h):
        self.id = wid
        self.client = client
        self.w, self.h = w, h
        self.flags, self.accent, self.title = flags, accent, title
        self.min_w, self.min_h = min_w, min_h
        self.fd = fd
        self.map = None
        self.damage_count = 0
        self.last_damage = time.time()
        self.cursor = 0
        self.x, self.y = 40 + 30 * (wid % 8), 40 + 30 * (wid % 8)
        self.visible = True

    def pixels(self):
        """返回 (w, h, BGRA 字节)"""
        return self.w, self.h, bytes(self.map[: self.w * self.h * 4])


class Client:
    def __init__(self, sock):
        self.sock = sock
        self.app_name = "?"
        self.pid = 0
        self.hello = False
        self.windows = {}


class MockDisplay:
    def __init__(self, socket_path="/tmp/skmock.sock", width=1024, height=768, verbose=False):
        self.socket_path = socket_path
        self.width, self.height = width, height
        self.verbose = verbose
        self.clients = []
        self.windows = {}
        self.next_win = 1
        self.requests = []          # (时间, 应用名, 请求名, 详情)
        self.errors = []
        self.clipboard = b""
        self.launches = []          # SK_REQ_LAUNCH 请求 (app_id, arg)
        self.procs = []
        self._lock = threading.RLock()
        self._stop = False
        self._thread = None
        self.listener = None

    # ---------- 生命周期 ----------
    def start(self):
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.listener.bind(self.socket_path)
        self.listener.listen(16)
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        return self

    def stop(self):
        self._stop = True
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
        if self._thread:
            self._thread.join(timeout=2)
        try:
            self.listener.close()
        except Exception:
            pass
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    def spawn(self, argv, env=None, **kw):
        e = dict(os.environ if env is None else env)
        e["SKETCH_SOCKET"] = self.socket_path
        e.setdefault("SKETCH_FONT_DIR", os.path.join(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "assets", "fonts"))
        p = subprocess.Popen(argv, env=e, **kw)
        self.procs.append(p)
        return p

    # ---------- 事件循环 ----------
    def _loop(self):
        while not self._stop:
            with self._lock:
                socks = [self.listener] + [c.sock for c in self.clients]
            try:
                r, _, _ = select.select(socks, [], [], 0.05)
            except (OSError, ValueError):
                continue
            for s in r:
                if s is self.listener:
                    try:
                        cs, _ = self.listener.accept()
                    except OSError:
                        continue
                    with self._lock:
                        self.clients.append(Client(cs))
                else:
                    with self._lock:
                        c = next((c for c in self.clients if c.sock is s), None)
                    if c:
                        self._recv(c)

    def _err(self, c, msg):
        text = "[%s] 协议错误: %s" % (c.app_name, msg)
        self.errors.append(text)
        print("mock_display:", text, file=sys.stderr)

    def _drop(self, c):
        with self._lock:
            for w in list(c.windows.values()):
                self._destroy_window(w)
            try:
                c.sock.close()
            except Exception:
                pass
            if c in self.clients:
                self.clients.remove(c)

    def _recv(self, c):
        try:
            data, ancdata, flags, _ = c.sock.recvmsg(16384, socket.CMSG_LEN(16 * 4))
        except OSError:
            self._drop(c)
            return
        fds = []
        for level, ctype, cdata in ancdata:
            if level == socket.SOL_SOCKET and ctype == socket.SCM_RIGHTS:
                fds += list(struct.unpack("%di" % (len(cdata) // 4), cdata[: len(cdata) - len(cdata) % 4]))
        if not data:
            self._drop(c)
            return
        try:
            self._handle(c, data, fds)
        finally:
            pass

    def _send(self, c, type_, payload=b""):
        msg = struct.pack("<HH", type_, 4 + len(payload)) + payload
        try:
            c.sock.send(msg)
        except OSError:
            pass

    def _handle(self, c, data, fds):
        if len(data) < 4:
            self._err(c, "报文太短 (%d 字节)" % len(data))
            return
        type_, size = struct.unpack_from("<HH", data)
        name = REQ_NAMES.get(type_, "UNKNOWN(%d)" % type_)
        if size != len(data):
            self._err(c, "%s: 头部 size=%d 但报文实际 %d 字节" % (name, size, len(data)))
        expect = REQ_SIZES.get(type_)
        if type_ == REQ_SET_CLIPBOARD:
            if len(data) < 8:
                self._err(c, "SET_CLIPBOARD 太短")
                return
        elif expect is None:
            self._err(c, "未知请求类型 %d" % type_)
            return
        elif len(data) != expect:
            self._err(c, "%s: 长度应为 %d，实际 %d" % (name, expect, len(data)))
            return
        if not c.hello and type_ != REQ_HELLO:
            self._err(c, "%s 在 HELLO 之前发送" % name)
        needs_fd = type_ in (REQ_CREATE_WINDOW, REQ_ATTACH_BUFFER)
        if needs_fd and len(fds) != 1:
            self._err(c, "%s 应附带 1 个 fd，实际 %d 个" % (name, len(fds)))
        if not needs_fd:
            for fd in fds:
                self._err(c, "%s 不应附带 fd" % name)
                os.close(fd)
        detail = ""
        with self._lock:
            if type_ == REQ_HELLO:
                ver, pid = struct.unpack_from("<II", data, 4)
                c.app_name = _cstr(data[12:12 + APPNAME_MAX])
                c.pid = pid
                c.hello = True
                detail = "version=%d pid=%d app=%s" % (ver, pid, c.app_name)
                if ver != PROTO_VERSION:
                    self._err(c, "协议版本 %d != %d" % (ver, PROTO_VERSION))
                self._send(c, EV_WELCOME, struct.pack("<Iii", PROTO_VERSION, self.width, self.height))
            elif type_ == REQ_CREATE_WINDOW:
                serial, w, h, flags, accent, min_w, min_h = struct.unpack_from("<IiiIIii", data, 4)
                title = _cstr(data[32:32 + TITLE_MAX])
                detail = "serial=%d %dx%d flags=%#x accent=%#x title=%s" % (serial, w, h, flags, accent, title)
                if not fds:
                    return
                fd = fds[0]
                if not (1 <= w <= MAX_WIN and 1 <= h <= MAX_WIN):
                    self._err(c, "窗口尺寸非法 %dx%d" % (w, h))
                    os.close(fd)
                    return
                if not self._check_buffer(c, fd, w, h):
                    os.close(fd)
                    return
                win = Window(self.next_win, c, w, h, flags, accent, title, fd, min_w, min_h)
                self.next_win += 1
                win.map = mmap.mmap(fd, w * h * 4, mmap.MAP_SHARED, mmap.PROT_READ)
                c.windows[win.id] = win
                self.windows[win.id] = win
                self._send(c, EV_WINDOW_CREATED, struct.pack("<II", serial, win.id))
                self._send(c, EV_FOCUS, struct.pack("<II", win.id, 1))
                detail += " -> win %d" % win.id
            elif type_ == REQ_ATTACH_BUFFER:
                wid, w, h = struct.unpack_from("<Iii", data, 4)
                detail = "win=%d %dx%d" % (wid, w, h)
                win = c.windows.get(wid)
                if not fds:
                    return
                fd = fds[0]
                if not win:
                    self._err(c, "ATTACH_BUFFER: 窗口 %d 不属于该客户端" % wid)
                    os.close(fd)
                    return
                if not (1 <= w <= MAX_WIN and 1 <= h <= MAX_WIN) or not self._check_buffer(c, fd, w, h):
                    os.close(fd)
                    return
                win.map.close()
                os.close(win.fd)
                win.fd, win.w, win.h = fd, w, h
                win.map = mmap.mmap(fd, w * h * 4, mmap.MAP_SHARED, mmap.PROT_READ)
                win.damage_count += 1
                win.last_damage = time.time()
            elif type_ == REQ_DAMAGE:
                wid, x, y, w, h = struct.unpack_from("<Iiiii", data, 4)
                detail = "win=%d (%d,%d %dx%d)" % (wid, x, y, w, h)
                win = c.windows.get(wid)
                if not win:
                    self._err(c, "DAMAGE: 窗口 %d 不属于该客户端" % wid)
                else:
                    if w <= 0 or h <= 0:
                        self._err(c, "DAMAGE: 空矩形 %s" % detail)
                    win.damage_count += 1
                    win.last_damage = time.time()
            elif type_ in (REQ_DESTROY_WINDOW, REQ_SHOW):
                (wid,) = struct.unpack_from("<I", data, 4)
                detail = "win=%d" % wid
                win = c.windows.get(wid)
                if not win:
                    self._err(c, "%s: 窗口 %d 不属于该客户端" % (name, wid))
                elif type_ == REQ_DESTROY_WINDOW:
                    self._destroy_window(win)
            elif type_ == REQ_SET_TITLE:
                (wid,) = struct.unpack_from("<I", data, 4)
                title = _cstr(data[8:8 + TITLE_MAX])
                detail = "win=%d title=%s" % (wid, title)
                if wid in c.windows:
                    c.windows[wid].title = title
                else:
                    self._err(c, "SET_TITLE: 未知窗口 %d" % wid)
            elif type_ == REQ_MOVE:
                wid, x, y = struct.unpack_from("<Iii", data, 4)
                detail = "win=%d (%d,%d)" % (wid, x, y)
                if wid in c.windows:
                    c.windows[wid].x, c.windows[wid].y = x, y
            elif type_ == REQ_SET_CURSOR:
                wid, cur = struct.unpack_from("<II", data, 4)
                detail = "win=%d cursor=%d" % (wid, cur)
                if wid in c.windows:
                    c.windows[wid].cursor = cur
            elif type_ == REQ_SET_CLIPBOARD:
                (ln,) = struct.unpack_from("<I", data, 4)
                if ln > CLIP_MAX or 8 + ln != len(data):
                    self._err(c, "SET_CLIPBOARD: len=%d 与报文长度 %d 不符" % (ln, len(data)))
                    return
                self.clipboard = data[8:8 + ln]
                detail = repr(self.clipboard[:40])
            elif type_ == REQ_GET_CLIPBOARD:
                payload = struct.pack("<I", len(self.clipboard)) + self.clipboard
                self._send(c, EV_CLIPBOARD, payload)
            elif type_ == REQ_LAUNCH:
                app_id = _cstr(data[4:4 + APPNAME_MAX])
                arg = _cstr(data[36:36 + 256])
                self.launches.append((app_id, arg))
                detail = "app=%s arg=%s" % (app_id, arg)
            self.requests.append((time.time(), c.app_name, name, detail))
            if self.verbose:
                print("mock_display: %s %s %s" % (c.app_name, name, detail), file=sys.stderr)

    def _check_buffer(self, c, fd, w, h):
        st = os.fstat(fd)
        if st.st_size < w * h * 4:
            self._err(c, "缓冲区太小：%d 字节，需要 %d" % (st.st_size, w * h * 4))
            return False
        try:
            seals = fcntl.fcntl(fd, F_GET_SEALS)
            if not seals & F_SEAL_SHRINK:
                self._err(c, "缓冲区没有 F_SEAL_SHRINK 封印（真正的 sketchd 会拒绝）")
                return False
        except OSError:
            self._err(c, "缓冲区不是 memfd（无法读取封印）")
            return False
        return True

    def _destroy_window(self, win):
        try:
            win.map.close()
        except Exception:
            pass
        try:
            os.close(win.fd)
        except Exception:
            pass
        win.client.windows.pop(win.id, None)
        self.windows.pop(win.id, None)

    # ---------- 测试用 API ----------
    def wait_window(self, timeout=10, count=1, title=None):
        """等待至少 count 个窗口存在（可按标题过滤），返回最新的那个 Window"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                wins = [w for w in self.windows.values() if title is None or title in w.title]
                if len(wins) >= count:
                    return max(wins, key=lambda w: w.id)
            for p in self.procs:
                if p.poll() is not None and p.returncode != 0:
                    raise RuntimeError("应用已退出，退出码 %d" % p.returncode)
            time.sleep(0.02)
        raise TimeoutError("等待窗口超时（已有窗口: %s）" % [w.title for w in self.windows.values()])

    def wait_idle(self, win, quiet=0.3, timeout=10):
        """等到窗口有过至少一次 damage，并且 quiet 秒内没有新的 damage"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if win.damage_count > 0 and time.time() - win.last_damage >= quiet:
                return True
            time.sleep(0.02)
        raise TimeoutError("窗口 %d 没有静止下来（damage 次数 %d）" % (win.id, win.damage_count))

    def wait_damage(self, win, since_count, timeout=5):
        """等待窗口出现新的 damage（damage_count > since_count）"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if win.damage_count > since_count:
                return True
            time.sleep(0.01)
        raise TimeoutError("窗口 %d 在 %.1f 秒内没有重绘" % (win.id, timeout))

    def save_window(self, win, path):
        if Image is None:
            raise RuntimeError("需要 Pillow")
        with self._lock:
            w, h, raw = win.pixels()
        img = Image.frombuffer("RGBA", (w, h), raw, "raw", "BGRA", 0, 1).convert("RGB")
        img.save(path)
        return path

    def save_screen(self, path, background=(0xFB, 0xF8, 0xEF)):
        """把所有窗口按位置拼到一张 width x height 的图上（简单示意，无装饰）"""
        if Image is None:
            raise RuntimeError("需要 Pillow")
        screen = Image.new("RGB", (self.width, self.height), background)
        with self._lock:
            for win in sorted(self.windows.values(), key=lambda w: w.id):
                w, h, raw = win.pixels()
                img = Image.frombuffer("RGBA", (w, h), raw, "raw", "BGRA", 0, 1).convert("RGB")
                screen.paste(img, (win.x, win.y))
        screen.save(path)
        return path

    def _client_send(self, win, type_, payload):
        self._send(win.client, type_, payload)

    def mouse(self, win, kind, x, y, buttons=0, button=0, wheel=0, clicks=0, modifiers=0):
        self._client_send(win, EV_MOUSE, struct.pack("<IIiiIIiII", win.id, kind, int(x), int(y),
                                                     buttons, button, wheel, clicks, modifiers))

    def move(self, win, x, y, buttons=0):
        self.mouse(win, MOUSE_MOVE, x, y, buttons)
        time.sleep(0.01)

    def click(self, win, x, y, button=BTN_LEFT, clicks=1, modifiers=0):
        self.move(win, x, y)
        self.mouse(win, MOUSE_DOWN, x, y, button, button, clicks=clicks, modifiers=modifiers)
        time.sleep(0.02)
        self.mouse(win, MOUSE_UP, x, y, 0, button, modifiers=modifiers)
        time.sleep(0.02)

    def double_click(self, win, x, y):
        self.click(win, x, y, clicks=1)
        self.click(win, x, y, clicks=2)

    def drag(self, win, x1, y1, x2, y2, steps=10, button=BTN_LEFT):
        self.move(win, x1, y1)
        self.mouse(win, MOUSE_DOWN, x1, y1, button, button, clicks=1)
        for i in range(1, steps + 1):
            self.move(win, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, buttons=button)
        self.mouse(win, MOUSE_UP, x2, y2, 0, button)
        time.sleep(0.02)

    def wheel(self, win, x, y, amount):
        self.mouse(win, MOUSE_WHEEL, x, y, wheel=amount)
        time.sleep(0.02)

    def raw_key(self, win, keycode, state, modifiers=0, codepoint=0):
        self._client_send(win, EV_KEY, struct.pack("<IIIII", win.id, keycode, state, modifiers, codepoint))

    def key(self, win, combo):
        """combo 例如 'ctrl+s'、'shift+tab'、'enter'、'left'、'a'"""
        parts = combo.lower().split("+")
        mods = 0
        for m in parts[:-1]:
            mods |= {"shift": MOD_SHIFT, "ctrl": MOD_CTRL, "alt": MOD_ALT, "super": MOD_SUPER}[m]
        k = parts[-1]
        code = KEY[k]
        cp = _SPECIAL_CP.get(k, 0)
        if len(k) == 1 and k.isalnum():
            cp = ord(k.upper()) if mods & MOD_SHIFT and k.isalpha() else ord(k)
        self.raw_key(win, code, 1, mods, cp)
        time.sleep(0.01)
        self.raw_key(win, code, 0, mods, cp)
        time.sleep(0.01)

    def type_text(self, win, text):
        for ch in text:
            mods, code, cp = 0, 0, ord(ch)
            if "a" <= ch <= "z" or "0" <= ch <= "9":
                code = KEY[ch]
            elif "A" <= ch <= "Z":
                code, mods = KEY[ch.lower()], MOD_SHIFT
            elif ch in _PLAIN_CHARS:
                code = KEY[_PLAIN_CHARS[ch]]
                cp = _SPECIAL_CP.get(_PLAIN_CHARS[ch], cp)
            elif ch in _SHIFT_CHARS:
                code, mods = KEY[_SHIFT_CHARS[ch]], MOD_SHIFT
            # 非 ASCII（例如中文）：keycode=0，只给 codepoint（相当于输入法提交的文字）
            self.raw_key(win, code, 1, mods, cp)
            self.raw_key(win, code, 0, mods, cp)
            time.sleep(0.005)

    def focus(self, win, focused=True):
        self._client_send(win, EV_FOCUS, struct.pack("<II", win.id, 1 if focused else 0))

    def resize(self, win, w, h, timeout=5):
        """模拟用户改变窗口大小：发 CONFIGURE，等待客户端 ATTACH_BUFFER"""
        old = (win.w, win.h, win.fd)
        self._client_send(win, EV_CONFIGURE, struct.pack("<Iii", win.id, w, h))
        deadline = time.time() + timeout
        while time.time() < deadline:
            if (win.w, win.h) == (w, h) and win.fd != old[2]:
                return True
            time.sleep(0.02)
        raise TimeoutError("客户端没有在 %.1f 秒内用新尺寸 %dx%d 重新 ATTACH" % (timeout, w, h))

    def close(self, win):
        self._client_send(win, EV_CLOSE, struct.pack("<I", win.id))


def main():
    ap = argparse.ArgumentParser(description="模拟 SketchOS 显示服务器，运行图形应用并截图")
    ap.add_argument("--socket", default="/tmp/skmock-%d.sock" % os.getpid())
    ap.add_argument("--size", default="1024x768")
    ap.add_argument("--out", default=".")
    ap.add_argument("--wait", type=float, default=10)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("argv", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    argv = a.argv[1:] if a.argv and a.argv[0] == "--" else a.argv
    if not argv:
        ap.error("需要指定要运行的应用")
    w, h = (int(v) for v in a.size.split("x"))
    os.makedirs(a.out, exist_ok=True)
    with MockDisplay(a.socket, w, h, verbose=a.verbose) as d:
        d.spawn(argv)
        win = d.wait_window(timeout=a.wait)
        d.wait_idle(win)
        time.sleep(0.3)
        for wn in list(d.windows.values()):
            p = os.path.join(a.out, "window-%d.png" % wn.id)
            d.save_window(wn, p)
            print("窗口 %d「%s」%dx%d -> %s" % (wn.id, wn.title, wn.w, wn.h, p))
        if d.errors:
            print("协议错误:\n  " + "\n  ".join(d.errors))
            sys.exit(1)


if __name__ == "__main__":
    main()
