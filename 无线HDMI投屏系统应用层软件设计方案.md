# 无线 HDMI 投屏系统应用层软件设计方案

## 1. 项目概述

本项目基于 RK3588 平台构建一套自研无线 HDMI 投屏系统，由发送端 TX 和接收端 RX 组成。系统使用 Wi-Fi Direct 建立高速无线链路，使用 BLE 完成设备发现、配对和参数配置，不要求兼容 Android、Windows Miracast。

系统数据链路如下：

```text
TX：HDMI-IN → V4L2 → 硬件编码 → RTP/UDP → Wi-Fi Direct

RX：Wi-Fi Direct → RTP/UDP → 硬件解码 → DRM/KMS → HDMI-OUT
```

其中：

- BLE 作为控制面，负责配网、配对、参数设置和状态查询；
- Wi-Fi 作为数据面，负责传输实时音视频；
- TX 采集外部 HDMI 信号并编码；
- RX 解码视频，并通过 HDMI-OUT 输出至显示器；
- 系统采用纯命令行 Linux 环境，不依赖 X11、Wayland 或桌面环境。

## 2. 设计目标

第一阶段目标如下：

- 支持 HDMI-IN 视频采集；
- 支持 1920×1080、60 fps 视频传输；
- 使用 H.264 硬件编码和解码；
- 使用 Wi-Fi Direct 建立 TX/RX 直连网络；
- 支持 TX 作为 P2P Group Owner、RX 作为 P2P Client；
- 支持 BLE 配网和设备参数配置；
- 支持 RX 通过 HDMI-OUT 全屏显示；
- 支持开机自动运行和异常自动恢复；
- 端到端延迟目标小于 100 ms；
- 为一发多收、音频和 4K 传输预留扩展能力。

后续版本可扩展：

- 4K@30 fps 或 4K@60 fps；
- H.265 编解码；
- 音频同步传输；
- 一发多收；
- 动态码率和分辨率调整；
- OTA 升级；
- 远程日志和故障诊断。

## 3. 总体软件架构

系统采用分层架构：

```text
┌──────────────────────────────────────┐
│ 应用层                               │
│ 设备状态机、投屏控制、配网、故障恢复 │
├──────────────────────────────────────┤
│ 媒体层                               │
│ GStreamer、V4L2、Rockchip MPP        │
├──────────────────────────────────────┤
│ 传输层                               │
│ RTP/UDP、TCP 控制协议                 │
├──────────────────────────────────────┤
│ 无线控制层                           │
│ wpa_supplicant、wpa_ctrl、BlueZ      │
├──────────────────────────────────────┤
│ Linux 系统层                         │
│ systemd、DRM/KMS、内核驱动           │
├──────────────────────────────────────┤
│ 硬件层                               │
│ HDMI-IN、HDMI-OUT、Wi-Fi、BLE        │
└──────────────────────────────────────┘
```

### 3.1 TX 发送端

TX 端负责：

- 通过 BLE 接收设备配置；
- 创建 Wi-Fi Direct P2P Group；
- 检测 RX 加入和离开；
- 从 HDMI-IN 获取视频；
- 使用硬件编码器生成 H.264/H.265 码流；
- 将视频封装为 RTP，并通过 UDP 发送；
- 监测链路质量并调整码率；
- 在 HDMI 输入或无线连接异常时自动恢复。

```text
BLE 配网服务 ──────────────┐
                          ▼
HDMI-IN → V4L2 → TX 控制服务 → 硬件编码 → RTP/UDP → Wi-Fi P2P
                          ▲
wpa_supplicant / wpa_ctrl ┘
```

### 3.2 RX 接收端

RX 端负责：

- 通过 BLE 接收配对和显示配置；
- 扫描并加入 TX 创建的 P2P Group；
- 接收 RTP/UDP 视频流；
- 使用硬件解码器解码；
- 通过 DRM/KMS 将视频输出到 HDMI；
- 监控丢包、帧率、缓冲和显示器状态；
- 在网络中断后自动重新连接并恢复播放。

```text
BLE 配网服务 ───────────────┐
                           ▼
Wi-Fi P2P → RTP/UDP → RX 控制服务 → 硬件解码 → DRM/KMS → HDMI-OUT
                           ▲
wpa_supplicant / wpa_ctrl ─┘
```

## 4. 开发框架与组件

### 4.1 Linux 和进程管理

建议使用 Rockchip/Orange Pi BSP Linux，保留 HDMI、V4L2、DRM/KMS、MPP、Wi-Fi 和 BLE 驱动。

应用服务由 systemd 管理，实现：

- 开机自动启动；
- 异常退出后自动重启；
- 服务依赖管理；
- 日志统一收集；
- 看门狗和故障恢复。

正式系统不需要安装 GNOME、KDE、X11 或 Wayland。

### 4.2 Wi-Fi Direct

使用以下组件：

- `wpa_supplicant`：P2P 发现、协商、建组和连接；
- `wpa_ctrl`：C/C++ 应用控制接口；
- `wpa_cli`：开发和调试工具；
- NetworkManager：可用于普通 Wi-Fi 管理，但不作为核心 P2P 控制框架。

角色分配：

```text
TX：P2P Group Owner（GO）
RX：P2P Client（GC）
```

连接成功后，双方建立普通 IP 网络，媒体应用不需要直接操作无线驱动。

### 4.3 BLE 配网

使用 BlueZ 实现自定义 BLE GATT 服务。BLE 用于：

- 发现 TX/RX；
- 设置设备角色；
- 建立 TX 与 RX 的绑定关系；
- 配置设备名称、信道和密码；
- 设置分辨率、帧率和码率；
- 控制开始、停止和重启；
- 查询温度、连接状态和软件版本；
- 推送连接、投屏和错误状态。

建议定义以下 GATT Characteristic：

| 名称 | 属性 | 用途 |
|---|---|---|
| DeviceInfo | Read | 型号、序列号、版本和角色 |
| NetworkConfig | Read/Write | P2P 名称、密码和信道 |
| PairCommand | Write | 开始、取消或清除配对 |
| VideoConfig | Read/Write | 编码、分辨率、帧率和码率 |
| DeviceControl | Write | 开始、停止、重启和恢复出厂 |
| Status | Read/Notify | 网络、媒体和错误状态 |

配置数据可使用 JSON 或 TLV。量产版本应增加：

- BLE 链路加密；
- 设备验证码或密钥；
- 配置窗口超时；
- 未授权写入保护；
- 敏感信息安全存储。

### 4.4 HDMI 输入与 V4L2

TX 的 HDMI-IN 由内核驱动暴露为 V4L2 设备，例如：

```text
/dev/video0
```

应用通过 GStreamer `v4l2src` 或直接调用 V4L2 API 获取视频。

常用检查命令：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
```

### 4.5 硬件编解码

使用 GStreamer 管理媒体管线，使用 Rockchip MPP 或 V4L2 硬件编解码接口降低 CPU 占用和延迟。

```text
TX：V4L2 → Rockchip 硬件编码器
RX：Rockchip 硬件解码器 → DRM/KMS
```

实际插件名称取决于系统镜像，可通过以下命令确认：

```bash
gst-inspect-1.0 | grep -Ei 'mpp|v4l2|h264|h265|264|265'
```

可能出现的插件包括：

```text
mpph264enc
mpph265enc
mppvideodec
v4l2h264enc
v4l2h264dec
```

### 4.6 视频传输

第一阶段采用 RTP/UDP：

```text
H.264 → RTP Payload → UDP → Wi-Fi Direct
```

选择 RTP/UDP 的原因：

- 延迟低；
- GStreamer 原生支持；
- 少量丢包不会阻塞后续帧；
- 便于扩展时间戳、序列号和丢包统计；
- 支持单播和组播。

建议端口规划：

| 端口 | 协议 | 用途 |
|---|---|---|
| 5000 | RTP/UDP | 视频 |
| 5001 | RTP/UDP | 音频 |
| 6000 | TCP | 控制和状态 |
| 6001 | UDP | 设备发现或心跳 |

视频走 UDP，控制指令走 TCP，避免视频丢包阻塞整个传输链路。

### 4.7 HDMI 输出

RX 使用 DRM/KMS 直接控制 HDMI 输出。GStreamer 可通过 `kmssink` 将解码画面直接送至显示控制器：

```text
硬件解码 → kmssink → DRM/KMS → HDMI-OUT
```

常用检查命令：

```bash
ls -l /dev/dri/
cat /sys/class/drm/card0-HDMI-A-1/status
cat /sys/class/drm/card0-HDMI-A-1/modes
modetest -M rockchip
```

## 5. 应用模块设计

建议将主程序命名为 `wireless-displayd`，内部包含以下模块：

```text
wireless-displayd
├── DeviceManager
├── BleManager
├── P2pManager
├── TxPipelineManager
├── RxPipelineManager
├── ControlServer
├── ConfigManager
├── StatisticsManager
└── HealthMonitor
```

### 5.1 DeviceManager

系统总状态机，统一协调 BLE、P2P 和媒体管线：

```text
BOOTING
  ↓
UNCONFIGURED
  ↓
CONFIGURING
  ↓
P2P_READY
  ↓
CONNECTING
  ↓
CONNECTED
  ↓
STREAMING
```

任何模块发生异常时进入 `RECOVERING` 或 `ERROR`，避免各模块独立重启造成状态冲突。

### 5.2 BleManager

负责：

- 注册 BlueZ GATT 服务；
- 处理特征值读写；
- 校验配置和权限；
- 将配置提交给 DeviceManager；
- 发送状态通知。

### 5.3 P2pManager

负责：

- 启动并连接 wpa_supplicant；
- 创建或加入 P2P Group；
- 获取客户端和 GO 地址；
- 维护设备列表；
- 监听连接事件；
- 断线重连。

建议状态：

```text
IDLE
DISCOVERING
NEGOTIATING
GROUP_STARTED
CONNECTED
DISCONNECTED
ERROR
```

### 5.4 TxPipelineManager

负责：

- HDMI 输入热插拔检测；
- 创建和销毁 GStreamer TX 管线；
- 设置编码格式、码率、GOP 和帧率；
- 管理一个或多个 RX 目标；
- 统计编码帧率和输出码率；
- 在输入丢失后等待信号恢复。

### 5.5 RxPipelineManager

负责：

- 创建和销毁 GStreamer RX 管线；
- RTP 接收、抖动缓冲和丢包处理；
- 硬件解码；
- HDMI 热插拔和模式选择；
- 处理首帧、黑屏和超时；
- 统计解码帧率、丢包率和延迟。

### 5.6 ConfigManager

负责：

- 保存 BLE 配网信息；
- 保存 TX/RX 角色和绑定关系；
- 保存媒体参数；
- 配置版本迁移；
- 恢复出厂设置。

配置文件可使用 JSON，敏感信息应限制文件权限或使用安全存储。

### 5.7 HealthMonitor

监测：

- Wi-Fi 信号强度和连接状态；
- RTP 丢包、乱序和抖动；
- 编码/解码帧率；
- 端到端延迟；
- HDMI 输入和输出状态；
- CPU、内存和温度；
- GStreamer 管线错误；
- 硬件编码器和解码器状态。

## 6. 媒体管线设计

### 6.1 TX 管线

逻辑管线：

```text
v4l2src
  → capsfilter
  → 格式转换
  → H.264 硬件编码
  → h264parse
  → rtph264pay
  → udpsink
```

软件编码测试示例：

```bash
gst-launch-1.0 -v \
v4l2src device=/dev/video0 ! \
video/x-raw,width=1920,height=1080,framerate=60/1 ! \
videoconvert ! \
x264enc tune=zerolatency bitrate=8000 speed-preset=ultrafast ! \
h264parse ! \
rtph264pay config-interval=1 pt=96 ! \
udpsink host=<RX_IP> port=5000
```

正式版本应将 `x264enc` 替换为 Rockchip 硬件编码器。

推荐第一阶段编码参数：

```text
编码：H.264
分辨率：1920×1080
帧率：60 fps
码率：6～12 Mbps
GOP：30～60
B 帧：关闭
码率控制：CBR 或受限 VBR
模式：低延迟
```

### 6.2 RX 管线

逻辑管线：

```text
udpsrc
  → rtpjitterbuffer
  → rtph264depay
  → h264parse
  → H.264 硬件解码
  → kmssink
```

软件解码测试示例：

```bash
gst-launch-1.0 -v \
udpsrc port=5000 \
caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" ! \
rtph264depay ! \
h264parse ! \
avdec_h264 ! \
kmssink
```

正式版本应将 `avdec_h264` 替换为 Rockchip 硬件解码器。

## 7. BLE 配网与连接流程

### 7.1 首次配置

```text
TX/RX 上电
  ↓
读取本地配置
  ↓
未配置设备启动 BLE 广播
  ↓
手机配置工具发现设备
  ↓
写入角色、绑定关系和网络参数
  ↓
设备保存配置
  ↓
启动或重新初始化 Wi-Fi P2P
```

### 7.2 TX 启动

```text
启动系统服务
  ↓
加载配置
  ↓
初始化 BLE
  ↓
启动 wpa_supplicant
  ↓
创建 P2P Group
  ↓
等待 RX 加入
  ↓
检测 HDMI 输入
  ↓
启动编码和发送
```

### 7.3 RX 启动

```text
启动系统服务
  ↓
加载配置
  ↓
初始化 BLE
  ↓
扫描绑定的 TX
  ↓
加入 P2P Group
  ↓
获取 IP
  ↓
建立控制连接
  ↓
启动接收、解码和 HDMI 输出
```

## 8. 控制协议

视频数据之外，应建立独立 TCP 控制连接。

开始投屏：

```json
{
  "version": 1,
  "cmd": "start_stream",
  "codec": "h264",
  "width": 1920,
  "height": 1080,
  "fps": 60,
  "bitrate_kbps": 8000,
  "video_port": 5000
}
```

RX 状态上报：

```json
{
  "version": 1,
  "state": "streaming",
  "decode_fps": 59.8,
  "packet_loss_percent": 0.2,
  "jitter_ms": 3.1,
  "estimated_latency_ms": 48
}
```

协议至少应包含：

- 协议版本；
- 设备 ID；
- 消息类型；
- 消息序号；
- 时间戳；
- 状态码或错误码；
- 可选校验字段。

## 9. 一发多收设计

Wi-Fi Direct 支持一个 P2P Group Owner 连接多个 P2P Client：

```text
TX（GO）
├── RX1
├── RX2
└── RX3
```

第一阶段采用单路编码、多路 UDP 单播：

```text
同一编码码流 → RX1 IP
             → RX2 IP
             → RX3 IP
```

优点是实现简单，并可独立管理每个 RX；缺点是无线带宽随 RX 数量增长。

后续可以评估 UDP 组播。组播只发送一份码流，但 Wi-Fi 组播速率、可靠性和驱动兼容性通常弱于单播，必须通过实际硬件测试确定。

## 10. 工程目录建议

```text
wireless-display/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── ble/
│   │   ├── ble_manager.cpp
│   │   └── gatt_service.cpp
│   ├── p2p/
│   │   ├── p2p_manager.cpp
│   │   └── wpa_ctrl_client.cpp
│   ├── media/
│   │   ├── tx_pipeline.cpp
│   │   ├── rx_pipeline.cpp
│   │   └── pipeline_config.cpp
│   ├── control/
│   │   ├── control_server.cpp
│   │   └── control_client.cpp
│   └── system/
│       ├── device_manager.cpp
│       ├── config_manager.cpp
│       └── health_monitor.cpp
├── config/
│   ├── tx.json
│   └── rx.json
├── systemd/
│   ├── wireless-display-tx.service
│   └── wireless-display-rx.service
└── tests/
```

应用主体建议使用 C/C++，便于集成 GStreamer、BlueZ D-Bus、wpa_ctrl、V4L2 和 DRM。Python 可用于快速验证、自动化测试和生产调试工具。

## 11. 开发流程

### 阶段一：硬件和驱动验证

- 验证 HDMI-IN；
- 验证 V4L2 设备和输入格式；
- 验证 DRM/KMS 和 HDMI-OUT；
- 验证 RTL8852BE P2P 能力；
- 验证 BLE 扫描、广播和 GATT。

### 阶段二：本机媒体回环

在一块开发板上验证：

```text
HDMI-IN → 编码 → UDP 127.0.0.1 → 解码 → HDMI-OUT
```

该阶段用于排除 Wi-Fi 对媒体链路的影响。

### 阶段三：有线或普通 Wi-Fi 双机传输

先使用以太网或普通 Wi-Fi 验证：

- RTP 传输；
- 硬件编解码；
- HDMI 输出；
- 延迟和丢包；
- 控制协议。

### 阶段四：Wi-Fi Direct

- TX 创建 P2P Group；
- RX 自动发现和加入；
- 建立 IP 通信；
- 自动启动投屏；
- 实现断线重连。

### 阶段五：BLE 配网

- 定义 GATT 服务；
- 开发手机配置工具或测试脚本；
- 完成设备绑定和参数下发；
- 增加认证和加密；
- 实现配置持久化。

### 阶段六：稳定性与产品化

- systemd 开机启动；
- 进程看门狗；
- HDMI 热插拔；
- 网络断线恢复；
- 温度和资源监测；
- 日志轮转；
- 压力和长稳测试；
- OTA 升级；
- 出厂配置和恢复出厂。

## 12. 测试计划

### 12.1 功能测试

- HDMI 信号插入、拔出和格式切换；
- P2P 建组、连接和断线重连；
- BLE 配置和权限校验；
- 开始、停止和恢复投屏；
- 不同显示器 EDID 和分辨率兼容性；
- TX/RX 重启后的自动恢复。

### 12.2 性能测试

- 1080p@60 fps 持续传输；
- 编码、网络、解码和显示分段延迟；
- CPU、内存、温度和功耗；
- 不同码率下的画质和稳定性；
- 弱信号和干扰环境下的丢包；
- 一发多收时的吞吐和延迟。

### 12.3 稳定性测试

- 24/72 小时连续运行；
- 高频插拔 HDMI；
- 高频连接和断开 P2P；
- Wi-Fi 模块重置；
- GStreamer 管线异常恢复；
- 电源异常和非正常重启。

## 13. 延迟优化

端到端延迟由以下部分组成：

```text
HDMI 采集
+ 视频编码
+ 网络发送
+ 接收抖动缓冲
+ 视频解码
+ HDMI 显示
```

优化措施：

- 使用硬件编码和解码；
- 关闭 B 帧；
- 使用低延迟编码模式；
- 缩短 GOP；
- 减小 GStreamer queue 和 RTP jitter buffer；
- 尽可能使用 DMA-BUF 零拷贝；
- 优先使用 5 GHz Wi-Fi；
- 固定或合理选择无线信道；
- 视频使用 UDP，避免 TCP 重传造成队头阻塞；
- 在应用中持续统计延迟、丢包和抖动；
- 根据链路质量动态调整码率。

## 14. systemd 服务示例

```ini
[Unit]
Description=Wireless HDMI Display Service
After=bluetooth.service network.target
Wants=bluetooth.service

[Service]
ExecStart=/usr/bin/wireless-displayd --role tx
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
```

RX 端将启动参数修改为：

```text
--role rx
```

## 15. 推荐技术栈总结

| 功能 | 推荐框架 |
|---|---|
| 操作系统 | Rockchip/Orange Pi BSP Linux |
| 应用语言 | C/C++ |
| BLE | BlueZ + D-Bus + GATT |
| Wi-Fi Direct | wpa_supplicant + wpa_ctrl |
| HDMI 输入 | V4L2 |
| 媒体框架 | GStreamer |
| 硬件编解码 | Rockchip MPP 或 V4L2 Codec |
| 视频传输 | RTP/UDP |
| 控制通道 | TCP + JSON/TLV |
| HDMI 输出 | DRM/KMS + kmssink |
| 配置存储 | JSON/SQLite |
| 服务管理 | systemd |
| 日志 | journald |

## 16. 结论

本方案采用 BLE 控制面与 Wi-Fi 高速数据面分离的设计。TX 通过 HDMI-IN 和 V4L2 获取视频，使用 RK3588 硬件编码器生成低延迟码流，经 Wi-Fi Direct 和 RTP/UDP 发送；RX 使用硬件解码器恢复视频，并通过 DRM/KMS 输出至 HDMI。

第一版应优先完成 1080p@60 fps、H.264、单播和单 TX/单 RX。在媒体链路稳定后，再逐步增加 BLE 产品化配网、一发多收、音频、动态码率和 4K 支持。
