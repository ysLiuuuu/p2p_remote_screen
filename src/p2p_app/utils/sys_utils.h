// Utils 模块 — 系统操作原生 API（rtnetlink + fork/execvp，不经过 shell）
#pragma once
#include <string>
#include <vector>

// ---- rtnetlink（替代 ip addr / ip link）----
// 设置网卡接口 up（IFF_UP）
bool netlink_set_link_up(const std::string& iface);
// 配置 IP 地址（替代 ip addr replace，ip_cidr 如 "192.168.49.1/24"）
bool netlink_set_addr(const std::string& iface, const std::string& ip_cidr);

// ---- 进程管理（替代 system / pkill）----
// 停止 systemd 服务（替代 systemctl stop，fork+execvp）
bool proc_stop_service(const std::string& service);
// 按进程名杀进程（替代 pkill，遍历 /proc + kill）
bool proc_kill_by_name(const std::string& name);
// 后台启动程序（替代 system，fork+execvp 不经过 shell）
bool proc_exec_background(const std::vector<std::string>& argv);
