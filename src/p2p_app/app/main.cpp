// main.cpp — 读取 JSON 配置文件启动
// 用法: p2p_manager [config_path]   默认 /etc/wireless-display/config.json
#include <spdlog/spdlog.h>

#include "ble/ble_manager.h"
#include "p2p/p2p_manager.h"
#include "utils/config.h"
#include "utils/logger.h"

int main(int argc, char** argv) {
    init_logger();

    std::string config_path = (argc > 1) ? argv[1] : "/etc/wireless-display/config.json";
    Config      c           = load_config(config_path);

    BleManager ble;
    if (c.ble_enabled &&
        !ble.start(c.ble_adapter, c.ble_name, config_path,
                   [](const std::string& characteristic, const nlohmann::json&) {
                       spdlog::info("[ble] {} 已处理，配置将在相关模块重载后生效",
                                    characteristic);
                   }))
        spdlog::warn("BLE 配网服务启动失败，P2P 仍继续启动");

    P2pManager pm;
    if (!pm.init(c.ctrl_dir, c.iface)) {
        spdlog::error("连接 wpa_supplicant 失败");
        return 1;
    }

    if (is_go(c.role)) {
        if (!pm.become_go(c.device_name, c.go.ip)) {
            spdlog::error("become GO 失败");
            return 2;
        }
    } else {
        if (!pm.become_client(c.device_name, c.gc.go_devname, c.gc.fallback_ip)) {
            spdlog::error("become GC 失败");
            return 2;
        }
    }

    spdlog::info("{} iface={}", role_name(c.role), pm.group_iface());
    ble.update_status({{"network", "connected"},
                       {"role", role_name(c.role)},
                       {"interface", pm.group_iface()},
                       {"media", "idle"},
                       {"error", nullptr}});
    pm.maintain_link(c.loop_sec);
    return 0;
}
