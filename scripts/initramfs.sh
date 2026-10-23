#!/bin/bash
# 第 4 步：组装根文件系统并打包成 initramfs (cpio + gzip)
#
#   scripts/initramfs.sh [--out 输出文件] [--stage 暂存目录] [--extra 目录]...
#
#   --out    默认 out/initramfs.cpio.gz
#   --stage  默认 $WORK/build/rootfs（并行测试时请各用各的目录）
#   --extra  额外叠加到根目录的文件夹（测试用，可多次指定）
#
# 根文件系统的来源：
#   rootfs/                 → 原样复制（/etc 配置、/root 家目录里的示例文件……）
#   用户态编译产物           → /sbin/sinit、/bin/*（skbox 的每个 applet 建一个符号链接）
#   assets/fonts/*.skf      → /usr/share/fonts/
#   docs/*.md               → /usr/share/doc/sketchos/（可以在系统里用记事本阅读）
# 打包用内核自带的 gen_init_cpio，它能在不需要 root 权限的情况下写入设备节点 /dev/console。
set -euo pipefail
source "$(dirname "$0")/env.sh"

OUTFILE="$OUT/initramfs.cpio.gz"
STAGE="$ROOTFS_STAGE"
EXTRAS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --out)   OUTFILE="$2"; shift 2 ;;
        --stage) STAGE="$2"; shift 2 ;;
        --extra) EXTRAS+=("$2"); shift 2 ;;
        *) die "未知参数: $1" ;;
    esac
done

UB="${USERSPACE_BUILD:-$WORK/build/userspace}"
GEN_CPIO="$WORK/toolchain/bin/gen_init_cpio"
[ -x "$GEN_CPIO" ] || gcc -O2 -o "$GEN_CPIO" "$KERNEL_SRC/usr/gen_init_cpio.c"
[ -d "$UB/bin" ] || die "没有找到用户态程序，请先运行 scripts/userspace.sh"

rm -rf "$STAGE"
mkdir -p "$STAGE"
for d in bin sbin etc dev proc sys tmp run mnt root var/log usr/share/fonts usr/share/doc/sketchos; do
    mkdir -p "$STAGE/$d"
done

# 1. 骨架
if [ -d "$ROOT/rootfs" ]; then
    cp -a "$ROOT/rootfs/." "$STAGE/"
    # Windows 盘上的文件在 WSL 里权限都是 777，这里统一规范：目录 755、文件 644、脚本 755
    find "$STAGE" -type d -exec chmod 0755 {} +
    find "$STAGE" -type f -exec chmod 0644 {} +
    find "$STAGE" -type f \( -name '*.sh' -o -path '*/bin/*' \) -exec chmod 0755 {} +
    # 去掉 Windows 换行符(CRLF)，否则脚本和配置文件在 Linux 里会解析出错
    find "$STAGE" -type f \( -name '*.sh' -o -name '*.conf' -o -name '*.txt' -o -path '*/etc/*' \) \
        -exec sed -i 's/\r$//' {} +
fi

# 2. 程序
for f in "$UB/bin/"*; do
    [ -f "$f" ] || continue
    name="$(basename "$f")"
    case "$name" in
        sinit) install -m 0755 "$f" "$STAGE/sbin/sinit" ;;
        *)     install -m 0755 "$f" "$STAGE/bin/$name" ;;
    esac
done
[ -f "$STAGE/sbin/sinit" ] && ln -sf /sbin/sinit "$STAGE/init" && ln -sf sinit "$STAGE/sbin/init"
[ -f "$STAGE/bin/sksh" ] && ln -sf sksh "$STAGE/bin/sh"
if [ -f "$STAGE/bin/skbox" ] && [ -f "$UB/skbox.applets" ]; then
    while read -r a; do
        [ -n "$a" ] || continue
        [ -e "$STAGE/bin/$a" ] || ln -s skbox "$STAGE/bin/$a"
    done < "$UB/skbox.applets"
fi

# 3. 字体与文档
cp "$ROOT"/assets/fonts/*.skf "$STAGE/usr/share/fonts/" 2>/dev/null || warn "assets/fonts 里没有 .skf 字体（界面会用内置后备字体）"
cp "$ROOT"/assets/fonts/*.txt "$STAGE/usr/share/fonts/" 2>/dev/null || true
cp "$ROOT"/docs/*.md "$STAGE/usr/share/doc/sketchos/" 2>/dev/null || true

# 4. 测试用叠加目录
for e in "${EXTRAS[@]}"; do
    cp -a "$e/." "$STAGE/"
done

# 5. 生成 gen_init_cpio 的清单
SPEC="$(mktemp)"
{
    echo "nod /dev/console 0600 0 0 c 5 1"
    echo "nod /dev/null 0666 0 0 c 1 3"
    (cd "$STAGE" && find . -mindepth 1 | LC_ALL=C sort) | while read -r p; do
        rel="${p#.}"
        [ "$rel" = "/dev/console" ] || [ "$rel" = "/dev/null" ] && continue
        if [ -L "$STAGE$rel" ]; then
            echo "slink $rel $(readlink "$STAGE$rel") 0777 0 0"
        elif [ -d "$STAGE$rel" ]; then
            mode="$(stat -c %a "$STAGE$rel")"
            case "$rel" in /tmp) mode=1777 ;; esac
            echo "dir $rel $mode 0 0"
        elif [ -f "$STAGE$rel" ]; then
            echo "file $rel $STAGE$rel $(stat -c %a "$STAGE$rel") 0 0"
        fi
    done
} > "$SPEC"

mkdir -p "$(dirname "$OUTFILE")"
"$GEN_CPIO" "$SPEC" | gzip -9 > "$OUTFILE.tmp"
mv "$OUTFILE.tmp" "$OUTFILE"
rm -f "$SPEC"
log "initramfs: $OUTFILE ($(du -h "$OUTFILE" | cut -f1), $(find "$STAGE" | wc -l) 个条目)"
