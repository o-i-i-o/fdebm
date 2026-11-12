cmake_minimum_required(VERSION 3.16)
project(fdebm VERSION 1.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 COMPONENTS Widgets DBus REQUIRED)

# 交叉编译默认不构建测试：测试目标依赖目标架构的 Qt5::Test，产物也无法在宿主直接执行。
# 安装了 qemu-user-static 时可显式加 -DBUILD_TESTING=ON（工具链会自动接管模拟器）。
if(CMAKE_CROSSCOMPILING AND NOT DEFINED BUILD_TESTING)
    set(BUILD_TESTING OFF CACHE BOOL "构建测试目标" FORCE)
endif()

include(CTest)
include_directories(include)

if(CMAKE_CROSSCOMPILING)
    message(STATUS "交叉编译目标：${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}"
                   "（BUILD_TESTING=${BUILD_TESTING}）")
endif()

set(SOURCES
    src/main.cpp
    src/packagemanager.cpp
    src/helperclient.cpp
    src/mainwindow.cpp
    src/operationprogressdialog.cpp
    src/common/programpaths.cpp
)

set(HEADERS
    include/packagemanager.h
    include/helperclient.h
    include/mainwindow.h
    include/operationprogressdialog.h
    src/common/programpaths.h
)

add_executable(fdebm ${SOURCES} ${HEADERS})

target_include_directories(fdebm PRIVATE src/common)
target_link_libraries(fdebm Qt5::Widgets Qt5::DBus)
target_compile_definitions(fdebm PRIVATE FDEBM_VERSION="${PROJECT_VERSION}")

install(TARGETS fdebm RUNTIME DESTINATION bin)

set(HELPER_SOURCES
    src/helper/main.cpp
    src/helper/aptbackend.cpp
    src/helper/helperservice.cpp
    src/helper/localpackages.cpp
    src/helper/polkitauthorizer.cpp
    src/common/packageargs.cpp
    src/common/programpaths.cpp
    src/common/proxyconfig.cpp
)

set(HELPER_HEADERS
    src/helper/aptbackend.h
    src/helper/helperservice.h
    src/helper/localpackages.h
    src/helper/polkitauthorizer.h
    src/common/packageargs.h
    src/common/programpaths.h
    src/common/proxyconfig.h
)

add_executable(fdebm-helperd ${HELPER_SOURCES} ${HELPER_HEADERS})

target_include_directories(fdebm-helperd PRIVATE src/helper src/common)
target_link_libraries(fdebm-helperd Qt5::Core Qt5::DBus)
target_compile_definitions(fdebm-helperd PRIVATE FDEBM_VERSION="${PROJECT_VERSION}")

install(TARGETS fdebm-helperd RUNTIME DESTINATION lib/fdebm)

install(FILES packaging/org.fdebm.Helper.conf
        DESTINATION share/dbus-1/system.d)
install(FILES packaging/org.fdebm.Helper.dbus-service
        DESTINATION share/dbus-1/system-services
        RENAME org.fdebm.Helper.service)
install(FILES packaging/org.fdebm.Helper.service
        DESTINATION lib/systemd/system)
install(FILES packaging/org.fdebm.helper.policy
        DESTINATION share/polkit-1/actions)
install(FILES packaging/fdebm.desktop
        DESTINATION share/applications)
install(FILES packaging/fdebm.svg
        DESTINATION share/icons/hicolor/scalable/apps)

if(BUILD_TESTING)
    find_package(Qt5 COMPONENTS Test REQUIRED)

    add_executable(packageargs_tests
        tests/test_packageargs.cpp
        src/common/packageargs.cpp
        src/common/programpaths.cpp
        src/common/proxyconfig.cpp
        src/helper/localpackages.cpp
    )
    target_include_directories(packageargs_tests PRIVATE src/common src/helper)
    target_link_libraries(packageargs_tests Qt5::Core)
    add_test(NAME packageargs COMMAND packageargs_tests)

    add_executable(gui_tests
        tests/test_gui.cpp
        src/mainwindow.cpp
        src/packagemanager.cpp
        src/helperclient.cpp
        src/operationprogressdialog.cpp
        src/common/programpaths.cpp
        include/mainwindow.h
        include/packagemanager.h
        include/helperclient.h
        include/operationprogressdialog.h
    )
    target_include_directories(gui_tests PRIVATE include src/common)
    target_link_libraries(gui_tests Qt5::Widgets Qt5::DBus Qt5::Test)
    add_test(NAME gui_interaction COMMAND gui_tests)

    add_executable(helperclient_tests
        tests/test_helperclient.cpp
        src/helperclient.cpp
        include/helperclient.h
    )
    target_include_directories(helperclient_tests PRIVATE include)
    target_link_libraries(helperclient_tests Qt5::DBus Qt5::Test)
    add_test(NAME helperclient COMMAND helperclient_tests)
endif()
