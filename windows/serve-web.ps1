<#
.SYNOPSIS
    启动 SketchOS 网页版：在本机开一个 HTTP 服务器，并用默认浏览器打开页面。

.DESCRIPTION
    双击 windows\启动网页版.bat 时运行的就是这个脚本。
    浏览器不允许 file:// 打开的页面读取本地文件，而 v86 模拟器需要下载内核和系统镜像，
    所以要用一个本地 HTTP 服务器（tools\serve.py，只用 Python 标准库）。
    电脑上没有 Python 时，退而使用 Node.js 跑一个内置的小服务器。

.PARAMETER Port
    端口，默认 8086（被占用时自动换下一个）。

.PARAMETER Build
    先在 WSL 里运行 scripts/web.sh，把最新的内核和 initramfs 复制进 web\。

.PARAMETER NoBrowser
    只启动服务器，不打开浏览器。

.PARAMETER Distro
    WSL 发行版名称，默认 Ubuntu。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File windows\serve-web.ps1 -Build
#>
param(
    [int]$Port = 8086,
    [switch]$Build,
    [switch]$NoBrowser,
    [string]$Distro = "Ubuntu"
)

try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$Root = Split-Path -Parent $PSScriptRoot
$Web = Join-Path $Root "web"

function Say([string]$Message, [string]$Color = "Cyan") {
    Write-Host "[SketchOS] $Message" -ForegroundColor $Color
}

if (-not (Test-Path -LiteralPath (Join-Path $Web "index.html"))) {
    Say "找不到 $Web\index.html，这个脚本应该放在项目的 windows\ 目录里运行。" Red
    exit 1
}

# ---------- 可选：先在 WSL 里构建网页资源 ----------
if ($Build) {
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        Say "没有找到 WSL，没法构建。请先安装 WSL 和 Ubuntu。" Red
        exit 1
    }
    Say "在 WSL（$Distro）里运行 scripts/web.sh ……"
    # --cd 接受 Windows 路径，路径里有中文也没问题
    & wsl.exe -d $Distro --cd "$Root" -- bash scripts/web.sh
    if ($LASTEXITCODE -ne 0) {
        Say "构建失败，请看上面的输出。" Red
        exit 1
    }
}

# ---------- 检查构建产物 ----------
$missing = @()
foreach ($f in @("lib\libv86.js", "lib\v86.wasm", "lib\seabios.bin", "lib\vgabios.bin", "images\bzImage", "images\initramfs.cpio.gz")) {
    if (-not (Test-Path -LiteralPath (Join-Path $Web $f))) { $missing += $f }
}
if ($missing.Count -gt 0) {
    Say ("web\ 里还缺少：" + ($missing -join "、")) Yellow
    Say "请先在 WSL 里运行 bash scripts/web.sh，或者运行：windows\启动网页版.bat -Build" Yellow
    Say "（网页照样会打开，页面上也会显示同样的提示。）" Yellow
}

# ---------- 找一个能用的 Python 3 ----------
function Find-Python {
    $candidates = @(
        @{ Name = "py"; Args = @("-3") },
        @{ Name = "python"; Args = @() },
        @{ Name = "python3"; Args = @() }
    )
    foreach ($c in $candidates) {
        $cmd = Get-Command $c.Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $cmd) { continue }
        # 应用商店的 python.exe 只是个“占位程序”，运行它只会提示去商店安装；真正跑一下才知道能不能用
        try {
            $probeArgs = $c.Args + @("-c", "import sys; print(sys.version_info[0])")
            $out = & $cmd.Source @probeArgs 2>$null
            if ($LASTEXITCODE -eq 0 -and ("$out").Trim() -eq "3") {
                return @{ Exe = $cmd.Source; Args = $c.Args }
            }
        } catch { }
    }
    return $null
}

$python = Find-Python
if ($python) {
    Say "用 Python 启动服务器：$($python.Exe)"
    $serverArgs = $python.Args + @((Join-Path $Root "tools\serve.py"), "--port", "$Port", "--dir", $Web)
    if (-not $NoBrowser) { $serverArgs += "--open" }
    & $python.Exe @serverArgs
    exit $LASTEXITCODE
}

# ---------- 没有 Python：用 Node.js 跑一个同样功能的小服务器 ----------
$node = Get-Command node -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $node) {
    Say "没有找到 Python 3，也没有找到 Node.js。请安装其中一个（https://www.python.org/ 安装时勾选 Add to PATH）。" Red
    exit 1
}

$nodeServer = @'
// SketchOS 网页版的备用服务器（电脑上没有 Python 时由 windows\serve-web.ps1 生成并运行）
// 和 tools/serve.py 一样：正确的 MIME 类型、images/ 不缓存、只监听本机。
"use strict";
const http = require("http");
const fs = require("fs");
const path = require("path");
const { exec } = require("child_process");

const root = path.resolve(process.argv[2]);
let port = parseInt(process.argv[3] || "8086", 10);
const openBrowser = process.argv[4] === "open";
const MIME = {
    ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8", ".txt": "text/plain; charset=utf-8",
    ".md": "text/plain; charset=utf-8", ".svg": "image/svg+xml", ".png": "image/png",
    ".ico": "image/x-icon", ".wasm": "application/wasm", ".bin": "application/octet-stream",
    ".gz": "application/octet-stream",
};

const server = http.createServer((req, res) => {
    if (req.method !== "GET" && req.method !== "HEAD") {
        res.writeHead(405, { Allow: "GET, HEAD" });
        return res.end();
    }
    let urlPath;
    try {
        urlPath = decodeURIComponent(new URL(req.url, "http://localhost").pathname);
    } catch (e) {
        res.writeHead(400);
        return res.end();
    }
    if (urlPath.endsWith("/")) urlPath += "index.html";
    const file = path.join(root, path.normalize(urlPath));
    if (!file.startsWith(root + path.sep)) {          // 不许用 ../ 跳出网站目录
        res.writeHead(403);
        return res.end();
    }
    fs.stat(file, (err, st) => {
        if (err || !st.isFile()) {
            res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
            return res.end("404 Not Found");
        }
        const noStore = urlPath.startsWith("/images/");
        const headers = {
            "Content-Type": MIME[path.extname(file).toLowerCase()] || "application/octet-stream",
            "Cache-Control": noStore ? "no-store" : "no-cache",
            "Last-Modified": st.mtime.toUTCString(),
            "X-Content-Type-Options": "nosniff",
        };
        const since = Date.parse(req.headers["if-modified-since"] || "");
        if (!noStore && since && Math.floor(st.mtimeMs / 1000) <= Math.floor(since / 1000)) {
            res.writeHead(304, headers);
            return res.end();
        }
        headers["Content-Length"] = st.size;
        res.writeHead(200, headers);
        if (req.method === "HEAD") return res.end();
        fs.createReadStream(file).pipe(res);
    });
});

let tries = 0;
function listen() {
    server.once("error", (e) => {
        if (e.code === "EADDRINUSE" && tries++ < 20) {
            port++;
            listen();
        } else {
            console.error("无法启动服务器: " + e.message);
            process.exit(1);
        }
    });
    server.listen(port, "127.0.0.1", () => {
        const url = `http://127.0.0.1:${port}/`;
        console.log("SketchOS 网页版已启动：" + url);
        console.log("网站目录：" + root);
        console.log("按 Ctrl+C 停止服务器。");
        if (openBrowser) exec(`start "" "${url}"`);
    });
}
listen();
'@

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "sketchos-serve-web.cjs"
[System.IO.File]::WriteAllText($tmp, $nodeServer, (New-Object System.Text.UTF8Encoding($false)))
Say "没有找到 Python，改用 Node.js：$($node.Source)"
$openFlag = if ($NoBrowser) { "no" } else { "open" }
& $node.Source $tmp $Web "$Port" $openFlag
exit $LASTEXITCODE
