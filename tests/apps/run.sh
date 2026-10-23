#!/bin/bash
# 图形应用（skterm、skfiles、sknote、skpaint、skmon、skclock、skabout）的自动化测试
#
#   bash tests/apps/run.sh                 编译全部用户态程序，运行 tests/apps/test_*.py
#   bash tests/apps/run.sh sknote skterm   只测这几个应用
#   bash tests/apps/run.sh sknote -- --only save,open   多余的参数传给测试脚本
#
# 不需要 QEMU：应用是 32 位静态程序，宿主机直接运行；tools/mock_display.py 扮演显示服务器。
# 截图保存在 $WORK/app-test-shots/<应用>/，测试结束后请打开看看（视觉效果也是测试的一部分）。
# 环境变量：USERSPACE_BUILD=目录（默认 $WORK/build/apptest）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/scripts/env.sh"
B="${USERSPACE_BUILD:-$WORK/build/apptest}"
SHOTS="${APP_SHOTS:-$WORK/app-test-shots}"

APPS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --) shift; break ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) APPS+=("$1"); shift ;;
    esac
done
if [ ${#APPS[@]} -eq 0 ]; then
    for f in "$ROOT"/tests/apps/test_*.py; do
        [ -e "$f" ] || continue
        a="$(basename "$f" .py)"
        APPS+=("${a#test_}")
    done
fi
[ ${#APPS[@]} -gt 0 ] || die "tests/apps/ 里还没有 test_<应用>.py"

[ -x "$SKCC" ] || die "找不到 $SKCC，请先运行 scripts/toolchain.sh"
# 全部编译，不只是被测的应用：终端的测试还要在里面运行 sksh、skbox，它们改了也要跟着重编
log "编译用户态程序 → $B"
make -C "$ROOT/userspace" CC="$SKCC" BUILD="$B" -j"$JOBS" all >/dev/null || die "编译失败"

PASSED=(); FAILED=()
for a in "${APPS[@]}"; do
    t="$ROOT/tests/apps/test_$a.py"
    [ -f "$t" ] || die "没有 $t"
    log "测试 $a（截图 $SHOTS/$a）"
    if python3 "$t" --bin "$B/bin" --out "$SHOTS/$a" "$@"; then PASSED+=("$a"); else FAILED+=("$a"); fi
done

echo
log "通过: ${PASSED[*]:-无}"
if [ ${#FAILED[@]} -gt 0 ]; then
    warn "失败: ${FAILED[*]}"
    exit 1
fi
log "全部通过"
