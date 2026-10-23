#!/usr/bin/env python3
"""
test_skfiles.py — 文件管理器 skfiles 的自动化测试

    bash tests/apps/run.sh skfiles
    （或 python3 tests/apps/test_skfiles.py --bin <用户态 bin 目录> --out <截图目录> [--only a,b]）

不需要 QEMU：skfiles 是 32 位静态程序，宿主机直接运行；tools/mock_display.py 扮演显示服务器。
skfiles 用环境变量 SKFILES_DEBUG=1 启动，它会把控件位置（geom）、列表内容（list/item/end）、
选中项（sel）、对话框正文（dialog）、启动的应用（launch）打印到标准输出——测试靠这些行
知道界面上发生了什么，再用 mock 显示服务器记录的 d.launches、DAMAGE 请求和真实的文件系统核对。
每个测试用一个新的临时 HOME（里面放好文件夹、文本、图片、脚本、二进制、隐藏文件、中文名文件）。
截图保存在 --out 目录，测试结束后请打开看看。
"""
import struct
import tempfile

from apptest import check, App, settle, shot, run_tests, temp_home, md, shutil, time, os, re, ROOT

FONT_LH = None          # sans-16 的行高（算列表的行位置用）


def font_line_height():
    global FONT_LH
    if FONT_LH is None:
        with open(os.path.join(ROOT, "assets", "fonts", "sans-16.skf"), "rb") as f:
            magic, _size, _asc, _desc, lh = struct.unpack_from("<4sHhhH", f.read(32), 0)
        assert magic == b"SKF1"
        FONT_LH = lh
    return FONT_LH


def header_h():
    return max(26, font_line_height() + 10)       # 与 ui_list.c 一致


def row_h():
    return max(28, font_line_height() + 2)        # skfiles 设置的行高 28


# ---------------------------------------------------------------- 测试用的 HOME

NOTES = "hello notes\n"


def make_home():
    """建一个临时 HOME，返回路径"""
    home = temp_home()
    os.makedirs(os.path.join(home, "projects", "deep", "inner"))
    os.makedirs(os.path.join(home, "照片"))
    files = {
        "notes.txt": NOTES.encode(),                                    # 12 字节
        "中文文件名.txt": "你好世界\n".encode(),                         # 13 字节
        "run.sh": b"#!/bin/sh\necho hi\n",                              # 18 字节
        "picture.ppm": b"P6 2 2 255\n" + bytes(12),                     # 23 字节
        "data.bin": bytes(range(256)) + b"\x00" * 44,                   # 300 字节，二进制
        "big.txt": b"x" * 5000,
        ".secret": b"hidden\n",
        "projects/readme.md": b"# readme\n",
        "projects/deep/inner/leaf.txt": b"leaf\n",
    }
    for name, data in files.items():
        with open(os.path.join(home, name), "wb") as f:
            f.write(data)
    os.chmod(os.path.join(home, "run.sh"), 0o755)
    os.chmod(os.path.join(home, "notes.txt"), 0o644)
    os.symlink("../notes.txt", os.path.join(home, "projects", "link-notes"))
    os.symlink("/nonexistent/sketchos-test", os.path.join(home, "projects", "broken-link"))
    return home


HOME_DIRS = ["projects", "照片"]
HOME_FILES = ["big.txt", "data.bin", "notes.txt", "picture.ppm", "run.sh", "中文文件名.txt"]


# ---------------------------------------------------------------- 驱动 skfiles

class Files:
    def __init__(self, d, args, home, extra=(), cwd=None, wrap=(), binary=None):
        self.d = d
        self.home = home
        binary = binary or os.path.join(args.bin, "skfiles")
        self.app = App(d, list(wrap) + [binary] + list(extra),
                       env={"HOME": home, "SKFILES_DEBUG": "1"}, cwd=cwd)
        d.wait_window(timeout=10)
        # 主窗口是这个程序创建的第一个窗口（启动时可能马上又弹出一个出错框）
        self.win = min(d.windows.values(), key=lambda w: w.id)
        self.last_dlg = self.win.id
        settle(d, self.win)

    # ---- 解析 SKFILES_DEBUG 的输出
    def lines(self):
        return list(self.app.lines)

    def mark(self):
        return len(self.app.lines)

    def geom(self):
        """最近一次布局的控件位置 {名字: (x, y, w, h)}"""
        ls = self.lines()
        start = 0
        for i in range(len(ls) - 1, -1, -1):
            if ls[i].startswith("skfiles: geom window "):
                start = i
                break
        g = {}
        for line in ls[start:]:
            m = re.match(r"skfiles: geom (\S+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
            if m:
                g[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        return g

    def listing(self):
        """最近一次完整的列表 {path, items: [(名字, 类型, 大小)], names, sel}"""
        ls = self.lines()
        end = next((i for i in range(len(ls) - 1, -1, -1) if ls[i].startswith("skfiles: end ")), None)
        if end is None:
            return None
        start = next((i for i in range(end - 1, -1, -1) if ls[i].startswith("skfiles: list ")), None)
        if start is None:
            return None
        items = []
        for line in ls[start + 1:end]:
            if line.startswith("skfiles: item "):
                parts = line[len("skfiles: item "):].split("\t")
                items.append(tuple((parts + ["", "", ""])[:3]))
        sel = re.match(r"skfiles: end \d+ sel=(.*)$", ls[end]).group(1)
        for line in ls[end + 1:]:                        # 之后的选中变化
            if line.startswith("skfiles: sel"):
                sel = line[len("skfiles: sel "):]
        return {"path": ls[start][len("skfiles: list "):], "items": items,
                "names": [it[0] for it in items], "sel": sel}

    def wait_listing(self, pred, timeout=6.0):
        deadline = time.time() + timeout
        L = None
        while time.time() < deadline:
            L = self.listing()
            if L is not None and pred(L):
                return L
            time.sleep(0.05)
        print("    （等待超时，最后的列表：%s）" % (L,))
        return None

    def wait_line(self, prefix, since, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for line in self.lines()[since:]:
                if line.startswith(prefix):
                    return line
            time.sleep(0.05)
        return None

    # ---- 鼠标和键盘
    def row_xy(self, i):
        x, y, w, h = self.geom()["list"]
        return x + 90, y + 3 + header_h() + i * row_h() + row_h() // 2

    def empty_xy(self):
        x, y, w, h = self.geom()["list"]
        return x + w // 2, y + h - 24

    def header_xy(self, col):
        """表头第 col 列的中点（列宽与 main.c 的 col_widths 一致，算法与 ui_list.c 一致：名称列占满剩余宽度）"""
        x, y, w, h = self.geom()["list"]
        body_x, body_w = x + 3, (w - 6) - 12 - 3
        x0, total = body_x + 4, body_w - 8
        widths = [max(40, total - (84 + 140 + 156)), 84, 140, 156]
        cx = x0 + sum(widths[:col])
        return cx + widths[col] // 2, y + 3 + header_h() // 2

    def tool_xy(self, name):
        x, y, w, h = self.geom()[name]
        return x + w // 2, y + h // 2

    def click_row(self, name, clicks=1, button=md.BTN_LEFT):
        L = self.listing()
        i = L["names"].index(name)
        x, y = self.row_xy(i)
        if clicks == 2:
            self.d.double_click(self.win, x, y)
        else:
            self.d.click(self.win, x, y, button=button)

    def select(self, name):
        self.click_row(name)
        return self.wait_listing(lambda L: L["sel"] == name, timeout=3) is not None

    def key(self, combo):
        self.d.key(self.win, combo)

    def dialog(self, timeout=5):
        """等一个新的对话框窗口出现并画完（比上一个对话框新，避免拿到正在关闭的那个）"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            new = [w for w in list(self.d.windows.values()) if w.id > self.last_dlg]
            if new:
                w = max(new, key=lambda w: w.id)
                self.last_dlg = w.id
                self.d.wait_idle(w, quiet=0.3)
                return w
            time.sleep(0.03)
        raise TimeoutError("等待对话框超时（窗口：%s）" % [w.title for w in self.d.windows.values()])

    def wait_dialog_closed(self, timeout=5):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if len(self.d.windows) == 1:
                return True
            time.sleep(0.05)
        return False

    def stop(self):
        """点窗口的关闭按钮，让程序自己正常退出（退出码 0）"""
        if self.app.alive() and self.win.id in self.d.windows:
            self.d.close(self.win)
            code = self.app.wait(3)
            check(code == 0, "关闭窗口后程序正常退出（退出码 %s）" % code)
        self.app.stop()


def wait_until(fn, timeout=4.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            return True
        time.sleep(0.05)
    return fn()


def damages_since(d, start, win):
    out = []
    for req in list(d.requests[start:]):
        if req[2] != "DAMAGE":
            continue
        m = re.match(r"win=(\d+) \((-?\d+),(-?\d+) (\d+)x(\d+)\)", req[3])
        if m and int(m.group(1)) == win.id:
            out.append(tuple(int(v) for v in m.groups()[1:]))
    return out


def quiet(d, win, q=0.3):
    """等窗口没有新的请求（包括 damage 以外的）"""
    deadline = time.time() + 5
    last, t = len(d.requests), time.time()
    while time.time() < deadline:
        time.sleep(0.03)
        if len(d.requests) != last:
            last, t = len(d.requests), time.time()
        elif time.time() - t >= q:
            return


# ================================================================ 测试

def test_listing(d, args):
    """默认列表：文件夹在前、按名字排序、隐藏文件不显示；Ctrl+H / 复选框切换；点表头排序"""
    home = make_home()
    f = Files(d, args, home)
    L = f.wait_listing(lambda L: L["path"] == home)
    check(L is not None, "启动后列出 $HOME")
    if not L:
        return
    check(L["names"] == HOME_DIRS + HOME_FILES, "文件夹在前、按名字排序：%s" % L["names"])
    check(".secret" not in L["names"], "隐藏文件默认不显示")
    types = {it[0]: it[1] for it in L["items"]}
    sizes = {it[0]: it[2] for it in L["items"]}
    check(types.get("projects") == "文件夹" and types.get("notes.txt") == "文本文件" and
          types.get("picture.ppm") == "PPM 图片" and types.get("run.sh") == "Shell 脚本",
          "类型列：%s" % types)
    check(sizes.get("big.txt") == "4.9 KB" and sizes.get("notes.txt") == "12 B" and sizes.get("projects") == "",
          "大小列：%s" % sizes)
    title = f.win.title
    check(title.endswith("文件"), "窗口标题：%s" % title)
    shot(d, f.win, args, "skfiles-home")

    f.key("ctrl+h")
    L = f.wait_listing(lambda L: ".secret" in L["names"])
    check(L is not None and L["names"] == HOME_DIRS + [".secret"] + HOME_FILES,
          "Ctrl+H 显示隐藏文件（排在文件的最前面）：%s" % (L and L["names"]))
    settle(d, f.win)
    shot(d, f.win, args, "skfiles-hidden")
    d.click(f.win, *f.tool_xy("hidden"))           # 复选框：再关掉
    L = f.wait_listing(lambda L: ".secret" not in L["names"])
    check(L is not None, "点“显示隐藏文件”复选框再次隐藏")

    # 点“大小”表头：升序（文件夹仍在前面）；再点一次：降序
    d.click(f.win, *f.header_xy(1))
    by_size = ["notes.txt", "中文文件名.txt", "run.sh", "picture.ppm", "data.bin", "big.txt"]
    L = f.wait_listing(lambda L: L["names"][2:] == by_size)
    check(L is not None and L["names"][:2] == HOME_DIRS, "按大小升序：%s" % (L and L["names"]))
    d.click(f.win, *f.header_xy(1))
    L = f.wait_listing(lambda L: L["names"][2:] == by_size[::-1])
    check(L is not None and L["names"][:2] == HOME_DIRS[::-1], "再点一次按大小降序：%s" % (L and L["names"]))
    settle(d, f.win)
    shot(d, f.win, args, "skfiles-sorted-size-desc")
    d.click(f.win, *f.header_xy(0))                # 回到按名称升序
    L = f.wait_listing(lambda L: L["names"] == HOME_DIRS + HOME_FILES)
    check(L is not None, "点“名称”表头恢复按名字升序")
    f.stop()
    shutil.rmtree(home)


def test_navigation(d, args):
    """双击进入文件夹；后退、上一级、主目录按钮；Backspace、Alt+↑；常用位置"""
    home = make_home()
    proj = os.path.join(home, "projects")
    f = Files(d, args, home)
    f.wait_listing(lambda L: L["path"] == home)
    f.click_row("projects", clicks=2)
    L = f.wait_listing(lambda L: L["path"] == proj)
    check(L is not None, "双击 projects 进入")
    if L:
        check(L["names"] == ["deep", "broken-link", "link-notes", "readme.md"], "projects 的内容：%s" % L["names"])
        types = {it[0]: it[1] for it in L["items"]}
        check(types.get("broken-link") == "失效的链接" and types.get("link-notes") == "文本文件（链接）",
              "符号链接的类型：%s" % types)
    shot(d, f.win, args, "skfiles-projects")
    check(proj in f.app.text() and f.win.title.startswith("projects"), "标题显示文件夹名：%s" % f.win.title)

    d.click(f.win, *f.tool_xy("back"))
    L = f.wait_listing(lambda L: L["path"] == home)
    check(L is not None and L["sel"] == "projects", "“后退”回到主目录并选中 projects：%s" % (L and L["sel"]))

    f.click_row("projects", clicks=2)
    f.wait_listing(lambda L: L["path"] == proj)
    f.click_row("deep", clicks=2)
    f.wait_listing(lambda L: L["path"] == proj + "/deep")
    d.click(f.win, *f.tool_xy("up"))
    L = f.wait_listing(lambda L: L["path"] == proj)
    check(L is not None and L["sel"] == "deep", "“上一级”并选中刚才的文件夹")
    d.click(f.win, *f.tool_xy("home"))
    L = f.wait_listing(lambda L: L["path"] == home)
    check(L is not None, "“主目录”按钮")

    f.key("backspace")                              # 退格 = 后退 → projects
    L = f.wait_listing(lambda L: L["path"] == proj)
    check(L is not None, "Backspace 后退")
    f.key("alt+left")                               # → projects/deep
    L = f.wait_listing(lambda L: L["path"] == proj + "/deep")
    check(L is not None, "Alt+← 再后退一步")
    f.key("alt+up")
    L = f.wait_listing(lambda L: L["path"] == proj)
    check(L is not None, "Alt+↑ 上一级")

    # 常用位置：“根目录 /”
    g = f.geom()
    if check("places" in g, "宽窗口显示常用位置"):
        px, py, pw, ph = g["places"]
        # 常用位置：主目录、根目录 /、进程 /proc（临时 HOME 里没有 文档/图片/示例）
        d.click(f.win, px + 60, py + 3 + 34 + 17)
        L = f.wait_listing(lambda L: L["path"] == "/")
        check(L is not None and "proc" in L["names"], "点常用位置“根目录 /”")
        d.click(f.win, px + 60, py + 3 + 17)
        L = f.wait_listing(lambda L: L["path"] == home)
        check(L is not None, "点常用位置“主目录”")
    check(f.app.alive(), "程序还活着")
    f.stop()
    shutil.rmtree(home)


def test_open(d, args):
    """打开各种文件：文本 → 记事本，图片 → 画板，可执行 → 终端，二进制 → 提示框，失效链接 → 出错"""
    home = make_home()
    f = Files(d, args, home)
    f.wait_listing(lambda L: L["path"] == home)

    f.select("notes.txt")
    f.key("enter")
    p = os.path.join(home, "notes.txt")
    check(wait_until(lambda: ("notepad", p) in d.launches), "回车打开文本 → notepad：%s" % d.launches)
    f.click_row("picture.ppm", clicks=2)
    p = os.path.join(home, "picture.ppm")
    check(wait_until(lambda: ("paint", p) in d.launches), "双击 .ppm → paint：%s" % d.launches)
    f.click_row("run.sh", clicks=2)
    p = os.path.join(home, "run.sh")
    check(wait_until(lambda: ("terminal", p) in d.launches), "双击可执行脚本 → terminal：%s" % d.launches)
    f.click_row("中文文件名.txt", clicks=2)
    p = os.path.join(home, "中文文件名.txt")
    check(wait_until(lambda: ("notepad", p) in d.launches), "中文文件名 → notepad：%s" % d.launches)
    f.click_row("big.txt", clicks=2)
    p = os.path.join(home, "big.txt")
    check(wait_until(lambda: ("notepad", p) in d.launches), "没有 0 字节的文件当作文本 → notepad")

    n = len(d.launches)
    m = f.mark()
    f.click_row("data.bin", clicks=2)
    dlg = f.dialog()
    check(dlg is not None, "二进制文件弹出提示框")
    line = f.wait_line("skfiles: dialog", m)
    check(line and "二进制" in line, "提示框说明是二进制文件：%s" % line)
    shot(d, dlg, args, "skfiles-binary-dialog")
    d.key(dlg, "esc")
    check(f.wait_dialog_closed(), "Esc 关闭提示框")
    check(len(d.launches) == n, "二进制文件不启动任何应用")

    # 符号链接：按它指向的东西打开；失效的链接报错
    f.click_row("projects", clicks=2)
    f.wait_listing(lambda L: L["path"].endswith("/projects"))
    f.click_row("link-notes", clicks=2)
    p = os.path.join(home, "projects", "link-notes")
    check(wait_until(lambda: ("notepad", p) in d.launches), "指向文本的链接 → notepad")
    m = f.mark()
    f.click_row("broken-link", clicks=2)
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog", m)
    check(line and "失效" in line and "/nonexistent/sketchos-test" in line, "失效链接报错：%s" % line)
    shot(d, dlg, args, "skfiles-broken-link")
    d.key(dlg, "enter")
    check(f.wait_dialog_closed(), "回车关闭出错框")

    # 键盘弹出右键菜单（Shift+F10），第一项“打开”
    f.select("readme.md")
    f.key("shift+f10")
    f.key("down")
    f.key("enter")
    p = os.path.join(home, "projects", "readme.md")
    check(wait_until(lambda: ("notepad", p) in d.launches), "Shift+F10 弹出菜单 → 打开")
    check(f.app.alive(), "程序还活着")
    f.stop()
    shutil.rmtree(home)


def test_file_ops(d, args):
    """右键菜单新建文件夹/文本文件；F2 重命名（含重名报错）；Delete 删除（确认、取消、递归删除文件夹）"""
    home = make_home()
    f = Files(d, args, home)
    f.wait_listing(lambda L: L["path"] == home)

    # 右键空白处 → 菜单第一项“新建文件夹”
    d.click(f.win, *f.empty_xy(), button=md.BTN_RIGHT)
    settle(d, f.win)
    shot(d, f.win, args, "skfiles-menu-empty")
    f.key("down")
    f.key("enter")
    dlg = f.dialog()
    shot(d, dlg, args, "skfiles-new-folder-dialog")
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, "新的文件夹")
    d.key(dlg, "enter")
    newdir = os.path.join(home, "新的文件夹")
    check(wait_until(lambda: os.path.isdir(newdir)), "新建文件夹（mkdir）")
    L = f.wait_listing(lambda L: "新的文件夹" in L["names"] and L["sel"] == "新的文件夹")
    check(L is not None, "列表里出现并选中新文件夹")

    # 右键空白处 → 第二项“新建文本文件”，用默认名字
    f.wait_dialog_closed()
    d.click(f.win, *f.empty_xy(), button=md.BTN_RIGHT)
    f.key("down")
    f.key("down")
    f.key("enter")
    dlg = f.dialog()
    d.key(dlg, "enter")
    newfile = os.path.join(home, "新建文本文件.txt")
    check(wait_until(lambda: os.path.isfile(newfile)), "新建文本文件（默认名字“新建文本文件.txt”）")
    f.wait_listing(lambda L: L["sel"] == "新建文本文件.txt")

    # F2 重命名
    f.wait_dialog_closed()
    f.select("notes.txt")
    f.key("f2")
    dlg = f.dialog()
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, "改名了.txt")
    d.key(dlg, "enter")
    check(wait_until(lambda: os.path.exists(os.path.join(home, "改名了.txt"))) and
          not os.path.exists(os.path.join(home, "notes.txt")), "F2 重命名 notes.txt → 改名了.txt")
    L = f.wait_listing(lambda L: L["sel"] == "改名了.txt")
    check(L is not None, "重命名后选中新名字")

    # 重命名成已存在的名字：报错，不覆盖
    f.wait_dialog_closed()
    f.key("f2")
    dlg = f.dialog()
    m = f.mark()
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, "run.sh")
    d.key(dlg, "enter")
    line = f.wait_line("skfiles: dialog 出错了", m)
    check(line and "已经有一个" in line, "重名时报错：%s" % line)
    err = f.dialog()
    shot(d, err, args, "skfiles-rename-exists")
    d.key(err, "enter")
    f.wait_dialog_closed()
    check(open(os.path.join(home, "run.sh"), "rb").read().startswith(b"#!/bin/sh"), "run.sh 没有被覆盖")

    # Delete：确认框里取消
    f.select("big.txt")
    f.key("delete")
    dlg = f.dialog()
    shot(d, dlg, args, "skfiles-delete-confirm")
    d.key(dlg, "esc")
    f.wait_dialog_closed()
    check(os.path.exists(os.path.join(home, "big.txt")), "取消删除：文件还在")
    # Delete：确认删除（回车 = 第一个按钮“删除”）
    f.key("delete")
    dlg = f.dialog()
    d.key(dlg, "enter")
    check(wait_until(lambda: not os.path.exists(os.path.join(home, "big.txt"))), "确认后删除了文件（unlink）")
    L = f.wait_listing(lambda L: "big.txt" not in L["names"])
    check(L is not None and L["sel"] == "data.bin", "删除后选中下一项：%s" % (L and L["sel"]))

    # 递归删除文件夹（里面有子文件夹、文件、符号链接；链接指向的文件必须安然无恙）
    f.wait_dialog_closed()
    f.select("projects")
    m = f.mark()
    d.click(f.win, *f.tool_xy("delete"))            # 工具栏的“删除”按钮
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog", m)
    check(line and "所有内容" in line, "删除文件夹的确认框：%s" % line)
    d.key(dlg, "enter")
    check(wait_until(lambda: not os.path.exists(os.path.join(home, "projects"))), "递归删除了 projects")
    check(os.path.exists(os.path.join(home, "改名了.txt")), "链接指向的文件还在")
    f.wait_listing(lambda L: "projects" not in L["names"])
    check(f.app.alive(), "程序还活着")
    f.stop()
    shutil.rmtree(home)


def test_copy_paste(d, args):
    """Ctrl+C / Ctrl+V：副本命名、保留权限、递归复制文件夹、不能粘贴进自己"""
    home = make_home()
    f = Files(d, args, home)
    f.wait_listing(lambda L: L["path"] == home)

    f.select("notes.txt")
    f.key("ctrl+c")
    check(wait_until(lambda: d.clipboard == os.path.join(home, "notes.txt").encode()),
          "系统剪贴板里放了路径：%r" % d.clipboard)
    f.key("ctrl+v")
    copy1 = os.path.join(home, "notes (副本).txt")
    check(wait_until(lambda: os.path.isfile(copy1)), "粘贴出“notes (副本).txt”")
    if os.path.isfile(copy1):
        check(open(copy1).read() == NOTES, "副本内容一样")
    L = f.wait_listing(lambda L: L["sel"] == "notes (副本).txt")
    check(L is not None, "粘贴后选中副本")
    f.key("ctrl+v")
    check(wait_until(lambda: os.path.isfile(os.path.join(home, "notes (副本 2).txt"))), "再粘贴：“notes (副本 2).txt”")

    f.select("run.sh")
    f.key("ctrl+c")
    f.key("ctrl+v")
    rcopy = os.path.join(home, "run (副本).sh")
    check(wait_until(lambda: os.path.isfile(rcopy)), "复制脚本")
    if os.path.isfile(rcopy):
        check(os.stat(rcopy).st_mode & 0o7777 == 0o755, "保留权限位 0755：%o" % (os.stat(rcopy).st_mode & 0o7777))

    # 复制整个文件夹（含子文件夹和符号链接）
    f.select("projects")
    f.key("ctrl+c")
    f.key("ctrl+v")
    pcopy = os.path.join(home, "projects (副本)")
    check(wait_until(lambda: os.path.isfile(os.path.join(pcopy, "deep", "inner", "leaf.txt"))), "递归复制文件夹")
    link = os.path.join(pcopy, "link-notes")
    check(os.path.islink(link) and os.readlink(link) == "../notes.txt", "符号链接复制成链接本身")
    check(os.path.islink(os.path.join(pcopy, "broken-link")), "失效的链接也照样复制")
    settle(d, f.win)
    shot(d, f.win, args, "skfiles-after-paste")

    # 进入 projects，把 projects 粘贴进它自己：报错
    f.click_row("projects", clicks=2)
    f.wait_listing(lambda L: L["path"] == os.path.join(home, "projects"))
    m = f.mark()
    f.key("ctrl+v")
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog", m)
    check(line and "自己里面" in line, "不能把文件夹粘贴进它自己：%s" % line)
    shot(d, dlg, args, "skfiles-paste-into-itself")
    d.key(dlg, "enter")
    f.wait_dialog_closed()
    check(sorted(os.listdir(os.path.join(home, "projects"))) == ["broken-link", "deep", "link-notes", "readme.md"],
          "projects 没有被改动")

    # 右键菜单里的“复制”“粘贴”：在子文件夹里粘贴主目录的文件
    f.key("alt+up")
    f.wait_listing(lambda L: L["path"] == home)
    f.click_row("中文文件名.txt", button=md.BTN_RIGHT)
    f.key("down")       # 打开
    f.key("down")       # 复制（文件没有“在终端中打开”）
    f.key("enter")
    f.click_row("照片", clicks=2)
    f.wait_listing(lambda L: L["path"] == os.path.join(home, "照片"))
    d.click(f.win, *f.empty_xy(), button=md.BTN_RIGHT)
    f.key("down")       # 粘贴（剪贴板里有东西时是第一项）
    f.key("enter")
    check(wait_until(lambda: os.path.isfile(os.path.join(home, "照片", "中文文件名.txt"))),
          "右键菜单复制粘贴到另一个文件夹（不冲突时保持原名）")
    f.stop()
    shutil.rmtree(home)


def test_properties(d, args):
    """属性对话框：权限字符串和八进制、inode、链接目标"""
    home = make_home()
    f = Files(d, args, home)
    f.wait_listing(lambda L: L["path"] == home)
    f.select("run.sh")
    m = f.mark()
    f.key("alt+enter")
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog 详细信息", m)
    ino = os.lstat(os.path.join(home, "run.sh")).st_ino
    check(line and "rwxr-xr-x" in line and "0755" in line, "属性里有权限 rwxr-xr-x（0755）：%s" % line)
    check(line and ("inode 号：%d" % ino) in line and "硬链接数：1" in line, "属性里有 inode 号和硬链接数")
    check(line and "18 字节" in line and "uid" in line, "属性里有大小和所有者")
    shot(d, dlg, args, "skfiles-properties")
    d.key(dlg, "enter")
    f.wait_dialog_closed()

    # 右键菜单 → 最后一项“属性”
    f.click_row("notes.txt", button=md.BTN_RIGHT)
    settle(d, f.win)
    shot(d, f.win, args, "skfiles-menu-item")
    m = f.mark()
    f.key("end")
    f.key("enter")
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog 详细信息", m)
    check(line and "rw-r--r--" in line and "0644" in line and "文本文件" in line, "右键 → 属性：%s" % line)
    d.key(dlg, "enter")
    f.wait_dialog_closed()

    # 符号链接的属性：链接目标
    f.click_row("projects", clicks=2)
    f.wait_listing(lambda L: L["path"].endswith("/projects"))
    f.select("link-notes")
    m = f.mark()
    f.key("alt+enter")
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog 详细信息", m)
    check(line and "符号链接" in line and "链接指向：../notes.txt" in line, "链接的属性显示目标：%s" % line)
    shot(d, dlg, args, "skfiles-properties-link")
    d.key(dlg, "enter")
    f.wait_dialog_closed()
    f.stop()
    shutil.rmtree(home)


def test_path_box(d, args):
    """地址栏：输入路径回车前往；不存在的路径报错；~；文件路径选中文件；/proc、/sys 不崩溃"""
    home = make_home()
    f = Files(d, args, home)
    f.wait_listing(lambda L: L["path"] == home)

    def go(text):
        d.click(f.win, *f.tool_xy("path"))
        f.key("ctrl+a")
        d.type_text(f.win, text)
        f.key("enter")

    go(home + "/projects/deep")
    L = f.wait_listing(lambda L: L["path"] == home + "/projects/deep")
    check(L is not None and L["names"] == ["inner"], "地址栏输入绝对路径")
    go("../../照片")
    L = f.wait_listing(lambda L: L["path"] == home + "/照片")
    check(L is not None, "地址栏输入相对路径（含 ..）")
    m = f.mark()
    go("/nonexistent/目录")
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog 出错了", m)
    check(line and "找不到" in line and "没有那个文件或目录" in line, "不存在的路径报错（中文错误信息）：%s" % line)
    shot(d, dlg, args, "skfiles-path-error")
    d.key(dlg, "enter")
    f.wait_dialog_closed()
    L = f.listing()
    check(L["path"] == home + "/照片", "报错后留在原来的文件夹")
    m = f.mark()
    go(home + "/notes.txt/x")
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog 出错了", m)
    check(line and "不是目录" in line, "穿过文件的路径报 ENOTDIR：%s" % line)
    d.key(dlg, "enter")
    f.wait_dialog_closed()
    f.key("esc")                                   # 放弃输入，地址栏恢复
    go("~")
    L = f.wait_listing(lambda L: L["path"] == home)
    check(L is not None, "地址栏输入 ~ 回到主目录")
    go(home + "/run.sh")
    L = f.wait_listing(lambda L: L["path"] == home and L["sel"] == "run.sh")
    check(L is not None, "地址栏输入文件路径：打开所在文件夹并选中它")

    # /proc：几百项也要快；数字按数值排序
    t0 = time.time()
    go("/proc")
    L = f.wait_listing(lambda L: L["path"] == "/proc")
    dt = time.time() - t0
    check(L is not None and len(L["names"]) > 20, "/proc 列出 %d 项，用时 %.2f 秒" % (len(L["names"]) if L else 0, dt))
    if L:
        pids = [int(n) for n in L["names"] if n.isdigit()]
        check(pids == sorted(pids) and len(pids) > 0, "进程号按数值排序：%s…" % pids[:8])
        check("meminfo" in L["names"], "/proc/meminfo 在列表里")
    check(dt < 3.0, "/proc 足够快（%.2f 秒）" % dt)
    settle(d, f.win)
    shot(d, f.win, args, "skfiles-proc")
    f.key("end")                                    # 滚到底，选最后一项
    f.key("pagedown")
    L = f.wait_listing(lambda L: L["sel"] != "")
    check(L is not None, "大文件夹里键盘移动正常：%s" % (L and L["sel"]))
    go("/proc/meminfo")
    L = f.wait_listing(lambda L: L["path"] == "/proc" and L["sel"] == "meminfo")
    f.key("enter")
    check(wait_until(lambda: ("notepad", "/proc/meminfo") in d.launches), "/proc/meminfo（大小为 0 的内核文件）→ 记事本")
    go("/sys")
    L = f.wait_listing(lambda L: L["path"] == "/sys")
    check(L is not None, "进入 /sys")
    go("/sys/kernel")
    f.wait_listing(lambda L: L["path"] == "/sys/kernel")
    f.key("f5")
    check(f.app.alive(), "/proc、/sys 里程序还活着")
    f.stop()
    shutil.rmtree(home)


def test_permissions(d, args):
    """以普通用户 nobody 运行：打不开的文件夹、删不掉/改不了名的文件都给出中文错误，程序不崩溃"""
    if os.geteuid() != 0 or not shutil.which("setpriv"):
        print("    （跳过：需要 root 权限和 setpriv 才能切换成普通用户）")
        return
    home = make_home()
    locked, ro = os.path.join(home, "locked"), os.path.join(home, "readonly")
    os.makedirs(locked)
    os.makedirs(ro)
    with open(os.path.join(locked, "secret.txt"), "w") as fp:
        fp.write("secret")
    with open(os.path.join(ro, "keep.txt"), "w") as fp:
        fp.write("keep")
    os.chmod(locked, 0)
    os.chmod(ro, 0o555)
    os.chmod(home, 0o755)
    # nobody 进不了 /root（构建目录在那里），把程序复制到 /tmp；套接字也要让它能连上
    bindir = tempfile.mkdtemp(prefix="skfiles-bin.")
    os.chmod(bindir, 0o755)
    binary = os.path.join(bindir, "skfiles")
    shutil.copy(os.path.join(args.bin, "skfiles"), binary)
    os.chmod(binary, 0o755)
    os.chmod(d.socket_path, 0o777)
    f = Files(d, args, home, binary=binary,
              wrap=["setpriv", "--reuid=65534", "--regid=65534", "--clear-groups", "--"])
    L = f.wait_listing(lambda L: L["path"] == home)
    check(L is not None and "locked" in L["names"], "nobody 也能列出主目录")

    m = f.mark()
    f.click_row("locked", clicks=2)
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog 出错了", m)
    check(line and "无法打开文件夹" in line and "权限不够" in line, "进不去的文件夹：%s" % line)
    shot(d, dlg, args, "skfiles-eacces")
    d.key(dlg, "enter")
    f.wait_dialog_closed()
    check(f.listing()["path"] == home, "留在原来的文件夹")

    f.click_row("readonly", clicks=2)
    f.wait_listing(lambda L: L["path"] == ro)
    f.select("keep.txt")
    f.key("delete")
    confirm = f.dialog()
    m = f.mark()
    d.key(confirm, "enter")                         # 确认删除
    err = f.dialog()
    line = f.wait_line("skfiles: dialog 出错了", m)
    check(line and "删除“keep.txt”失败" in line and "权限不够" in line, "只读文件夹里删除失败：%s" % line)
    d.key(err, "enter")
    f.wait_dialog_closed()
    check(os.path.exists(os.path.join(ro, "keep.txt")), "文件还在")
    f.key("f2")
    rn = f.dialog()
    d.key(rn, "ctrl+a")
    d.type_text(rn, "new.txt")
    m = f.mark()
    d.key(rn, "enter")
    err = f.dialog()
    line = f.wait_line("skfiles: dialog 出错了", m)
    check(line and "无法把“keep.txt”重命名" in line and "权限不够" in line, "只读文件夹里重命名失败：%s" % line)
    d.key(err, "enter")
    f.wait_dialog_closed()

    f.key("ctrl+c")
    f.key("alt+up")
    f.wait_listing(lambda L: L["path"] == home)
    m = f.mark()
    f.key("ctrl+v")                                 # 主目录对 nobody 是只读的
    err = f.dialog()
    line = f.wait_line("skfiles: dialog 出错了", m)
    check(line and "权限不够" in line and ("出错的位置：%s/keep.txt" % home) in line,
          "没有写权限时粘贴失败，指出是目标那一边：%s" % line)
    d.key(err, "enter")
    f.wait_dialog_closed()
    check(f.app.alive(), "程序还活着")
    f.stop()
    os.chmod(locked, 0o755)
    shutil.rmtree(home)
    shutil.rmtree(bindir)


def test_file_arg(d, args):
    """命令行参数：文件 → 打开所在文件夹并选中它；文件夹；不存在 → 报错并打开主目录"""
    home = make_home()
    f = Files(d, args, home, extra=[os.path.join(home, "picture.ppm")])
    L = f.wait_listing(lambda L: L["path"] == home and L["sel"] == "picture.ppm")
    check(L is not None, "参数是文件：选中 picture.ppm")
    settle(d, f.win)
    shot(d, f.win, args, "skfiles-arg-file")
    f.stop()
    wait_until(lambda: len(d.windows) == 0)

    f = Files(d, args, home, extra=["projects"], cwd=home)      # 相对路径
    L = f.wait_listing(lambda L: L["path"] == home + "/projects")
    check(L is not None, "参数是相对路径的文件夹")
    f.stop()
    wait_until(lambda: len(d.windows) == 0)

    f = Files(d, args, home, extra=[home + "/不存在"])
    dlg = f.dialog()
    line = f.wait_line("skfiles: dialog 出错了", 0)
    check(line and "不存在" in line, "参数不存在时报错：%s" % line)
    d.key(dlg, "enter")
    f.wait_dialog_closed()
    L = f.wait_listing(lambda L: L["path"] == home)
    check(L is not None, "然后打开主目录")
    f.stop()
    shutil.rmtree(home)


def test_damage(d, args):
    """选中变化只重画两行和状态栏；鼠标在列表里移动只重画悬停的行"""
    home = make_home()
    f = Files(d, args, home)
    f.wait_listing(lambda L: L["path"] == home)
    f.select("data.bin")
    x, y = f.row_xy(f.listing()["names"].index("notes.txt"))
    d.move(f.win, x, y)
    quiet(d, f.win)
    start = len(d.requests)
    d.click(f.win, x, y)
    f.wait_listing(lambda L: L["sel"] == "notes.txt")
    quiet(d, f.win)
    rects = damages_since(d, start, f.win)
    area = sum(w * h for _, _, w, h in rects)
    total = f.win.w * f.win.h
    lx, ly, lw, lh = f.geom()["list"]
    sx, sy, sw, sh = f.geom()["status"]
    print("    选中变化的重画：%s（%.1f%% 的窗口）" % (rects, 100.0 * area / total))
    check(rects and max(h for _, _, _, h in rects) <= 2 * row_h() + 8, "每块重画区域都很矮（一行 / 状态栏）")
    check(all((r[1] >= ly and r[1] + r[3] <= ly + lh) or r[1] >= sy - 8 for r in rects),
          "只重画列表里的行和状态栏")
    check(area < total * 0.2, "选中变化只重画一小部分窗口：%.1f%%" % (100.0 * area / total))

    start = len(d.requests)
    for i in range(4):                              # 鼠标在行之间移动：悬停效果
        d.move(f.win, x + 40, f.row_xy(i)[1])
    quiet(d, f.win)
    rects = damages_since(d, start, f.win)
    print("    悬停移动的重画：%s" % rects)
    check(rects and max(h for _, _, _, h in rects) <= 2 * row_h() + 8, "悬停只重画相关的行")

    start = len(d.requests)
    m = f.mark()
    f.key("f5")                                     # 内容没变的刷新：几乎不重画
    f.wait_line("skfiles: end", m)
    quiet(d, f.win)
    rects = [r for r in damages_since(d, start, f.win) if r[2] > 16]
    print("    F5（内容没变）的重画：%s" % rects)
    check(sum(w * h for _, _, w, h in rects) < total * 0.05, "内容没变的刷新不重画列表")
    f.stop()
    shutil.rmtree(home)


def test_resize(d, args):
    """改变窗口大小：窄窗口隐藏常用位置、工具栏只显示图标"""
    home = make_home()
    f = Files(d, args, home)
    f.wait_listing(lambda L: L["path"] == home)
    d.resize(f.win, 620, 420)
    settle(d, f.win)
    g = f.geom()
    check(g.get("window", (0, 0, 0, 0))[2:] == (620, 420), "布局收到新尺寸：%s" % (g.get("window"),))
    check("places" not in g and g["list"][0] == 10, "窄窗口不显示常用位置，列表靠左：%s" % (g.get("list"),))
    check(g["back"][2] <= 48, "窄窗口工具栏只显示图标：%s" % (g.get("back"),))
    check(g["list"][0] + g["list"][2] <= 620 and g["hidden"][0] + g["hidden"][2] <= 620, "控件都在窗口里")
    shot(d, f.win, args, "skfiles-narrow")
    d.resize(f.win, 1000, 640)
    settle(d, f.win)
    g = f.geom()
    check("places" in g and g["back"][2] > 48, "拉宽后恢复常用位置和文字按钮")
    f.select("picture.ppm")
    settle(d, f.win)
    shot(d, f.win, args, "skfiles-wide-selected")
    f.stop()
    shutil.rmtree(home)


if __name__ == "__main__":
    run_tests([
        ("listing", test_listing),
        ("navigation", test_navigation),
        ("open", test_open),
        ("file_ops", test_file_ops),
        ("copy_paste", test_copy_paste),
        ("properties", test_properties),
        ("path_box", test_path_box),
        ("permissions", test_permissions),
        ("file_arg", test_file_arg),
        ("damage", test_damage),
        ("resize", test_resize),
    ])
