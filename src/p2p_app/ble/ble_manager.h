// BLE 模块 — BlueZ D-Bus GATT 配网服务（设计方案 §4.3 / §5.2）
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

class BleManager {
   public:
    using WriteCallback =
        std::function<void(const std::string& characteristic, const nlohmann::json& value)>;

    BleManager();
    ~BleManager();
    BleManager(const BleManager&)            = delete;
    BleManager& operator=(const BleManager&) = delete;

    bool start(const std::string& adapter, const std::string& local_name,
               const std::string& config_path, WriteCallback callback = {});
    void stop();
    bool running() const { return running_; }
    void update_status(const nlohmann::json& status);

    static constexpr const char* service_uuid() {
        return "7b7f0000-6e7d-4c8d-9f4b-8f57e34a9100";
    }

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool>     running_{false};
    std::thread           thread_;
};
