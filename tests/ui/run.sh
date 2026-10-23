#!/bin/bash
# tests/ui/run.sh — 编译并运行 libsketch 客户端库 + 控件核心的全部测试（在 WSL 里运行）
#
#   bash tests/ui/run.sh                 全部测试
#   bash tests/ui/run.sh --only form     只跑某几项（gallery,form,loop,misuse,disconnect,skhello,cli）
#
# 环境变量：
#   USERSPACE_BUILD=目录   编译输出目录（默认 $WORK/build/uitest）
#   UI_SHOTS=目录          截图输出目录（默认 $USERSPACE_BUILD/ui-shots）
#
# 做三件事：
#   1. 用交叉编译器编出 libsketch、skhello、sketch 和测试程序 tests/ui/ui_test_app.c（32 位静态程序）；
#   2. 用 tools/mock_display.py 模拟显示服务器，运行 tests/ui/test_ui.py（交互、局部重绘、事件循环……）；
#   3. 用本机 gcc 编译并运行单元测试 userspace/gui/tests/test_ui_core.c（自带一个假的显示服务器线程）。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/scripts/env.sh"
B="${USERSPACE_BUILD:-$WORK/build/uitest}"
SHOTS="${UI_SHOTS:-$B/ui-shots}"
mkdir -p "$B/tests" "$SHOTS"

log "编译 libsketch / skhello / sketch → $B"
make -C "$ROOT/userspace" CC="$SKCC" BUILD="$B" -j"$JOBS" libsketch "$B/libcommon.a" skhello sketch >/dev/null
"$SKCC" -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
    -I"$ROOT/userspace/common" -I"$ROOT/userspace/gui/include" \
    -o "$B/tests/ui_test_app" "$ROOT/tests/ui/ui_test_app.c" "$B/libsketch.a" "$B/libcommon.a" -lm

status=0
log "集成测试（模拟显示服务器），截图保存在 $SHOTS"
python3 "$ROOT/tests/ui/test_ui.py" --bin "$B/tests/ui_test_app" --skhello "$B/bin/skhello" \
    --sketch "$B/bin/sketch" --out "$SHOTS" "$@" || status=1

if [ $# -eq 0 ]; then
    log "宿主机单元测试 test_ui_core"
    make -C "$ROOT/userspace" BUILD="$B" "$B/host/test_ui_core" >/dev/null
    SKETCH_FONT_DIR="$ROOT/assets/fonts" "$B/host/test_ui_core" || status=1
fi

if [ $status -eq 0 ]; then log "全部通过"; else warn "有测试失败"; fi
exit $status
