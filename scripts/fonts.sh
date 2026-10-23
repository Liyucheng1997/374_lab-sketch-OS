#!/bin/bash
# 生成系统字体：下载霞鹜文楷 (LXGW WenKai, SIL OFL 1.1) → 用 tools/mkfont.py 光栅化成 .skf
#
#   scripts/fonts.sh             下载（已缓存则跳过）并重新生成 assets/fonts/ 和内置后备字体
#   scripts/fonts.sh --download  只下载
#
# 为什么要“预先光栅化”？
#   TrueType 字体里存的是轮廓（贝塞尔曲线），显示前要经过 FreeType 这样的光栅化引擎
#   把曲线变成像素。完整的 FreeType 有几十万行代码，而我们的系统只用 4 个固定字号，
#   所以干脆在构建时（宿主机上，用 Python 的 Pillow 调 FreeType）把每个字都画成灰度位图，
#   存成我们自己的 .skf 格式。系统运行时只需要 mmap 文件、查表、贴图，非常简单快速。
#
# 产物（都会提交进仓库，平时不需要重新生成）：
#   assets/fonts/{sans-16,sans-20,sans-32,mono-16}.skf
#   assets/fonts/OFL.txt、assets/fonts/README.md
#   userspace/gui/libsketch/font_builtin.c   内置 ASCII 后备字体（字体文件缺失时用）
set -euo pipefail
source "$(dirname "$0")/env.sh"

FILES=(LXGWWenKai-Regular.ttf LXGWWenKai-Medium.ttf LXGWWenKaiMono-Regular.ttf LXGWWenKaiMono-Medium.ttf)
OFL_URL="https://raw.githubusercontent.com/lxgw/LxgwWenKai/${FONT_VERSION}/OFL.txt"

# 下载一个文件到缓存；已存在且非空就跳过。先下到 .part 再改名，
# 这样中途断网不会留下一个“看起来存在”的残缺文件。
fetch() {
    local url="$1" dst="$2"
    if [ -s "$dst" ]; then
        log "已缓存: $(basename "$dst")"
        return 0
    fi
    log "下载 $url"
    curl -fsSL --retry 3 --retry-delay 2 --connect-timeout 30 -o "$dst.part" "$url" \
        || die "下载失败: $url （需要能访问 github.com；也可以手动把文件放到 $FONT_CACHE/）"
    mv "$dst.part" "$dst"
}

mkdir -p "$FONT_CACHE"
for f in "${FILES[@]}"; do
    fetch "$FONT_BASE_URL/$f" "$FONT_CACHE/$f"
done
fetch "$OFL_URL" "$FONT_CACHE/OFL.txt"

# 简单的完整性检查：TTF 文件以 0x00010000 或 "true" 开头
for f in "${FILES[@]}"; do
    magic="$(head -c 4 "$FONT_CACHE/$f" | od -An -tx1 | tr -d ' \n')"
    case "$magic" in
        00010000|74727565) ;;
        *) rm -f "$FONT_CACHE/$f"; die "$f 不是有效的 TrueType 文件（已删除缓存，请重试）" ;;
    esac
done
(cd "$FONT_CACHE" && sha256sum "${FILES[@]}" OFL.txt)

[ "${1:-}" = "--download" ] && exit 0

python3 -c 'import PIL' 2>/dev/null || die "需要 python3-pil（sudo apt install python3-pil）"
python3 "$ROOT/tools/mkfont.py" \
    --src "$FONT_CACHE" \
    --root "$ROOT" \
    --out "$ROOT/assets/fonts" \
    --builtin "$ROOT/userspace/gui/libsketch/font_builtin.c"
log "字体生成完毕: $ROOT/assets/fonts"
ls -l "$ROOT/assets/fonts"
