#!/bin/bash
# 在 WSL 里用 QEMU 运行 SketchOS：图形界面在一个窗口里（WSLg），串口 shell 在当前终端里
#
#   bash scripts/run-qemu.sh [选项]
#
#   --nogui            纯文字模式：内核命令行加 sketch.nogui，不开窗口 (-nographic)，在当前终端里用 shell
#   --mem MB           内存大小，默认 512
#   --kernel 文件      内核，默认 out/bzImage
#   --initrd 文件      根文件系统，默认 out/initramfs.cpio.gz
#   --append "参数"    追加内核参数，例如 "sketch.tz=JST-9" 或 "sketch.autostart=terminal,clock"
#   --mode 宽x高       桌面分辨率，默认 1024x768
#   --no-kvm           不使用 KVM 硬件加速（默认：/dev/kvm 可用就用）
#   --dry-run          只打印 QEMU 命令，不运行
#
# 在终端里：先按 Ctrl+A 再按 X 退出 QEMU；Ctrl+A 再按 C 切换到 QEMU 监视器（再按一次切回来）。
# 在系统里：输入 sinit poweroff 关机（QEMU 会自动退出），sinit reboot 重启。
set -euo pipefail
source "$(dirname "$0")/env.sh"

KERNEL_IMG="$OUT/bzImage"
INITRD_IMG="$OUT/initramfs.cpio.gz"
MEM=512
MODE=1024x768
NOGUI=0
KVM=1
DRY=0
EXTRA=""
while [ $# -gt 0 ]; do
    case "$1" in
        --nogui)   NOGUI=1; shift ;;
        --mem)     MEM="$2"; shift 2 ;;
        --kernel)  KERNEL_IMG="$2"; shift 2 ;;
        --initrd)  INITRD_IMG="$2"; shift 2 ;;
        --append)  EXTRA="$EXTRA $2"; shift 2 ;;
        --mode)    MODE="$2"; shift 2 ;;
        --no-kvm)  KVM=0; shift ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) die "未知参数: $1（用 --help 查看用法）" ;;
    esac
done

QEMU="$(command -v qemu-system-i386 || true)"
[ -n "$QEMU" ] || die "找不到 qemu-system-i386，请运行：sudo apt install qemu-system-x86（或者 bash scripts/toolchain.sh）"
[ -f "$KERNEL_IMG" ] || die "找不到内核 $KERNEL_IMG，请先运行 bash scripts/build.sh"
[ -f "$INITRD_IMG" ] || die "找不到根文件系统 $INITRD_IMG，请先运行 bash scripts/build.sh"
[[ "$MODE" =~ ^[0-9]+x[0-9]+$ ]] || die "--mode 的格式是 宽x高，例如 1024x768"

# 内核命令行（docs/开发规范.md 4.1）。最后一个 console= 决定 /dev/console：这里是串口，
# 所以开机信息和 sinit 的提示会出现在这个终端里；屏幕 (tty1) 上同样能看到内核消息。
CMDLINE="console=tty1 console=ttyS0 tsc=reliable mitigations=off random.trust_cpu=on loglevel=4 sketch.mode=$MODE"
[ "$NOGUI" = 1 ] && CMDLINE="$CMDLINE sketch.nogui"
CMDLINE="$CMDLINE${EXTRA:+ ${EXTRA# }}"

ARGS=(-name "SketchOS"
      -m "$MEM"
      -vga std
      -kernel "$KERNEL_IMG" -initrd "$INITRD_IMG" -append "$CMDLINE"
      -rtc base=utc)          # 硬件时钟按 UTC 走，sinit 再用 TZ 换算成本地时间

# KVM：让虚拟机的指令直接在真实 CPU 上执行，比纯软件模拟 (TCG) 快得多
if [ "$KVM" = 1 ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ARGS+=(-accel kvm)
    ACCEL="KVM 硬件加速"
else
    ARGS+=(-accel tcg)
    ACCEL="软件模拟 (TCG)"
fi

if [ "$NOGUI" = 1 ]; then
    # -nographic：不开窗口，串口和 QEMU 监视器都接到当前终端
    ARGS+=(-nographic)
    DISPLAY_DESC="不开窗口，shell 就在这个终端里"
else
    # 选一个 QEMU 支持的图形后端；WSLg 会把窗口显示在 Windows 桌面上
    backends="$("$QEMU" -display help 2>/dev/null || true)"
    if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && grep -qw gtk <<<"$backends"; then
        ARGS+=(-display gtk)
        DISPLAY_DESC="GTK 窗口"
    elif [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && grep -qw sdl <<<"$backends"; then
        ARGS+=(-display sdl)
        DISPLAY_DESC="SDL 窗口"
    else
        ARGS+=(-display none -vnc 127.0.0.1:0)
        DISPLAY_DESC="没有检测到图形环境，画面输出到 VNC：用 VNC 客户端连接 127.0.0.1:5900"
    fi
    # mon:stdio：串口接到当前终端，并且可以用 Ctrl+A C 切到 QEMU 监视器
    ARGS+=(-serial mon:stdio)
fi

log "QEMU：$QEMU"
log "加速：$ACCEL；内存 ${MEM}MB；显示：$DISPLAY_DESC"
log "内核命令行：$CMDLINE"
if [ "$DRY" = 1 ]; then
    printf '%q ' "$QEMU" "${ARGS[@]}"
    echo
    exit 0
fi
log "按 Ctrl+A 再按 X 退出 QEMU；系统里输入 sinit poweroff 关机"
exec "$QEMU" "${ARGS[@]}"
