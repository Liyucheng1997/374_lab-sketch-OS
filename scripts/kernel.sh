#!/bin/bash
# 第 2 步：配置并编译 Linux 内核
#   1. allnoconfig + kernel/sketchos.config  → 最小化 .config
#   2. 逐项检查我们要求的选项是否真的生效（依赖不满足时 Kconfig 会悄悄忽略）
#   3. 编译 bzImage，复制到 out/bzImage
set -euo pipefail
source "$(dirname "$0")/env.sh"

[ -d "$KERNEL_SRC" ] || die "找不到内核源码 $KERNEL_SRC，请先运行 scripts/toolchain.sh"
FRAG="$ROOT/kernel/sketchos.config"
mkdir -p "$KERNEL_BUILD" "$OUT"

log "生成内核配置 (allnoconfig + sketchos.config) ..."
make -C "$KERNEL_SRC" O="$KERNEL_BUILD" ARCH=i386 \
     KCONFIG_ALLCONFIG="$FRAG" allnoconfig >/dev/null

# ---- 检查配置 ----
bad=0
while IFS= read -r line; do
    line="${line%%$'\r'}"
    case "$line" in
        CONFIG_*=*)
            sym="${line%%=*}"; want="${line#*=}"
            got="$(grep -E "^${sym}=" "$KERNEL_BUILD/.config" | head -1 | cut -d= -f2- || true)"
            if [ "$want" = "n" ]; then
                [ -z "$got" ] || { warn "$sym 期望关闭, 实际 $got"; bad=1; }
            elif [ "$got" != "$want" ]; then
                warn "$sym 期望 $want, 实际 ${got:-未设置}"; bad=1
            fi ;;
        "# CONFIG_"*" is not set")
            sym="${line#\# }"; sym="${sym%% *}"
            if grep -qE "^${sym}=" "$KERNEL_BUILD/.config"; then
                warn "$sym 期望关闭, 实际 $(grep -E "^${sym}=" "$KERNEL_BUILD/.config")"; bad=1
            fi ;;
    esac
done < "$FRAG"
[ $bad -eq 0 ] || die "内核配置与 kernel/sketchos.config 不一致（见上面的警告）"
log "配置检查通过"

log "编译内核 (make -j$JOBS bzImage) ..."
make -C "$KERNEL_SRC" O="$KERNEL_BUILD" ARCH=i386 -j"$JOBS" bzImage 2>&1 | \
    grep -E "error|Error|warning: unmet|Kernel: " || true
[ -f "$KERNEL_BUILD/arch/x86/boot/bzImage" ] || die "内核编译失败"

cp "$KERNEL_BUILD/arch/x86/boot/bzImage" "$OUT/bzImage"
cp "$KERNEL_BUILD/.config" "$OUT/kernel.config"

# gen_init_cpio：内核自带的 initramfs 打包工具（不需要 root 权限就能创建 /dev/console 设备节点）
gcc -O2 -o "$WORK/toolchain/bin/gen_init_cpio" "$KERNEL_SRC/usr/gen_init_cpio.c"

log "内核完成: out/bzImage ($(du -h "$OUT/bzImage" | cut -f1))"
