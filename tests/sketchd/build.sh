#!/bin/bash
# tests/sketchd/build.sh — 编译 sketchd 和它的测试程序，打包一个测试用 initramfs
#
# 用法（在 WSL 里）：
#   bash tests/sketchd/build.sh            # 有可用的 sinit 就用 sinit，否则用 testinit
#   bash tests/sketchd/build.sh --testinit # 强制用 tests/sketchd/testinit.c 当 1 号进程
# 环境变量：
#   USERSPACE_BUILD  用户态输出目录（默认 $WORK/agents/sketchd/build）
#   SKD_TEST_OUT     测试产物目录（默认 $WORK/agents/sketchd），生成 test.cpio.gz
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/scripts/env.sh"
B="${USERSPACE_BUILD:-$WORK/agents/sketchd/build}"
OUTDIR="${SKD_TEST_OUT:-$WORK/agents/sketchd}"
OVL="$OUTDIR/overlay"
USE_TESTINIT=0
[ "${1:-}" = "--testinit" ] && USE_TESTINIT=1

# 1. sketchd + 库。别人的库暂时有重复符号时，退而用 --allow-multiple-definition 链接 sketchd
make -C "$ROOT/userspace" CC="$SKCC" BUILD="$B" -j"$JOBS" -k all > "$OUTDIR/build.log" 2>&1 || true
if [ ! -x "$B/bin/sketchd" ] || [ "$(find "$ROOT/userspace/gui/sketchd" -newer "$B/bin/sketchd" -name '*.[ch]' | wc -l)" != 0 ]; then
    make -C "$ROOT/userspace" CC="$SKCC" BUILD="$B" -k sketchd >> "$OUTDIR/build.log" 2>&1 || \
    "$SKCC" -Wl,--gc-sections -s -Wl,--allow-multiple-definition -o "$B/bin/sketchd" \
        "$B"/obj/gui/sketchd/*.o "$B/libsketch.a" "$B/libcommon.a" -lm
fi
[ -x "$B/bin/sketchd" ] || die "sketchd 编译失败，见 $OUTDIR/build.log"

# 2. 测试程序
rm -rf "$OVL"
mkdir -p "$OVL/bin" "$OVL/sbin" "$OVL/etc/sketch"
CFLAGS="-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I$ROOT/userspace/gui/include -I$ROOT/userspace/common"
"$SKCC" $CFLAGS -o "$OVL/bin/skdtest" "$ROOT/tests/sketchd/testclient.c" "$B/libsketch.a" "$B/libcommon.a" -lm \
    -Wl,--allow-multiple-definition
"$SKCC" $CFLAGS -o "$OVL/bin/skdbad" "$ROOT/tests/sketchd/badclient.c"

# 3. 测试用的应用列表：真实应用 + 测试客户端（“终端”换成测试客户端，Ctrl+Alt+T 能验证）
cat > "$OVL/etc/sketch/apps.conf" <<'EOF'
# 测试用 apps.conf（tests/sketchd/build.sh 生成）
terminal  terminal  终端        #4A7BD0  /bin/skdtest --name term --resizable --title 终端（测试）
t1        program   测试窗口    #E0605A  /bin/skdtest --name t1 --windows 2 --resizable --min 200x120
dlg       text      对话框      #9B7BD4  /bin/skdtest --name dlg --dialog --size 300x150
hang      clock     卡死的程序  #F2C94C  /bin/skdbad hang
dragdie   paint     拖动就崩溃  #F28DB2  /bin/skdbad dragdie
notepad   notepad   记事本      #5BAA62  /bin/sknote %f
about     about     关于本机    #F2C94C  /bin/skdtest --name about --center --size 360x220 --title 关于本机（测试）
EOF

# 4. 1 号进程
if [ $USE_TESTINIT = 1 ] || [ ! -x "$B/bin/sinit" ] || [ ! -x "$B/bin/sksh" ]; then
    echo "使用 testinit 作为 1 号进程"
    "$SKCC" $CFLAGS -o "$OVL/sbin/sinit" "$ROOT/tests/sketchd/testinit.c"
    ln -sf sbin/sinit "$OVL/init"
fi

# 5. 打包
USERSPACE_BUILD="$B" bash "$ROOT/scripts/initramfs.sh" --stage "$OUTDIR/stage" --out "$OUTDIR/test.cpio.gz" --extra "$OVL"
ls -la "$OUTDIR/test.cpio.gz"
