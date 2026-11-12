# Debian/Ubuntu multiarch 交叉编译公共设置
#
# 本文件不直接使用，由 linux-<arch>.cmake 预设以下变量后 include：
#   FDEBM_CROSS_TRIPLET    目标 GNU 三元组，如 aarch64-linux-gnu
#   FDEBM_CROSS_PROCESSOR  CMake 目标处理器名，如 aarch64

if(NOT FDEBM_CROSS_TRIPLET OR NOT FDEBM_CROSS_PROCESSOR)
    message(FATAL_ERROR "请通过 cmake/toolchains/linux-<arch>.cmake 使用本文件")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR "${FDEBM_CROSS_PROCESSOR}")
set(CMAKE_LIBRARY_ARCHITECTURE "${FDEBM_CROSS_TRIPLET}")

set(CMAKE_C_COMPILER "${FDEBM_CROSS_TRIPLET}-gcc")
set(CMAKE_CXX_COMPILER "${FDEBM_CROSS_TRIPLET}-g++")
set(CMAKE_AR "${FDEBM_CROSS_TRIPLET}-ar")
set(CMAKE_RANLIB "${FDEBM_CROSS_TRIPLET}-ranlib")
set(CMAKE_STRIP "${FDEBM_CROSS_TRIPLET}-strip")

# 目标架构的 Qt5 配置位于 multiarch 目录，如 /usr/lib/aarch64-linux-gnu/cmake/Qt5
# 工具链文件会在 cmake_minimum_required 之前被多次读入（策略仍为 OLD，不可用 IN_LIST），
# 因此这里用策略无关的写法去重，避免 CMAKE_PREFIX_PATH 重复项
set(_fdebm_cross_prefix "/usr/lib/${FDEBM_CROSS_TRIPLET}/cmake")
string(FIND "${CMAKE_PREFIX_PATH}" "${_fdebm_cross_prefix}" _fdebm_prefix_pos)
if(_fdebm_prefix_pos EQUAL -1)
    list(APPEND CMAKE_PREFIX_PATH "${_fdebm_cross_prefix}")
endif()

# 目标库与头文件同时位于 /usr/<triplet> 与 /usr/lib/<triplet> 两处
set(CMAKE_FIND_ROOT_PATH "/usr/${FDEBM_CROSS_TRIPLET}" "/usr")

# moc/uic/rcc 必须是宿主架构的可执行文件，因此程序查找不重定向
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# 可选：安装 qemu-user-static 后，cmake -DBUILD_TESTING=ON 的 ctest 可直接运行交叉产物
if(NOT DEFINED CMAKE_CROSSCOMPILING_EMULATOR)
    find_program(FDEBM_CROSS_EMULATOR
        "qemu-${FDEBM_CROSS_PROCESSOR}-static"
        "qemu-${FDEBM_CROSS_PROCESSOR}")
    if(FDEBM_CROSS_EMULATOR)
        set(CMAKE_CROSSCOMPILING_EMULATOR
            "${FDEBM_CROSS_EMULATOR};-L;/usr/${FDEBM_CROSS_TRIPLET}")
    endif()
endif()
