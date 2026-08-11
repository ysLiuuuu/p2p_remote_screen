# p2p_app：应用入口、BLE、Wi-Fi Direct、系统工具和显示演示。
set(P2P_APP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# BLE 服务通过 libdbus 直接访问 BlueZ。
if(CMAKE_CROSSCOMPILING)
    # 两块板的运行时 rootfs 有 libdbus-1.so.3，但未安装 -dev 包（没有
    # libdbus-1.so 链接名）。交叉编译时直接链接 SONAME 文件；API 头文件
    # 与架构无关，取编译机的 libdbus-1-dev。
    find_path(DBUS_INCLUDE_DIR dbus/dbus.h PATHS /usr/include/dbus-1.0
        NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    find_path(DBUS_ARCH_INCLUDE_DIR dbus/dbus-arch-deps.h
        PATHS /usr/lib/x86_64-linux-gnu/dbus-1.0/include
        NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    find_file(DBUS_LIBRARY libdbus-1.so.3
        PATHS
            ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu
            ${CMAKE_SYSROOT}/lib/aarch64-linux-gnu
        NO_DEFAULT_PATH REQUIRED)
    set(DBUS_INCLUDE_DIRS ${DBUS_INCLUDE_DIR} ${DBUS_ARCH_INCLUDE_DIR})
    set(DBUS_TARGET ${DBUS_LIBRARY})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    set(DBUS_TARGET PkgConfig::DBUS)
endif()

# 公共系统能力：rtnetlink、进程管理等原生 Linux API。
add_library(p2p_app_utils STATIC
    utils/sys_utils.cpp)
target_include_directories(p2p_app_utils PUBLIC ${P2P_APP_INCLUDE_DIR})
target_link_libraries(p2p_app_utils PRIVATE spdlog::spdlog)

# Wi-Fi Direct：P2P 状态机和 wpa_supplicant 控制通道。
add_library(p2p_core STATIC
    p2p/p2p_manager.cpp
    p2p/wpa_ctrl_client.cpp)
target_include_directories(p2p_core PUBLIC ${P2P_APP_INCLUDE_DIR})
target_link_libraries(p2p_core
    PUBLIC p2p_app_utils
    PRIVATE spdlog::spdlog)

# BLE：BlueZ D-Bus GATT 配网服务，供主进程和独立配网进程复用。
add_library(ble_core STATIC
    ble/ble_manager.cpp)
target_include_directories(ble_core PUBLIC ${P2P_APP_INCLUDE_DIR})
if(CMAKE_CROSSCOMPILING)
    target_include_directories(ble_core PRIVATE ${DBUS_INCLUDE_DIRS})
endif()
target_link_libraries(ble_core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE spdlog::spdlog ${DBUS_TARGET})

# 主程序：组合 BLE 配网与 Wi-Fi Direct 建链。
add_executable(p2p_manager app/main.cpp)
target_link_libraries(p2p_manager PRIVATE
    ble_core
    p2p_core
    spdlog::spdlog
    nlohmann_json::nlohmann_json)

# 独立 BLE 配网服务。
add_executable(ble_provisioner ble/provisioner_main.cpp)
target_link_libraries(ble_provisioner PRIVATE
    ble_core
    spdlog::spdlog
    nlohmann_json::nlohmann_json)
