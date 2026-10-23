<#
.SYNOPSIS
    在 Windows 上用 QEMU 运行 SketchOS（手绘操作系统）。

.DESCRIPTION
    启动一台 QEMU 虚拟机：图形桌面显示在 QEMU 窗口里，串口 shell 显示在这个控制台窗口里。
    需要先在 WSL 里构建好系统（bash scripts/build.sh），产物在 out\bzImage 和 out\initramfs.cpio.gz。

    找 QEMU 的顺序：-Qemu 参数 → PATH → C:\Program Files\qemu → 其它常见安装位置。
    找不到时会说明安装方法，并提供改用 WSL 里的 QEMU 运行的选项。

.PARAMETER NoGui
    纯文字模式：内核命令行加 sketch.nogui，不开窗口，shell 就在这个控制台里。
.PARAMETER Mem
    内存大小（MB），默认 512。
.PARAMETER Mode
    桌面分辨率，默认 1024x768。
.PARAMETER Append
    追加的内核参数，例如 "sketch.tz=JST-9"。
.PARAMETER Qemu
    qemu-system-i386.exe 的完整路径（不在常见位置时使用）。
.PARAMETER Accel
    加速方式：tcg（纯软件，默认，最稳妥）或 whpx（Windows 虚拟机平台，需要在“启用或关闭 Windows 功能”里打开）。
.PARAMETER Wsl
    不找 Windows 版 QEMU，直接用 WSL 里的 QEMU 运行。
.PARAMETER Distro
    WSL 发行版名称，默认 Ubuntu。
.PARAMETER DryRun
    只打印将要执行的命令，不真正运行（用来检查脚本）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File windows\run-qemu.ps1
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File windows\run-qemu.ps1 -NoGui
#>
[CmdletBinding()]
param(
    [switch]$NoGui,
    [int]$Mem = 512,
    [string]$Mode = "1024x768",
    [string]$Append = "",
    [string]$Qemu = "",
    [ValidateSet("tcg", "whpx")]
    [string]$Accel = "tcg",
    [switch]$Wsl,
    [string]$Distro = "Ubuntu",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

# QEMU 把串口收到的字节原样写到控制台；系统里的中文是 UTF-8，所以控制台也要按 UTF-8 显示
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$Root = Split-Path -Parent $PSScriptRoot        # windows\ 的上一级就是项目根目录
if ($Root.Length -gt 3) { $Root = $Root.TrimEnd('\') }
$OutDir = Join-Path $Root "out"
$Kernel = Join-Path $OutDir "bzImage"
$Initrd = Join-Path $OutDir "initramfs.cpio.gz"

function Write-Step([string]$msg) { Write-Host "[sketchos] $msg" -ForegroundColor Cyan }
function Write-Warn([string]$msg) { Write-Host "[sketchos] $msg" -ForegroundColor Yellow }

function Ask-YesNo([string]$question) {
    if ($DryRun) { return $false }
    try {
        $answer = Read-Host "$question [Y/n]"
    } catch {
        return $false          # 没有交互式控制台（比如被别的程序调用）
    }
    return ($answer -eq "" -or $answer -match "^(y|yes|是|好)$")
}

function Find-Qemu {
    if ($Qemu) {
        if (Test-Path -LiteralPath $Qemu) { return (Resolve-Path -LiteralPath $Qemu).Path }
        Write-Warn "-Qemu 指定的文件不存在：$Qemu"
    }
    $cmd = Get-Command "qemu-system-i386.exe" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $env:ProgramFiles "qemu\qemu-system-i386.exe"),
        "C:\Program Files\qemu\qemu-system-i386.exe"
    )
    if (${env:ProgramFiles(x86)}) { $candidates += (Join-Path ${env:ProgramFiles(x86)} "qemu\qemu-system-i386.exe") }
    if ($env:LOCALAPPDATA) { $candidates += (Join-Path $env:LOCALAPPDATA "Programs\qemu\qemu-system-i386.exe") }
    if ($env:USERPROFILE) { $candidates += (Join-Path $env:USERPROFILE "scoop\apps\qemu\current\qemu-system-i386.exe") }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    return $null
}

# 把参数列表拼成一条命令行字符串（含空格的参数加引号），给 Start-Process 用
function ConvertTo-ArgString([string[]]$list) {
    $quoted = foreach ($a in $list) {
        if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a }
    }
    return ($quoted -join " ")
}

# 在 WSL 里运行项目里的一个脚本，返回退出码。
# 用 Start-Process -NoNewWindow 让 wsl 直接使用当前控制台（键盘输入、Ctrl+A X 都正常），
# 而不是把它的输出收集进 PowerShell 的管道。
function Invoke-Wsl([string[]]$scriptArgs, [string]$what) {
    $wslExe = Get-Command "wsl.exe" -ErrorAction SilentlyContinue
    if (-not $wslExe) {
        Write-Warn "也没有找到 WSL（wsl.exe）。请先安装 QEMU 或 WSL。"
        return 1
    }
    # --cd 接受 Windows 路径，WSL 会自动换算成 /mnt/... 路径（中文路径也没问题）
    $wslArgs = @("-d", $Distro, "--cd", $Root, "--", "bash") + $scriptArgs
    $argString = ConvertTo-ArgString $wslArgs
    Write-Step "${what}：wsl $argString"
    if ($DryRun) { return 0 }
    $p = Start-Process -FilePath $wslExe.Source -ArgumentList $argString -NoNewWindow -Wait -PassThru
    return $p.ExitCode
}

function Get-WslRunArgs {
    $a = @("scripts/run-qemu.sh", "--mem", "$Mem", "--mode", $Mode)
    if ($NoGui) { $a += "--nogui" }
    if ($Append) { $a += @("--append", $Append) }
    return $a
}

# ---------------- 用 WSL 里的 QEMU ----------------
if ($Wsl) {
    exit (Invoke-Wsl (Get-WslRunArgs) "用 WSL 运行")
}

# ---------------- 系统镜像 ----------------
if (-not ((Test-Path -LiteralPath $Kernel) -and (Test-Path -LiteralPath $Initrd))) {
    Write-Warn "还没有构建系统镜像（找不到 out\bzImage 或 out\initramfs.cpio.gz）。"
    Write-Host "  构建方法：在 WSL Ubuntu 里进入项目目录，运行  bash scripts/build.sh"
    if ($DryRun -or (Ask-YesNo "现在用 WSL 构建吗？（第一次需要较长时间）")) {
        $code = Invoke-Wsl @("scripts/build.sh") "构建"
        if ($code -ne 0) { exit $code }
    }
    if (-not $DryRun -and -not ((Test-Path -LiteralPath $Kernel) -and (Test-Path -LiteralPath $Initrd))) {
        exit 1
    }
}

# ---------------- 找 QEMU ----------------
$QemuExe = Find-Qemu
if (-not $QemuExe) {
    Write-Warn "没有找到 Windows 版的 QEMU（qemu-system-i386.exe）。"
    Write-Host ""
    Write-Host "  安装方法（任选一种），装好后重新运行本脚本："
    Write-Host "    1. 在 PowerShell 里运行：  winget install SoftwareFreedomConservancy.QEMU"
    Write-Host "    2. 从 https://qemu.weilnetz.de/w64/ 下载安装包，装到默认位置 C:\Program Files\qemu"
    Write-Host "    3. 装在别的地方的话：  run-qemu.ps1 -Qemu D:\某处\qemu-system-i386.exe"
    Write-Host ""
    Write-Host "  也可以改用 WSL 里的 QEMU 运行（窗口通过 WSLg 显示）："
    Write-Host "    wsl -d $Distro -- bash scripts/run-qemu.sh"
    Write-Host ""
    if ($DryRun -or (Ask-YesNo "现在改用 WSL 运行吗？")) {
        exit (Invoke-Wsl (Get-WslRunArgs) "用 WSL 运行")
    }
    exit 1
}

# ---------------- 组装 QEMU 命令 ----------------
# 内核命令行见 docs/开发规范.md 4.1：最后一个 console= 是串口，sinit 的提示会出现在这个控制台里
$cmdline = "console=tty1 console=ttyS0 tsc=reliable mitigations=off random.trust_cpu=on loglevel=4 sketch.mode=$Mode"
if ($NoGui) { $cmdline += " sketch.nogui" }
if ($Append) { $cmdline += " $Append" }

# 注意这里给 QEMU 的是“相对路径”，并且把工作目录设成 out\：
# 项目路径里有中文，而 Windows 版 QEMU 按系统的 ANSI 代码页解析命令行参数，
# 直接传中文绝对路径可能打不开文件；相对路径只含英文，由操作系统按当前目录（内部是 Unicode）找到文件。
$qargs = @(
    "-name", "SketchOS",
    "-m", "$Mem",
    "-vga", "std",
    "-kernel", "bzImage",
    "-initrd", "initramfs.cpio.gz",
    "-append", $cmdline,
    "-rtc", "base=utc",
    "-accel", $Accel,
    # 声卡：Sound Blaster 16（和网页版一样），声音经 Windows 的 DirectSound 放出来
    "-audiodev", "dsound,id=snd0",
    "-device", "sb16,audiodev=snd0"
)
if ($NoGui) {
    $qargs += "-nographic"                      # 不开窗口，串口和监视器都在这个控制台里
} else {
    $qargs += @("-serial", "mon:stdio")         # 图形在 QEMU 窗口里，串口 shell 在这个控制台里
}

Write-Step "QEMU：$QemuExe"
Write-Step "工作目录：$OutDir"
Write-Step "内核命令行：$cmdline"
$shown = ($qargs | ForEach-Object { if ($_ -match "\s") { "`"$_`"" } else { $_ } }) -join " "
if ($DryRun) {
    Write-Host "`"$QemuExe`" $shown"
    exit 0
}
Write-Step "在这个窗口里按 Ctrl+A 再按 X 可以退出 QEMU；系统里输入 sinit poweroff 关机"

Push-Location -LiteralPath $OutDir
try {
    & $QemuExe @qargs
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($code -ne 0) { Write-Warn "QEMU 退出码 $code" }
exit $code
