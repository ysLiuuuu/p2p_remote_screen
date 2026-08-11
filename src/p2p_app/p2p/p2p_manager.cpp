// p2p_manager.cpp
#include "p2p/p2p_manager.h"
#include "utils/sys_utils.h"

#include <spdlog/spdlog.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>

// ---------------- wpa_supplicant 启动 / 控制接口连接 ----------------

bool P2pManager::setup_wpa(const std::string& iface, const std::string& ctrl_dir) {
    spdlog::info("[p2p] wpa_supplicant 未运行，自动启动...");
    proc_stop_service("NetworkManager");
    proc_kill_by_name("wpa_supplicant");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    //写配置文件
    std::ofstream f("/tmp/wd_p2p.conf");
    if (!f)
        return false;
    f << "ctrl_interface=" << ctrl_dir << "\n"      //指定生成的控制接口目录
      << "device_type=10-0050F204-5\n"              //设备类型
      << "ap_scan=1\n";                             //让 wpa_supplicant 接管扫描
    f.close();

    // 启动 wpa_supplicant 最常用的启动命令
    if (!proc_exec_background(
            {"wpa_supplicant", "-B", "-D", "nl80211", "-i", iface, "-c", "/tmp/wd_p2p.conf"}))
        return false;

    // 等待 ctrl socket 就绪
    // 该socket的通讯对象是本程序与wpa_supplicant进程
    std::string sock = ctrl_dir + "/" + iface;
    for (int i = 0; i < 30; ++i) {
        if (::access(sock.c_str(), F_OK) == 0) {
            spdlog::info("[p2p] wpa_supplicant 就绪 ({})", sock);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    spdlog::error("[p2p] wpa_supplicant ctrl socket 未就绪");
    return false;
}

bool P2pManager::init(const std::string& ctrl_dir, const std::string& iface) {
    iface_           = iface;
    ctrl_dir_        = ctrl_dir;
    std::string sock = ctrl_dir + "/" + iface;
    if (::access(sock.c_str(), F_OK) != 0) {
        if (!setup_wpa(iface, ctrl_dir)) {
            state_ = State::Error;
            return false;
        }
    }
    if (!ctrl_.open(sock)) {
        state_ = State::Error;
        return false;
    }
    state_ = State::Connected;
    return true;
}

// ---------------- 事件解析 ----------------

std::string P2pManager::parse_group_iface(const std::string& ev) {
    auto pos = ev.find("IFNAME=");
    if (pos != std::string::npos) {
        pos += 7;
        auto end = ev.find(' ', pos);
        return ev.substr(pos, end - pos);
    }
    pos = ev.find("GROUP-STARTED");
    if (pos != std::string::npos) {
        pos      = ev.find_first_not_of(' ', pos + 13);
        auto end = ev.find(' ', pos);
        return ev.substr(pos, end - pos);
    }
    return {};
}

std::string P2pManager::parse_dev_found_mac(const std::string& ev) {
    auto p = ev.find("P2P-DEVICE-FOUND");
    if (p == std::string::npos)
        return {};
    p = ev.find_first_not_of(' ', p + 16);
    if (p == std::string::npos)
        return {};
    auto e = ev.find(' ', p);
    return ev.substr(p, e - p);
}

// ---------------- 建链（可重入，供首次建链与重连复用）----------------

bool P2pManager::connect_go() {
    if (!ctrl_.is_open())
        return false;
    if (!ctrl_.attach())
        return false;
    if (!name_.empty())
        ctrl_.request("SET device_name " + name_);

    if (ctrl_.request("P2P_GROUP_ADD").empty())
        return false;
    spdlog::info("[p2p] P2P_GROUP_ADD issued");

    group_iface_.clear();
    time_t end = std::time(nullptr) + 15;
    while (std::time(nullptr) < end && group_iface_.empty()) {
        std::string ev = ctrl_.recv_event(500);
        if (ev.find("GROUP-STARTED") != std::string::npos) {
            group_iface_ = parse_group_iface(ev);
            spdlog::info("[p2p] GROUP-STARTED: {}", group_iface_);
        }
    }
    if (group_iface_.empty()) {  // rtw89 单接口模式：主接口直接变 GO
        group_iface_ = iface_;
        spdlog::info("[p2p] (单接口模式) GO on {}", iface_);
    }

    ctrl_.request("WPS_PBC");  // autonomous GO 必需，GC 才能 pbc join
    spdlog::info("[p2p] WPS_PBC active");

    if (!go_ip_.empty()) {
        netlink_set_addr(group_iface_, go_ip_);
        netlink_set_link_up(group_iface_);
        spdlog::info("[p2p] GO ip {} on {}", go_ip_, group_iface_);
    }
    state_ = State::GoUp;
    return true;
}

bool P2pManager::connect_gc() {
    if (!ctrl_.is_open())
        return false;
    if (!ctrl_.attach())
        return false;
    if (!name_.empty())
        ctrl_.request("SET device_name " + name_);

    ctrl_.request("P2P_FIND");
    spdlog::info("[p2p] p2p_find, looking for {}", go_devname_);
    std::string go_mac;
    time_t      end = std::time(nullptr) + 30;
    while (std::time(nullptr) < end && go_mac.empty()) {
        std::string ev = ctrl_.recv_event(500);
        if (ev.find("P2P-DEVICE-FOUND") != std::string::npos &&
            ev.find("name='" + go_devname_ + "'") != std::string::npos) {
            go_mac = parse_dev_found_mac(ev);
            spdlog::info("[p2p] found GO {} @ {}", go_devname_, go_mac);
        }
    }
    if (go_mac.empty()) {
        spdlog::error("[p2p] GO not found");
        return false;
    }
    ctrl_.request("P2P_STOP_FIND");

    ctrl_.request("P2P_CONNECT " + go_mac + " pbc join");
    spdlog::info("[p2p] p2p_connect {} pbc join", go_mac);

    group_iface_.clear();
    end = std::time(nullptr) + 30;
    while (std::time(nullptr) < end && group_iface_.empty()) {
        std::string ev = ctrl_.recv_event(500);
        if (ev.find("GROUP-STARTED") != std::string::npos) {
            group_iface_ = parse_group_iface(ev);
            spdlog::info("[p2p] GROUP-STARTED: {}", group_iface_);
        }
    }
    if (group_iface_.empty()) {
        spdlog::error("[p2p] no group iface");
        return false;
    }
    netlink_set_link_up(group_iface_);

    // 等 connected（超时也继续，靠配 IP 兜底）
    bool connected = false;
    end            = std::time(nullptr) + 15;
    while (std::time(nullptr) < end && !connected) {
        std::string ev = ctrl_.recv_event(500);
        if (ev.find("EAPOL-4WAY-HS-COMPLETED") != std::string::npos ||
            ev.find("CTRL-EVENT-CONNECTED") != std::string::npos)
            connected = true;
    }
    spdlog::info("[p2p] {}", connected ? "connected" : "connect pending");

    if (!fallback_ip_.empty()) {
        netlink_set_addr(group_iface_, fallback_ip_);
        spdlog::info("[p2p] GC ip {} on {}", fallback_ip_, group_iface_);
    }
    state_ = State::ClientUp;
    return true;
}

bool P2pManager::become_go(const std::string& device_name, const std::string& go_ip_cidr,
                           int /*wait_event_sec*/) {
    if (!ctrl_.is_open()) {
        state_ = State::Error;
        return false;
    }
    role_  = Role::GO;
    name_  = device_name;
    go_ip_ = go_ip_cidr;
    if (!connect_go()) {
        state_ = State::Error;
        return false;
    }
    return true;
}

bool P2pManager::become_client(const std::string& device_name, const std::string& go_devname,
                               const std::string& fallback_ip, int /*wait_sec*/) {
    if (!ctrl_.is_open()) {
        state_ = State::Error;
        return false;
    }
    role_        = Role::GC;
    name_        = device_name;
    go_devname_  = go_devname;
    fallback_ip_ = fallback_ip;
    if (!connect_gc()) {
        state_ = State::Error;
        return false;
    }
    return true;
}

// ---------------- 断开感知 + 自动重连 ----------------

void P2pManager::maintain_link(int seconds) {
    time_t end = std::time(nullptr) + seconds;
    spdlog::info("[p2p] maintain_link 启动（监听断开，自动重连）");
    while (seconds <= 0 || std::time(nullptr) < end) {
        std::string ev = ctrl_.recv_event(1000);
        // 无事件时 PING wpa_supplicant，确认 ctrl 还活着（防止 wpa 被杀后 socket
        // 断开、收不到事件"假活"）
        if (ev.empty()) {
            if (ctrl_.request("PING", 2) != "PONG\n") {
                spdlog::warn("[p2p] wpa_supplicant 无响应（ctrl 断），重新启动...");
                state_ = State::Reconnecting;
                ctrl_.close();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (setup_wpa(iface_, ctrl_dir_) && ctrl_.open(ctrl_dir_ + "/" + iface_)) {
                    bool ok = (role_ == Role::GO) ? connect_go() : connect_gc();
                    if (ok)
                        spdlog::info("[p2p] wpa 重启 + 重连完成");
                    else
                        spdlog::error("[p2p] 重连失败，继续尝试");
                }
            }
            continue;
        }
        std::string line = ev.substr(0, ev.find('\n'));
        spdlog::debug("[event] {}", line);

        bool down = ev.find("GROUP-REMOVED") != std::string::npos ||
                    ev.find("CTRL-EVENT-DISCONNECTED") != std::string::npos ||
                    ev.find("AP-STA-DISCONNECTED") != std::string::npos;
        if (!down)
            continue;

        // GO 侧 GC 断开（AP-STA-DISCONNECTED）：GO 还在，只需重 wps_pbc 让 GC 重连
        if (role_ == Role::GO && ev.find("AP-STA-DISCONNECTED") != std::string::npos) {
            spdlog::warn("[p2p] GC 断开，重新激活 WPS_PBC 等待 GC 重连");
            state_ = State::Reconnecting;
            ctrl_.request("WPS_PBC");
            state_ = State::GoUp;
            continue;
        }

        // 其他断开（GROUP-REMOVED / GC DISCONNECTED）→ 完整重连
        spdlog::warn("[p2p] 检测到断开（{}），3s 后重连...", line);
        state_ = State::Reconnecting;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        bool ok = (role_ == Role::GO) ? connect_go() : connect_gc();
        if (ok)
            spdlog::info("[p2p] 重连成功");
        else
            spdlog::error("[p2p] 重连失败，继续监听等待下次");
    }
}

// ---------------- 简单事件循环（无重连，观察用）----------------

void P2pManager::run_event_loop(int seconds) {
    time_t end = std::time(nullptr) + seconds;
    while (std::time(nullptr) < end) {
        std::string ev = ctrl_.recv_event(1000);
        if (!ev.empty()) {
            std::string line = ev.substr(0, ev.find('\n'));
            spdlog::debug("[event] {}", line);
        }
    }
}
