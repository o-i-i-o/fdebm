#!/bin/bash
# DEB 包打包脚本：编译 → 按 CMake 安装规则填充 → 生成 deb
#
# 用法: packaging/package_deb.sh [构建目录]
#   构建目录也可通过 BUILD_DIR 环境变量指定，默认为 <仓库>/build-deb。
#   脚本不依赖当前工作目录。
#
# 交叉编译: 指定 CMAKE_TOOLCHAIN_FILE 即可，包架构取自产物 ELF，不需要手工声明：
#   CMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm64.cmake \
#     packaging/package_deb.sh build-arm64
#   交叉编译必须使用全新的构建目录；包架构与产物不一致时脚本会报错。
#   DEB_ARCH 可显式覆盖包架构，仅在产物架构无法识别时需要。

set -euo pipefail

APP_NAME="fdebm"
MAINTAINER="oi-io <boss@oi-io.cc>"
HOMEPAGE="https://github.com/o-i-i-o/fdebm"
DEPENDS="apt, polkitd | policykit-1, libc6, libstdc++6, libqt5core5a, libqt5widgets5, libqt5dbus5"

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
SOURCE_DIR=$REPO_DIR
BUILD_DIR=${1:-${BUILD_DIR:-$REPO_DIR/build-deb}}

if [ ! -f "$SOURCE_DIR/CMakeLists.txt" ]; then
    echo "错误：未找到 $SOURCE_DIR/CMakeLists.txt" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)

CMAKE_ARGS=(-S "$SOURCE_DIR" -B "$BUILD_DIR" -DCMAKE_INSTALL_PREFIX=/usr)
TOOLCHAIN=""
if [ -n "${CMAKE_TOOLCHAIN_FILE:-}" ]; then
    # 相对路径先按当前目录解析，再回退到仓库根，撑住“不依赖当前工作目录”的约定
    if [ ! -f "$CMAKE_TOOLCHAIN_FILE" ] && [ -f "$SOURCE_DIR/$CMAKE_TOOLCHAIN_FILE" ]; then
        CMAKE_TOOLCHAIN_FILE="$SOURCE_DIR/$CMAKE_TOOLCHAIN_FILE"
    fi
    if [ ! -f "$CMAKE_TOOLCHAIN_FILE" ]; then
        echo "错误：未找到工具链文件 $CMAKE_TOOLCHAIN_FILE" >&2
        exit 1
    fi
    TOOLCHAIN=$(realpath -- "$CMAKE_TOOLCHAIN_FILE")
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN")
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
        cached_toolchain=$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[A-Za-z]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -1)
        if [ -z "$cached_toolchain" ]; then
            echo "错误：$BUILD_DIR 已是原生构建目录，交叉编译请指定全新的构建目录" >&2
            exit 1
        fi
        if [ "$cached_toolchain" != "$TOOLCHAIN" ]; then
            echo "错误：$BUILD_DIR 已用工具链 $cached_toolchain 配置，与本次 $TOOLCHAIN 不一致，请换全新构建目录" >&2
            exit 1
        fi
    fi
fi

echo "=== DEB 包打包脚本 ==="
echo "构建目录: $BUILD_DIR"
if [ -n "$TOOLCHAIN" ]; then
    echo "工具链: $TOOLCHAIN"
fi

echo "1. 配置与编译..."
cmake "${CMAKE_ARGS[@]}" >/dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)" >/dev/null

# 读取产物 ELF 头判定真实架构，避免交叉编译时把包标成宿主架构
# 强制 C locale：中文环境下的 readelf 会输出“系统架构”而不是 Machine
# 匹配放宽：不同 binutils 版本的 Machine 写法不一致（AArch64 / ARM aarch64 / Aarch64）
elf_arch() {
    local machine
    machine=$(LC_ALL=C readelf -h "$1" 2>/dev/null | sed -n 's/^ *Machine: *//p') || true
    case "$machine" in
        *[Xx]86-64*|*x86_64*) echo amd64 ;;
        *[Aa]arch64*) echo arm64 ;;
        *80386*) echo i386 ;;
        ARM) if [ "$(dpkg --print-architecture)" = "armel" ]; then echo armel; else echo armhf; fi ;;
        *[Rr]ISC-[Vv]*|*riscv*) echo riscv64 ;;
        *) echo "" ;;
    esac
}

BINARY_ARCH=$(elf_arch "$BUILD_DIR/$APP_NAME")
ARCH=${DEB_ARCH:-$BINARY_ARCH}

# 产物与声明不一致时绝不打包，避免产出错架构 deb
if [ -n "$BINARY_ARCH" ] && [ "$ARCH" != "$BINARY_ARCH" ]; then
    echo "错误：产物架构为 $BINARY_ARCH，与 DEB_ARCH=$ARCH 不一致" >&2
    exit 1
fi
if [ -z "$ARCH" ]; then
    # 架构未知（缺少 readelf 或遇到未识别的 ELF）：交叉编译下绝不回退到宿主架构
    if [ -n "$TOOLCHAIN" ]; then
        echo "错误：无法从产物确认架构，请安装 readelf（binutils）或显式设置 DEB_ARCH" >&2
        exit 1
    fi
    ARCH="$(dpkg --print-architecture)"
    echo "警告：未识别产物 ELF 架构，按宿主架构 $ARCH 打包" >&2
elif [ -z "$BINARY_ARCH" ]; then
    echo "警告：未校验产物 ELF 架构，直接按 DEB_ARCH=$ARCH 打包" >&2
fi

# 版本号唯一来源是 CMakeLists.txt 的 project(VERSION)，这里只读取配置结果
VERSION=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD_DIR/CMakeCache.txt")
if [ -z "$VERSION" ]; then
    echo "错误：无法从 $BUILD_DIR/CMakeCache.txt 读取项目版本" >&2
    exit 1
fi
echo "版本: $VERSION"
echo "架构: $ARCH"
DEB_DIR="$BUILD_DIR/${APP_NAME}_${VERSION}_${ARCH}"
OUTPUT_DEB="$BUILD_DIR/${APP_NAME}_${VERSION}_${ARCH}.deb"

echo "2. 安装到打包目录..."
rm -rf "$DEB_DIR"
mkdir -p "$DEB_DIR/DEBIAN"
DESTDIR="$DEB_DIR" cmake --install "$BUILD_DIR" >/dev/null

echo "3. 生成 control..."
INSTALLED_SIZE="$(du -sk "$DEB_DIR" | cut -f1)"
cat > "$DEB_DIR/DEBIAN/control" <<EOF
Package: $APP_NAME
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER
Homepage: $HOMEPAGE
Installed-Size: $INSTALLED_SIZE
Depends: $DEPENDS
Section: admin
Priority: optional
Description: DEB package manager with a polkit-authorized helper
 桌面端 DEB 包管理器。安装、卸载、更新等特权操作通过系统总线调用
 fdebm-helperd，由 polkit 完成授权，客户端无需以 root 运行。
EOF

echo "4. 生成维护脚本..."
cat > "$DEB_DIR/DEBIAN/postinst" <<'EOF'
#!/bin/bash
set -e

if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi

exit 0
EOF

cat > "$DEB_DIR/DEBIAN/postrm" <<'EOF'
#!/bin/bash
set -e

if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi

exit 0
EOF

chmod 0755 "$DEB_DIR/DEBIAN/postinst" "$DEB_DIR/DEBIAN/postrm"

echo "5. 打包..."
dpkg-deb --build --root-owner-group "$DEB_DIR" "$OUTPUT_DEB" >/dev/null
rm -rf "$DEB_DIR"

echo "=== 打包完成：$OUTPUT_DEB ==="
dpkg-deb --info "$OUTPUT_DEB" | sed -n '1,18p'
echo
echo "安装：sudo apt install $OUTPUT_DEB"
echo "卸载：sudo apt remove $APP_NAME"
echo "验证：busctl introspect org.fdebm.Helper /org/fdebm/Helper"
