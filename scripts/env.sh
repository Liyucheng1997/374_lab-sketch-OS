#!/bin/bash
# SketchOS 构建环境变量（被其它脚本 source）
# 所有脚本都在 WSL (Ubuntu) 里运行。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 重活（内核编译、musl 编译）放在 WSL 自己的 ext4 文件系统里：
# Windows 盘 (/mnt/x) 在 WSL 下很慢，而且内核源码里有只差大小写的文件名。
WORK="${SKETCHOS_WORK:-$HOME/sketchos-work}"

KERNEL_VERSION="${KERNEL_VERSION:-6.12.114}"
KERNEL_TARBALL="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/${KERNEL_TARBALL}"
KERNEL_SRC="$WORK/src/linux-${KERNEL_VERSION}"
KERNEL_BUILD="${KERNEL_BUILD:-$WORK/build/kernel}"

MUSL_VERSION="${MUSL_VERSION:-1.2.5}"
MUSL_GIT="https://git.musl-libc.org/git/musl"
MUSL_SRC="$WORK/src/musl"
MUSL_PREFIX="$WORK/toolchain/musl-i386"
KHDR="$WORK/toolchain/kheaders-i386"

V86_GIT="https://github.com/copy/v86.git"
V86_SRC="$WORK/src/v86"
V86_NPM_VERSION="${V86_NPM_VERSION:-0.5.472}"

FONT_VERSION="${FONT_VERSION:-v1.530}"
FONT_BASE_URL="https://github.com/lxgw/LxgwWenKai/releases/download/${FONT_VERSION}"
FONT_CACHE="$WORK/fonts"

# 交叉编译器包装脚本：gcc -m32 + musl + 内核头文件，产出静态链接的 i686 程序
SKCC="$WORK/toolchain/bin/sk-cc"

OUT="${SKETCHOS_OUT:-$ROOT/out}"   # 最终产物：bzImage、initramfs、网页镜像
ROOTFS_STAGE="$WORK/build/rootfs" # 打包 initramfs 前的根文件系统目录
JOBS="${JOBS:-$(nproc)}"

log()  { printf '\033[1;36m[sketchos]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[sketchos]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[sketchos] 错误:\033[0m %s\n' "$*" >&2; exit 1; }
