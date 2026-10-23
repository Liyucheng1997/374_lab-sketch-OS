#!/bin/bash
# tests/web/run_tests.sh — SketchOS 网页版（v86）的自动化测试，在 WSL 里运行：
#
#   bash tests/web/run_tests.sh [--out 目录] [--system-initrd 文件] [--no-system]
#
#   --out            输出目录（截图、串口日志），默认 $WORK/test-web
#   --system-initrd  第 5 项用这个 initramfs；默认用 out/initramfs.cpio.gz，没有就用当前源码现编一个
#   --no-system      跳过第 5 项
#
# 测试内容：
#   1. JS 单元测试：串口终端的转义序列处理、手绘线条的可复现性、harness 的按键表/PNG 编码/鼠标坐标换算
#   2. tools/serve.py：MIME 类型、缓存头、二进制镜像原样发送、不列目录、拒绝 ../（WSL 和 Windows 的 Python 各跑一遍）
#   3. 最小测试系统（/init = tests/web/guest/webtest.c）在 v86 里跑 tests/web/fixture.txt：
#      串口收发（含中文）、PS/2 键盘、VMware 绝对坐标鼠标（左/中/右键、双击、拖动、滚轮）、截图、重启、关机
#   4. 文本模式：nomodeset 启动，读出 VGA 文本屏幕并截图（tests/web/textmode.txt）
#   5. 当前的完整系统：等 sinit 的日志里程碑；有 sksh 就在串口执行命令，有 sketchd 就等桌面就绪；截图
#
# 需要：Windows 上的 node（WSL 能直接运行 node.exe）、scripts/toolchain.sh 准备好的 sk-cc、out/bzImage。
# web/lib（v86 + BIOS）不存在时会自动运行 scripts/web.sh --lib-only。
set -uo pipefail
source "$(dirname "$0")/../../scripts/env.sh"

OUTDIR="$WORK/test-web"
SYSTEM_INITRD=""
RUN_SYSTEM=1
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUTDIR="$2"; shift 2 ;;
        --system-initrd) SYSTEM_INITRD="$2"; shift 2 ;;
        --no-system) RUN_SYSTEM=0; shift ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) die "未知参数: $1" ;;
    esac
done
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"

NODE="$(command -v node.exe || command -v node || true)"
[ -n "$NODE" ] || die "找不到 node.exe：harness 需要 Windows 上的 Node.js（WSL 互操作调用）"
# Windows 上的 node 要把 /root/... 这样的 WSL 路径换算成 \\wsl.localhost\<发行版>\...，告诉它发行版名字
export SKETCHOS_WSL_DISTRO="${WSL_DISTRO_NAME:-Ubuntu}"
export WSLENV="${WSLENV:+$WSLENV:}SKETCHOS_WSL_DISTRO"
cd "$ROOT"

PASSED=(); FAILED=(); SKIPPED=()
ok()   { PASSED+=("$1"); printf '\033[1;32m[通过]\033[0m %s\n' "$1"; }
bad()  { FAILED+=("$1"); printf '\033[1;31m[失败]\033[0m %s\n' "$1"; }
skip() { SKIPPED+=("$1"); printf '\033[1;33m[跳过]\033[0m %s\n' "$1"; }

# harness <名字> <initrd> <脚本模板> [额外参数...]：@OUT@ 换成输出目录后运行，失败时打印串口日志结尾
harness() {
    local name="$1" initrd="$2" template="$3"; shift 3
    local script="$OUTDIR/$name.txt" log="$OUTDIR/$name-serial.log"
    sed "s|@OUT@|$OUTDIR|g" "$template" > "$script"
    log "v86: $name（串口日志 $log）"
    if "$NODE" tools/v86_harness.mjs --quiet --initrd "$initrd" --script "$script" --log "$log" "$@"; then
        return 0
    fi
    echo "---- $name 串口日志结尾 ----"
    tail -n 40 "$log" 2>/dev/null
    return 1
}

png() {   # png 文件 [宽x高]
    python3 -I "$ROOT/tests/web/check_png.py" "$@"
}

# ---------- 0. 准备 ----------
[ -f "$OUT/bzImage" ] || die "没有 out/bzImage，请先运行 scripts/kernel.sh"
if [ ! -f web/lib/libv86.mjs ] || [ ! -f web/lib/v86.wasm ] || [ ! -f web/lib/seabios.bin ]; then
    bash scripts/web.sh --lib-only || die "scripts/web.sh --lib-only 失败"
fi

# ---------- 1. JS 单元测试 ----------
log "1. JS 单元测试"
if "$NODE" tests/web/unit_test.mjs; then ok "JS 单元测试"; else bad "JS 单元测试"; fi

# ---------- 2. serve.py ----------
log "2. tools/serve.py（WSL python3）"
if python3 -I tests/web/test_serve.py; then ok "serve.py（Linux）"; else bad "serve.py（Linux）"; fi
WINPY="$(command -v py.exe || true)"
if [ -n "$WINPY" ]; then
    log "2. tools/serve.py（Windows Python，项目路径里有中文）"
    if "$WINPY" -3 "$(wslpath -w "$ROOT/tests/web/test_serve.py")"; then ok "serve.py（Windows）"; else bad "serve.py（Windows）"; fi
else
    skip "serve.py（Windows）：没有 py.exe"
fi

# ---------- 3/4. 最小测试系统 ----------
log "3. 编译测试系统 webtest 并打包 initramfs"
FIX="$OUTDIR/fixture"
mkdir -p "$FIX"
GEN_CPIO="$WORK/toolchain/bin/gen_init_cpio"
[ -x "$GEN_CPIO" ] || gcc -O2 -o "$GEN_CPIO" "$KERNEL_SRC/usr/gen_init_cpio.c"
if "$SKCC" -O2 -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
        -Wno-missing-field-initializers -s -o "$FIX/webtest" tests/web/guest/webtest.c; then
    cat > "$FIX/spec.txt" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /tmp 1777 0 0
file /init $FIX/webtest 0755 0 0
EOF
    "$GEN_CPIO" "$FIX/spec.txt" | gzip -9 > "$OUTDIR/fixture.cpio.gz"
    ok "webtest 编译（-Werror）并打包"
    rm -f "$OUTDIR"/fixture-*.png "$OUTDIR/textmode.png"
    if harness fixture "$OUTDIR/fixture.cpio.gz" tests/web/fixture.txt; then
        ok "v86：串口/键盘/鼠标/截图/重启/关机（fixture.txt）"
    else
        bad "v86：串口/键盘/鼠标/截图/重启/关机（fixture.txt）"
    fi
    for f in fixture-1 fixture-2; do
        if png "$OUTDIR/$f.png" 1024x768; then ok "截图 $f.png（图形模式 1024x768）"; else bad "截图 $f.png"; fi
    done
    log "4. 文本模式"
    if harness textmode "$OUTDIR/fixture.cpio.gz" tests/web/textmode.txt --append nomodeset &&
            png "$OUTDIR/textmode.png" 720x400; then
        ok "v86：VGA 文本模式截图（textmode.txt）"
    else
        bad "v86：VGA 文本模式截图（textmode.txt）"
    fi
else
    bad "webtest 编译"
fi

# ---------- 5. 当前的完整系统 ----------
if [ "$RUN_SYSTEM" = 1 ]; then
    log "5. 当前的完整系统镜像"
    if [ -z "$SYSTEM_INITRD" ] && [ -f "$OUT/initramfs.cpio.gz" ]; then
        SYSTEM_INITRD="$OUT/initramfs.cpio.gz"
    fi
    if [ -z "$SYSTEM_INITRD" ]; then
        log "out/ 里没有 initramfs：用现在的源码编译一个（KEEP_GOING=1，编不过的程序先跳过）"
        US="$OUTDIR/userspace"
        if ! USERSPACE_BUILD="$US" KEEP_GOING=1 bash scripts/userspace.sh > "$OUTDIR/userspace.log" 2>&1; then
            if grep -q $'\r' scripts/userspace.sh userspace/Makefile 2>/dev/null; then
                # 不是我们的文件，只在临时副本里去掉 Windows 换行符(CRLF)
                warn "scripts/userspace.sh 或 userspace/Makefile 是 CRLF 换行，bash/make 无法正确解析；改用去掉 CR 的临时副本编译"
                sed 's/\r$//' userspace/Makefile > "$OUTDIR/Makefile.lf"
                make -C userspace -f "$OUTDIR/Makefile.lf" CC="$SKCC" BUILD="$US" -j"$JOBS" -k >> "$OUTDIR/userspace.log" 2>&1
            fi
            warn "有程序没有编译成功，详情见 $OUTDIR/userspace.log"
        fi
        if [ -d "$US/bin" ] && USERSPACE_BUILD="$US" bash scripts/initramfs.sh --stage "$OUTDIR/stage" \
                --out "$OUTDIR/system.cpio.gz" > "$OUTDIR/initramfs.log" 2>&1; then
            SYSTEM_INITRD="$OUTDIR/system.cpio.gz"
            log "编出来的程序：$(ls "$US/bin" | tr '\n' ' ')"
        fi
    fi
    if [ -z "$SYSTEM_INITRD" ] || [ ! -f "$SYSTEM_INITRD" ]; then
        bad "完整系统：没有可用的 initramfs（见 $OUTDIR/userspace.log）"
    else
        # 看看镜像里有哪些程序，决定能测到哪一步
        HAVE="$(python3 -I - "$SYSTEM_INITRD" <<'PY'
import gzip, sys
data = gzip.open(sys.argv[1]).read()
pos, names = 0, set()
while pos + 110 <= len(data) and data[pos:pos + 6] in (b"070701", b"070702"):
    namesize = int(data[pos + 94:pos + 102], 16)
    filesize = int(data[pos + 54:pos + 62], 16)
    name = data[pos + 110:pos + 110 + namesize - 1].decode("utf-8", "replace")
    pos = (pos + 110 + namesize + 3) & ~3
    pos = (pos + filesize + 3) & ~3
    if name == "TRAILER!!!":
        break
    names.add(name.lstrip("./").lstrip("/"))
print(" ".join(sorted(n for n in names if n.startswith(("bin/", "sbin/")) or n == "init")))
PY
)"
        has() { case " $HAVE " in *" $1 "*) return 0 ;; esac; return 1; }
        T="$OUTDIR/system-template.txt"
        {
            echo "# 由 run_tests.sh 按镜像内容生成"
            echo "wait sinit: SketchOS \\S+ starting 120"
            echo "wait sinit: services started 120"
            echo "sleep 2"
            echo "shot @OUT@/system-boot.png"
            if has bin/sksh; then
                echo "serial \\n"
                echo "wait # \$ 60"
                echo "run uname -a"
                echo "wait Linux sketchos 10"
                echo "run echo 网页版串口测试 \$((6*7))"
                echo "wait 网页版串口测试 42 10"
            fi
            if has bin/sketchd; then
                echo "wait sketchd: ready \\d+x\\d+ backend=\\S+ mouse=\\S+ 180"
                echo "sleep 3"
                echo "move 512 384"
                echo "sleep 1"
                echo "shot @OUT@/system-desktop.png"
            fi
            if has bin/sksh && has bin/reboot; then
                # 真系统的重启：sinit 关掉服务 → 内核复位 CPU → harness 冷启动新的模拟器 → 再次开机
                echo "mark"
                echo "serial reboot\\n"
                echo "wait reboot: Restarting system 60"
                echo "wait sinit: SketchOS \\S+ starting 120"
                echo "wait sinit: services started 120"
                echo "serial \\n"
                echo "wait # \$ 60"
                echo "run echo 重启后 \$((40+2))"
                echo "wait 重启后 42 10"
            fi
            if has bin/sksh && has bin/poweroff; then
                echo "mark"
                echo "serial poweroff\\n"
                echo "wait reboot: (Power down|System halted|Power off not available) 60"
            fi
        } > "$T"
        log "镜像 $SYSTEM_INITRD 里有：$HAVE"
        if harness system "$SYSTEM_INITRD" "$T"; then
            ok "完整系统：sinit 里程碑$(has bin/sksh && echo "、串口 shell")$(has bin/sketchd && echo "、桌面就绪")"
        else
            bad "完整系统（脚本 $OUTDIR/system.txt）"
        fi
        if png "$OUTDIR/system-boot.png"; then ok "截图 system-boot.png"; else bad "截图 system-boot.png"; fi
        if has bin/sketchd; then
            if png "$OUTDIR/system-desktop.png" --min-colors 8; then ok "截图 system-desktop.png"; else bad "截图 system-desktop.png"; fi
        else
            skip "桌面截图：镜像里还没有 /bin/sketchd"
        fi
        has bin/sksh || skip "串口 shell 测试：镜像里还没有 /bin/sksh"
    fi
else
    skip "完整系统（--no-system）"
fi

# ---------- 汇总 ----------
echo
log "通过 ${#PASSED[@]} 项，失败 ${#FAILED[@]} 项，跳过 ${#SKIPPED[@]} 项。截图和日志在 $OUTDIR"
for f in "${FAILED[@]}"; do printf '  \033[1;31m失败\033[0m %s\n' "$f"; done
for s in "${SKIPPED[@]}"; do printf '  \033[1;33m跳过\033[0m %s\n' "$s"; done
[ ${#FAILED[@]} -eq 0 ]
