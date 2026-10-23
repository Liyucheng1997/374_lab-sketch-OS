#!/usr/bin/env python3
"""
SketchOS 网页版的本地 HTTP 服务器（只用 Python 标准库，Windows / WSL / Linux 都能用）。

    python3 tools/serve.py [--port 8086] [--host 127.0.0.1] [--dir web] [--open]

    --port   端口，默认 8086；被占用时自动往后找一个空闲端口（--strict-port 则直接报错）
    --host   监听地址，默认只监听本机 127.0.0.1（想让局域网里的手机访问，用 --host 0.0.0.0）
    --dir    网站根目录，默认仓库里的 web/
    --open   启动后用默认浏览器打开页面
    --quiet  不打印每个请求

为什么不能直接双击 index.html？浏览器出于安全考虑，不允许 file:// 打开的页面用 fetch/XHR
读取本地文件，而 v86 要下载 v86.wasm、BIOS 和系统镜像，所以需要一个真正的 HTTP 服务器。

和 Python 自带的 `python -m http.server` 相比，这里额外保证了：
  - 正确的 MIME 类型：.wasm 必须是 application/wasm（浏览器才能边下载边编译），.mjs 是 JavaScript；
    镜像文件按二进制发送，并且绝不带 Content-Encoding（否则浏览器会偷偷把 initramfs 解压掉）。
  - images/ 下的系统镜像不缓存（重新构建后刷新页面就能拿到新系统），其它文件每次都向服务器确认是否有更新。
  - Windows 上不会出现两个服务器抢同一个端口的情况。
"""
import argparse
import functools
import http.server
import os
import socket
import sys
import threading
import webbrowser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PORT = 8086

MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".htm": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".map": "application/json; charset=utf-8",
    ".txt": "text/plain; charset=utf-8",
    ".md": "text/plain; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".ico": "image/x-icon",
    ".wasm": "application/wasm",
    ".woff2": "font/woff2",
    ".bin": "application/octet-stream",
    ".gz": "application/octet-stream",
    ".cpio": "application/octet-stream",
}


class SketchHandler(http.server.SimpleHTTPRequestHandler):
    """静态文件处理器：在标准库实现的基础上改 MIME 类型、缓存策略，并禁止列目录"""

    server_version = "SketchOS-serve/1.0"
    quiet = False

    def guess_type(self, path):
        ext = os.path.splitext(path)[1].lower()
        # 没有扩展名的文件（比如 bzImage）一律按二进制发送
        return MIME_TYPES.get(ext, "application/octet-stream")

    def end_headers(self):
        url_path = self.path.split("?", 1)[0].split("#", 1)[0]
        if url_path.startswith("/images/"):
            # 系统镜像每次构建都会变，不许浏览器缓存
            self.send_header("Cache-Control", "no-store")
        else:
            # 可以缓存，但每次使用前都要向服务器确认（文件没变时服务器回 304，很快）
            self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()

    def list_directory(self, path):
        self.send_error(404, "Directory listing is disabled")
        return None

    def log_message(self, fmt, *args):
        if not self.quiet:
            sys.stderr.write("[serve] %s %s\n" % (self.address_string(), fmt % args))


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    # Windows 上的 SO_REUSEADDR 含义和 Linux 不同：它允许两个程序同时绑定同一个端口，
    # 结果是新服务器“启动成功”却收不到请求。所以 Windows 上关掉它，端口被占用时才能正确报错。
    allow_reuse_address = os.name != "nt"
    allow_reuse_port = False


def make_server(host, port, directory, strict_port=False, tries=20):
    handler = functools.partial(SketchHandler, directory=directory)
    last_error = None
    for p in ([port] if strict_port or port == 0 else range(port, port + tries)):
        try:
            return Server((host, p), handler)
        except OSError as e:
            last_error = e
    raise OSError("无法监听 %s:%d（%s）" % (host, port, last_error))


def check_assets(directory):
    """提前检查构建产物，缺了就在终端里提示（网页上也会有提示）"""
    missing = [f for f in ("lib/libv86.js", "lib/v86.wasm", "lib/seabios.bin", "lib/vgabios.bin",
                           "images/bzImage", "images/initramfs.cpio.gz")
               if not os.path.isfile(os.path.join(directory, f))]
    if missing:
        print("注意：web/ 里还缺少这些构建产物：" + "、".join(missing))
        print("      请先在 WSL 里运行：bash scripts/web.sh（需要先编好内核和 initramfs）")
    return not missing


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")   # 老式 Windows 控制台编码打不出中文时不至于崩溃
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description="SketchOS 网页版本地服务器")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--strict-port", action="store_true", help="端口被占用时直接报错，不自动换端口")
    ap.add_argument("--dir", default=os.path.join(ROOT, "web"))
    ap.add_argument("--open", action="store_true", help="启动后打开浏览器")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    directory = os.path.abspath(args.dir)
    if not os.path.isfile(os.path.join(directory, "index.html")):
        print("错误：%s 里没有 index.html" % directory, file=sys.stderr)
        return 2
    SketchHandler.quiet = args.quiet
    try:
        httpd = make_server(args.host, args.port, directory, args.strict_port)
    except OSError as e:
        print("错误：%s" % e, file=sys.stderr)
        return 1

    host, port = httpd.server_address[:2]
    shown = "127.0.0.1" if host in ("0.0.0.0", "") else host
    if ":" in shown:
        shown = "[%s]" % shown
    url = "http://%s:%d/" % (shown, port)
    check_assets(directory)
    print("SketchOS 网页版已启动：%s" % url)
    print("网站目录：%s" % directory)
    if host in ("0.0.0.0", "::"):
        print("局域网里的其它设备可以用本机 IP 访问：http://%s:%d/" % (local_ip(), port))
    print("按 Ctrl+C 停止服务器。", flush=True)
    if args.open:
        threading.Timer(0.3, webbrowser.open, args=(url,)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止。")
    finally:
        httpd.server_close()
    return 0


def local_ip():
    """找出本机的局域网地址（UDP connect 不会真的发包，只是让系统选一块网卡）"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    except OSError:
        return "本机IP"
    finally:
        s.close()


if __name__ == "__main__":
    sys.exit(main())
