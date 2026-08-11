# Wi-Fi Direct 与 BLE 最小 Demo 验证报告（详细版）

> 对应《无线HDMI投屏系统应用层软件设计方案》阶段一「硬件和驱动验证」中的 Wi-Fi Direct、BLE 两项。
> 目标：两块板各自把数据跑通，不做 BLE↔P2P 联动配网。
> 本文逐步骤记录每个操作、观察到的现象、遇到的问题、排查过程与解决方案，可作为可复现的工程文档。

---

## 1. 系统环境验证（起步探查）

### 1.1 确认内核 / WiFi 驱动 / 接口
对两板分别执行：
```bash
ssh orangepi@192.168.137.2 'uname -r; echo "--- ip ---"; ip -br link; echo "--- drv ---"; lsmod | grep -iE "rtw89|8852"; echo "--- iw dev ---"; /sbin/iw dev; echo "--- combos ---"; /sbin/iw list | grep -A6 "valid interface combinations"'
```
** opi 观察到：**
- 内核 `6.1.99-rockchip-rk3588`
- WiFi 驱动模块：`rtw89_8852be / rtw89_8852b / rtw89_pci / rtw89_core`（已加载）
- WiFi 接口：`wlP2p33s0`
- phy combos：`#{ managed, P2P-client } <= 2, #{ AP, P2P-GO } <= 1` —— **含 P2P-GO/client**
- P2P types：`P2P-GO / P2P-client / P2P-device` 均 advertised

**lcat 观察到：**
- 内核 `6.1.99-rk3576`，驱动同为 `rtw89_8852be`，接口 `wlan0`，P2P types 同上。

结论：两板 WiFi 驱动均支持 P2P GO/GC。

### 1.2 确认 SSH 走有线（P2P 操作不会断控制通道）
```bash
ssh orangepi@192.168.137.2 'ip -br addr; ip route'
```
- opi：SSH 走 `enP3p49s0`（192.168.137.2），WiFi 接口 `wlP2p33s0` 空闲。
- lcat：SSH 走 `eth0`（192.168.137.3），`wlan0` 空闲。

结论：停 NetworkManager、改 WiFi 接口做 P2P，**不会断 SSH**。

### 1.3 确认 Rockchip 媒体设备（后续媒体管线依赖）
```bash
ssh orangepi@192.168.137.2 'ls /dev/mpp_service /dev/rga /dev/video0 /dev/video-dec0 /dev/video-enc0'
```
全部存在：MPP 硬编解码、RGA、HDMI-IN(V4L2)、VPU 解码/编码。

### 1.4 确认工具链与蓝牙
```bash
ssh orangepi@192.168.137.2 'command -v iperf3 wpa_cli; /sbin/iw --version 2>/dev/null; hciconfig hci0; dpkg -l | grep bluez'
```
- `iperf3` 已装；`/sbin/{iw,wpa_cli,wpa_supplicant}` 齐全；bluez `5.66`。
- opi hci0：`54:AE:BC:40:2E:AD`（UP RUNNING）；lcat hci0：`A4:6B:40:32:EF:89`（UP RUNNING）。

### 1.5 环境小结
| 项 | opi（TX/GO/外设） | lcat（RX/GC/中央） |
|---|---|---|
| 内核 | 6.1.99-rockchip-rk3588 | 6.1.99-rk3576 |
| WiFi 驱动 | rtw89_8852be | rtw89_8852be |
| WiFi 接口 | wlP2p33s0 | wlan0 |
| 有线（SSH） | enP3p49s0 @ .2 | eth0 @ .3 |
| 蓝牙 hci0 | 54:AE:BC:40:2E:AD | A4:6B:40:32:EF:89 |

---

## 2. Demo 1：Wi-Fi Direct（opi=GO / lcat=GC）

### 2.1 方案与脚本
按设计方案 §4.2 用 `wpa_supplicant + wpa_cli` 控制 P2P（应用层最终用的 `wpa_ctrl` 接口）。
- `board_p2p.sh`（板端，root）：`setup | go | gc | status | teardown`
- `run.sh`（PC，ssh 协调）：`setup | go | gc | status | verify | teardown | all`

### 2.2 步骤详解

#### 步骤 1 — setup：停 NetworkManager，起独立 wpa_supplicant
板端 `board_p2p.sh setup` 实际执行：
```bash
systemctl stop NetworkManager          # 释放对 phy 的管理
pkill -x wpa_supplicant                # 干掉 NM 的 wpa 实例
# 清残留 p2p 接口
for i in $(ls /sys/class/net | grep '^p2p-'); do ip link set $i down; iw dev $i del; done
# 写配置
cat > /tmp/wd_p2p.conf <<EOF
ctrl_interface=/var/run/wd_p2p
device_type=10-0050F204-5
p2p_listen_channel=6
p2p_oper_channel=6
ap_scan=1
EOF
wpa_supplicant -B -D nl80211 -i wlP2p33s0 -c /tmp/wd_p2p.conf
```
**现象**：`wpa_cli PONG OK`；`p2p_device_address=54:ae:bc:40:2e:ac`；SSH 全程未断（有线静态 IP 保留）。两板都 setup 成功。

#### 步骤 2 — go：opi 做 GO，遇到「单接口模式」问题
执行 `wpa_cli p2p_group_add`：
- **现象**：返回 `OK`，`wpa_cli status` 显示 `mode=P2P GO`、`ssid=DIRECT-xx`、`freq=2437`。但脚本报 `FAIL: no group iface created`（`first_p2p_iface` 找不到 `p2p-wlanX-0`）。
- **排查**：`iw dev` 查看 ——
  ```
  Interface wlP2p33s0
      ssid DIRECT-l2
      type P2P-GO            ← GO 直接建在主接口上
      channel 6 (2437 MHz)
  ```
  `wpa_cli interface` 也只有 `wlP2p33s0` 一个接口。
- **根因**：rtw89 在 6.1 下用**单接口模式**，不新建 `p2p-wlanX-N`，而是把主接口直接切为 P2P-GO/client。
- **解决**：`board_p2p.sh` 增加 `group_iface()`，先找独立组接口，找不到则检测主接口 type：
  ```bash
  group_iface(){
    local g t
    g=$(first_p2p_iface); [ -n "$g" ] && { echo "$g"; return 0; }
    t=$(iw dev "$wlan" info 2>/dev/null | awk '$1=="type"{print $2}')
    case "$t" in P2P-GO|P2P-client) echo "$wlan"; return 0 ;; esac
    return 1
  }
  ```
  `wait_iface` 改为调用 `group_iface`。修后重跑：`group iface = wlP2p33s0`，配 IP `192.168.49.1`，dnsmasq 起在 `wlP2p33s0`。

#### 步骤 3 — wps_pbc：autonomous GO 必须显式激活（决定性问题）
首次让 GC 连接时遇到障碍，逐项排查：

**尝试 A**：GC 用 `p2p_connect <mac> pbc`（无 join）→ 卡住。
- 排查：带 `-dd` 日志重启 lcat wpa_supplicant，看到循环：
  ```
  P2P: Timeout (state=WAIT_PEER_CONNECT)
  P2P: Go to Listen state while waiting for the peer to become ready for GO Negotiation
  WPS IE Device Password ID: 4        (= PBC)
  ```
- 根因：`pbc`（无 join）发起的是 **GO Negotiation**（双方协商谁当 GO），但 opi 已是 autonomous GO、不参与协商 → 超时。

**尝试 B**：GC 改 `p2p_connect <mac> pbc join` → 接口创建了但连不上。
- 根因：autonomous GO 创建后默认不响应 GC 的 join 请求，需显式激活 WPS PBC 接受窗口。

**解决**：GO 端建组后立即 `wpa_cli ... wps_pbc`：
```
GO:   p2p_group_add → wps_pbc
GC:   p2p_find → p2p_connect <GO_mac> pbc join
```
已固化进 `board_p2p.sh` 的 `go`：`p2p_w wps_pbc`。

#### 步骤 4 — gc：lcat 加入组
lcat 执行：
```bash
wpa_cli -p /var/run/wd_p2p -i wlan0 p2p_find     # 发现 opi GO: 54:ae:bc:40:2e:ac
wpa_cli ... p2p_connect 54:ae:bc:40:2e:ac pbc join
```
**现象**（wpa 日志）：
```
p2p-wlan0-0: State: GROUP_HANDSHAKE -> COMPLETED
p2p-wlan0-0: CTRL-EVENT-CONNECTED - Connection to 54:ae:bc:40:2e:ac completed
EAPOL authentication completed - result=SUCCESS
```
GC 接口 `p2p-wlan0-0` 创建并连上 GO（lcat 用独立组接口模式，与 opi 单接口不同，`group_iface` 两种都兼容）。

#### 步骤 5 — gc DHCP：等连接完成再发（时序问题）
- **现象**：`wait_iface` 一找到 `p2p-wlan0-0` 就立即 `dhclient`，但此时 4-way handshake 可能未完成、接口 NO-CARRIER → `DHCPDISCOVER` 无回应、超时。
- **解决**：`gc` 里找到接口后，先轮询 `iw dev <gi> link` 直到出现 `connected`，再发 DHCP。
- **结果**：`DHCPACK of 192.168.49.2 from 192.168.49.1`，`p2p-wlan0-0` 获得 192.168.49.2。

#### 步骤 6 — verify：iperf3 数据通路
GO 侧 `iperf3 -s -D`（192.168.49.1），GC 侧 `iperf3 -c 192.168.49.1`。GO 关联确认：
```
iw dev wlP2p33s0 station dump → lcat 已关联，signal -46 dBm，connected time 65s
cat /tmp/wd_p2p.leases → 192.168.49.2 lubancat
```
iperf 结果：
```
TCP  GC→GO 8s : 50.1 Mbits/sec   0 重传
UDP  GC→GO 8s : 50.0 Mbits/sec   0/34528 丢包 (0%)   jitter 0.07ms
```
**反向 iperf 小坑**：脚本原用 `iperf3 --rcv-timeout 3000`，opi 的 iperf3 版本不认该参数（打印 usage）。去掉后反向也通：
```
TCP  GO→GC 8s : 45.5 Mbits/sec   0 重传
```
> iperf3 用了 `-b 50M` 限速；不限速实测 66+ Mbps。设计方案 1080p H.264 需 6–12 Mbps，余量充足。

### 2.3 回退保障
`teardown`：`pkill dnsmasq/dhclient/wpa_supplicant` → 删 p2p 接口 → `systemctl start NetworkManager`。两板恢复有线，SSH 全程未断。

---

## 3. Demo 2：BLE（btgatt 交叉编译）

### 3.1 编译环境探查
```bash
# 板上 bluez 版本 + 是否有开发头
ssh orangepi@192.168.137.2 'dpkg -l | grep bluez; ls /usr/include/bluetooth/bluetooth.h'
```
- 两板 bluez `5.66-1+deb12u2`，但 **都没有 libbluetooth-dev 头文件**。
- 交叉 sysroot 也没有用户态 `<bluetooth/bluetooth.h>`（lcat 找到的是内核 `net/bluetooth/bluetooth.h`，不是用户态）。

### 3.2 获取 bluez 源码
kernel.org 国际源慢（60s 才 1MB），换国内镜像：
```bash
curl -o bluez-5.66.tar.xz http://mirrors.ustc.edu.cn/debian/pool/main/b/bluez/bluez_5.66.orig.tar.xz
tar xf bluez-5.66.tar.xz
```
成功（1.8M）。版本与板载 bluez 5.66 一致。

### 3.3 分析 btgatt 的真实依赖
查 `Makefile.tools`：
```
tools_btgatt_server_SOURCES = tools/btgatt-server.c src/uuid-helper.c
tools_btgatt_server_LDADD   = src/libshared-mainloop.la  lib/libbluetooth-internal.la
```
关键结论：
- btgatt **不是单文件**，依赖 bluez 两个**内部库**：`libshared-mainloop`（src/shared/ 的 att/gatt-server/gatt-db/util/queue/mainloop…）+ `libbluetooth-internal`（lib/bluetooth.c hci.c sdp.c）。
- **不依赖系统 libbluetooth-dev、不依赖 ell/glib/dbus**（btgatt 用 `libshared-mainloop` 变体，自带 mainloop）。
- tarball 不自带 `configure`（需 autotools 生成）；所有源 `#include <config.h>`。

### 3.4 交叉编译流程（PC）
```bash
sudo apt install -y autoconf automake libtool pkg-config bison flex
cd bluez-5.66
./bootstrap                                  # 生成 configure
GCC=~/orangepi-build/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc
./configure --host=aarch64-linux-gnu CC="$GCC" \
    --enable-testing --enable-library \
    --disable-glib --disable-dbus --disable-udev --disable-systemd \
    --disable-cups --disable-network --disable-obex --disable-client \
    --disable-mesh --disable-btpclient --disable-monitor --disable-manpages \
    --disable-hid2hci --disable-experimental --disable-android \
    --disable-sixaxis --disable-midi --disable-nfc
make -j4 tools/btgatt-server tools/btgatt-client
```
- configure 成功生成 `config.h` + Makefile（`--disable-glib/dbus` 是 unrecognized options，被忽略但无害；glib/dbus 检测到的是 build host 的，btgatt 不链接它们）。
- 产物：`tools/btgatt-server` / `tools/btgatt-client` —— `ELF 64-bit ARM aarch64` ✅

### 3.5 部署运行（逐问题排查）

#### 问题 ①｜直接前台运行 → ssh 超时
- 现象：`ssh opi '/tmp/btgatt-server ... | head'`，btgatt-server 前台阻塞，命令 120s 超时。
- 原因：btgatt-server 是常驻进程，前台运行不退出。

#### 问题 ②｜后台启动方式 → ssh exit 255
- 尝试 `ssh opi 'sudo nohup cmd &'` / `setsid cmd &` 都报 `ssh exit 255`。
- 原因：后台进程继承 ssh 的 fd，通道无法正确关闭。
- **解决**：用 `systemd-run`（transient service，完全脱离 ssh）：
  ```bash
  systemd-run --unit=btgatt-svc /tmp/btgatt-server -i hci0
  ```

#### 问题 ③｜`-i 0` 报 Invalid adapter
- 现象：`-i 0` → `Invalid adapter: No such device`。
- 排查：源码 `btgatt-server.c:1198` —— `dev_id = hci_devid(optarg)`，`hci_devid("0")` 按**名字**找 adapter（找不到）。
- **解决**：`-i hci0`（adapter 名）。server 成功 `active (running)`。

#### 问题 ④｜client `Connecting to device...` 卡住
- 现象：`btgatt-client -d 54:AE:BC:40:2E:AD` 连接卡住。
- 排查：源码 `l2cap_le_att_listen_and_accept` —— server 只 `bind+listen` ATT channel，**不主动 advertise**，client 的 LE direct connect 无响应。
- **解决**：server 启动后 `hciconfig hci0 leadv 0`（connectable advertising）。

#### 问题 ⑤｜每次连接前要重新 leadv
- BLE 建立连接后 advertising 暂停，断开后不自动恢复。
- 解决：client 每次连接前在 server 侧重设 `hciconfig hci0 leadv 0`（已固化进 `run_ble.sh client`）。

#### 问题 ⑥｜client 报 `Destination address required!`
- 原因：btgatt-client 需在启动时用 `-d <address>` 指定目标。
- 解决：`btgatt-client -d 54:AE:BC:40:2E:AD -i hci0`。连接成功：`Connecting to device... Done`，进入 `[GATT client]#`。

#### 问题 ⑦｜`list-attributes` / `read-value` 时序
- 现象：连接后立即发命令，要么 `Unknown command`（discovery 完成会自动列出），要么 `GATT client not initialized`。
- 原因：连接 + GATT discovery 是异步的，`bt_gatt_client` 需要 discovery 完成 + service resolve 后才可用。
- 解决：连接后 `sleep 8` 等 `GATT discovery procedures complete`，`read-value` 后 `sleep 4` 等异步回调，再 `quit`：
  ```bash
  { sleep 8; printf 'read-value 0x0003\n'; sleep 4; printf 'quit\n'; } \
    | timeout 35 /tmp/btgatt-client -d 54:AE:BC:40:2E:AD -i hci0
  ```

#### 问题 ⑧｜server 无 tty 时交互 console 崩溃（重跑验证发现）
- 现象：重跑时 client `GATT discovery procedures failed - error 0x00`；查 server 日志：
  ```
  journalctl -u btgatt-svc：
  Failed to initialize console
  Started listening on ATT channel. Waiting for connections
  Connect from A4:6B:40:32:EF:89        ← client 连进来了
  Main process exited, status=1/FAILURE   ← 连接后崩溃
  ```
- 根因：`btgatt-server` 是交互式程序（src/shared/shell 的 readline console），`systemd-run` 无 tty → `Failed to initialize console`，连接处理后崩溃（首次成功有偶发性）。
- **解决**：用 `script -qfc` 提供伪终端：
  ```bash
  systemd-run --unit=btgatt-svc /usr/bin/script -qfc "/tmp/btgatt-server -i hci0" /dev/null
  ```
- 已固化进 `run_ble.sh` 的 `server` 子命令。

#### 问题 ⑨｜P2P 之后跑 BLE，hci0 状态残留（第二次重跑验证发现）
- 现象：P2P demo teardown 后紧接着跑 BLE，client `Failed to connect: Connection timed out`，第二次 `Function not implemented`。
- 原因：P2P 操作（停 NM / wpa_supplicant / 重启 NetworkManager）波及了 BT hci0 状态（WiFi 与 BT 虽独立，但系统级操作有残留）。
- 排查：`hciconfig hci0` 显示仍 UP RUNNING，但实际 LE 连接功能异常；手动 `hciconfig hci0 down/up` 重置后即恢复。
- 解决：BLE server 启动前（opi）、client 连接前（lcat）各做一次 `hciconfig hci0 down; sleep 1; hciconfig hci0 up`。已固化进 `run_ble.sh` 的 `server` 和 `client` 子命令，P2P→BLE 切换无需手动干预。

### 3.6 验证结果
修复后 client 完整跑通：
```
Connecting to device... Done
[GATT client]# GATT discovery procedures complete
  service 00001800 (GAP)        : Device Name(2a00) / Appearance(2a01) / Privacy(2aa6)
  service 00001801 (GATT)       : Service Changed(2a05+CCCD) / ...
  service 0000180a (DeviceInfo) : PnP ID(2a50)

read-value 0x0003 →
Read value (22 bytes): 56 65 72 79 20 4c 6f 6e 67 20 54 65 73 74 20 44 65 76 69 63 65 20
                     = "Very Long Test Device "   （btgatt-server 注册的默认 GAP Device Name）
```
client（lcat）成功从 server（opi）读到 Device Name，**BLE GATT 数据通路验证通过**（数据 server→client）。

### 3.7 回退
`run_ble.sh teardown`：`systemctl stop btgatt-svc` + `hciconfig hci0 noleadv` + `hci0 down/up`，恢复 hci0 由 bluetoothd 管理。

---

## 4. 关键经验汇总

**P2P（3 个坑）**
1. rtw89 用**单接口模式**：GO/GC 直接在主接口，要用 `iw dev info` 的 type 判断，而非找 `p2p-wlanX-0`。
2. autonomous GO **必须 `wps_pbc`** 才能被 `pbc join`；`pbc`（无 join）发起的 GO Negotiation 对已是 GO 的设备无效。
3. GC 要等 `iw link connected`（handshake 完成）再 DHCP。

**BLE（8 个坑）**
1. btgatt 不是单文件，依赖 bluez 内部库 → 需 autotools 编译（`bootstrap`+`configure --host`+`make`），但可 `--disable` 掉 ell/glib/dbus。
2. `btgatt-* -i` 传 adapter **名**（`hci0`），不是 index（`0`）—— `hci_devid()` 按名找。
3. btgatt-server 不 advertise，需 `hciconfig hci0 leadv 0`，且**每次连接前重设**。
4. 用 `systemd-run` 启动常驻进程，避开 ssh 后台 fd 问题（`nohup/&` 会 ssh exit 255）。
5. btgatt-server 是交互式，无 tty 会 `Failed to initialize console` 并崩溃 → 用 `script -qfc` 给伪终端。
6. btgatt-client 异步：留足时间等 `discovery complete` 与 read 回调。

**通用**
- 停 NetworkManager 期间，静态 IP 的有线口不受影响 → SSH 不断，可安全操作 WiFi。
- 两板 SSH 都走有线，WiFi/BT 操作与控制通道解耦。

---

## 5. 资产清单

```
demos/
├── P2P与BLE-验证报告.md      # 本文档
├── p2p/
│   ├── run.sh                # PC 主控：setup|go|gc|status|verify|teardown|all
│   ├── board_p2p.sh          # 板端：group_iface(单接口) + wps_pbc + 等连接 + DHCP
│   └── diag_go.sh            # P2P GO 能力诊断脚本
└── ble/
    ├── run_ble.sh            # build|deploy|server(pty)|client|teardown|all
    └── bluez-5.66/           # bluez 源码 + 交叉编译产物 tools/btgatt-{server,client}
```

两个 demo 均已**重跑验证、可稳定复现**：
- `cd demos/p2p && ./run.sh all`
- `cd demos/ble && ./run_ble.sh all`（build 一次后，日常用 `./run_ble.sh server` + `./run_ble.sh client`）
