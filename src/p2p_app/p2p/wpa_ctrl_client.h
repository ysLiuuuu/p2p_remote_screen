// P2P 模块 — C++ 封装 wpa_supplicant 控制接口
// 复刻 wpa_ctrl 的 unix domain socket 协议（SOCK_DGRAM + 文本），
// 不依赖 wpa_supplicant 源码（纯标准 socket）。对应设计方案 §4.2 的 wpa_ctrl。
//
// 协议要点（参考 wpa_supplicant src/common/wpa_ctrl.c）：
//   - socket(PF_UNIX, SOCK_DGRAM)：bind 一个本地客户端 socket 作为回复地址，
//     connect 到 wpa_supplicant 的 ctrl_interface。
//   - request：send 命令，recv 回复；回复若以 '<' 或 "IFNAME=" 开头是异步事件，跳过继续等。
//   - attach：订阅事件后，事件以 "<level>CTRL-EVENT-..." 异步到达。
//   - unix socket 模式无需 cookie（cookie 仅 UDP ctrl 模式需要）。
#pragma once
#include <string>

class WpaCtrlClient {
   public:
    WpaCtrlClient() = default;
    ~WpaCtrlClient();
    WpaCtrlClient(const WpaCtrlClient&)            = delete;
    WpaCtrlClient& operator=(const WpaCtrlClient&) = delete;

    // 连接 wpa_supplicant 控制接口（ctrl_path，如 /var/run/wd_p2p）
    bool open(const std::string& ctrl_path);
    void close();
    bool is_open() const { return fd_ >= 0; }
    int  fd() const { return fd_; }

    // 订阅 / 取消订阅异步事件
    bool attach();
    bool detach();

    // 发命令并取回复（自动跳过异步事件）。失败/超时返回空串。
    std::string request(const std::string& cmd, int timeout_sec = 10);

    // 读一个异步事件（需先 attach）。超时返回空串。
    std::string recv_event(int timeout_ms);
    bool        has_pending(int timeout_ms = 0);

   private:
    int         fd_ = -1;
    std::string local_path_;  // bind 的客户端 socket 文件，析构时 unlink
};
