#!/usr/bin/env python3
"""
tests/apps/test_skai.py — 图形版 AI 助手 skai 的测试

skai 本身只是 AI 服务 skaid 的前台。这里把三样东西接在一起：
  模拟显示服务器（mock_display）←→ skai ←→ 真正的 skaid ←→ 假的网页 AI 桥（tests/ai/test_skaid.py 的 FakeBridge）
所以测到的是完整的链路：在输入框里打字、回车 → skaid 发请求 → 假桥按剧本回答、调用工具 → 文字出现在窗口里。
"""
import json
import pty
import subprocess
import sys
import tty

from apptest import check, App, settle, shot, run_tests, os, time, HERE

sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ai"))
from test_skaid import FakeBridge, message, ev_text, ev_tool  # noqa: E402


class Service:
    """启动 skaid（AI 服务）和假的 AI 桥"""

    def __init__(self, args, ready=True):
        self.tmp = "/tmp/skai-test-%d" % os.getpid()
        os.makedirs(self.tmp, exist_ok=True)
        self.sock = os.path.join(self.tmp, "ai.sock")
        if os.path.exists(self.sock):
            os.unlink(self.sock)
        master, self.slave = pty.openpty()
        tty.setraw(master)
        self.bridge = FakeBridge(master)
        if not ready:                      # 网页还没填密钥：回应 READY 0
            orig = self.bridge.send
            self.bridge.send = lambda s: orig(s.replace("READY 1", "READY 0"))
        self.bridge.start()
        env = dict(os.environ, SKAI_SOCKET=self.sock, SKAID_BRIDGE=os.ttyname(self.slave),
                   SKAID_SHELL=os.path.join(args.bin, "sksh"))
        self.p = subprocess.Popen([os.path.join(args.bin, "skaid"), "-d"], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(self.sock):
                break
            time.sleep(0.05)
        time.sleep(0.2)

    def stop(self):
        self.p.terminate()
        self.p.wait(timeout=5)


def start(d, args, svc, argv=()):
    app = App(d, [os.path.join(args.bin, "skai")] + list(argv), env={"SKAI_SOCKET": svc.sock, "SKAI_DEBUG": "1"})
    win = d.wait_window(timeout=10, title="AI 助手")
    settle(d, win)
    return app, win


def wait_for(app, pattern, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(__import__("re").search(pattern, l) for l in list(app.lines)):
            return True
        time.sleep(0.05)
    return False


def test_chat(d, args):
    svc = Service(args)
    try:
        app, win = start(d, args, svc)
        check(wait_for(app, r"skai: status ready mock-opus"), "连上 AI 服务，状态 ready")
        shot(d, win, args, "skai-welcome")
        svc.bridge.script = [
            lambda b: message(ev_text(0, "好的，我先看看系统信息。") +
                              ev_tool(1, "toolu_1", "run_command", {"command": "echo 内核是 Linux"}), "tool_use"),
            lambda b: message(ev_text(0, "看完了：这台电脑运行的是 Linux 内核。还有什么要我做的吗？"), "end_turn"),
        ]
        d.type_text(win, "hello")
        d.key(win, "enter")
        check(wait_for(app, r"skai: send hello"), "回车把问题发出去了")
        check(wait_for(app, r"skai: done end_turn"), "回答结束")
        settle(d, win)
        check(len(svc.bridge.requests) == 2, "请求了 2 次模型（中间执行了一次工具）")
        if len(svc.bridge.requests) == 2:
            tr = svc.bridge.requests[1]["messages"][-1]["content"][0]
            check("内核是 Linux" in tr.get("content", ""), "工具真的运行了")
        shot(d, win, args, "skai-answer")
        app.stop()
    finally:
        svc.stop()


def test_argument(d, args):
    """skai "问题"：连上就绪后自动提问"""
    svc = Service(args)
    try:
        svc.bridge.script = [lambda b: message(ev_text(0, "收到命令行的问题"), "end_turn")]
        app, win = start(d, args, svc, ["你好", "世界"])
        check(wait_for(app, r"skai: send 你好 世界"), "命令行参数拼成问题自动发送")
        check(wait_for(app, r"skai: done end_turn"), "回答结束")
        app.stop()
    finally:
        svc.stop()


def test_nokey(d, args):
    svc = Service(args, ready=False)
    try:
        app, win = start(d, args, svc)
        check(wait_for(app, r"skai: status nokey"), "网页没有密钥时状态是 nokey")
        settle(d, win)
        shot(d, win, args, "skai-nokey")
        app.stop()
    finally:
        svc.stop()


def test_no_service(d, args):
    """skaid 没有运行：窗口照样打开，显示错误并定时重试"""
    app = App(d, [os.path.join(args.bin, "skai")], env={"SKAI_SOCKET": "/tmp/不存在的/ai.sock", "SKAI_DEBUG": "1"})
    win = d.wait_window(timeout=10, title="AI 助手")
    settle(d, win)
    check(app.alive(), "没有 AI 服务时 skai 不会退出")
    shot(d, win, args, "skai-noservice")
    app.stop()


if __name__ == "__main__":
    run_tests([("chat", test_chat), ("argument", test_argument), ("nokey", test_nokey),
               ("no_service", test_no_service)])
