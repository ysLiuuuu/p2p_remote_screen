// p2p_manager.h — Wi-Fi Direct P2P 管理（对应设计方案 §5.3 P2pManager）
// 支持 GO/GC 建链 + 断开感知 + 自动重连。
#pragma once
#include <string>

#include "p2p/wpa_ctrl_client.h"

class P2pManager {
   public:
    enum class State { Disconnected, Connected, GoUp, ClientUp, Reconnecting, Error };
    enum class Role { GO, GC };

    // 连接 wpa_supplicant 控制接口（ctrl_dir 如 /var/run/wd_p2p，自动拼 <ctrl_dir>/<iface>）
    // 若 ctrl 不存在，自动停 NM + 起 wpa_supplicant
    bool init(const std::string& ctrl_dir, const std::string& iface);

    // 建链（一次性）。内部存参数，供 maintain_link 重连复用。
    bool become_go(const std::string& device_name, const std::string& go_ip_cidr,
                   int wait_event_sec = 15);
    bool become_client(const std::string& device_name, const std::string& go_devname,
                       const std::string& fallback_ip, int wait_sec = 30);

    // 链路维护：监听断开事件（GROUP-REMOVED / DISCONNECTED / AP-STA-DISCONNECTED）
    //           → 自动重连（seconds<=0 无限循环）
    void maintain_link(int seconds);

    // 简单事件打印循环（无重连，仅供观察）
    void run_event_loop(int seconds);

    State              state() const { return state_; }
    const std::string& group_iface() const { return group_iface_; }

   private:
    static std::string parse_group_iface(const std::string& event);
    static std::string parse_dev_found_mac(const std::string& event);
    static bool        setup_wpa(const std::string& iface, const std::string& ctrl_dir);
    bool               connect_go();  // 建链 GO（可重入，供重连）
    bool               connect_gc();  // 建链 GC（可重入，供重连）

    WpaCtrlClient ctrl_;
    std::string   iface_;
    std::string   ctrl_dir_;
    std::string   group_iface_;
    State         state_ = State::Disconnected;
    Role          role_  = Role::GO;
    // 建链参数（供重连复用）
    std::string name_;
    std::string go_ip_;
    std::string go_devname_;
    std::string fallback_ip_;
};
