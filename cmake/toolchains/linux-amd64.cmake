# 交叉编译到 Linux amd64（典型宿主：arm64 / aarch64）
#
# 前置（在 Debian/Ubuntu arm64 宿主上，需要 root）：
#   sudo dpkg --add-architecture amd64
#   sudo apt install crossbuild-essential-amd64 qtbase5-dev:amd64 qtbase5-dev-tools
#
# 用法：
#   cmake -S . -B build-amd64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-amd64.cmake
#   cmake --build build-amd64 -j"$(nproc)"
#
# 交叉编译时测试默认关闭；如需在 qemu 下运行，安装 qemu-user-static 后
# 显式加 -DBUILD_TESTING=ON。

set(FDEBM_CROSS_TRIPLET x86_64-linux-gnu)
set(FDEBM_CROSS_PROCESSOR x86_64)

include("${CMAKE_CURRENT_LIST_DIR}/debian-multiarch-cross.cmake")
