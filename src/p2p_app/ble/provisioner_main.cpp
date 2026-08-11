// 独立 BLE 配网进程，便于 systemd 单独托管或不启动 P2P 时联调。
#include <atomic>
#include <csignal>
#include <chrono>
#include <thread>

#include <spdlog/spdlog.h>

#include "ble/ble_manager.h"
#include "utils/config.h"
#include "utils/logger.h"

namespace {
std::atomic<bool> running{true};
void stop_handler(int) {
    running = false;
}
}  // namespace

int main(int argc, char** argv) {
    init_logger();
    const std::string path =
        argc > 1 ? argv[1] : "/etc/wireless-display/config.json";
    Config config = load_config(path);

    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);

    BleManager ble;
    if (!ble.start(config.ble_adapter, config.ble_name, path,
                   [](const std::string& name, const nlohmann::json&) {
                       spdlog::info("[ble] {} 配置已提交", name);
                   }))
        return 1;

    while (running && ble.running())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return ble.running() ? 0 : 2;
}
