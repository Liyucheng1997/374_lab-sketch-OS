#!/bin/bash
# 第 1 步：准备工具链
#   - 安装 Ubuntu 编译依赖（gcc、内核编译工具、QEMU）
#   - 下载 Linux 内核源码
#   - 用 gcc -m32 编译 musl libc（32 位 x86 版本）
#   - 导出内核的用户态头文件（linux/input.h、drm/drm.h 等）
#   - 生成 sk-cc 包装脚本：以后所有用户态程序都用它编译
set -euo pipefail
source "$(dirname "$0")/env.sh"

mkdir -p "$WORK/src" "$WORK/toolchain/bin"

need_pkgs=()
for p in build-essential gcc-multilib flex bison bc libelf-dev libssl-dev cpio \
         qemu-system-x86 xz-utils python3 python3-pil git curl; do
    dpkg -s "$p" >/dev/null 2>&1 || need_pkgs+=("$p")
done
if [ ${#need_pkgs[@]} -gt 0 ]; then
    log "安装依赖: ${need_pkgs[*]}"
    sudo DEBIAN_FRONTEND=noninteractive apt-get update -y
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${need_pkgs[@]}"
fi

# ---- Linux 内核源码 ----
if [ ! -d "$KERNEL_SRC" ]; then
    cd "$WORK/src"
    if [ ! -f "$KERNEL_TARBALL" ]; then
        log "下载 Linux $KERNEL_VERSION ..."
        curl -fL --retry 3 -o "$KERNEL_TARBALL.part" "$KERNEL_URL"
        mv "$KERNEL_TARBALL.part" "$KERNEL_TARBALL"
    fi
    log "解压内核源码 ..."
    tar -xf "$KERNEL_TARBALL"
fi

# ---- musl libc (i386) ----
if [ ! -f "$MUSL_PREFIX/lib/libc.a" ]; then
    if [ ! -d "$MUSL_SRC" ]; then
        log "下载 musl $MUSL_VERSION ..."
        git clone -q --depth 1 -b "v$MUSL_VERSION" "$MUSL_GIT" "$MUSL_SRC"
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
if [ ! -f "$KHDR/include/linux/input.h" ]; then
    log "导出内核头文件 ..."
    # 用 O= 在源码树外生成，保持源码树干净（否则后面 O= 编译内核会报 "source tree is not clean"）
    make -C "$KERNEL_SRC" O="$WORK/build/khdr" ARCH=i386 headers_install INSTALL_HDR_PATH="$KHDR" >/dev/null
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
