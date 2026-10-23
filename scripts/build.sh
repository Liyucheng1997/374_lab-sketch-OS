#!/bin/bash
# 一键完整构建 SketchOS（在 WSL Ubuntu 里运行）
#
#   bash scripts/build.sh [--clean] [--kernel] [--no-web]
#
#   --clean    先删掉用户态编译产物、根文件系统暂存目录和 out/initramfs.cpio.gz，再从头构建
#              （内核编译很慢，--clean 不会动它；要重编内核请加 --kernel）
#   --kernel   强制重新编译内核（默认：out/bzImage 比 kernel/sketchos.config 新就跳过）
#   --no-web   不生成网页版资源（web/lib、web/images）
#
# 依次执行：
#   1. toolchain.sh   编译工具链（已经装好时只做检查，很快）
#   2. kernel.sh      Linux 内核 → out/bzImage
#   3. fonts.sh       字体 → assets/fonts/*.skf（已有就跳过；生成需要联网）
#   4. userspace.sh   我们自己写的全部用户态程序
#   5. initramfs.sh   打包根文件系统 → out/initramfs.cpio.gz
#   6. web.sh         网页版（v86 模拟器 + 镜像）→ web/
#
# 环境变量：KEEP_GOING=1  某个程序编译失败时继续编译其它程序（并行开发时有用）
set -Eeuo pipefail     # -E：让下面的 ERR 陷阱在函数里也生效
source "$(dirname "$0")/env.sh"

CLEAN=0
FORCE_KERNEL=0
WEB=1
while [ $# -gt 0 ]; do
    case "$1" in
        --clean)  CLEAN=1 ;;
        --kernel) FORCE_KERNEL=1 ;;
        --no-web) WEB=0 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) die "未知参数: $1（可用 --clean --kernel --no-web）" ;;
    esac
    shift
done

T_START=$(date +%s)
STEP=0
STEPS_DONE=()
trap 'printf "\n\033[1;31m[sketchos] 构建在第 %d 步失败，请看上面的错误信息\033[0m\n" "$STEP" >&2' ERR

# 运行 scripts/ 下的一个步骤脚本，记录用时。
# 如果脚本在 Windows 上被编辑器或 git 改成了 CRLF 换行，bash 会报 "$'\r': command not found"
# 这类莫名其妙的错误；这里检测到就临时去掉 \r 再运行（bash -c 的第一个参数会成为 $0，
# 这样脚本里 source "$(dirname "$0")/env.sh" 依然有效），并提醒把文件改回 LF。
step() {   # step 名称 脚本 [参数...]
    local name="$1" script="$ROOT/scripts/$2"
    shift 2
    STEP=$((STEP + 1))
    local t0
    t0=$(date +%s)
    printf '\n\033[1;35m==== 第 %d 步：%s ====\033[0m\n' "$STEP" "$name"
    local rc=0
    if grep -q $'\r' "$script"; then
        warn "$(basename "$script") 是 Windows 换行 (CRLF)，临时转换后运行；请把它改成 LF：sed -i 's/\\r\$//' scripts/$(basename "$script")"
        bash -c "$(tr -d '\r' < "$script")" "$script" "$@" || rc=$?
    else
        bash "$script" "$@" || rc=$?
    fi
    # 显式返回退出码：在 if 条件里调用函数时 set -e 不生效，不能指望它自动停下
    [ "$rc" -eq 0 ] || return "$rc"
    STEPS_DONE+=("$(printf '%4d 秒   %s' $(( $(date +%s) - t0 )) "$name")")
}

skip() {   # skip 名称 原因
    STEP=$((STEP + 1))
    printf '\n\033[1;35m==== 第 %d 步：%s ====\033[0m\n' "$STEP" "$1"
    log "跳过：$2"
    STEPS_DONE+=("$(printf '  跳过   %s（%s）' "$1" "$2")")
}

UB="${USERSPACE_BUILD:-$WORK/build/userspace}"
if [ "$CLEAN" = 1 ]; then
    log "清理：$UB、$ROOTFS_STAGE、$OUT/initramfs.cpio.gz"
    rm -rf "$UB" "$ROOTFS_STAGE" "$OUT/initramfs.cpio.gz"
fi

# 1. 工具链
step "工具链" toolchain.sh

# 2. 内核：配置片段没改、内核也已经编过，就不必再花几分钟重编
KCONF="$ROOT/kernel/sketchos.config"
if [ "$FORCE_KERNEL" = 0 ] && [ -f "$OUT/bzImage" ] && [ "$OUT/bzImage" -nt "$KCONF" ]; then
    skip "内核" "out/bzImage 比 kernel/sketchos.config 新，加 --kernel 可强制重编"
else
    step "内核" kernel.sh
fi

# 3. 字体：.skf 文件会提交进仓库，一般已经存在
if [ ! -f "$ROOT/scripts/fonts.sh" ]; then
    skip "字体" "没有 scripts/fonts.sh"
elif compgen -G "$ROOT/assets/fonts/*.skf" >/dev/null; then
    skip "字体" "assets/fonts/ 里已经有 .skf 字体"
else
    step "字体" fonts.sh
fi

# 4~5. 用户态程序和根文件系统
# KEEP_GOING=1 时，个别程序编译失败（比如别人正在写、还没写完）不中断整个构建：
# 已经编好的程序照样打包进根文件系统，方便先测试其它部分。
if [ "${KEEP_GOING:-0}" = 1 ]; then
    if ! step "用户态程序" userspace.sh; then
        warn "有程序编译失败（KEEP_GOING=1，继续用已经编好的程序打包）"
        STEPS_DONE+=("  失败   用户态程序（部分程序没编出来，见上面的错误）")
    fi
else
    step "用户态程序" userspace.sh
fi
step "根文件系统" initramfs.sh

# 6. 网页版
if [ "$WEB" = 0 ]; then
    skip "网页版" "使用了 --no-web"
elif [ ! -f "$ROOT/scripts/web.sh" ]; then
    skip "网页版" "没有 scripts/web.sh"
else
    step "网页版" web.sh
fi

# ---------------- 总结 ----------------
size_of() { [ -e "$1" ] && du -sh "$1" 2>/dev/null | cut -f1 || echo "（没有）"; }
nprog=0
if [ -d "$UB/bin" ]; then
    nprog=$(find "$UB/bin" -maxdepth 1 -type f | wc -l)
fi

printf '\n\033[1;32m================ 构建完成（共 %d 秒） ================\033[0m\n' $(( $(date +%s) - T_START ))
for s in "${STEPS_DONE[@]}"; do
    printf '  %s\n' "$s"
done
echo
echo "  产物："
printf '    %-24s %6s   %s\n' "out/bzImage" "$(size_of "$OUT/bzImage")" "Linux 内核"
printf '    %-24s %6s   %s\n' "out/initramfs.cpio.gz" "$(size_of "$OUT/initramfs.cpio.gz")" "根文件系统（我们写的全部用户态程序都在里面）"
printf '    %-24s %6s   %s\n' "$(basename "$UB")/bin" "$nprog 个" "用户态程序（$UB/bin）"
if [ -d "$ROOT/web/images" ]; then
    printf '    %-24s %6s   %s\n' "web/" "$(size_of "$ROOT/web")" "网页版（v86 模拟器 + 系统镜像）"
fi
echo
echo "  运行方法："
echo "    WSL 里（图形窗口）:   bash scripts/run-qemu.sh"
echo "    WSL 里（纯文字）:     bash scripts/run-qemu.sh --nogui"
echo "    Windows:              双击 windows\\运行QEMU版.bat"
if [ "$WEB" = 1 ] && [ -f "$ROOT/tools/serve.py" ]; then
    echo "    浏览器:               python3 tools/serve.py，然后打开它显示的网址"
fi
echo "    自动测试:             python3 tests/boot/boot_test.py"
