// Utils 模块 — JSON 配置文件加载（嵌套结构体，按角色分组）
#pragma once
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

// ---- 角色 ----
enum class Role { GO, GC };

inline const char* role_name(Role r) {
    return r == Role::GO ? "GO" : "GC";
}

inline bool is_go(Role r) {
    return r == Role::GO;
}

// ---- 配置（通用 + 角色专属子结构）----
struct Config {
    // 通用（两种角色都用）
    Role        role     = Role::GO;
    std::string ctrl_dir = "/var/run/wd_p2p";
    std::string iface;
    std::string device_name;
    int         loop_sec = 0;  // maintain 时长，0=无限
    bool        ble_enabled = true;
    std::string ble_adapter = "hci0";
    std::string ble_name;

    // GO 专属
    struct {
        std::string ip = "192.168.49.1/24";
    } go;

    // GC 专属
    struct {
        std::string go_devname  = "WD-OPI-TX";
        std::string fallback_ip = "192.168.49.100/24";
    } gc;
};

// ---- 从 JSON 文件加载配置 ----
// 格式（见 config/go.json, config/gc.json）：
//   {
//     "role": "go",
//     "iface": "...", "device_name": "...", "loop_sec": 0,
//     "go": { "ip": "192.168.49.1/24" },
//     "gc": { "go_devname": "...", "fallback_ip": "..." }
//   }
inline Config load_config(const std::string& path) {
    Config c;

    std::ifstream f(path);
    if (!f) {
        spdlog::warn("配置文件 {} 打开失败，使用默认配置（GO）", path);
        c.iface       = "wlP2p33s0";
        c.device_name = "WD-OPI-TX";
        return c;
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        spdlog::error("配置文件 JSON 解析失败: {}", e.what());
        c.iface       = "wlP2p33s0";
        c.device_name = "WD-OPI-TX";
        return c;
    }

    // 通用字段
    if (j.contains("role"))
        c.role = (j["role"] == "gc") ? Role::GC : Role::GO;
    if (j.contains("ctrl_dir"))
        c.ctrl_dir = j["ctrl_dir"].get<std::string>();
    if (j.contains("iface"))
        c.iface = j["iface"].get<std::string>();
    if (j.contains("device_name"))
        c.device_name = j["device_name"].get<std::string>();
    if (j.contains("loop_sec"))
        c.loop_sec = j["loop_sec"].get<int>();
    if (j.contains("ble_enabled"))
        c.ble_enabled = j["ble_enabled"].get<bool>();
    if (j.contains("ble_adapter"))
        c.ble_adapter = j["ble_adapter"].get<std::string>();
    if (j.contains("ble_name"))
        c.ble_name = j["ble_name"].get<std::string>();

    // GO 专属
    if (j.contains("go")) {
        auto& g = j["go"];
        if (g.contains("ip"))
            c.go.ip = g["ip"].get<std::string>();
    }

    // GC 专属
    if (j.contains("gc")) {
        auto& g = j["gc"];
        if (g.contains("go_devname"))
            c.gc.go_devname = g["go_devname"].get<std::string>();
        if (g.contains("fallback_ip"))
            c.gc.fallback_ip = g["fallback_ip"].get<std::string>();
    }

    // 未指定 iface / device_name 时按角色补默认
    if (c.iface.empty())
        c.iface = is_go(c.role) ? "wlP2p33s0" : "wlan0";
    if (c.device_name.empty())
        c.device_name = is_go(c.role) ? "WD-OPI-TX" : "WD-LCAT-RX";
    if (c.ble_name.empty())
        c.ble_name = c.device_name;

    spdlog::info("配置加载: {}  role={}  iface={}  name={}", path, role_name(c.role), c.iface,
                 c.device_name);
    return c;
}
