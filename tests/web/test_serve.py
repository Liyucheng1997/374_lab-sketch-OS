#!/usr/bin/env python3
"""
tools/serve.py 的自动化测试（只用标准库；WSL 的 python3 和 Windows 的 py -3 都能跑）：

    python3 tests/web/test_serve.py

启动一个临时服务器（端口由系统分配），检查：
  - 页面、JS、CSS 的 MIME 类型；.wasm 必须是 application/wasm，.mjs 必须是 JavaScript；
  - 系统镜像按二进制原样发送（没有 Content-Encoding，内容和磁盘上的文件逐字节相同），并且不缓存；
  - 其它文件是 no-cache，文件没变时能回 304；
  - 不列目录，不能用 ../ 读到网站目录外面的文件。
web/lib 或 web/images 还没构建时，相关项目标记为“跳过”。
"""
import hashlib
import http.client
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WEB = os.path.join(ROOT, "web")
results = {"pass": 0, "fail": 0, "skip": 0}


def check(name, ok, detail=""):
    results["pass" if ok else "fail"] += 1
    print("  %s %s%s" % ("通过" if ok else "失败", name, ("  (" + detail + ")") if detail and not ok else ""))


def skip(name, why):
    results["skip"] += 1
    print("  跳过 %s（%s）" % (name, why))


def request(port, method, path, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    conn.request(method, path, headers=headers or {})
    r = conn.getresponse()
    body = r.read()
    conn.close()
    return r.status, {k.lower(): v for k, v in r.getheaders()}, body


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except AttributeError:
            pass
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
    proc = subprocess.Popen([sys.executable, "-u", os.path.join(ROOT, "tools", "serve.py"),
                             "--port", "0", "--strict-port", "--quiet"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    port = None
    deadline = time.time() + 15
    output = []
    try:
        while time.time() < deadline:
            line = proc.stdout.readline().decode("utf-8", "replace")
            if not line:
                break
            output.append(line)
            m = re.search(r"http://127\.0\.0\.1:(\d+)/", line)
            if m:
                port = int(m.group(1))
                break
        if not port:
            print("服务器没有启动：\n" + "".join(output))
            return 1
        print("serve.py 监听在 127.0.0.1:%d" % port)

        st, h, body = request(port, "GET", "/")
        check("GET / 返回首页", st == 200 and b"SketchOS" in body, str(st))
        check("index.html 的 MIME", h.get("content-type", "").startswith("text/html"), h.get("content-type"))
        check("index.html 是 no-cache", h.get("cache-control") == "no-cache", h.get("cache-control"))
        st, h, _ = request(port, "GET", "/js/app.js")
        check("app.js 的 MIME", st == 200 and h.get("content-type", "").startswith("text/javascript"), h.get("content-type"))
        st, h, _ = request(port, "GET", "/css/sketch.css")
        check("sketch.css 的 MIME", st == 200 and h.get("content-type", "").startswith("text/css"), h.get("content-type"))

        lm = request(port, "HEAD", "/index.html")[1].get("last-modified")
        st, _, _ = request(port, "GET", "/index.html", {"If-Modified-Since": lm or ""})
        check("文件没变时回 304", st == 304, str(st))

        if os.path.isfile(os.path.join(WEB, "lib", "v86.wasm")):
            st, h, body = request(port, "GET", "/lib/v86.wasm")
            check("v86.wasm 的 MIME 是 application/wasm", st == 200 and h.get("content-type") == "application/wasm",
                  h.get("content-type"))
            check("v86.wasm 内容完整", body[:4] == b"\0asm" and len(body) == os.path.getsize(
                os.path.join(WEB, "lib", "v86.wasm")))
            st, h, _ = request(port, "HEAD", "/lib/libv86.mjs")
            check("libv86.mjs 的 MIME 是 JavaScript", st == 200 and h.get("content-type", "").startswith("text/javascript"),
                  h.get("content-type"))
            st, h, _ = request(port, "HEAD", "/lib/seabios.bin")
            check("seabios.bin 按二进制发送", st == 200 and h.get("content-type") == "application/octet-stream",
                  h.get("content-type"))
        else:
            skip("web/lib 检查", "还没运行 scripts/web.sh --lib-only")

        img = os.path.join(WEB, "images")
        if os.path.isfile(os.path.join(img, "initramfs.cpio.gz")) and os.path.isfile(os.path.join(img, "bzImage")):
            st, h, _ = request(port, "HEAD", "/images/bzImage")
            check("bzImage 按二进制发送", st == 200 and h.get("content-type") == "application/octet-stream",
                  h.get("content-type"))
            check("bzImage 不缓存", h.get("cache-control") == "no-store", h.get("cache-control"))
            check("bzImage 的 Content-Length", int(h.get("content-length", -1)) == os.path.getsize(
                os.path.join(img, "bzImage")))
            st, h, body = request(port, "GET", "/images/initramfs.cpio.gz", {"Accept-Encoding": "gzip, deflate, br"})
            disk = open(os.path.join(img, "initramfs.cpio.gz"), "rb").read()
            check("initramfs 没有 Content-Encoding（浏览器不会偷偷解压）", "content-encoding" not in h,
                  h.get("content-encoding", ""))
            check("initramfs 内容和磁盘上逐字节相同", hashlib.sha256(body).digest() == hashlib.sha256(disk).digest())
            st, h, body = request(port, "GET", "/images/manifest.json")
            check("manifest.json 是 JSON", st == 200 and h.get("content-type", "").startswith("application/json")
                  and body.strip().startswith(b"{"), h.get("content-type"))
        else:
            skip("web/images 检查", "还没运行 scripts/web.sh")

        st, _, _ = request(port, "GET", "/lib/")
        check("不列目录", st == 404, str(st))
        for evil in ("/../tools/serve.py", "/%2e%2e/tools/serve.py", "/..%2ftools/serve.py", "/js/../../tools/serve.py"):
            st, _, body = request(port, "GET", evil)
            check("拒绝越界路径 " + evil, st in (400, 403, 404) and b"SketchHandler" not in body, str(st))
        st, _, _ = request(port, "GET", "/no-such-file.txt")
        check("不存在的文件回 404", st == 404, str(st))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    print("serve.py 测试：%(pass)d 通过，%(fail)d 失败，%(skip)d 跳过" % results)
    return 1 if results["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())
