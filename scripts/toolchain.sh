#!/bin/bash
# 第 1 步：准备工具链
#   - 安装 Ubuntu 编译依赖（gcc、内核编译工具、QEMU）
#   - 下载 Linux 内核源码
#   - 用 gcc -m32 编译 musl libc（32 位 x86 版本）
#   - 导出内核的用户态头文件（linux/input.h、drm/drm.h 等）
#   - 生成 sk-cc 包装脚本：以后所有用户态程序都用它编译
#
# 网络受限时（例如云端环境只允许访问 Ubuntu 软件源）的后备方案：
#   - 下载不到内核源码：内核头文件改用发行版的 linux-libc-dev。用户态程序照样能编译、
#     测试（tests/skbox、shell、ui、widgets、apps），但编译内核、打包 initramfs、
#     QEMU/v86 测试要等能访问 cdn.kernel.org 之后再做。
#   - 拉不到 musl 的 git 仓库：从 Ubuntu 软件源下载 musl 上游发布的源码包（校验 sha256）。
#
# 环境变量 SKETCHOS_NO_QEMU=1：不装 QEMU（连同它依赖的几十个图形、固件包）。只构建、不在本机
# 运行虚拟机时用，例如 GitHub Actions 构建网页版。
set -euo pipefail
source "$(dirname "$0")/env.sh"

mkdir -p "$WORK/src" "$WORK/toolchain/bin"

pkgs=(build-essential gcc-multilib flex bison bc libelf-dev libssl-dev cpio xz-utils python3 python3-pil git curl)
[ "${SKETCHOS_NO_QEMU:-0}" = 1 ] || pkgs+=(qemu-system-x86)
need_pkgs=()
for p in "${pkgs[@]}"; do
    dpkg -s "$p" >/dev/null 2>&1 || need_pkgs+=("$p")
done
if [ ${#need_pkgs[@]} -gt 0 ]; then
    log "安装依赖: ${need_pkgs[*]}"
    sudo DEBIAN_FRONTEND=noninteractive apt-get update -y
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${need_pkgs[@]}"
fi

# ---- Linux 内核源码 ----
HAVE_KSRC=1
if [ ! -d "$KERNEL_SRC" ]; then
    cd "$WORK/src"
    if [ ! -f "$KERNEL_TARBALL" ]; then
        log "下载 Linux $KERNEL_VERSION ..."
        if curl -fL --retry 3 -o "$KERNEL_TARBALL.part" "$KERNEL_URL"; then
            mv "$KERNEL_TARBALL.part" "$KERNEL_TARBALL"
        else
            rm -f "$KERNEL_TARBALL.part"
            HAVE_KSRC=0
            warn "下载内核源码失败（网络受限？）：先只准备用户态工具链，内核头文件改用发行版的 linux-libc-dev。"
            warn "编译内核（scripts/kernel.sh）、打包 initramfs 和 QEMU 测试需要能访问 $KERNEL_URL"
        fi
    fi
    if [ "$HAVE_KSRC" = 1 ]; then
        log "解压内核源码 ..."
        tar -xf "$KERNEL_TARBALL"
    fi
fi

# ---- musl libc (i386) ----
if [ ! -f "$MUSL_PREFIX/lib/libc.a" ]; then
    if [ ! -d "$MUSL_SRC" ]; then
        log "下载 musl $MUSL_VERSION ..."
        if ! git clone -q --depth 1 -b "v$MUSL_VERSION" "$MUSL_GIT" "$MUSL_SRC"; then
            # 后备：Ubuntu 软件源里的 musl 原始源码包，内容就是 musl 上游发布的 tar 包，
            # 用 sha256 确认它没被改过（和 musl 官网 musl-1.2.4.tar.gz 的校验值相同）
            rm -rf "$MUSL_SRC"
            warn "无法访问 $MUSL_GIT，改从 Ubuntu 软件源下载 musl $MUSL_FALLBACK_VERSION 的源码包"
            tgz="$WORK/src/musl_$MUSL_FALLBACK_VERSION.orig.tar.gz"
            curl -fL --retry 3 -o "$tgz.part" "$MUSL_FALLBACK_URL"
            echo "$MUSL_FALLBACK_SHA256  $tgz.part" | sha256sum -c --quiet - || die "musl 源码包校验失败"
            mv "$tgz.part" "$tgz"
            tar -xzf "$tgz" -C "$WORK/src"
            mv "$WORK/src/musl-$MUSL_FALLBACK_VERSION" "$MUSL_SRC"
        fi
    fi
    log "编译 musl (i386) ..."
    cd "$MUSL_SRC"
    make distclean >/dev/null 2>&1 || true
    ./configure --prefix="$MUSL_PREFIX" --target=i386 --disable-shared \
        CC="gcc -m32" AR=ar RANLIB=ranlib CROSS_COMPILE= CFLAGS="-O2 -march=i686" >/dev/null
    make -j"$JOBS" >/dev/null
    make install >/dev/null
fi

# ---- 内核用户态头文件 ----
# 上次用的是发行版头文件（.from-distro 标记）、现在有了内核源码：重新导出，和内核版本保持一致
if [ "$HAVE_KSRC" = 1 ] && [ -f "$KHDR/.from-distro" ]; then
    rm -rf "$KHDR"
fi
if [ ! -f "$KHDR/include/linux/input.h" ]; then
    if [ "$HAVE_KSRC" = 1 ]; then
        log "导出内核头文件 ..."
        # 用 O= 在源码树外生成，保持源码树干净（否则后面 O= 编译内核会报 "source tree is not clean"）
        make -C "$KERNEL_SRC" O="$WORK/build/khdr" ARCH=i386 headers_install INSTALL_HDR_PATH="$KHDR" >/dev/null
    else
        # linux-libc-dev 是 64 位发行版的包，但 x86 的 asm/ 头文件同时支持 32 位和 64 位
        # （按 __i386__ 选择 unistd_32.h 之类），所以 gcc -m32 编译时一样能用
        log "使用发行版的内核头文件（/usr/include，linux-libc-dev）..."
        mkdir -p "$KHDR/include"
        for d in linux asm-generic drm mtd sound video; do
            if [ -d "/usr/include/$d" ]; then cp -a "/usr/include/$d" "$KHDR/include/"; fi
        done
        cp -a "/usr/include/$(gcc -print-multiarch)/asm" "$KHDR/include/asm"
        touch "$KHDR/.from-distro"
    fi
fi

# ---- sk-cc 包装脚本 ----
cat > "$SKCC" <<EOF
#!/bin/sh
# SketchOS 交叉编译器：生成静态链接的 32 位 x86 (i686) Linux 程序，C 库是 musl。
exec gcc -m32 -march=i686 -mtune=generic -fno-pie -no-pie -static -Wl,-melf_i386 \\
    -specs "$MUSL_PREFIX/lib/musl-gcc.specs" \\
    -isystem "$KHDR/include" "\$@"
EOF
chmod +x "$SKCC"

# musl-gcc.specs 覆盖了 gcc 的 link spec，丢掉了 -m32 对应的 "-m elf_i386"，所以上面手动加了 -Wl,-melf_i386
# 冒烟测试
tmp="$(mktemp -d)"
cat > "$tmp/t.c" <<'EOF'
#include <stdio.h>
#include <linux/input.h>
int main(void) { printf("hello %d\n", (int)sizeof(struct input_event)); return 0; }
EOF
"$SKCC" -O2 -o "$tmp/t" "$tmp/t.c"
file_info="$(file "$tmp/t" 2>/dev/null || true)"
rm -rf "$tmp"
log "工具链就绪: $SKCC"
[ -n "$file_info" ] && log "  测试程序: ${file_info#*: }"
