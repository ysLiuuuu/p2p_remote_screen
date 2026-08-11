#define MODULE_TAG "wd_rx"

#include "video_transport.h"

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <rk_mpi.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

volatile std::sig_atomic_t running = 1;
void stop_handler(int) { running = 0; }

struct Options {
    std::string bind_address = "0.0.0.0";
    uint16_t port = 5004;
    std::string drm_card = "/dev/dri/card0";
};

void usage(const char* name) {
    std::cout << "用法: " << name
              << " [--bind 0.0.0.0] [--port 5004] [--drm /dev/dri/card0]\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help" || key == "-h") {
            usage(argv[0]);
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::runtime_error("参数缺少值: " + key);
        const std::string value = argv[++i];
        if (key == "--bind") options.bind_address = value;
        else if (key == "--port") options.port = static_cast<uint16_t>(std::stoul(value));
        else if (key == "--drm") options.drm_card = value;
        else throw std::runtime_error("未知参数: " + key);
    }
    if (!options.port) throw std::runtime_error("端口必须大于 0");
    return options;
}

drmModeConnector* find_hdmi(int fd, drmModeRes* resources) {
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0 &&
            connector->connector_type == DRM_MODE_CONNECTOR_HDMIA)
            return connector;
        drmModeFreeConnector(connector);
    }
    return nullptr;
}

uint32_t connector_crtc(int fd, drmModeConnector* connector) {
    if (connector->encoder_id) {
        drmModeEncoder* encoder = drmModeGetEncoder(fd, connector->encoder_id);
        if (encoder) {
            uint32_t id = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
            if (id) return id;
        }
    }
    return 0;
}

bool supports_nv12(const drmModePlane* plane) {
    for (uint32_t i = 0; i < plane->count_formats; ++i)
        if (plane->formats[i] == DRM_FORMAT_NV12) return true;
    return false;
}

class DrmDisplay {
public:
    explicit DrmDisplay(const std::string& card) {
        fd_ = open(card.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) throw std::runtime_error("打开 " + card + " 失败: " + std::strerror(errno));
        drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        resources_ = drmModeGetResources(fd_);
        connector_ = resources_ ? find_hdmi(fd_, resources_) : nullptr;
        if (!resources_ || !connector_) throw std::runtime_error("未找到已连接的 HDMI-A 输出");
        crtc_id_ = connector_crtc(fd_, connector_);
        saved_crtc_ = crtc_id_ ? drmModeGetCrtc(fd_, crtc_id_) : nullptr;
        if (!saved_crtc_ || !saved_crtc_->mode_valid)
            throw std::runtime_error("HDMI CRTC 尚未启用，请先启用显示输出");

        int crtc_index = -1;
        for (int i = 0; i < resources_->count_crtcs; ++i)
            if (resources_->crtcs[i] == crtc_id_) crtc_index = i;
        drmModePlaneRes* planes = drmModeGetPlaneResources(fd_);
        if (!planes || crtc_index < 0) {
            drmModeFreePlaneResources(planes);
            throw std::runtime_error("读取 DRM Plane 资源失败");
        }
        uint32_t fallback = 0;
        for (uint32_t i = 0; i < planes->count_planes; ++i) {
            drmModePlane* plane = drmModeGetPlane(fd_, planes->planes[i]);
            if (plane && (plane->possible_crtcs & (1u << crtc_index)) && supports_nv12(plane)) {
                if (!fallback) fallback = plane->plane_id;
                if (plane->crtc_id == crtc_id_) plane_id_ = plane->plane_id;
            }
            drmModeFreePlane(plane);
            if (plane_id_) break;
        }
        drmModeFreePlaneResources(planes);
        if (!plane_id_) plane_id_ = fallback;
        if (!plane_id_) throw std::runtime_error("当前 CRTC 没有支持线性 NV12 的 Plane");
        std::cout << "DRM 已就绪: connector=" << connector_->connector_id
                  << " crtc=" << crtc_id_ << " plane=" << plane_id_ << " mode="
                  << saved_crtc_->mode.hdisplay << "x" << saved_crtc_->mode.vdisplay << "@"
                  << saved_crtc_->mode.vrefresh << "\n";
    }

    ~DrmDisplay() {
        if (fd_ >= 0 && saved_crtc_ && connector_) {
            uint32_t connector_id = connector_->connector_id;
            drmModeSetCrtc(fd_, saved_crtc_->crtc_id, saved_crtc_->buffer_id,
                           saved_crtc_->x, saved_crtc_->y, &connector_id, 1,
                           &saved_crtc_->mode);
        }
        for (auto& frame : retained_) release(frame);
        retained_.clear();
        drmModeFreeCrtc(saved_crtc_);
        drmModeFreeConnector(connector_);
        drmModeFreeResources(resources_);
        if (fd_ >= 0) close(fd_);
    }

    bool show(MppFrame frame) {
        const MppFrameFormat format = mpp_frame_get_fmt(frame);
        if ((format & MPP_FRAME_FMT_MASK) != MPP_FMT_YUV420SP || MPP_FRAME_FMT_IS_FBC(format)) {
            std::cerr << "拒绝非线性 NV12 解码帧，format=0x" << std::hex << format << std::dec << "\n";
            return false;
        }
        MppBuffer buffer = mpp_frame_get_buffer(frame);
        if (!buffer) return false;

        ImportedFrame next;
        next.frame = frame;
        next.width = mpp_frame_get_width(frame);
        next.height = mpp_frame_get_height(frame);
        const uint32_t horizontal_stride = mpp_frame_get_hor_stride(frame);
        const uint32_t vertical_stride = mpp_frame_get_ver_stride(frame);
        const int dma_fd = mpp_buffer_get_fd(buffer);
        if (dma_fd < 0 || drmPrimeFDToHandle(fd_, dma_fd, &next.handle) != 0) {
            std::cerr << "导入 MPP DMA-BUF 失败: " << std::strerror(errno) << "\n";
            next.frame = nullptr;
            return false;
        }

        uint32_t handles[4] = {next.handle, next.handle, 0, 0};
        uint32_t pitches[4] = {horizontal_stride, horizontal_stride, 0, 0};
        uint32_t offsets[4] = {0, horizontal_stride * vertical_stride, 0, 0};
        if (drmModeAddFB2(fd_, next.width, next.height, DRM_FORMAT_NV12,
                          handles, pitches, offsets, &next.fb_id, 0) != 0) {
            std::cerr << "为 MPP 帧创建 NV12 framebuffer 失败: " << std::strerror(errno) << "\n";
            next.frame = nullptr;
            close_handle(next.handle);
            return false;
        }

        const drmModeModeInfo& mode = saved_crtc_->mode;
        if (drmModeSetPlane(fd_, plane_id_, crtc_id_, next.fb_id, 0,
                            0, 0, mode.hdisplay, mode.vdisplay,
                            0, 0, next.width << 16, next.height << 16) != 0) {
            std::cerr << "DRM 显示解码帧失败: " << std::strerror(errno) << "\n";
            next.frame = nullptr;
            release(next);
            return false;
        }

        // legacy set-plane 走同步 KMS commit，不再额外等待一次 vblank。保留最近
        // 三个 framebuffer，避免驱动提交尾部与 MPP 缓冲复用发生竞争。
        retained_.push_back(next);
        next = {};
        while (retained_.size() > 3) {
            release(retained_.front());
            retained_.pop_front();
        }
        return true;
    }

private:
    struct ImportedFrame {
        MppFrame frame = nullptr;
        uint32_t handle = 0;
        uint32_t fb_id = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    void close_handle(uint32_t& handle) {
        if (!handle || fd_ < 0) return;
        drm_gem_close close_request{};
        close_request.handle = handle;
        ioctl(fd_, DRM_IOCTL_GEM_CLOSE, &close_request);
        handle = 0;
    }

    void release(ImportedFrame& frame) {
        if (fd_ >= 0 && frame.fb_id) drmModeRmFB(fd_, frame.fb_id);
        close_handle(frame.handle);
        if (frame.frame) mpp_frame_deinit(&frame.frame);
        frame = {};
    }

    int fd_ = -1;
    drmModeRes* resources_ = nullptr;
    drmModeConnector* connector_ = nullptr;
    drmModeCrtc* saved_crtc_ = nullptr;
    uint32_t crtc_id_ = 0;
    uint32_t plane_id_ = 0;
    std::deque<ImportedFrame> retained_;
};

class MppH264Decoder {
public:
    MppH264Decoder() {
        if (mpp_create(&context_, &api_) != MPP_OK ||
            mpp_init(context_, MPP_CTX_DEC, MPP_VIDEO_CodingAVC) != MPP_OK)
            throw std::runtime_error("初始化 MPP H.264 解码器失败");

        MppDecCfg config = nullptr;
        if (mpp_dec_cfg_init(&config) != MPP_OK ||
            api_->control(context_, MPP_DEC_GET_CFG, config) != MPP_OK)
            throw std::runtime_error("读取 MPP 解码配置失败");
        mpp_dec_cfg_set_u32(config, "base:split_parse", 1);
        if (api_->control(context_, MPP_DEC_SET_CFG, config) != MPP_OK) {
            mpp_dec_cfg_deinit(config);
            throw std::runtime_error("设置 MPP 解码配置失败");
        }
        mpp_dec_cfg_deinit(config);
        MppFrameFormat output_format = MPP_FMT_YUV420SP;
        if (api_->control(context_, MPP_DEC_SET_OUTPUT_FORMAT, &output_format) != MPP_OK)
            throw std::runtime_error("MPP 不支持强制线性 NV12 输出");
    }

    ~MppH264Decoder() {
        if (context_) {
            api_->reset(context_);
            mpp_destroy(context_);
        }
        if (group_) mpp_buffer_group_put(group_);
    }

    uint32_t submit(const wd::EncodedFrame& encoded, DrmDisplay& display) {
        MppPacket packet = nullptr;
        if (mpp_packet_init(&packet, const_cast<uint8_t*>(encoded.data.data()), encoded.data.size()) != MPP_OK)
            throw std::runtime_error("创建 MPP 输入包失败");
        mpp_packet_set_pos(packet, const_cast<uint8_t*>(encoded.data.data()));
        mpp_packet_set_size(packet, encoded.data.size());
        mpp_packet_set_length(packet, encoded.data.size());
        mpp_packet_set_pts(packet, encoded.timestamp_us);

        MPP_RET result = MPP_NOK;
        for (int attempt = 0; attempt < 20 && running; ++attempt) {
            result = api_->decode_put_packet(context_, packet);
            if (result == MPP_OK) break;
            drain(display);
            usleep(1000);
        }
        mpp_packet_deinit(&packet);
        if (result != MPP_OK) throw std::runtime_error("MPP decode_put_packet 持续失败");
        return drain(display);
    }

private:
    uint32_t drain(DrmDisplay& display) {
        uint32_t displayed = 0;
        while (running) {
            MppFrame frame = nullptr;
            MPP_RET result = api_->decode_get_frame(context_, &frame);
            if (result != MPP_OK || !frame) break;
            if (mpp_frame_get_info_change(frame)) {
                const uint32_t width = mpp_frame_get_width(frame);
                const uint32_t height = mpp_frame_get_height(frame);
                const uint32_t horizontal_stride = mpp_frame_get_hor_stride(frame);
                const uint32_t vertical_stride = mpp_frame_get_ver_stride(frame);
                const uint32_t buffer_size = mpp_frame_get_buf_size(frame);
                std::cout << "MPP 解码信息: " << width << "x" << height << " stride="
                          << horizontal_stride << "x" << vertical_stride
                          << " buffer=" << buffer_size << "\n";
                if (!group_) {
                    if (mpp_buffer_group_get_internal(&group_, MPP_BUFFER_TYPE_DRM) != MPP_OK ||
                        mpp_buffer_group_limit_config(group_, buffer_size, 16) != MPP_OK ||
                        api_->control(context_, MPP_DEC_SET_EXT_BUF_GROUP, group_) != MPP_OK) {
                        mpp_frame_deinit(&frame);
                        throw std::runtime_error("配置 MPP DRM 解码缓冲池失败");
                    }
                }
                if (api_->control(context_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr) != MPP_OK) {
                    mpp_frame_deinit(&frame);
                    throw std::runtime_error("确认 MPP 分辨率变化失败");
                }
                mpp_frame_deinit(&frame);
                continue;
            }

            const uint32_t errors = mpp_frame_get_errinfo(frame);
            const uint32_t discard = mpp_frame_get_discard(frame);
            if (!errors && !discard && display.show(frame)) {
                ++displayed;  // frame 的所有权已转移给 DRM 显示器。
            } else {
                if (errors || discard)
                    std::cerr << "丢弃损坏解码帧: err=" << errors << " discard=" << discard << "\n";
                mpp_frame_deinit(&frame);
            }
        }
        return displayed;
    }

    MppCtx context_ = nullptr;
    MppApi* api_ = nullptr;
    MppBufferGroup group_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout.setf(std::ios::unitbuf);
        std::cerr.setf(std::ios::unitbuf);
        const Options options = parse_options(argc, argv);
        std::signal(SIGINT, stop_handler);
        std::signal(SIGTERM, stop_handler);

        wd::UdpFrameReceiver receiver(options.bind_address, options.port);
        if (!receiver.valid()) throw std::runtime_error(receiver.error());
        MppH264Decoder decoder;
        DrmDisplay display(options.drm_card);  // 先析构显示帧，再销毁解码器缓冲池。

        std::cout << "RX 已启动: udp://" << options.bind_address << ":" << options.port
                  << " -> MPP H.264 -> DRM HDMI\n";
        uint64_t interval_start = wd::monotonic_time_us();
        uint64_t interval_bytes = 0;
        uint32_t interval_packets = 0;
        uint32_t interval_displayed = 0;
        while (running) {
            wd::EncodedFrame frame;
            if (receiver.receive(frame, 200)) {
                interval_bytes += frame.data.size();
                ++interval_packets;
                interval_displayed += decoder.submit(frame, display);
            }
            const uint64_t now = wd::monotonic_time_us();
            if (now - interval_start >= 1000000) {
                const double seconds = (now - interval_start) / 1000000.0;
                std::cout << "RX " << interval_packets / seconds << " encoded fps, "
                          << interval_displayed / seconds << " displayed fps, "
                          << interval_bytes * 8.0 / seconds / 1000000.0 << " Mbit/s, transport_drop="
                          << receiver.dropped_frames() << "\n";
                interval_start = now;
                interval_bytes = 0;
                interval_packets = 0;
                interval_displayed = 0;
            }
        }
        std::cout << "RX 已停止，恢复原 HDMI 显示\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wd_rx: " << error.what() << "\n";
        return 1;
    }
}
