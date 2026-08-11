// wpa_ctrl_client.cpp — wpa_supplicant 控制接口的 C++ socket 实现
#include "p2p/wpa_ctrl_client.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>

WpaCtrlClient::~WpaCtrlClient() {
    close();
}

void WpaCtrlClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!local_path_.empty()) {
        ::unlink(local_path_.c_str());
        local_path_.clear();
    }
}

bool WpaCtrlClient::open(const std::string& ctrl_path) {
    fd_ = ::socket(PF_UNIX, SOCK_DGRAM, 0);
    if (fd_ < 0)
        return false;

    // bind 一个唯一的本地 socket（wpa_supplicant 据此回复）
    struct sockaddr_un local {};
    local.sun_family = AF_UNIX;
    std::snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/wpa_ctrl_%d_%ld",
                  static_cast<int>(::getpid()), static_cast<long>(::time(nullptr)));
    local_path_ = local.sun_path;
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        ::unlink(local_path_.c_str());
        local_path_.clear();
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // connect wpa_supplicant 的控制 socket
    struct sockaddr_un dest {};
    dest.sun_family = AF_UNIX;
    if (ctrl_path.size() >= sizeof(dest.sun_path)) {
        close();
        return false;
    }
    std::strncpy(dest.sun_path, ctrl_path.c_str(), sizeof(dest.sun_path) - 1);
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        close();
        return false;
    }
    return true;
}

bool WpaCtrlClient::attach() {
    return request("ATTACH") == "OK\n";
}
bool WpaCtrlClient::detach() {
    return request("DETACH") == "OK\n";
}

std::string WpaCtrlClient::request(const std::string& cmd, int timeout_sec) {
    if (fd_ < 0)
        return {};
    if (::send(fd_, cmd.c_str(), cmd.size(), 0) < 0)
        return {};

    char buf[8192];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd_, &rfds);
        struct timeval tv {
            timeout_sec, 0
        };
        int n = ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
        if (n <= 0)
            return {};  // 超时或出错
        ssize_t r = ::recv(fd_, buf, sizeof(buf) - 1, 0);
        if (r <= 0)
            return {};
        buf[r] = '\0';
        // 异步事件以 '<'（<level>EVENT）或 "IFNAME=" 开头，跳过继续等回复
        if (buf[0] == '<' || (r > 6 && std::strncmp(buf, "IFNAME=", 7) == 0))
            continue;
        return std::string(buf, static_cast<size_t>(r));
    }
}

std::string WpaCtrlClient::recv_event(int timeout_ms) {
    if (fd_ < 0)
        return {};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd_, &rfds);
    struct timeval tv {
        timeout_ms / 1000, (timeout_ms % 1000) * 1000
    };
    int n = ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
    if (n <= 0)
        return {};
    char    buf[8192];
    ssize_t r = ::recv(fd_, buf, sizeof(buf) - 1, 0);
    if (r <= 0)
        return {};
    return std::string(buf, static_cast<size_t>(r));
}

bool WpaCtrlClient::has_pending(int timeout_ms) {
    if (fd_ < 0)
        return false;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd_, &rfds);
    struct timeval tv {
        timeout_ms / 1000, (timeout_ms % 1000) * 1000
    };
    return ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv) > 0;
}
