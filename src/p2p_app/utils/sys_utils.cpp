// sys_utils.cpp — rtnetlink + fork/execvp 原生实现
#include "utils/sys_utils.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

// ==================== rtnetlink ====================

bool netlink_set_link_up(const std::string& iface) {
    unsigned int ifindex = if_nametoindex(iface.c_str());
    if (ifindex == 0) {
        spdlog::error("[netlink] 接口 {} 不存在", iface);
        return false;
    }

    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        spdlog::error("[netlink] socket 失败");
        return false;
    }

    struct {
        struct nlmsghdr  nlh;
        struct ifinfomsg ifi;
    } req{};

    req.nlh.nlmsg_len   = sizeof(req);
    req.nlh.nlmsg_type  = RTM_NEWLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST;
    req.nlh.nlmsg_seq   = 1;
    req.ifi.ifi_family  = AF_UNSPEC;
    req.ifi.ifi_index   = ifindex;
    req.ifi.ifi_change  = IFF_UP;
    req.ifi.ifi_flags   = IFF_UP;

    struct sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;

    ssize_t n = sendto(fd, &req, sizeof(req), 0, (struct sockaddr*)&sa, sizeof(sa));
    close(fd);

    if (n < 0) {
        spdlog::error("[netlink] set_link_up({}) 发送失败", iface);
        return false;
    }
    spdlog::debug("[netlink] {} up (ifindex={})", iface, ifindex);
    return true;
}

bool netlink_set_addr(const std::string& iface, const std::string& ip_cidr) {
    // 解析 "192.168.49.1/24"
    auto slash = ip_cidr.find('/');
    if (slash == std::string::npos) {
        spdlog::error("[netlink] IP 格式错误（缺 /prefix）: {}", ip_cidr);
        return false;
    }
    std::string ip_str  = ip_cidr.substr(0, slash);
    int          prefix = std::stoi(ip_cidr.substr(slash + 1));

    unsigned int ifindex = if_nametoindex(iface.c_str());
    if (ifindex == 0) {
        spdlog::error("[netlink] 接口 {} 不存在", iface);
        return false;
    }

    struct in_addr addr{};
    if (inet_pton(AF_INET, ip_str.c_str(), &addr) != 1) {
        spdlog::error("[netlink] IP 解析失败: {}", ip_str);
        return false;
    }

    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        spdlog::error("[netlink] socket 失败");
        return false;
    }

    // 构造 RTM_NEWADDR 消息
    struct {
        struct nlmsghdr  nlh;
        struct ifaddrmsg ifa;
        char             attr_buf[64];
    } req{};

    req.nlh.nlmsg_len     = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.nlh.nlmsg_type    = RTM_NEWADDR;
    req.nlh.nlmsg_flags   = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    req.nlh.nlmsg_seq     = 1;
    req.ifa.ifa_family    = AF_INET;
    req.ifa.ifa_prefixlen = prefix;
    req.ifa.ifa_index     = ifindex;
    req.ifa.ifa_scope     = RT_SCOPE_UNIVERSE;

    // IFA_LOCAL attribute
    struct rtattr* rta = (struct rtattr*)req.attr_buf;
    rta->rta_type       = IFA_LOCAL;
    rta->rta_len        = RTA_LENGTH(sizeof(struct in_addr));
    memcpy(RTA_DATA(rta), &addr, sizeof(addr));
    req.nlh.nlmsg_len += RTA_ALIGN(rta->rta_len);

    struct sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;

    ssize_t n = sendto(fd, &req, req.nlh.nlmsg_len, 0, (struct sockaddr*)&sa, sizeof(sa));
    close(fd);

    if (n < 0) {
        spdlog::error("[netlink] set_addr({} {}) 发送失败", iface, ip_cidr);
        return false;
    }
    spdlog::debug("[netlink] {} addr {} (ifindex={})", iface, ip_cidr, ifindex);
    return true;
}

// ==================== 进程管理 ====================

bool proc_stop_service(const std::string& service) {
    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("[proc] fork 失败");
        return false;
    }
    if (pid == 0) {
        // 子进程：execvp（不经过 shell）
        execlp("systemctl", "systemctl", "stop", service.c_str(), nullptr);
        _exit(127);  // execvp 失败
    }
    // 父进程：等待退出
    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = (WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (!ok)
        spdlog::warn("[proc] systemctl stop {} 退出码={}", service, WEXITSTATUS(status));
    return ok;
}

bool proc_kill_by_name(const std::string& name) {
    DIR* dir = opendir("/proc");
    if (!dir) return false;

    int            killed = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type != DT_DIR) continue;
        int pid = atoi(ent->d_name);
        if (pid <= 0) continue;

        // 读 /proc/[pid]/comm
        std::string comm_path = "/proc/" + std::string(ent->d_name) + "/comm";
        FILE*       f         = fopen(comm_path.c_str(), "r");
        if (!f) continue;
        char comm[256] = {};
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = '\0';
            if (name == comm) {
                if (kill(pid, SIGTERM) == 0) {
                    killed++;
                    spdlog::debug("[proc] kill {} (pid={})", name, pid);
                }
            }
        }
        fclose(f);
    }
    closedir(dir);
    return killed > 0;
}

bool proc_exec_background(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;

    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("[proc] fork 失败");
        return false;
    }
    if (pid == 0) {
        // 子进程：execvp（不经过 shell）
        std::vector<char*> c_argv;
        for (auto& a : argv)
            c_argv.push_back(const_cast<char*>(a.c_str()));
        c_argv.push_back(nullptr);
        execvp(c_argv[0], c_argv.data());
        _exit(127);
    }
    // 父进程：不等待（后台）
    spdlog::debug("[proc] exec {} (pid={})", argv[0], pid);
    return true;
}
