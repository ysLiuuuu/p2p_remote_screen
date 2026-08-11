# Wireless Display

基于 Rockchip Linux 开发板的低延迟无线 HDMI 投屏实验项目。

项目采用 Wi-Fi Direct 建立设备间 IP 链路，使用 BLE GATT 完成设备配置，
在 OPI 发射端通过 V4L2/RGA/MPP 获取并编码 HDMI 输入，在 LCAT 接收端通过
MPP/DRM-KMS 解码并输出 HDMI。

当前版本：**0.1.11**（见 [VERSION](VERSION)）

## 功能概览

- Wi-Fi Direct GO/GC 建链与断线自动重连
- BlueZ D-Bus 自定义 GATT 配网服务
- Rockchip MPP H.264 硬件编码和解码
- RGA DMA-BUF 图像格式转换
- DRM/KMS Plane 直接输出 HDMI
- 自定义低延迟 UDP 帧分片协议

## 系统结构

![无线 HDMI 投屏系统结构图](docs/system-architecture.png)

## 验证硬件与运行时环境

以下信息对应当前已验证的实验环境；不同板卡镜像可能需要替换工具链、sysroot
或系统版本。

| 设备 | SoC/架构 | 系统与内核 | 项目角色 | 关键设备 |
| --- | --- | --- | --- | --- |
| OPI | Rockchip RK3588 / aarch64 | Debian 12；`6.1.99-rockchip-rk3588` | TX / Wi-Fi Direct GO | `/dev/video0` HDMI-IN、`/dev/rga`、`/dev/mpp_service` |
| LCAT | Rockchip RK3576 / aarch64 | Debian 12；`6.1.99-rk3576` | RX / Wi-Fi Direct GC | `/dev/dri/card0`、`/dev/mpp_service`、HDMI-A-1 |
| PC 编译机 | x86_64 | Ubuntu 22.04.5 | 交叉编译与部署 | 对应板卡 SDK、sysroot 和 CMake 工具链 |

两块板使用 glibc 2.36。已验证的无线驱动为`rtw89_8852be`；
## 目录

| 路径 | 内容 |
| --- | --- |
| `src/p2p_app` | BLE、P2P 和系统工具；包含 `p2p_manager`、`ble_provisioner` |
| `src/media` | UDP 传输、MPP 编码/解码和 DRM 显示；包含 `wd_tx`、`wd_rx` |
| `config` | GO/GC 示例配置 |
| `docs` | 协议和 BLE 配网说明 |
| `third_party` | nlohmann/json、spdlog、wpa_supplicant 相关依赖 |

## 构建脚本

使用 `scripts/build.sh` 进行交叉编译。脚本默认使用
`~/path/cmake/arm64-toolchain.cmake` 作为工具链，并将构建目录和产物
放在项目根目录的 `build/` 下：

```bash
# 默认构建
./scripts/build.sh

# 删除 build/ 后全量重建
./scripts/build.sh -c

# 使用其他交叉编译工具链
TOOLCHAIN=/path/to/arm64-toolchain.cmake ./scripts/build.sh
```

脚本默认关闭 DRM/KMS 接收端目标（`ENABLE_DRM_TARGETS=OFF`），构建完成后会检查
`build/src/p2p_app/p2p_manager` 是否为 aarch64 可执行文件。使用前请确保工具链
及其 sysroot 已提供项目所需的 Rockchip MPP、RGA、D-Bus 等开发文件。

## 配置

- [GO 配置](config/go.json)
- [GC 配置](config/gc.json)
- [BLE 配网协议](docs/蓝牙配网协议.md)

## 版本管理

版本号集中维护在根目录 `VERSION`，CMake 配置阶段会读取该文件作为项目版本。
版本变更记录见 [CHANGELOG.md](CHANGELOG.md)。

## License

项目许可证尚未确定。公开发布前请根据项目归属补充合适的许可证文件。
