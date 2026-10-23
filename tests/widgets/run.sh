#!/bin/bash
# tests/widgets/run.sh — 复杂控件（多行编辑器 / 列表 / 对话框）和控件展示程序的全部测试
#
#   bash tests/widgets/run.sh [-k 测试名] [--out 截图目录]
#
# 在 WSL 里运行，不需要 QEMU：
#   1. 编译 libsketch、skwidgets（i686 静态程序）和测试小程序 twidgets；
#   2. 宿主机单元测试 gui/tests/test_ui_widgets.c（文本模型、折行、撤销、性能）；
#   3. 用 tools/mock_display.py 模拟显示服务器，驱动 twidgets / skwidgets 做界面测试，
#      截图保存在 --out 目录（默认 $WORK/widget-test-shots），请打开看看效果。
# 环境变量 USERSPACE_BUILD 指定编译输出目录（并行开发时各用各的）。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/scripts/env.sh"
B="${USERSPACE_BUILD:-$WORK/build/userspace}"
OUT="$WORK/widget-test-shots"
PYARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        -k)    PYARGS+=(-k "$2"); shift 2 ;;
        *)     die "未知参数: $1" ;;
    esac
done

log "编译 libsketch 和 skwidgets → $B"
make -C "$ROOT/userspace" CC="$SKCC" BUILD="$B" -j"$JOBS" libsketch skwidgets "$B/libcommon.a" >/dev/null

log "编译测试小程序 twidgets"
mkdir -p "$B/tests"
"$SKCC" -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/userspace/gui/include" \
    -I"$ROOT/userspace/common" -o "$B/tests/twidgets" "$ROOT/tests/widgets/twidgets.c" \
    "$B/libsketch.a" "$B/libcommon.a" -lm

log "宿主机单元测试 test_ui_widgets"
make -C "$ROOT/userspace" BUILD="$B" "$B/host/test_ui_widgets" >/dev/null
SKETCH_FONT_DIR="$ROOT/assets/fonts" "$B/host/test_ui_widgets"

log "界面测试（模拟显示服务器）"
SKETCH_FONT_DIR="$ROOT/assets/fonts" python3 "$ROOT/tests/widgets/test_widgets.py" \
    --bin "$B/bin" --twidgets "$B/tests/twidgets" --out "$OUT" "${PYARGS[@]}"
