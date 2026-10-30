#!/usr/bin/env python3
"""
tests/ai/test_skaid.py — AI 服务 skaid 和命令行 ai 的自动测试（不需要 QEMU，也不需要真的 API 密钥）

在 WSL 里运行（32 位静态程序可以直接在 WSL 里跑）：
    python3 tests/ai/test_skaid.py [用户态程序目录]     默认 ~/sketchos-work/build/userspace/bin

怎么做到的：
  - skaid 平时通过 /dev/ttyS1 和网页上的 AI 桥说话；这里用环境变量 SKAID_BRIDGE 让它改用一个
    伪终端（pty），伪终端的另一头由这个脚本扮演“网页”：回应 HELLO，收下 REQ 请求体并检查它，
    再按剧本送回 Claude 风格的流式事件（EV）——就像真的 API 那样一段段到达。
  - SKAI_SOCKET 让 skaid 在临时目录里监听，SKAID_SHELL 指定宿主机上编译好的 sksh。
  - 然后运行真正的命令行客户端 ai（指向 skaid 的符号链接），检查它打印的内容和工具的效果。
"""
import json
import os
import pty
import shutil
import subprocess
import sys
import tempfile
import termios
import threading
import time
import tty

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/sketchos-work/build/userspace/bin")
SKAID = os.path.join(BIN, "skaid")
FAILS = []


def check(cond, msg):
    print(("  ✓ " if cond else "  ✗ ") + msg)
    if not cond:
        FAILS.append(msg)


def ev_text(index, text):
    return [
        {"type": "content_block_start", "index": index, "content_block": {"type": "text", "text": ""}},
        {"type": "content_block_delta", "index": index, "delta": {"type": "text_delta", "text": text}},
        {"type": "content_block_stop", "index": index},
    ]


def ev_tool(index, tid, name, args, split=12):
    raw = json.dumps(args, ensure_ascii=False)
    out = [{"type": "content_block_start", "index": index,
            "content_block": {"type": "tool_use", "id": tid, "name": name, "input": {}}}]
    for i in range(0, len(raw), split):          # 参数 JSON 切成碎片，像真的流式回应一样
        out.append({"type": "content_block_delta", "index": index,
                    "delta": {"type": "input_json_delta", "partial_json": raw[i:i + split]}})
    out.append({"type": "content_block_stop", "index": index})
    return out


def message(blocks, stop):
    return ([{"type": "message_start", "message": {"usage": {"input_tokens": 120, "output_tokens": 1}}}] + blocks +
            [{"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 42}},
             {"type": "message_stop"}])


class FakeBridge(threading.Thread):
    """扮演网页上的 AI 桥：按剧本回应每个请求。script 是函数列表，每个函数收到请求体，返回事件列表或 ("ERR", 消息)"""

    def __init__(self, master):
        super().__init__(daemon=True)
        self.fd = master
        self.buf = b""
        self.script = []
        self.requests = []
        self.cancels = []
        self.hellos = 0

    def _read(self):
        data = os.read(self.fd, 65536)
        if not data:
            raise EOFError
        self.buf += data

    def _line(self):
        while b"\n" not in self.buf:
            self._read()
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode()

    def _bytes(self, n):
        while len(self.buf) < n:
            self._read()
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def send(self, s):
        os.write(self.fd, s.encode())

    def run(self):
        try:
            while True:
                line = self._line()
                if line.startswith("HELLO"):
                    self.hellos += 1
                    self.send("READY 1 mock-opus\n")
                elif line.startswith("REQ "):
                    _, rid, n = line.split()
                    body = json.loads(self._bytes(int(n)).decode())
                    self._bytes(1)
                    self.requests.append(body)
                    step = self.script.pop(0) if self.script else (lambda b: message(ev_text(0, "（剧本用完了）"), "end_turn"))
                    res = step(body)
                    if isinstance(res, tuple):
                        self.send("ERR %s %s\n" % (rid, res[1]))
                        continue
                    for ev in res:
                        self.send("EV %s %s\n" % (rid, json.dumps(ev, ensure_ascii=False)))
                    self.send("END %s\n" % rid)
                elif line.startswith("CANCEL "):
                    self.cancels.append(line)
        except (EOFError, OSError):
            pass


def start_skaid(tmp, bridge_dev):
    env = dict(os.environ, SKAI_SOCKET=os.path.join(tmp, "ai.sock"), SKAID_BRIDGE=bridge_dev,
               SKAID_SHELL=os.path.join(BIN, "sksh"), TERM="dumb")
    log = open(os.path.join(tmp, "skaid.log"), "w")
    p = subprocess.Popen([SKAID, "-d"], env=env, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(50):
        if os.path.exists(env["SKAI_SOCKET"]):
            break
        time.sleep(0.05)
    return p, env


def run_ai(tmp, env, args, stdin="", timeout=30):
    ai = os.path.join(tmp, "ai")
    if not os.path.exists(ai):
        os.symlink(SKAID, ai)
    return subprocess.run([ai] + args, env=env, input=stdin, capture_output=True, text=True, timeout=timeout)


def main():
    if not os.access(SKAID, os.X_OK):
        print("找不到 %s，请先运行 scripts/userspace.sh" % SKAID)
        return 1
    tmp = tempfile.mkdtemp(prefix="skaid-test-")
    master, slave = pty.openpty()
    tty.setraw(master)
    attrs = termios.tcgetattr(master)
    bridge = FakeBridge(master)
    bridge.start()
    skaid, env = start_skaid(tmp, os.ttyname(slave))
    work = os.path.join(tmp, "work")
    os.mkdir(work)
    try:
        print("1. 状态：网页回应了 READY")
        time.sleep(0.3)
        r = run_ai(tmp, env, ["--status"])
        check(r.returncode == 0 and "ready" in r.stdout and "mock-opus" in r.stdout, "ai --status 显示 ready 和模型名：" + r.stdout.strip().replace("\n", " / "))
        check(bridge.hellos >= 1, "skaid 启动时向桥打了招呼")

        print("2. 一次完整的智能体循环：思考 → 文字 → 调用 run_command → 把结果交回 → 回答")
        bridge.script = [
            lambda b: message(
                [{"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": "", "signature": ""}},
                 {"type": "content_block_delta", "index": 0, "delta": {"type": "thinking_delta", "thinking": "想一想"}},
                 {"type": "content_block_delta", "index": 0, "delta": {"type": "signature_delta", "signature": "c2lnLTEyMw=="}},
                 {"type": "content_block_stop", "index": 0}]
                + ev_text(1, "我来运行命令。")
                + ev_tool(2, "toolu_01", "run_command", {"command": "echo 你好 && echo $((1+2))"}), "tool_use"),
            lambda b: message(ev_text(0, "完成啦 \U0001F600 结果是 3。"), "end_turn"),
        ]
        r = run_ai(tmp, env, ["测试一下"])
        check(r.returncode == 0, "ai 退出码 0（实际 %d，stderr=%r）" % (r.returncode, r.stderr[-300:]))
        check("我来运行命令。" in r.stdout and "完成啦" in r.stdout, "流式文字都打印出来了")
        check("\U0001F600" in r.stdout, "JSON 里的代理对（emoji）正确转成了 UTF-8")
        check("运行命令：echo 你好" in r.stderr, "显示了工具调用过程")
        check(len(bridge.requests) == 2, "一共请求了 2 次模型（实际 %d 次）" % len(bridge.requests))
        if len(bridge.requests) == 2:
            b1, b2 = bridge.requests
            check("model" not in b1, "请求体里不写 model（由网页填）")
            check(b1.get("stream") is True and b1.get("max_tokens", 0) > 1000, "stream=true，max_tokens 足够大")
            names = [t["name"] for t in b1.get("tools", [])]
            check({"run_command", "read_file", "write_file", "edit_file", "list_directory", "open_app"} <= set(names),
                  "工具齐全：" + ",".join(names))
            check("SketchOS" in b1.get("system", ""), "系统提示词介绍了 SketchOS")
            m = b2["messages"]
            check([x["role"] for x in m] == ["user", "assistant", "user"], "第二次请求的历史是 user/assistant/user")
            a = m[1]["content"]
            check(a[0] == {"type": "thinking", "thinking": "想一想", "signature": "c2lnLTEyMw=="},
                  "thinking 块连同签名原样交回：%r" % (a[0],))
            check(a[2].get("input") == {"command": "echo 你好 && echo $((1+2))"}, "工具参数由碎片拼成了 JSON 对象")
            tr = m[2]["content"][0]
            check(tr["type"] == "tool_result" and tr["tool_use_id"] == "toolu_01", "tool_result 对上了 tool_use 的 id")
            check("你好" in tr["content"] and "3" in tr["content"] and "退出码 0" in tr["content"],
                  "命令真的执行了：%r" % tr["content"][:80])

        print("3. 一条回答里并行调用多个工具：write_file + edit_file，再 read_file")
        path = os.path.join(work, "笔记", "a.txt")
        bridge.script = [
            lambda b: message(ev_tool(0, "toolu_w", "write_file", {"path": path, "content": "第一行\n第二行\n"}) +
                              ev_tool(1, "toolu_e", "edit_file", {"path": path, "old_text": "第二行", "new_text": "第 2 行"}),
                              "tool_use"),
            lambda b: message(ev_tool(0, "toolu_r", "read_file", {"path": path}) +
                              ev_tool(1, "toolu_l", "list_directory", {"path": os.path.dirname(path)}), "tool_use"),
            lambda b: message(ev_text(0, "写好了。"), "end_turn"),
        ]
        bridge.requests.clear()
        r = run_ai(tmp, env, ["-q", "写个文件"])
        check(r.returncode == 0 and "写好了" in r.stdout, "回答完成")
        check(r.stderr.strip() == "", "-q 不显示工具过程")
        try:
            content = open(path, encoding="utf-8").read()
        except OSError as e:
            content = str(e)
        check(content == "第一行\n第 2 行\n", "文件内容正确（自动建了上级目录）：%r" % content)
        if len(bridge.requests) == 3:
            res = bridge.requests[1]["messages"][-1]["content"]
            check(len(res) == 2 and all(x["type"] == "tool_result" for x in res), "两个工具的结果放在同一条 user 消息里")
            res = bridge.requests[2]["messages"][-1]["content"]
            check("第 2 行" in res[0]["content"] and "a.txt" in res[1]["content"], "read_file / list_directory 的结果正确")
        else:
            check(False, "应该请求 3 次（实际 %d 次）" % len(bridge.requests))

        print("4. 出错的情况")
        bridge.script = [lambda b: ("ERR", "API 返回 401：密钥无效")]
        r = run_ai(tmp, env, ["你好"])
        check(r.returncode == 1 and "密钥无效" in r.stderr, "网页报告的错误显示出来，退出码 1")
        bridge.script = [lambda b: message(ev_tool(0, "toolu_x", "open_app", {"app": "不存在的应用"}) +
                                           ev_tool(1, "toolu_y", "run_command", {"command": "skterm"}), "tool_use"),
                         lambda b: message(ev_text(0, "好的。"), "end_turn")]
        bridge.requests.clear()
        r = run_ai(tmp, env, ["打开一个应用"])
        if len(bridge.requests) == 2:
            res = bridge.requests[1]["messages"][-1]["content"]
            check(res[0].get("is_error") is True, "不存在的应用 → is_error")
            check(res[1].get("is_error") is True and "open_app" in res[1]["content"], "用 run_command 启动图形应用会被拦下")
        else:
            check(False, "应该请求 2 次（实际 %d 次）" % len(bridge.requests))
        # 回答被截断（max_tokens）时，没写完的工具调用也要有 tool_result，下一次提问才不会被 API 拒绝
        bridge.script = [lambda b: message(ev_text(0, "我要写一个很长的文件") +
                                           [{"type": "content_block_start", "index": 1, "content_block":
                                             {"type": "tool_use", "id": "toolu_t", "name": "write_file", "input": {}}},
                                            {"type": "content_block_delta", "index": 1,
                                             "delta": {"type": "input_json_delta", "partial_json": "{\"path\": \"x"}},
                                            {"type": "content_block_stop", "index": 1}], "max_tokens")]
        bridge.requests.clear()
        r = run_ai(tmp, env, ["写长文件"])
        check(r.returncode == 1 and "截断" in r.stderr, "max_tokens：提示回答被截断")

        print("5. 管道：把标准输入附在问题后面")
        bridge.script = [lambda b: message(ev_text(0, "收到"), "end_turn")]
        bridge.requests.clear()
        r = run_ai(tmp, env, ["总结一下"], stdin="第一行内容\n第二行内容\n")
        q = bridge.requests[0]["messages"][-1]["content"][-1]["text"] if bridge.requests else ""
        check(q.startswith("总结一下") and "第二行内容" in q, "问题里带上了管道内容")

        print("6. 对话模式（多轮）和 /new")
        bridge.script = [lambda b: message(ev_text(0, "第一次回答"), "end_turn"),
                         lambda b: message(ev_text(0, "第二次回答"), "end_turn"),
                         lambda b: message(ev_text(0, "新对话"), "end_turn")]
        bridge.requests.clear()
        # 标准输入不是终端时 ai 走一次性模式，所以给它一个伪终端当标准输入
        m, s = pty.openpty()
        p = subprocess.Popen([os.path.join(tmp, "ai")], stdin=s, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        os.close(s)
        os.write(m, "问题一\n问题二\n/new\n问题三\n/exit\n".encode())
        out, _ = p.communicate(timeout=30)
        os.close(m)
        lens = [len(b["messages"]) for b in bridge.requests]
        check(lens == [1, 3, 1], "第二个问题带着前面的历史，/new 之后从头开始：%r" % lens)
        check("第二次回答" in out.decode(errors="replace"), "对话模式打印了回答")

        print("7. 没有 AI 桥（比如 QEMU 里）")
        skaid2, env2 = start_skaid(tempfile.mkdtemp(prefix="skaid-nobridge-"), "/nonexistent/ttyS9")
        time.sleep(0.3)
        r = run_ai(tmp, env2, ["--status"])
        check(r.returncode == 1 and "nobridge" in r.stdout and "网页版" in r.stdout, "状态是 nobridge，并说明要在网页版里用")
        skaid2.terminate()
    finally:
        skaid.terminate()
        skaid.wait(timeout=5)
    log = open(os.path.join(tmp, "skaid.log"), encoding="utf-8", errors="replace").read()
    if FAILS:
        print("\nskaid 的日志：\n" + log[-3000:])
        print("\n失败 %d 项" % len(FAILS))
        return 1
    print("\n全部通过")
    shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
