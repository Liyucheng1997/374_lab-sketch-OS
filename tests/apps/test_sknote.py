#!/usr/bin/env python3
"""
tests/apps/test_sknote.py — 记事本 sknote 的自动化测试（不需要 QEMU：tools/mock_display.py 扮演 sketchd）

    bash tests/apps/run.sh sknote
    python3 tests/apps/test_sknote.py --bin <用户态 bin 目录> --out <截图目录> [--only open,save]

程序用 SKNOTE_DEBUG=1 启动，它会把状态打印到标准输出（格式见 userspace/gui/apps/sknote/main.c 开头）：
    sknote: state modified=0 bytes=96 chars=67 lines=4 line=1 col=1 wrap=1 numbers=0 crlf=0 path=/x/y.txt
    sknote: title *y.txt - 记事本
    sknote: saved 96 /x/y.txt
    sknote: error 无法打开……
    sknote: ask 1
    sknote: found 12 6 wrapped
    sknote: notfound
对话框是独立的窗口：d.wait_window(count=2) 等第二个窗口出现。
"""
import os
import re
import stat
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apptest import check, App, settle, shot, run_tests, md  # noqa: E402

# 工具栏布局（与 sknote/main.c 的 layout() 一致）
TOOLS_END = 10 + 82 * 4 + 100 + 4 * 4         # 与 sknote/main.c 的 TOOLS_END 一致
CHK_WRAP = (TOOLS_END + 22 + 54, 24)
CHK_NUMBERS = (TOOLS_END + 22 + 114 + 36, 24)
FIND_BUTTON = (10 + (82 + 4) * 3 + 100 + 4 + 41, 24)


# ---------------------------------------------------------------- 小工具

def parse_state(rest):
    head, _, path = rest.partition(" path=")
    st = {"path": path}
    for kv in head.split():
        k, v = kv.split("=", 1)
        st[k] = int(v)
    return st


def wait_line(app, since, pattern, timeout=5.0):
    """等 app.lines[since:] 里出现匹配 pattern 的行，返回 match 对象或 None"""
    rx = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in list(app.lines[since:]):
            m = rx.search(line)
            if m:
                return m
        time.sleep(0.02)
    return None


def wait_state(app, since, pred=lambda s: True, timeout=5.0):
    """等 since 之后第一个满足 pred 的 state 行"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in list(app.lines[since:]):
            if line.startswith("sknote: state "):
                st = parse_state(line[len("sknote: state "):])
                if pred(st):
                    return st
        time.sleep(0.02)
    return None


def wait_title(d, win, pred, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred(win.title):
            return True
        time.sleep(0.02)
    return False


def wait_gone(d, win, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if win.id not in d.windows:
            return True
        time.sleep(0.02)
    return False


def damages_since(d, start, win):
    out = []
    for req in list(d.requests[start:]):
        if req[2] != "DAMAGE":
            continue
        m = re.match(r"win=(\d+) \((-?\d+),(-?\d+) (\d+)x(\d+)\)", req[3])
        if m and int(m.group(1)) == win.id:
            out.append(tuple(int(v) for v in m.groups()[1:]))
    return out


def write(path, data):
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))


def read(path):
    with open(path, "rb") as f:
        return f.read()


def new_home():
    home = tempfile.mkdtemp(prefix="sknote-test.")
    os.makedirs(os.path.join(home, "文档"))
    return home


def start(d, args, home, path=None, expect_window=True):
    argv = [os.path.join(args.bin, "sknote")] + ([path] if path is not None else [])
    app = App(d, argv, env={"SKNOTE_DEBUG": "1", "HOME": home}, cwd=home)
    win = d.wait_window(timeout=10, title="记事本") if expect_window else None
    if win:
        settle(d, win)
    return app, win




def ask_dialog(d, app, main):
    """关闭主窗口（有未保存的修改）→ 等“要保存吗？”对话框"""
    d.close(main)
    dlg = d.wait_window(timeout=5, count=2)
    settle(d, dlg)
    return dlg


# ---------------------------------------------------------------- 测试

def test_open(d, args):
    home = new_home()
    path = os.path.join(home, "文档", "问候.txt")
    text = "你好，世界！\n这是 SketchOS 的记事本。\nThe quick brown fox jumps over the lazy dog.\n"
    write(path, text)
    app, win = start(d, args, home, path)
    st = wait_state(app, 0, lambda s: s["path"] == path)
    check(st is not None, "打开已有文件：状态里的路径是 %s" % path)
    if st:
        check(st["bytes"] == len(text.encode()), "读进来的字节数 %d == 文件大小 %d" % (st["bytes"], len(text.encode())))
        check(st["chars"] == len(text.replace("\n", "")), "字符数（不含换行）%d" % st["chars"])
        check(st["lines"] == 4 and st["modified"] == 0, "4 行（末尾换行后的空行也算），未修改: %s" % st)
    check(win.title == "问候.txt - 记事本", "标题是“问候.txt - 记事本”：%s" % win.title)
    shot(d, win, args, "sknote-open")
    # 相对路径参数：相对于进程当前目录（这里 cwd=home）
    app.stop()
    app2, win2 = start(d, args, home, "文档/问候.txt")
    check(wait_state(app2, 0, lambda s: s["path"] == path) is not None, "相对路径被转换成绝对路径")
    app2.stop()


def test_type_and_save(d, args):
    home = new_home()
    path = os.path.join(home, "笔记.txt")
    write(path, "你好，世界\n第二行\n")
    os.chmod(path, 0o640)
    ino0 = os.stat(path).st_ino
    app, win = start(d, args, home, path)
    wait_state(app, 0, lambda s: s["path"] == path)
    mark = len(app.lines)
    d.type_text(win, "ABC中文")
    check(wait_title(d, win, lambda t: t.startswith("*")), "打字后标题以 * 开头：%s" % win.title)
    st = wait_state(app, mark, lambda s: s["modified"] == 1 and s["col"] == 6)
    check(st is not None, "状态：已修改，光标在第 1 行第 6 列")
    settle(d, win)
    shot(d, win, args, "sknote-modified")
    mark = len(app.lines)
    d.key(win, "ctrl+s")
    m = wait_line(app, mark, r"^sknote: saved (\d+) (.*)$")
    check(m is not None and m.group(2) == path, "Ctrl+S 保存到原文件")
    want = "ABC中文你好，世界\n第二行\n".encode()
    check(read(path) == want, "文件内容一字不差（UTF-8，末尾的换行原样保留）：%r" % read(path))
    check(stat.S_IMODE(os.stat(path).st_mode) == 0o640, "保留原文件权限 0640：%o" % stat.S_IMODE(os.stat(path).st_mode))
    check(os.stat(path).st_ino != ino0, "保存是“写临时文件 + rename”（换了一个新的 inode）")
    check(not [f for f in os.listdir(home) if ".sknote-" in f], "没有留下临时文件：%s" % os.listdir(home))
    check(wait_title(d, win, lambda t: t == "笔记.txt - 记事本"), "保存后标题去掉 *：%s" % win.title)
    # 撤销回到保存时的状态 → 又变成“未修改”
    d.type_text(win, "x")
    check(wait_title(d, win, lambda t: t.startswith("*")), "再打一个字：*")
    d.key(win, "ctrl+z")
    check(wait_title(d, win, lambda t: t == "笔记.txt - 记事本"), "撤销到保存时的样子：* 消失 (%s)" % win.title)
    app.stop()

    # 末尾没有换行的文件：保存时也不会补上
    path2 = os.path.join(home, "无换行.txt")
    write(path2, "最后一行没有换行")
    app, win = start(d, args, home, path2)
    wait_state(app, 0, lambda s: s["path"] == path2)
    d.key(win, "ctrl+end")
    d.type_text(win, "。")
    mark = len(app.lines)
    d.key(win, "ctrl+s")
    check(wait_line(app, mark, r"^sknote: saved") is not None, "保存成功")
    check(read(path2) == "最后一行没有换行。".encode(), "不会自动补末尾换行：%r" % read(path2))
    app.stop()

    # Windows 换行（\r\n）：打开时变成 \n，保存时写回 \r\n
    path3 = os.path.join(home, "windows.txt")
    write(path3, b"line1\r\nline2\r\n")
    app, win = start(d, args, home, path3)
    st = wait_state(app, 0, lambda s: s["path"] == path3)
    check(st and st["crlf"] == 1 and st["bytes"] == 12, "CRLF 文件：crlf=1，编辑器里是 \\n：%s" % st)
    d.type_text(win, "X")
    mark = len(app.lines)
    d.key(win, "ctrl+s")
    wait_line(app, mark, r"^sknote: saved")
    check(read(path3) == b"Xline1\r\nline2\r\n", "保存时写回 \\r\\n：%r" % read(path3))
    app.stop()


def test_new_file(d, args):
    home = new_home()
    path = os.path.join(home, "新建的笔记.txt")
    app, win = start(d, args, home, path)
    st = wait_state(app, 0, lambda s: s["path"] == path)
    check(st is not None and st["bytes"] == 0, "不存在的文件：空文档，路径已绑定")
    check(win.title == "新建的笔记.txt - 记事本", "标题是文件名：%s" % win.title)
    check(not os.path.exists(path), "打开时还没有创建文件")
    d.type_text(win, "第一行\n第二行")
    mark = len(app.lines)
    d.key(win, "ctrl+s")
    check(wait_line(app, mark, r"^sknote: saved") is not None, "Ctrl+S 保存")
    check(os.path.exists(path) and read(path) == "第一行\n第二行".encode(), "保存时创建文件：%r" %
          (read(path) if os.path.exists(path) else None))
    um = os.umask(0)
    os.umask(um)
    mode = stat.S_IMODE(os.stat(path).st_mode)
    check(mode == 0o666 & ~um, "新文件权限 = 0666 & ~umask = %o：%o" % (0o666 & ~um, mode))
    app.stop()

    # 保存失败（所在的文件夹不存在）：弹出错误框，文档仍然是“已修改”
    bad = os.path.join(home, "没有这个文件夹", "a.txt")
    app, win = start(d, args, home, bad)
    wait_state(app, 0, lambda s: s["path"] == bad)
    d.type_text(win, "内容")
    wait_title(d, win, lambda t: t.startswith("*"))
    mark = len(app.lines)
    d.key(win, "ctrl+s")
    dlg = d.wait_window(timeout=5, count=2)
    settle(d, dlg)
    m = wait_line(app, mark, r"^sknote: error (.*)$")
    check(m is not None and "无法保存" in m.group(1) and "没有那个文件或目录" in m.group(1),
          "保存失败：中文错误信息（%s）" % (m and m.group(1)))
    shot(d, dlg, args, "sknote-error-save")
    d.key(dlg, "enter")
    check(wait_gone(d, dlg), "回车关闭错误框")
    check(win.title.startswith("*") and app.alive(), "保存失败后文档仍然是“已修改”：%s" % win.title)
    check(not os.path.exists(os.path.dirname(bad)), "没有创建任何东西")
    app.stop()


def test_close_dialog(d, args):
    home = new_home()
    path = os.path.join(home, "关闭测试.txt")
    write(path, "原来的内容\n")

    # 取消（Esc）、取消（点按钮）：窗口还在，文件不变
    app, win = start(d, args, home, path)
    wait_state(app, 0, lambda s: s["path"] == path)
    d.type_text(win, "改动")
    wait_title(d, win, lambda t: t.startswith("*"))
    dlg = ask_dialog(d, app, win)
    check(dlg.title == "记事本" and dlg.flags & 0x8, "关闭时弹出“要保存吗？”对话框")
    shot(d, dlg, args, "sknote-ask")
    mark = len(app.lines)
    d.key(dlg, "esc")
    check(wait_line(app, mark, r"^sknote: ask -1$") is not None, "Esc = 取消")
    check(wait_gone(d, dlg) and app.alive() and win.id in d.windows, "取消后记事本还开着")
    dlg = ask_dialog(d, app, win)
    mark = len(app.lines)
    d.click(dlg, dlg.w - 20 - 44, dlg.h - 20 - 13)   # 最右边的“取消”按钮
    check(wait_line(app, mark, r"^sknote: ask 2$") is not None, "点“取消”按钮")
    check(wait_gone(d, dlg) and app.alive(), "点“取消”后记事本还开着")
    check(read(path) == "原来的内容\n".encode(), "取消：文件没被改动")

    # 不保存（Tab 两次到第二个按钮 + 回车）：退出，文件不变
    dlg = ask_dialog(d, app, win)
    mark = len(app.lines)
    d.key(dlg, "tab")
    d.key(dlg, "tab")
    d.key(dlg, "enter")
    check(wait_line(app, mark, r"^sknote: ask 1$") is not None, "选“不保存”")
    check(app.wait(5) == 0, "不保存：程序退出（退出码 0）")
    check(read(path) == "原来的内容\n".encode(), "不保存：文件没被改动")

    # 保存（回车 = 第一个按钮）：保存后退出
    app, win = start(d, args, home, path)
    wait_state(app, 0, lambda s: s["path"] == path)
    d.type_text(win, "新")
    wait_title(d, win, lambda t: t.startswith("*"))
    dlg = ask_dialog(d, app, win)
    mark = len(app.lines)
    d.key(dlg, "enter")
    check(wait_line(app, mark, r"^sknote: ask 0$") is not None, "选“保存”")
    check(app.wait(5) == 0, "保存：程序退出（退出码 0）")
    check(read(path) == "新原来的内容\n".encode(), "保存：文件已更新 %r" % read(path))

    # 没有修改：直接关闭，不问
    app, win = start(d, args, home, path)
    wait_state(app, 0, lambda s: s["path"] == path)
    d.close(win)
    check(app.wait(5) == 0, "没有修改时关闭窗口直接退出")
    check(not any(l.startswith("sknote: ask") for l in app.lines), "没有弹出“要保存吗？”")

    # 未命名文档选“保存”：先弹出“另存为”
    app, win = start(d, args, home)
    d.type_text(win, "未命名的内容")
    wait_title(d, win, lambda t: t.startswith("*"))
    dlg = ask_dialog(d, app, win)
    d.key(dlg, "enter")
    check(wait_gone(d, dlg), "询问框关闭")
    fdlg = d.wait_window(timeout=5, count=2)
    settle(d, fdlg)
    check(fdlg.title == "另存为", "接着弹出“另存为”：%s" % fdlg.title)
    d.key(fdlg, "ctrl+a")
    d.type_text(fdlg, "起个名字.txt")
    d.key(fdlg, "enter")
    check(app.wait(5) == 0, "另存为之后退出")
    saved = os.path.join(home, "文档", "起个名字.txt")
    check(os.path.exists(saved) and read(saved) == "未命名的内容".encode(),
          "保存到 $HOME/文档（对话框的起始目录）")


def test_save_as(d, args):
    home = new_home()
    path = os.path.join(home, "原件.txt")
    write(path, "原件内容\n")
    other = os.path.join(home, "已存在.txt")
    write(other, "会被替换\n")
    app, win = start(d, args, home, path)
    wait_state(app, 0, lambda s: s["path"] == path)
    d.type_text(win, "改过的")
    d.key(win, "ctrl+shift+s")
    fdlg = d.wait_window(timeout=5, count=2)
    settle(d, fdlg)
    check(fdlg.title == "另存为" and fdlg.flags & 0x8, "Ctrl+Shift+S 打开“另存为”对话框")
    shot(d, fdlg, args, "sknote-save-as")
    d.key(fdlg, "ctrl+a")
    d.type_text(fdlg, "副本.txt")
    mark = len(app.lines)
    d.key(fdlg, "enter")
    copy = os.path.join(home, "副本.txt")
    m = wait_line(app, mark, r"^sknote: saved \d+ (.*)$")
    check(m is not None and m.group(1) == copy, "另存为到当前文件所在的目录：%s" % (m.group(1) if m else None))
    check(read(copy) == "改过的原件内容\n".encode(), "副本的内容")
    check(read(path) == "原件内容\n".encode(), "原件没被改动")
    check(wait_title(d, win, lambda t: t == "副本.txt - 记事本"), "标题换成新文件名：%s" % win.title)
    check(wait_state(app, mark, lambda s: s["path"] == copy and s["modified"] == 0) is not None, "当前文档变成副本")
    # 另存为一个已经存在的文件：先问“要替换它吗？”
    d.key(win, "ctrl+shift+s")
    fdlg = d.wait_window(timeout=5, count=2)
    settle(d, fdlg)
    d.key(fdlg, "ctrl+a")
    d.type_text(fdlg, "已存在.txt")
    d.key(fdlg, "enter")
    confirm = d.wait_window(timeout=5, count=3)
    settle(d, confirm)
    check(confirm.title == "确认替换", "已存在：确认替换 (%s)" % confirm.title)
    mark = len(app.lines)
    d.key(confirm, "enter")
    check(wait_line(app, mark, r"^sknote: saved") is not None, "替换后保存")
    check(read(other) == "改过的原件内容\n".encode(), "已存在的文件被替换：%r" % read(other))
    # 在对话框里取消：什么都不发生
    d.key(win, "ctrl+shift+s")
    fdlg = d.wait_window(timeout=5, count=2)
    settle(d, fdlg)
    mark = len(app.lines)
    d.key(fdlg, "esc")
    check(wait_gone(d, fdlg), "Esc 关掉另存为对话框")
    time.sleep(0.3)
    check(not any("saved" in l for l in app.lines[mark:]), "取消另存为：没有保存")
    app.stop()


def test_error_args(d, args):
    home = new_home()
    big = os.path.join(home, "大文件.txt")
    write(big, b"a" * (1536 * 1024))
    binf = os.path.join(home, "程序.bin")
    write(binf, b"\x7fELF\x01\x01\x01\x00\x00\x00binary")
    cases = [(os.path.join(home, "文档"), "文件夹", "dir"), (binf, "二进制", "binary"), (big, "太大", "big")]
    for path, word, tag in cases:
        # 主窗口和错误框的标题里都有“记事本”：先等两个窗口都出现，再按标题区分
        app, _ = start(d, args, home, path, expect_window=False)
        dlg = d.wait_window(timeout=10, count=2)
        win = d.wait_window(timeout=5, title="未命名 - 记事本")
        check(dlg.id != win.id and dlg.title == "记事本", "%s：主窗口之外弹出了错误框" % tag)
        settle(d, dlg)
        m = wait_line(app, 0, r"^sknote: error (.*)$")
        check(m is not None and word in m.group(1), "%s：错误框说明原因（%s）" % (tag, m.group(1) if m else None))
        check(dlg.flags & 0x8, "%s：错误框是对话框窗口" % tag)
        shot(d, dlg, args, "sknote-error-" + tag)
        mark = len(app.lines)
        d.key(dlg, "enter")
        check(wait_gone(d, dlg), "%s：回车关闭错误框" % tag)
        check(app.alive() and win.id in d.windows, "%s：记事本还在运行" % tag)
        check(win.title == "未命名 - 记事本", "%s：得到一个空的未命名文档（%s）" % (tag, win.title))
        d.type_text(win, "x")
        st = wait_state(app, mark, lambda s: s["bytes"] == 1)
        check(st is not None and st["path"] == "", "%s：还可以正常编辑，路径为空" % tag)
        app.stop()
    check(read(big) == b"a" * (1536 * 1024), "大文件没被碰过")


def test_damage(d, args):
    home = new_home()
    path = os.path.join(home, "重画.txt")
    write(path, "第一行\n第二行\n第三行\n" + "很长的一段文字，" * 40 + "\n最后一行\n")
    app, win = start(d, args, home, path)
    wait_state(app, 0, lambda s: s["path"] == path)
    d.key(win, "down")
    settle(d, win, quiet=0.4)
    start_i = len(d.requests)
    d.type_text(win, "字")
    time.sleep(0.4)                      # 等字数统计（最多延迟 150ms）
    settle(d, win, quiet=0.3)
    rects = damages_since(d, start_i, win)
    area = sum(r[2] * r[3] for r in rects)
    total = win.w * win.h
    print("    打一个字的重画：%s，共 %d 像素（窗口 %d）" % (rects, area, total))
    check(rects, "打字后有重画")
    check(area < total * 0.12, "打一个字只重画一小块：%.1f%% 的窗口" % (100.0 * area / total))
    check(all(r[3] <= 40 for r in rects), "每块都不高于一行：%s" % [r[3] for r in rects])
    start_i = len(d.requests)
    d.key(win, "right")
    settle(d, win, quiet=0.3)
    rects = damages_since(d, start_i, win)
    area = sum(r[2] * r[3] for r in rects)
    check(rects and area < total * 0.05, "移动光标：只重画光标和状态栏的行列号（%d 像素）" % area)
    app.stop()


def test_find(d, args):
    home = new_home()
    path = os.path.join(home, "水果.txt")
    text = "苹果 香蕉 苹果\n第二行：apple 苹果\n"
    write(path, text)
    app, win = start(d, args, home, path)
    raw = text.encode()
    hits = [m.start() for m in re.finditer("苹果".encode(), raw)]      # 字节偏移
    # Ctrl+F：输入框对话框，输入要找的文字，回车
    since = len(app.lines)
    d.key(win, "ctrl+f")
    dlg = d.wait_window(timeout=5, count=2)
    settle(d, dlg)
    d.type_text(dlg, "苹果")
    d.key(dlg, "enter")
    m = wait_line(app, since, r"^sknote: found (\d+) (\d+)( wrapped)?$")
    check(m and int(m.group(1)) == hits[0] and int(m.group(2)) == 6 and not m.group(3),
          "Ctrl+F 找到第一个“苹果”（字节偏移 %d）：%s" % (hits[0], m and m.group(0)))
    st = wait_state(app, since, lambda s: s["line"] == 1 and s["col"] == 3)
    check(st is not None, "光标停在选中文字的末尾（第 1 行第 3 列），状态栏跟着更新")
    settle(d, win)
    shot(d, win, args, "sknote-find")
    # F3：接着往后找；找到最后一个之后从头找
    for i, want in enumerate(hits[1:] + [hits[0]]):
        since = len(app.lines)
        d.key(win, "f3")
        m = wait_line(app, since, r"^sknote: found (\d+) (\d+)( wrapped)?$")
        wrapped = i == len(hits) - 1
        check(m and int(m.group(1)) == want and bool(m.group(3)) == wrapped,
              "F3 第 %d 次找到偏移 %d%s：%s" % (i + 1, want, "（从头找）" if wrapped else "", m and m.group(0)))
    # 找到后直接打字：替换选中的文字
    d.type_text(win, "梨")
    d.key(win, "ctrl+s")                       # 事件按顺序处理：保存一定在打字之后
    check(wait_line(app, 0, r"^sknote: saved ") is not None, "保存")
    check(read(path).decode() == "梨 香蕉 苹果\n第二行：apple 苹果\n", "打字替换了选中的“苹果”：%r" % read(path).decode())
    # 工具栏“查找”按钮：找不到时提示
    since = len(app.lines)
    d.click(win, *FIND_BUTTON)
    dlg = d.wait_window(timeout=5, count=2)
    settle(d, dlg)
    d.key(dlg, "ctrl+a")
    d.type_text(dlg, "西瓜")
    d.key(dlg, "enter")
    check(wait_line(app, since, r"^sknote: notfound$") is not None, "工具栏“查找”：找不到“西瓜”")
    check(wait_gone(d, dlg), "输入框关掉了")
    msg = d.wait_window(timeout=5, title="查找")    # 输入框和提示框的标题都是“查找”：等前者消失再找
    settle(d, msg)
    shot(d, msg, args, "sknote-find-notfound")
    d.key(msg, "enter")
    check(wait_gone(d, msg), "提示框按回车关掉")
    # Esc 取消查找：什么也不做，也不弹提示
    since = len(app.lines)
    d.key(win, "ctrl+f")
    dlg = d.wait_window(timeout=5, count=2)
    settle(d, dlg)
    d.key(dlg, "esc")
    time.sleep(0.3)
    check(not wait_line(app, since, r"^sknote: (found|notfound)", timeout=0.3), "Esc 取消查找")
    app.stop()


def test_toggles_and_shortcuts(d, args):
    home = new_home()
    diary = os.path.join(home, "文档", "日记.txt")
    write(diary, "今天天气很好。\n")
    path = os.path.join(home, "长行.txt")
    write(path, "".join("第 %d 行：%s\n" % (i, "一行很长很长的文字，用来看自动换行关掉以后能不能水平滚动。" * 2)
                        for i in range(1, 30)))
    app, win = start(d, args, home, path)
    wait_state(app, 0, lambda s: s["path"] == path)
    mark = len(app.lines)
    d.click(win, *CHK_NUMBERS)
    check(wait_state(app, mark, lambda s: s["numbers"] == 1) is not None, "点“行号”：显示行号")
    mark = len(app.lines)
    d.click(win, *CHK_WRAP)
    check(wait_state(app, mark, lambda s: s["wrap"] == 0) is not None, "点“自动换行”：关闭自动换行")
    settle(d, win)
    shot(d, win, args, "sknote-numbers-nowrap")
    d.click(win, *CHK_WRAP)
    settle(d, win)
    shot(d, win, args, "sknote-numbers-wrap")

    # Ctrl+N：有修改 → 先问；选“不保存” → 空的未命名文档
    d.type_text(win, "改动")
    wait_title(d, win, lambda t: t.startswith("*"))
    d.key(win, "ctrl+n")
    dlg = d.wait_window(timeout=5, count=2)
    settle(d, dlg)
    mark = len(app.lines)
    d.key(dlg, "tab")
    d.key(dlg, "tab")
    d.key(dlg, "enter")
    st = wait_state(app, mark, lambda s: s["path"] == "")
    check(st is not None and st["bytes"] == 0 and st["modified"] == 0, "Ctrl+N 新建空文档：%s" % st)
    check(wait_title(d, win, lambda t: t == "未命名 - 记事本"), "标题：未命名 - 记事本（%s）" % win.title)

    # Ctrl+O：未命名 → 对话框从 $HOME/文档 开始；选中第一个文件、回车
    d.key(win, "ctrl+o")
    fdlg = d.wait_window(timeout=5, count=2)
    settle(d, fdlg)
    check(fdlg.title == "打开", "Ctrl+O 打开文件对话框：%s" % fdlg.title)
    shot(d, fdlg, args, "sknote-open-dialog")
    mark = len(app.lines)
    d.key(fdlg, "down")
    d.key(fdlg, "enter")
    st = wait_state(app, mark, lambda s: s["path"] == diary)
    check(st is not None and st["bytes"] == len("今天天气很好。\n".encode()), "打开了 $HOME/文档/日记.txt")
    settle(d, win)
    check(win.title == "日记.txt - 记事本", "标题：%s" % win.title)

    # 改变窗口大小：重新布局，路径太长时省略开头
    d.resize(win, 690, 360)                    # 最小宽度 680：工具栏的按钮和开关正好放得下
    settle(d, win)
    shot(d, win, args, "sknote-small")
    app.stop()


if __name__ == "__main__":
    run_tests([
        ("open", test_open),
        ("save", test_type_and_save),
        ("newfile", test_new_file),
        ("close", test_close_dialog),
        ("saveas", test_save_as),
        ("errors", test_error_args),
        ("damage", test_damage),
        ("find", test_find),
        ("toggles", test_toggles_and_shortcuts),
    ])
