#!/bin/bash
# 第 3 步：编译所有用户态程序（sinit、sksh、skbox、sketchd、图形应用）
# 额外参数会传给 make，例如:
#   scripts/userspace.sh                 编译全部
#   scripts/userspace.sh sksh skbox      只编译某几个程序
#   scripts/userspace.sh clean
# 环境变量:
#   USERSPACE_BUILD=目录   输出目录（默认 $WORK/build/userspace；并行开发时各用各的）
#   KEEP_GOING=1           某个程序编译失败时继续编译其它程序（make -k）
set -euo pipefail
source "$(dirname "$0")/env.sh"
[ -x "$SKCC" ] || die "找不到 $SKCC，请先运行 scripts/toolchain.sh"
BUILD="${USERSPACE_BUILD:-$WORK/build/userspace}"
extra=()
[ "${KEEP_GOING:-0}" = "1" ] && extra+=(-k)
make -C "$ROOT/userspace" CC="$SKCC" BUILD="$BUILD" -j"$JOBS" "${extra[@]}" "$@"
