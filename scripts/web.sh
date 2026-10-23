#!/bin/bash
# 网页版资源：把 v86 模拟器、BIOS 和系统镜像放进 web/，让浏览器能直接启动 SketchOS
#
#   scripts/web.sh [--kernel 文件] [--initrd 文件] [--lib-only] [--force]
#
#   --kernel    内核镜像，默认 out/bzImage
#   --initrd    根文件系统镜像，默认 out/initramfs.cpio.gz（测试时可以换成自己打包的）
#   --lib-only  只准备 web/lib/（模拟器 + BIOS），不复制系统镜像
#   --force     忽略缓存，重新下载 v86 的 npm 包
#
# 产物（都是生成的，不要手工修改，已在 .gitignore 里）：
#   web/lib/     libv86.js libv86.mjs v86.wasm v86-fallback.wasm   ← npm 包 v86@$V86_NPM_VERSION
#                seabios.bin vgabios.bin                           ← v86 仓库自带的 BIOS
#                v86-LICENSE.txt seabios-COPYING.LESSER.txt        ← 许可证（BSD-2 / LGPLv3）
#   web/images/  bzImage initramfs.cpio.gz manifest.json
#
# 为什么要这些文件？
#   v86 是一个用 JavaScript + WebAssembly 写的 x86 PC 模拟器：v86.wasm 里是 CPU 模拟器和
#   把 x86 机器码即时编译成 WebAssembly 的 JIT，libv86.js 模拟其余硬件（显卡、PS/2、串口……）。
#   一台 PC 开机先运行 BIOS（SeaBIOS）做硬件初始化，显卡自己还有一段 VGA BIOS；
#   然后 v86 像 QEMU 的 -kernel 参数那样，直接把 bzImage 和 initramfs 放进内存启动 Linux。
#   npm 包里没有 BIOS，所以 BIOS 从 v86 的 git 仓库里拿。
set -euo pipefail
source "$(dirname "$0")/env.sh"

KERNEL_IMG="$OUT/bzImage"
INITRD_IMG="$OUT/initramfs.cpio.gz"
LIB_ONLY=0
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --kernel)   KERNEL_IMG="$2"; shift 2 ;;
        --initrd)   INITRD_IMG="$2"; shift 2 ;;
        --lib-only) LIB_ONLY=1; shift ;;
        --force)    FORCE=1; shift ;;
        -h|--help)  sed -n '2,12p' "$0"; exit 0 ;;
        *) die "未知参数: $1" ;;
    esac
done

WEB="$ROOT/web"
LIB="$WEB/lib"
IMAGES="$WEB/images"
CACHE="$WORK/cache/v86-npm/$V86_NPM_VERSION"
TGZ="$CACHE/v86-$V86_NPM_VERSION.tgz"
PKG="$CACHE/package"

command -v curl >/dev/null || die "需要 curl（sudo apt install curl）"
command -v python3 >/dev/null || die "需要 python3"

# 先写到临时文件再改名：浏览器可能正开着页面，改名是原子的，不会读到写了一半的文件
install_atomic() {   # install_atomic 源文件 目标文件
    cp --preserve=timestamps "$1" "$2.tmp.$$"
    chmod 0644 "$2.tmp.$$"
    mv -f "$2.tmp.$$" "$2"
}

# ---------- 1. v86 的 npm 包 ----------
fetch_v86_npm() {
    mkdir -p "$CACHE"
    local meta_url="https://registry.npmjs.org/v86/$V86_NPM_VERSION"
    local tgz_url="https://registry.npmjs.org/v86/-/v86-$V86_NPM_VERSION.tgz"
    if [ "$FORCE" = 1 ]; then
        rm -rf "$PKG" "$TGZ"
    fi
    if [ ! -f "$TGZ" ]; then
        log "下载 v86@$V86_NPM_VERSION: $tgz_url"
        curl -fL --retry 3 --connect-timeout 20 -o "$TGZ.part" "$tgz_url" || die "下载 v86 npm 包失败（检查网络）"
        # 用 npm 仓库元数据里的 sha512 校验下载内容，防止下到损坏或被篡改的包
        local integrity
        if integrity="$(curl -fsL --retry 3 --connect-timeout 20 "$meta_url" |
                python3 -I -c 'import json,sys; print(json.load(sys.stdin)["dist"]["integrity"])')"; then
            local actual
            actual="sha512-$(openssl dgst -sha512 -binary "$TGZ.part" | base64 -w0)"
            [ "$actual" = "$integrity" ] || { rm -f "$TGZ.part"; die "v86 npm 包校验失败: $actual != $integrity"; }
            log "sha512 校验通过"
        else
            warn "拿不到 npm 元数据，跳过 sha512 校验"
        fi
        mv "$TGZ.part" "$TGZ"
        rm -rf "$PKG"
    fi
    if [ ! -d "$PKG" ]; then
        # 解压到缓存目录下单独的子目录里；包里只有数据文件，我们不执行其中任何东西
        tar -xzf "$TGZ" -C "$CACHE" --no-same-owner --no-same-permissions
    fi
    for f in libv86.js libv86.mjs v86.wasm v86-fallback.wasm; do
        [ -f "$PKG/build/$f" ] || die "npm 包里缺少 build/$f（版本 $V86_NPM_VERSION 不对？）"
    done
}

# ---------- 2. v86 git 仓库（BIOS） ----------
fetch_v86_git() {
    if [ ! -f "$V86_SRC/bios/seabios.bin" ]; then
        log "克隆 v86 仓库到 $V86_SRC（只为了拿 BIOS 文件）"
        command -v git >/dev/null || die "需要 git"
        rm -rf "$V86_SRC"
        mkdir -p "$(dirname "$V86_SRC")"
        git clone --depth 1 "$V86_GIT" "$V86_SRC" || die "克隆 $V86_GIT 失败"
    fi
    for f in seabios.bin vgabios.bin COPYING.LESSER; do
        [ -f "$V86_SRC/bios/$f" ] || die "v86 仓库里缺少 bios/$f"
    done
}

fetch_v86_npm
fetch_v86_git

mkdir -p "$LIB"
for f in libv86.js libv86.mjs v86.wasm v86-fallback.wasm; do
    install_atomic "$PKG/build/$f" "$LIB/$f"
done
install_atomic "$V86_SRC/bios/seabios.bin" "$LIB/seabios.bin"
install_atomic "$V86_SRC/bios/vgabios.bin" "$LIB/vgabios.bin"
install_atomic "$PKG/LICENSE" "$LIB/v86-LICENSE.txt"
install_atomic "$V86_SRC/bios/COPYING.LESSER" "$LIB/seabios-COPYING.LESSER.txt"
log "web/lib: v86 $V86_NPM_VERSION + SeaBIOS（$(du -sh "$LIB" | cut -f1)）"

[ "$LIB_ONLY" = 1 ] && exit 0

# ---------- 3. 系统镜像 ----------
[ -f "$KERNEL_IMG" ] || die "找不到内核 $KERNEL_IMG，请先运行 scripts/kernel.sh"
[ -f "$INITRD_IMG" ] || die "找不到根文件系统 $INITRD_IMG，请先运行 scripts/userspace.sh 和 scripts/initramfs.sh"
# initramfs 已经是 gzip 压缩过的，内核启动时自己解压，所以这里原样复制即可
gzip -t "$INITRD_IMG" 2>/dev/null || die "$INITRD_IMG 不是有效的 gzip 文件"

mkdir -p "$IMAGES"
install_atomic "$KERNEL_IMG" "$IMAGES/bzImage"
install_atomic "$INITRD_IMG" "$IMAGES/initramfs.cpio.gz"

# manifest.json：网页先读它，确认镜像存在并显示大小；sha256 方便核对是哪一次构建
python3 -I - "$IMAGES" "$V86_NPM_VERSION" "$KERNEL_VERSION" <<'PY' > "$IMAGES/manifest.json.tmp"
import hashlib, json, os, sys, time
images, v86_ver, kver = sys.argv[1:4]
def info(name):
    p = os.path.join(images, name)
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    st = os.stat(p)
    return {"size": st.st_size, "sha256": h.hexdigest(),
            "mtime": time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(st.st_mtime))}
manifest = {
    "name": "SketchOS",
    "built": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    "kernel_version": kver,
    "v86_version": v86_ver,
    "files": {n: info(n) for n in ("bzImage", "initramfs.cpio.gz")},
}
json.dump(manifest, sys.stdout, ensure_ascii=False, indent=2)
sys.stdout.write("\n")
PY
mv -f "$IMAGES/manifest.json.tmp" "$IMAGES/manifest.json"
chmod 0644 "$IMAGES/manifest.json"
log "web/images: bzImage $(du -h "$IMAGES/bzImage" | cut -f1)，initramfs $(du -h "$IMAGES/initramfs.cpio.gz" | cut -f1)"
log "完成。Windows 下双击 windows/启动网页版.bat，或运行 python3 tools/serve.py"
