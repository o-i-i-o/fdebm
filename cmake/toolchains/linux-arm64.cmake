# 交叉编译到 Linux arm64（典型宿主：amd64 / x86_64）
#
# 前置（在 Debian/Ubuntu 宿主上，需要 root）：
#   sudo dpkg --add-architecture arm64
#   sudo apt install crossbuild-essential-arm64 qtbase5-dev:arm64 qtbase5-dev-tools
#
# 用法：
#   cmake -S . -B build-arm64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm64.cmake
#   cmake --build build-arm64 -j"$(nproc)"
#
# 交叉编译时测试默认关闭；如需在 qemu 下运行，安装 qemu-user-static 后
# 显式加 -DBUILD_TESTING=ON。

set(FDEBM_CROSS_TRIPLET aarch64-linux-gnu)
set(FDEBM_CROSS_PROCESSOR aarch64)

include("${CMAKE_CURRENT_LIST_DIR}/debian-multiarch-cross.cmake")
