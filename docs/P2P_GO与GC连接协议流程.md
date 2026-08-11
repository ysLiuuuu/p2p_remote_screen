# P2P GO 与 GC 连接协议流程

本文结合 `src/p2p_app/p2p/p2p_manager.cpp` 的当前实现，说明 GO（Group Owner）与 GC（Group Client）从启动 `wpa_supplicant` 到建立业务通信的完整过程，并深入描述各协议阶段发送、提供和校验的内容。

## 1. 当前连接模式

本程序采用以下模式：

```text
GO：P2P_GROUP_ADD → 创建 autonomous GO → WPS_PBC
GC：P2P_FIND → P2P_CONNECT <GO Device Address> pbc join
```

GO 预先自主创建 P2P Group，GC 再以 `join` 方式加入。因此本流程不执行 GO Negotiation：双方不交换 GO Intent，也不协商由谁担任 GO。

协议可划分为六个阶段：

1. 应用程序与 `wpa_supplicant` 建立本地控制连接；
2. GO 创建 Group 并开放 WPS PBC 配对窗口；
3. GC 使用 P2P Device Discovery 发现 GO；
4. WPS 为 GC 配置 Group 凭据；
5. GC 进行 802.11 关联和 WPA2 四次握手；
6. 双方配置 IP，开始 TCP/UDP 业务通信。

## 2. 总体时序

```text
GO 应用           GO wpa_supplicant       无线链路       GC wpa_supplicant       GC 应用
  │                       │                                    │                 │
  ├─P2P_GROUP_ADD────────>│                                    │                 │
  │                       ├─创建 autonomous Group              │                 │
  │<─P2P-GROUP-STARTED────┤                                    │                 │
  ├─WPS_PBC───────────────>│                                    │                 │
  │                       │                                    │<────P2P_FIND────┤
  │                       │<──Probe Request + P2P IE───────────┤                 │
  │                       ├──Probe Response + P2P/WPS IE──────>│                 │
  │                       │                                    ├─DEVICE-FOUND───>│
  │                       │                                    │<─CONNECT pbc────┤
  │                       │<────P2P/WPS 加入流程────────────────┤                 │
  │                       │<══════WPS EAP-WSC M1～M8══════════>│                 │
  │                       │       GC 获得 Group 凭据             │                 │
  │                       │<────802.11 Authentication───────────┤                 │
  │                       │<────Association Request + RSN IE────┤                 │
  │                       ├────Association Response────────────>│                 │
  │                       │<══════WPA2 四次握手 M1～M4═════════>│                 │
  │                       │                                    ├─CONNECTED──────>│
  ├─配置 GO IP            │                                    │      配置 GC IP─┤
  │                       │<══════ARP / IP / TCP / UDP════════>│                 │
```

## 3. 本地控制面初始化

### 3.1 生成配置并启动 `wpa_supplicant`

两端生成以下配置：

```conf
ctrl_interface=/var/run/wpa_supplicant
device_type=10-0050F204-5
ap_scan=1
```

随后执行：

```bash
wpa_supplicant -B -D nl80211 -i <iface> -c /tmp/wd_p2p.conf
```

各组件之间的通信关系为：

```text
本程序
  │ Unix Domain Socket
  ▼
wpa_supplicant
  │ nl80211 / Netlink
  ▼
Linux 无线驱动
  │ 802.11 无线帧
  ▼
对端设备
```

`ctrl_dir + "/" + iface`（例如 `/var/run/wpa_supplicant/wlan0`）只是本程序和本机 `wpa_supplicant` 之间的控制 socket，不会通过无线链路发给对端。

### 3.2 订阅事件和设置设备名称

`ctrl_.attach()` 实际向控制接口发送：

```text
ATTACH
```

成功后程序可以收到 `P2P-DEVICE-FOUND`、`P2P-GROUP-STARTED`、`CTRL-EVENT-CONNECTED` 等异步事件。

程序还会发送：

```text
SET device_name <name>
```

设备名称会出现在 P2P/WPS Information Element 中，供对端发现和显示，但设备名不是密码，也不是可靠的身份认证凭据。

## 4. GO 创建 autonomous Group

GO 应用发送：

```text
P2P_GROUP_ADD
```

`wpa_supplicant` 随后完成：

1. 选择工作信道；
2. 创建独立 P2P Group 接口，或将主接口切换为 `P2P-GO`；
3. 生成类似 `DIRECT-ab-RemoteScreen` 的 Group SSID；
4. 生成随机 Group Passphrase/PSK；
5. 启动类似 AP 的 GO 功能并发送 Beacon；
6. 通知应用：

```text
P2P-GROUP-STARTED <group_iface> GO ...
```

独立接口模式下，`group_iface` 可能是 `p2p-wlan0-0`；rtw89 单接口模式下可能直接使用主接口。

GO 的 Beacon 和 Probe Response 可携带：

- SSID、BSSID 和工作信道；
- RSN IE，描述 WPA2/CCMP 等安全能力；
- WPS IE，描述设备类型和配置方式；
- P2P IE，描述 Device Address、设备名、Group 能力等。

Group 密码不会直接放在 Beacon 中公开广播。

## 5. GO 开启 WPS PBC

GO 发送：

```text
WPS_PBC
```

此时 GO 充当 WPS Registrar，进入限时 PBC 接收状态；GC 将充当 WPS Enrollee。项目内置的 `wpa_supplicant 2.10` 将 PBC 窗口定义为 120 秒。

PBC 不是一个共享密码。它表达的是“GO 当前允许用户授权的新设备进行配网”。其安全性主要依赖有限时间窗口、用户操作和 PBC overlap 检测，而不是依赖 GC 输入秘密。

## 6. GC 发现 GO

GC 发送：

```text
P2P_FIND
```

P2P Device Discovery 主要使用 802.11 Probe Request、Probe Response，以及其中携带的 P2P/WPS Information Element。发现过程通常会使用 2.4 GHz 社交信道 1、6、11。

GO 的响应会向 GC 提供：

- P2P Device Address；
- Device Name 和 Device Type；
- 支持的 WPS Configuration Methods；
- P2P Device/Group Capability；
- 已有 Group 的相关信息。

`wpa_supplicant` 解析无线帧后向应用报告：

```text
P2P-DEVICE-FOUND <device-address> name='<device-name>' ...
```

当前程序按 `go_devname_` 匹配设备，再提取 P2P Device Address。设备名称可能重复，因此它适合发现和显示，不适合作为强身份认证手段。

## 7. GC 请求加入已有 Group

GC 找到 GO 后发送：

```text
P2P_STOP_FIND
P2P_CONNECT <GO Device Address> pbc join
```

参数含义：

- `<GO Device Address>`：目标 P2P 设备地址；
- `pbc`：使用 WPS Push Button Configuration；
- `join`：作为 GC 加入一个已经存在的 autonomous Group。

如果省略 `join`，`P2P_CONNECT <addr> pbc` 通常会尝试 GO Negotiation。由于本程序的 GO 已通过 `P2P_GROUP_ADD` 自主建组，不参与该协商，GC 可能停留在 `WAIT_PEER_CONNECT`。

## 8. WPS 配置 Group 凭据

### 8.1 WPS 承载方式

WPS 使用 EAP-WSC 协议，通常承载于 EAPOL。它和后面的 WPA 四次握手都会出现 EAPOL 帧，但用途不同：

```text
WPS：EAPOL / EAP-WSC，用于安全地配置网络凭据
WPA：EAPOL-Key，用于证明持有凭据并建立会话密钥
```

### 8.2 WPS M1～M8

简化后的交互如下：

```text
GC / Enrollee                            GO / Registrar
     │                                        │
     ├─M1：设备信息、公钥、随机数────────────>│
     │<─M2：设备信息、公钥、随机数、认证器────┤
     ├─M3：承诺值、认证器────────────────────>│
     │<─M4：承诺值、加密设置、认证器──────────┤
     ├─M5：证明信息、认证器──────────────────>│
     │<─M6：证明信息、认证器──────────────────┤
     ├─M7：加密设置、认证器──────────────────>│
     │<─M8：Group 凭据、认证器────────────────┤
     ├─Done / Ack────────────────────────────>│
```

具体实现的封装和消息细节可能存在差异，但过程中的关键材料包括：

- 双方设备信息和 Nonce；
- Diffie-Hellman 公钥；
- PBC/PIN 配置方法；
- 消息认证器和承诺值；
- 加密保护的配置数据。

### 8.3 GO 给 GC 的内容

WPS 最终为 GC 配置的 Group Credential 主要包括：

- Group SSID；
- WPA2-Personal 等安全类型；
- AES/CCMP 等加密类型；
- Group Passphrase、PSK 或等效密钥信息；
- GO/BSSID 等网络参数。

可以近似理解为“Wi-Fi 名称 + 安全方式 + Wi-Fi 密码”，但敏感配置受到 WPS 会话密钥保护，不是通过 Beacon 明文广播。

### 8.4 WPS 校验内容

WPS 主要校验：

- 消息是否属于当前会话；
- Nonce 和前后消息是否一致；
- 消息认证器是否正确，内容是否被篡改；
- 加密配置能否正确解密；
- 对端能否完成当前 WPS 方法要求的证明；
- PBC 窗口是否有效；
- 是否存在多个同时活跃的 PBC 会话。

PBC 不校验“GC 是否知道一个共享密码”。它校验协议会话完整性，而用户授权来自 GO 主动打开的配对窗口。

## 9. 802.11 认证和关联

GC 获得 Group 凭据后执行：

```text
GC                                      GO
 │──802.11 Authentication Request──────>│
 │<─802.11 Authentication Response──────│
 │──Association Request + RSN IE────────>│
 │<─Association Response────────────────│
```

WPA2-Personal 下的 802.11 Authentication 通常采用 Open System Authentication。这里的“Authentication”名称容易误导：它本身不验证 Wi-Fi 密码。

Association Request 中的 RSN IE 声明 GC 支持的协议、加密套件和密钥管理方式。GO 会检查双方的 WPA2、CCMP、PSK 等安全能力是否兼容。

真正的凭据持有证明发生在 WPA 四次握手阶段。

## 10. WPA2 四次握手

### 10.1 PMK

WPS 配置完成后，GO 和 GC 能得到相同的 PMK（Pairwise Master Key）。传统 WPA2-PSK 使用 passphrase 时通常按以下方式推导：

```text
PMK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 256 bit)
```

如果配置的是等效的 256 位 PSK，则可直接形成相应 PMK。PMK 不会在 WPA 四次握手中直接传输。

### 10.2 M1：GO → GC

GO 生成 `ANonce`，向 GC 发送：

- ANonce；
- Replay Counter；
- 密钥相关参数。

GC 收到后生成 `SNonce`，并使用 PMK、双方 MAC、ANonce 和 SNonce 推导 PTK：

```text
PTK = PRF(PMK,
          min(GO_MAC, GC_MAC) || max(GO_MAC, GC_MAC) ||
          min(ANonce, SNonce) || max(ANonce, SNonce))
```

PTK 进一步包含：

- KCK：计算握手消息 MIC；
- KEK：保护密钥材料；
- TK：加密双方的单播数据帧。

### 10.3 M2：GC → GO

GC 发送：

- SNonce；
- GC 的 RSN IE；
- Replay Counter；
- 使用 KCK 计算的 MIC。

GO 使用自己的 PMK 和相同上下文独立推导 PTK，然后校验 M2 的 MIC。MIC 正确说明 GC 拥有能够得到相同 PMK/PTK 的正确 Group Credential。

### 10.4 M3：GO → GC

GO 向 GC 发送：

- PTK 安装指示；
- 使用 KEK 保护的 GTK 等 Group Key 材料；
- Replay Counter；
- MIC。

GC 校验 Replay Counter、MIC 和 RSN 参数，解密 GTK，并安装：

- PTK/TK：保护与 GO 之间的单播帧；
- GTK：保护 GO 发送的广播和组播帧。

### 10.5 M4：GC → GO

GC 返回带有 Replay Counter、确认标志和 MIC 的 M4。GO 校验成功后，认为 GC 已正确安装密钥。

随后程序可能收到：

```text
EAPOL-4WAY-HS-COMPLETED
CTRL-EVENT-CONNECTED
```

### 10.6 WPA 四次握手校验内容

四次握手校验：

- 双方是否能从相同 PMK 推导出相同 PTK；
- M2、M3、M4 的 MIC 是否正确；
- Nonce 是否属于当前连接；
- Replay Counter 是否有效，防止旧消息重放；
- RSN 安全参数是否与关联阶段一致；
- GTK 等密钥材料是否可正确处理和安装。

Passphrase、PSK、PMK 和 PTK 均不会以明文在四次握手中直接发送。

## 11. IP 配置和业务通信

GO 当前通过 Netlink 为 Group 接口设置静态地址，例如：

```text
GO：192.168.49.1/24
```

GC 等待 `EAPOL-4WAY-HS-COMPLETED` 或 `CTRL-EVENT-CONNECTED` 后，为 Group 接口设置 `fallback_ip_`，例如：

```text
GC：192.168.49.2/24
```

双方首先通过 ARP 将对端 IP 解析为 Group 接口 MAC，随后才能运行 ICMP、TCP、UDP 和远程屏幕业务协议。

IP 数据包最终被封装为 802.11 数据帧，并由 WPA2 四次握手生成、安装的 TK/GTK 进行无线链路层加密和完整性保护。

## 12. 各阶段汇总

| 阶段 | 发送的内容 | 向对端提供的内容 | 主要校验内容 |
|---|---|---|---|
| 本地控制 | `ATTACH`、`SET`、`P2P_*` 控制命令 | 应用意图 | 控制 socket 响应 |
| P2P 发现 | Probe Request/Response、P2P/WPS IE | 设备名、设备地址、能力、Group 信息 | 是否为目标设备、能力是否兼容 |
| P2P 加入 | `pbc join` 对应的加入流程 | 目标 GO 和配置方式 | GO 是否存在并允许加入 |
| WPS | EAP-WSC M1～M8 | SSID、安全参数、Passphrase/PSK | Nonce、公钥派生密钥、Authenticator、PBC 窗口、消息完整性 |
| 802.11 关联 | Authentication、Association、RSN IE | 双方无线和安全能力 | RSN 和加密套件是否兼容 |
| WPA 四次握手 | EAPOL-Key M1～M4 | ANonce、SNonce、加密的 GTK；不发送 PMK | PMK 持有证明、MIC、Replay Counter、安全参数 |
| IP 配置 | 静态 Netlink 配置或 DHCP | IP、前缀、网关等 | 网络参数是否有效 |
| 数据传输 | ARP、IP、TCP/UDP | 业务数据 | WPA 帧完整性、TCP/应用协议校验 |

## 13. 安全边界

本流程中需要明确：

```text
设备名称不是身份认证；
P2P Device Address 也不是秘密；
PBC 本身不包含共享密码；
WPS 负责安全地配置 Group Credential；
WPA 四次握手负责证明双方持有正确凭据并建立会话密钥；
WPA2/CCMP 负责保护后续无线数据帧。
```

若产品要求只允许明确授权的 GC 接入，建议由实体或界面按钮开启 `WPS_PBC`，在 GC 成功加入或超时后发送 `WPS_CANCEL`，避免长期开放配对窗口。

## 14. 当前实现的注意事项

### 14.1 控制命令响应检查不严格

当前部分代码只检查响应是否为空。`FAIL\n` 同样是非空字符串，因此应明确检查命令是否返回 `OK`。

### 14.2 Group 控制接口

独立 Group 接口模式下，`WPS_PBC`、`STATUS` 和 Group 事件可能需要通过 `/var/run/wpa_supplicant/<group_iface>` 对应的控制连接操作。当前单一 `ctrl_` 在 rtw89 单接口模式中通常可用，但应兼容独立 Group 接口。

### 14.3 连接超时不能由静态 IP 兜底

当前 GC 在四次握手等待超时后仍配置 IP 并返回成功。静态 IP 无法替代 802.11 关联和 WPA 握手。超时后应查询：

```text
STATUS
wpa_state=COMPLETED
```

只有确认二层连接成功后才能进入 IP 配置和业务通信阶段。

### 14.4 PBC 生命周期

当前 GO 建组后立即开启 `WPS_PBC`，GC 断开后又自动重新开启。更安全的产品逻辑是：

```text
用户按下配对按钮
        ↓
GO 执行 WPS_PBC
        ↓
收到 AP-STA-CONNECTED 或达到超时
        ↓
GO 执行 WPS_CANCEL
```

关闭 PBC 窗口不会断开已经成功加入的 GC，只会停止接受新的 WPS PBC 配对。

## 15. `wpa_ctrl` 常用控制命令

`wpa_ctrl` 本身是 `wpa_supplicant` 控制接口的客户端封装。应用通过 Unix Domain Socket 向 `wpa_supplicant` 发送文本命令，并读取直接响应或异步事件。

典型调用形式为：

```cpp
std::string reply = ctrl_.request("COMMAND ...");
```

命令由 `wpa_supplicant` 的 control interface 实现，并非 `wpa_ctrl` 自己执行无线操作。

### 15.1 控制连接

#### `PING`

检查 `wpa_supplicant` 控制接口是否存活：

```text
PING
```

正常返回：

```text
PONG
```

#### `ATTACH` / `DETACH`

订阅或取消订阅异步事件：

```text
ATTACH
DETACH
```

通常由封装函数调用：

```cpp
ctrl_.attach();
ctrl_.detach();
```

#### `TERMINATE`

要求当前 `wpa_supplicant` 进程退出：

```text
TERMINATE
```

只有确认该实例由本程序独占管理时才应使用，避免终止系统或其他业务正在使用的实例。

### 15.2 状态和能力查询

#### `STATUS`

查询当前接口状态：

```text
STATUS
```

可能返回：

```text
bssid=54:ae:bc:40:2e:ac
freq=2437
ssid=DIRECT-ab-RemoteScreen
mode=station
pairwise_cipher=CCMP
group_cipher=CCMP
key_mgmt=WPA2-PSK
wpa_state=COMPLETED
address=02:11:22:33:44:55
```

判断 GC 是否真正完成二层连接时，应重点检查：

```text
wpa_state=COMPLETED
```

#### `STATUS-DRIVER`

查询驱动相关状态：

```text
STATUS-DRIVER
```

#### `SIGNAL_POLL`

查询信号强度、速率和当前频率：

```text
SIGNAL_POLL
```

可能返回：

```text
RSSI=-46
LINKSPEED=72
NOISE=9999
FREQUENCY=2437
```

#### `GET_CAPABILITY`

查询 `wpa_supplicant` 和驱动暴露的能力：

```text
GET_CAPABILITY key_mgmt
GET_CAPABILITY pairwise
GET_CAPABILITY group
GET_CAPABILITY channels
```

#### `GET`

读取运行时全局字段，具体可用字段取决于版本：

```text
GET device_name
GET country
```

### 15.3 运行时设置

#### `SET`

修改当前 `wpa_supplicant` 进程中的运行时参数：

```text
SET device_name Remote-Screen
SET country CN
SET p2p_go_intent 7
SET p2p_ssid_postfix -Screen
```

运行时设置通常不会跨 `wpa_supplicant` 重启永久保存。

#### `LOG_LEVEL`

查询或修改日志等级：

```text
LOG_LEVEL
LOG_LEVEL DEBUG
LOG_LEVEL INFO
```

### 15.4 普通 Wi-Fi STA 命令

虽然本程序主要使用 P2P，以下命令常用于传统 STA 连接。

#### 扫描和扫描结果

```text
SCAN
SCAN_RESULTS
BSS 0
BSS <bssid>
BSS FIRST
BSS NEXT-<bssid>
```

`SCAN` 返回 `OK` 仅代表扫描请求被接受；扫描完成应等待：

```text
CTRL-EVENT-SCAN-RESULTS
```

#### 网络配置

```text
ADD_NETWORK
SET_NETWORK <id> ssid "MyWiFi"
SET_NETWORK <id> psk "password"
ENABLE_NETWORK <id>
SELECT_NETWORK <id>
DISABLE_NETWORK <id>
REMOVE_NETWORK <id>
LIST_NETWORKS
```

例如：

```text
ADD_NETWORK
→ 0

SET_NETWORK 0 ssid "MyWiFi"
SET_NETWORK 0 psk "12345678"
ENABLE_NETWORK 0
SELECT_NETWORK 0
```

通过控制接口传递字符串值时，通常需要保留值外层的双引号。

#### 连接控制

```text
DISCONNECT
RECONNECT
REASSOCIATE
```

- `DISCONNECT`：主动断开；
- `RECONNECT`：恢复自动连接；
- `REASSOCIATE`：立即重新关联。

#### `SAVE_CONFIG`

将运行时网络配置写回配置文件：

```text
SAVE_CONFIG
```

通常要求配置中允许更新：

```conf
update_config=1
```

### 15.5 Wi-Fi Direct / P2P 命令

#### `P2P_FIND`

开始发现 P2P 设备：

```text
P2P_FIND
P2P_FIND 30
P2P_FIND type=social
P2P_FIND type=progressive
```

相关事件包括：

```text
P2P-DEVICE-FOUND
P2P-DEVICE-LOST
```

#### `P2P_STOP_FIND`

停止 P2P 设备发现：

```text
P2P_STOP_FIND
```

#### `P2P_LISTEN`

让 P2P Device 进入监听状态：

```text
P2P_LISTEN
P2P_LISTEN 60
```

#### `P2P_PEERS` / `P2P_PEER`

查询已经发现并缓存的 P2P 设备：

```text
P2P_PEERS
P2P_PEER <device-address>
P2P_PEER FIRST
P2P_PEER NEXT-<device-address>
```

#### `P2P_CONNECT`

向 P2P 设备发起连接：

```text
P2P_CONNECT <addr> pbc
P2P_CONNECT <addr> pbc join
P2P_CONNECT <addr> 12345670 keypad
P2P_CONNECT <addr> pin display
```

本程序 GC 使用：

```text
P2P_CONNECT <GO Device Address> pbc join
```

#### `P2P_GROUP_ADD`

创建 autonomous Group：

```text
P2P_GROUP_ADD
P2P_GROUP_ADD freq=2437
P2P_GROUP_ADD persistent
```

本程序 GO 使用无参数形式。

#### `P2P_GROUP_REMOVE`

删除 P2P Group：

```text
P2P_GROUP_REMOVE <group-interface>
```

例如：

```text
P2P_GROUP_REMOVE p2p-wlan0-0
```

单接口模式下参数可能是主接口名。

#### `P2P_CANCEL`

取消正在进行的连接、邀请或 GO Negotiation：

```text
P2P_CANCEL
```

它与 `P2P_STOP_FIND` 不同，后者只负责停止设备发现。

#### `P2P_FLUSH`

清理已发现的 P2P Peer 缓存：

```text
P2P_FLUSH
```

#### `P2P_INVITE`

邀请设备加入已有或持久化 Group：

```text
P2P_INVITE group=<group_iface> peer=<peer_addr>
```

#### P2P 服务发现

```text
P2P_SERV_DISC_REQ <addr> <query>
P2P_SERV_DISC_CANCEL_REQ <id>
```

这组命令用于 P2P Service Discovery，不是本程序当前建链流程的必需部分。

### 15.6 WPS 命令

#### `WPS_PBC`

开启 WPS PBC 配对窗口：

```text
WPS_PBC
```

也可在支持的上下文中限制目标 BSSID：

```text
WPS_PBC <bssid>
```

#### `WPS_PIN`

使用 WPS PIN：

```text
WPS_PIN any
WPS_PIN <bssid> <pin>
```

#### `WPS_CANCEL`

取消正在进行的 WPS 配对：

```text
WPS_CANCEL
```

它不会断开已经完成配对和连接的 GC。

#### `WPS_CHECK_PIN`

检查 WPS PIN 格式和校验位：

```text
WPS_CHECK_PIN 12345670
```

### 15.7 GO/AP 侧客户端管理

以下命令通常应发往 GO Group 接口对应的控制 socket。

#### 查询关联客户端

```text
STA <mac>
STA-FIRST
STA-NEXT <mac>
```

#### 主动断开客户端

```text
DISASSOCIATE <mac>
DEAUTHENTICATE <mac>
```

- `DISASSOCIATE`：取消客户端的 802.11 关联；
- `DEAUTHENTICATE`：取消 802.11 认证，通常更彻底。

### 15.8 常见异步事件

调用 `ATTACH` 后，应用可能收到以下事件。

普通连接事件：

```text
CTRL-EVENT-SCAN-RESULTS
CTRL-EVENT-CONNECTED
CTRL-EVENT-DISCONNECTED
CTRL-EVENT-ASSOC-REJECT
CTRL-EVENT-AUTH-REJECT
CTRL-EVENT-SSID-TEMP-DISABLED
```

P2P 事件：

```text
P2P-DEVICE-FOUND
P2P-DEVICE-LOST
P2P-GO-NEG-SUCCESS
P2P-GO-NEG-FAILURE
P2P-GROUP-FORMATION-SUCCESS
P2P-GROUP-FORMATION-FAILURE
P2P-GROUP-STARTED
P2P-GROUP-REMOVED
```

WPS 事件：

```text
WPS-SUCCESS
WPS-FAIL
WPS-TIMEOUT
WPS-PBC-ACTIVE
WPS-PBC-DISABLE
WPS-OVERLAP-DETECTED
```

WPA 和 GO 客户端事件：

```text
EAPOL-4WAY-HS-COMPLETED
AP-STA-CONNECTED
AP-STA-DISCONNECTED
```

### 15.9 本程序建议重点使用的命令

GO 侧：

```text
PING
ATTACH
SET device_name <name>
P2P_GROUP_ADD
WPS_PBC
WPS_CANCEL
STATUS
STA-FIRST
P2P_GROUP_REMOVE <iface>
```

GC 侧：

```text
PING
ATTACH
SET device_name <name>
P2P_FIND
P2P_PEER <addr>
P2P_STOP_FIND
P2P_CONNECT <addr> pbc join
STATUS
SIGNAL_POLL
P2P_GROUP_REMOVE <iface>
```

### 15.10 直接响应与异步完成事件

控制命令的同步返回和操作最终完成是两个概念。例如：

```text
应用 → P2P_GROUP_ADD
wpa_supplicant → OK
```

`OK` 只表示命令被接受。Group 真正建立成功应等待：

```text
P2P-GROUP-STARTED
```

同理：

```text
P2P_CONNECT <addr> pbc join
→ OK
```

不代表 GC 已完成关联和 WPA 握手。最终应等待：

```text
CTRL-EVENT-CONNECTED
```

或查询：

```text
STATUS
wpa_state=COMPLETED
```

程序应同时检查：

1. 命令直接响应是否为 `OK`，而不只是检查字符串是否非空；
2. 是否在超时范围内收到对应的成功事件；
3. 超时时通过 `STATUS` 等查询命令复核实际状态；
4. 是否收到 `FAIL`、`WPS-FAIL`、`GROUP-FORMATION-FAILURE` 等失败信息。
