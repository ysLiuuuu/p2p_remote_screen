# Wireless Display

基于 Rockchip Linux 开发板的低延迟无线 HDMI 投屏实验项目。

项目采用 Wi-Fi Direct 建立设备间 IP 链路，使用 BLE GATT 完成设备配置，
在 OPI 发射端通过 V4L2/RGA/MPP 获取并编码 HDMI 输入，在 LCAT 接收端通过
MPP/DRM-KMS 解码并输出 HDMI。

当前版本：**0.1.0**（见 [VERSION](VERSION)）

## 功能概览

- Wi-Fi Direct GO/GC 建链与断线自动重连
- BlueZ D-Bus 自定义 GATT 配网服务
- Rockchip MPP H.264 硬件编码和解码
- RGA DMA-BUF 图像格式转换
- DRM/KMS Plane 直接输出 HDMI
- 自定义低延迟 UDP 帧分片协议（`WDHM`）

## 系统结构

```text
BLE GATT 配网 ───────┐
                     ├─ p2p_manager ─ Wi-Fi Direct ─┐
OPI HDMI-IN → V4L2 → RGA → MPP H.264 → UDP          │
                                                     ↓
LCAT HDMI-OUT ← DRM/KMS ← MPP H.264 ← UDP ───────────┘
```

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/p2p_app` | BLE、P2P 和系统工具；包含 `p2p_manager`、`ble_provisioner`、`ble_config_tui` |
| `src/media` | UDP 传输、MPP 编码/解码和 DRM 显示；包含 `wd_tx`、`wd_rx` |
| `config` | GO/GC 示例配置 |
| `docs` | 协议、媒体链路和 BLE 配网说明 |
| `third_party` | nlohmann/json、spdlog、wpa_supplicant 相关依赖 |

公开仓库不包含实验环境部署脚本、Shell 脚本、构建目录和解压的 BlueZ 源码。

## 构建

### P2P/BLE 应用

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/aarch64-toolchain.cmake \
  -DENABLE_DRM_TARGETS=OFF
cmake --build build -j$(nproc)
```

产物位于 `build/src/p2p_app/`。

### 媒体链路

媒体程序需要对应板卡的 Rockchip MPP、RGA、DRM 头文件和运行库。分别使用
OPI 和 LCAT 的交叉工具链配置：

```bash
cmake -S . -B build-opi \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/opi-toolchain.cmake \
  -DENABLE_DRM_TARGETS=OFF
cmake --build build-opi --target wd_tx -j$(nproc)

cmake -S . -B build-lcat \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/lcat-toolchain.cmake \
  -DENABLE_DRM_TARGETS=ON
cmake --build build-lcat --target wd_rx -j$(nproc)
```

## 配置

- [GO 配置](config/go.json)
- [GC 配置](config/gc.json)
- [P2P 建链协议](docs/P2P_GO与GC连接协议流程.md)
- [BLE 配网协议](docs/蓝牙配网协议.md)
- [媒体链路说明](docs/媒体链路使用说明.md)

开发板验证记录保留在本地实验资料中，不纳入公开仓库。

## 当前限制

- 当前媒体传输层是项目自定义 UDP 协议，不是标准 RTP。
- `p2p_manager` 负责 P2P 链路和静态地址配置；DHCP 由外部部署流程负责。
- BLE 配置写入后会持久化，运行中模块重载仍需由上层状态机完成。
- BLE 产品化部署前仍应启用配对/加密、设备验证码和授权校验。
- 当前主要面向 OPI/RK3588 与 LCAT/RK3576 实验环境。

## 版本管理

版本号集中维护在根目录 `VERSION`，CMake 配置阶段会读取该文件作为项目版本。
版本变更记录见 [CHANGELOG.md](CHANGELOG.md)。

## License

项目许可证尚未确定。公开发布前请根据项目归属补充合适的许可证文件。
