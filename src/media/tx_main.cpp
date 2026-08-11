#define MODULE_TAG "wd_tx"

#include "video_transport.h"

#include <im2d.h>
#include <linux/videodev2.h>
#include <rk_mpi.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t running = 1;
void stop_handler(int) { running = 0; }

uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool xioctl(int fd, unsigned long request, void* argument) {
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

struct Options {
    std::string device = "/dev/video0";
    std::string host = "192.168.49.100";
    uint16_t port = 5004;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrate = 12000000;
};

void usage(const char* name) {
    std::cout << "用法: " << name
              << " [--device /dev/video0] [--host 192.168.49.100] [--port 5004]"
                 " [--width 1920] [--height 1080] [--fps 60] [--bitrate 12000000]\n";
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
        if (key == "--device") options.device = value;
        else if (key == "--host") options.host = value;
        else if (key == "--port") options.port = static_cast<uint16_t>(std::stoul(value));
        else if (key == "--width") options.width = std::stoul(value);
        else if (key == "--height") options.height = std::stoul(value);
        else if (key == "--fps") options.fps = std::stoul(value);
        else if (key == "--bitrate") options.bitrate = std::stoul(value);
        else throw std::runtime_error("未知参数: " + key);
    }
    if (!options.width || !options.height || !options.fps || !options.bitrate || !options.port)
        throw std::runtime_error("分辨率、帧率、码率和端口必须大于 0");
    return options;
}

struct CaptureBuffer {
    void* map = MAP_FAILED;
    size_t length = 0;
    int dma_fd = -1;
};

class HdmiCapture {
public:
    explicit HdmiCapture(const Options& options) {
        fd_ = open(options.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) throw std::runtime_error("打开 " + options.device + " 失败: " + std::strerror(errno));

        v4l2_capability capability{};
        if (!xioctl(fd_, VIDIOC_QUERYCAP, &capability) ||
            !(capability.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ||
            !(capability.device_caps & V4L2_CAP_STREAMING)) {
            throw std::runtime_error("设备不支持多平面流式采集");
        }

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        format.fmt.pix_mp.width = options.width;
        format.fmt.pix_mp.height = options.height;
        format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_BGR24;
        format.fmt.pix_mp.field = V4L2_FIELD_NONE;
        if (!xioctl(fd_, VIDIOC_S_FMT, &format))
            throw std::runtime_error(std::string("VIDIOC_S_FMT 失败: ") + std::strerror(errno));
        if (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_BGR24 || format.fmt.pix_mp.num_planes != 1)
            throw std::runtime_error("HDMI RX 未接受 BGR24 单平面格式");
        width_ = format.fmt.pix_mp.width;
        height_ = format.fmt.pix_mp.height;
        bytes_per_line_ = format.fmt.pix_mp.plane_fmt[0].bytesperline;
        if (bytes_per_line_ < width_ * 3 || bytes_per_line_ % 3)
            throw std::runtime_error("BGR24 行跨度无效");

        v4l2_streamparm parameters{};
        parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        parameters.parm.capture.timeperframe.numerator = 1;
        parameters.parm.capture.timeperframe.denominator = options.fps;
        xioctl(fd_, VIDIOC_S_PARM, &parameters);  // HDMI RX 可能不实现，锁定输入时序即可。

        v4l2_requestbuffers request{};
        request.count = 4;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        request.memory = V4L2_MEMORY_MMAP;
        if (!xioctl(fd_, VIDIOC_REQBUFS, &request) || request.count < 2)
            throw std::runtime_error(std::string("申请 V4L2 缓冲失败: ") + std::strerror(errno));
        buffers_.resize(request.count);

        for (uint32_t i = 0; i < request.count; ++i) {
            v4l2_plane plane{};
            v4l2_buffer buffer{};
            buffer.type = request.type;
            buffer.memory = request.memory;
            buffer.index = i;
            buffer.length = 1;
            buffer.m.planes = &plane;
            if (!xioctl(fd_, VIDIOC_QUERYBUF, &buffer))
                throw std::runtime_error("VIDIOC_QUERYBUF 失败");
            buffers_[i].length = plane.length;
            buffers_[i].map = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                   fd_, plane.m.mem_offset);
            if (buffers_[i].map == MAP_FAILED)
                throw std::runtime_error(std::string("mmap V4L2 缓冲失败: ") + std::strerror(errno));

            v4l2_exportbuffer export_buffer{};
            export_buffer.type = request.type;
            export_buffer.index = i;
            export_buffer.plane = 0;
            export_buffer.flags = O_CLOEXEC;
            if (!xioctl(fd_, VIDIOC_EXPBUF, &export_buffer))
                throw std::runtime_error(std::string("导出采集 DMA-BUF 失败: ") + std::strerror(errno));
            buffers_[i].dma_fd = export_buffer.fd;
            queue(i);
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (!xioctl(fd_, VIDIOC_STREAMON, &type))
            throw std::runtime_error(std::string("VIDIOC_STREAMON 失败: ") + std::strerror(errno));
        streaming_ = true;
    }

    ~HdmiCapture() {
        if (fd_ >= 0 && streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);
        }
        for (auto& buffer : buffers_) {
            if (buffer.dma_fd >= 0) close(buffer.dma_fd);
            if (buffer.map != MAP_FAILED) munmap(buffer.map, buffer.length);
        }
        if (fd_ >= 0) close(fd_);
    }

    bool dequeue(uint32_t& index, int timeout_ms) {
        pollfd descriptor{fd_, POLLIN, 0};
        int result;
        do {
            result = poll(&descriptor, 1, timeout_ms);
        } while (result < 0 && errno == EINTR && running);
        if (result <= 0) return false;

        v4l2_plane plane{};
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (!xioctl(fd_, VIDIOC_DQBUF, &buffer)) {
            if (errno == EAGAIN) return false;
            throw std::runtime_error(std::string("VIDIOC_DQBUF 失败: ") + std::strerror(errno));
        }
        index = buffer.index;
        return true;
    }

    void queue(uint32_t index) {
        v4l2_plane plane{};
        plane.length = buffers_.empty() ? 0 : buffers_[index].length;
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (!xioctl(fd_, VIDIOC_QBUF, &buffer))
            throw std::runtime_error(std::string("VIDIOC_QBUF 失败: ") + std::strerror(errno));
    }

    int dma_fd(uint32_t index) const { return buffers_.at(index).dma_fd; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t pixel_stride() const { return bytes_per_line_ / 3; }

private:
    int fd_ = -1;
    bool streaming_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t bytes_per_line_ = 0;
    std::vector<CaptureBuffer> buffers_;
};

class MppH264Encoder {
public:
    MppH264Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate)
        : width_(width), height_(height), hor_stride_(align_up(width, 16)),
          ver_stride_(align_up(height, 16)) {
        const size_t frame_size = static_cast<size_t>(hor_stride_) * ver_stride_ * 3 / 2;
        if (mpp_buffer_group_get_internal(&group_, MPP_BUFFER_TYPE_DRM) != MPP_OK ||
            mpp_buffer_get(group_, &frame_buffer_, frame_size) != MPP_OK ||
            mpp_buffer_get(group_, &packet_buffer_, width * height) != MPP_OK)
            throw std::runtime_error("申请 MPP 编码缓冲失败");
        if (mpp_create(&context_, &api_) != MPP_OK)
            throw std::runtime_error("mpp_create 编码器失败");
        MppPollType timeout = MPP_POLL_BLOCK;
        if (api_->control(context_, MPP_SET_OUTPUT_TIMEOUT, &timeout) != MPP_OK ||
            mpp_init(context_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK)
            throw std::runtime_error("mpp_init H.264 编码器失败");

        MppEncCfg config = nullptr;
        if (mpp_enc_cfg_init(&config) != MPP_OK ||
            api_->control(context_, MPP_ENC_GET_CFG, config) != MPP_OK)
            throw std::runtime_error("读取 MPP 编码配置失败");
        mpp_enc_cfg_set_s32(config, "codec:type", MPP_VIDEO_CodingAVC);
        mpp_enc_cfg_set_s32(config, "prep:width", width_);
        mpp_enc_cfg_set_s32(config, "prep:height", height_);
        mpp_enc_cfg_set_s32(config, "prep:hor_stride", hor_stride_);
        mpp_enc_cfg_set_s32(config, "prep:ver_stride", ver_stride_);
        mpp_enc_cfg_set_s32(config, "prep:format", MPP_FMT_YUV420SP);
        mpp_enc_cfg_set_s32(config, "prep:range", MPP_FRAME_RANGE_MPEG);
        mpp_enc_cfg_set_s32(config, "rc:mode", MPP_ENC_RC_MODE_CBR);
        mpp_enc_cfg_set_s32(config, "rc:fps_in_flex", 0);
        mpp_enc_cfg_set_s32(config, "rc:fps_in_num", fps);
        mpp_enc_cfg_set_s32(config, "rc:fps_in_denom", 1);
        mpp_enc_cfg_set_s32(config, "rc:fps_out_flex", 0);
        mpp_enc_cfg_set_s32(config, "rc:fps_out_num", fps);
        mpp_enc_cfg_set_s32(config, "rc:fps_out_denom", 1);
        mpp_enc_cfg_set_s32(config, "rc:bps_target", bitrate);
        mpp_enc_cfg_set_s32(config, "rc:bps_max", bitrate * 17 / 16);
        mpp_enc_cfg_set_s32(config, "rc:bps_min", bitrate * 15 / 16);
        mpp_enc_cfg_set_s32(config, "rc:qp_init", -1);
        mpp_enc_cfg_set_s32(config, "rc:qp_max", 48);
        mpp_enc_cfg_set_s32(config, "rc:qp_min", 10);
        mpp_enc_cfg_set_s32(config, "rc:qp_max_i", 48);
        mpp_enc_cfg_set_s32(config, "rc:qp_min_i", 10);
        mpp_enc_cfg_set_s32(config, "rc:qp_ip", 2);
        mpp_enc_cfg_set_s32(config, "rc:gop", fps);
        mpp_enc_cfg_set_s32(config, "h264:profile", 100);
        mpp_enc_cfg_set_s32(config, "h264:level", fps > 30 ? 42 : 40);
        mpp_enc_cfg_set_s32(config, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(config, "h264:cabac_idc", 0);
        mpp_enc_cfg_set_s32(config, "h264:trans8x8", 1);
        if (api_->control(context_, MPP_ENC_SET_CFG, config) != MPP_OK) {
            mpp_enc_cfg_deinit(config);
            throw std::runtime_error("设置 MPP 编码配置失败");
        }
        mpp_enc_cfg_deinit(config);

        MppPacket header_packet = nullptr;
        mpp_packet_init_with_buffer(&header_packet, packet_buffer_);
        mpp_packet_set_length(header_packet, 0);
        if (api_->control(context_, MPP_ENC_GET_HDR_SYNC, header_packet) != MPP_OK)
            throw std::runtime_error("获取 H.264 SPS/PPS 失败");
        const auto* position = static_cast<const uint8_t*>(mpp_packet_get_pos(header_packet));
        const size_t length = mpp_packet_get_length(header_packet);
        header_.assign(position, position + length);
        mpp_packet_deinit(&header_packet);
    }

    ~MppH264Encoder() {
        if (context_) mpp_destroy(context_);
        if (packet_buffer_) mpp_buffer_put(packet_buffer_);
        if (frame_buffer_) mpp_buffer_put(frame_buffer_);
        if (group_) mpp_buffer_group_put(group_);
    }

    void convert_bgr_to_nv12(int source_fd, uint32_t source_stride) {
        rga_buffer_t source = wrapbuffer_fd(source_fd, width_, height_, RK_FORMAT_BGR_888,
                                            static_cast<int>(source_stride), static_cast<int>(height_));
        rga_buffer_t destination = wrapbuffer_fd(mpp_buffer_get_fd(frame_buffer_), width_, height_,
                                                 RK_FORMAT_YCbCr_420_SP,
                                                 static_cast<int>(hor_stride_),
                                                 static_cast<int>(ver_stride_));
        IM_STATUS status = imcvtcolor(source, destination, RK_FORMAT_BGR_888,
                                      RK_FORMAT_YCbCr_420_SP);
        if (status != IM_STATUS_SUCCESS)
            throw std::runtime_error(std::string("RGA BGR->NV12 失败: ") + imStrError(status));
    }

    std::vector<uint8_t> encode(uint64_t timestamp_us, bool& keyframe) {
        MppFrame frame = nullptr;
        if (mpp_frame_init(&frame) != MPP_OK) throw std::runtime_error("mpp_frame_init 失败");
        mpp_frame_set_width(frame, width_);
        mpp_frame_set_height(frame, height_);
        mpp_frame_set_hor_stride(frame, hor_stride_);
        mpp_frame_set_ver_stride(frame, ver_stride_);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_pts(frame, timestamp_us);
        mpp_frame_set_buffer(frame, frame_buffer_);

        MppPacket output = nullptr;
        mpp_packet_init_with_buffer(&output, packet_buffer_);
        mpp_packet_set_length(output, 0);
        mpp_meta_set_packet(mpp_frame_get_meta(frame), KEY_OUTPUT_PACKET, output);
        MPP_RET result = api_->encode_put_frame(context_, frame);
        mpp_frame_deinit(&frame);
        if (result != MPP_OK) {
            mpp_packet_deinit(&output);
            throw std::runtime_error("MPP encode_put_frame 失败");
        }
        result = api_->encode_get_packet(context_, &output);
        if (result != MPP_OK || !output) {
            if (output) mpp_packet_deinit(&output);
            throw std::runtime_error("MPP encode_get_packet 失败");
        }
        const auto* position = static_cast<const uint8_t*>(mpp_packet_get_pos(output));
        const size_t length = mpp_packet_get_length(output);
        keyframe = contains_idr(position, length);
        std::vector<uint8_t> bytes;
        bytes.reserve(length + (keyframe ? header_.size() : 0));
        if (keyframe) bytes.insert(bytes.end(), header_.begin(), header_.end());
        bytes.insert(bytes.end(), position, position + length);
        mpp_packet_deinit(&output);
        return bytes;
    }

private:
    static bool contains_idr(const uint8_t* data, size_t length) {
        for (size_t i = 0; i + 4 < length; ++i) {
            size_t nal = 0;
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) nal = i + 3;
            else if (i + 4 < length && data[i] == 0 && data[i + 1] == 0 &&
                     data[i + 2] == 0 && data[i + 3] == 1) nal = i + 4;
            if (nal && nal < length && (data[nal] & 0x1f) == 5) return true;
        }
        return false;
    }

    uint32_t width_;
    uint32_t height_;
    uint32_t hor_stride_;
    uint32_t ver_stride_;
    MppBufferGroup group_ = nullptr;
    MppBuffer frame_buffer_ = nullptr;
    MppBuffer packet_buffer_ = nullptr;
    MppCtx context_ = nullptr;
    MppApi* api_ = nullptr;
    std::vector<uint8_t> header_;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout.setf(std::ios::unitbuf);
        std::cerr.setf(std::ios::unitbuf);
        const Options options = parse_options(argc, argv);
        std::signal(SIGINT, stop_handler);
        std::signal(SIGTERM, stop_handler);

        HdmiCapture capture(options);
        if (capture.width() != options.width || capture.height() != options.height)
            throw std::runtime_error("HDMI RX 返回的分辨率与请求不一致");
        MppH264Encoder encoder(capture.width(), capture.height(), options.fps, options.bitrate);
        wd::UdpFrameSender sender(options.host, options.port);
        if (!sender.valid()) throw std::runtime_error(sender.error());

        std::cout << "TX 已启动: " << capture.width() << "x" << capture.height() << "@"
                  << options.fps << " BGR24 -> RGA NV12 -> MPP H.264 -> udp://"
                  << options.host << ":" << options.port << "\n";
        uint32_t frame_id = 0;
        uint64_t interval_start = wd::monotonic_time_us();
        uint64_t interval_bytes = 0;
        uint32_t interval_frames = 0;
        while (running) {
            uint32_t index = 0;
            if (!capture.dequeue(index, 1000)) continue;
            try {
                const uint64_t timestamp = wd::monotonic_time_us();
                encoder.convert_bgr_to_nv12(capture.dma_fd(index), capture.pixel_stride());
                bool keyframe = false;
                auto bytes = encoder.encode(timestamp, keyframe);
                wd::EncodedFrame frame{frame_id++, static_cast<uint16_t>(keyframe ? wd::kFlagKeyFrame : 0),
                                       timestamp, std::move(bytes)};
                if (!sender.send(frame)) throw std::runtime_error(sender.error());
                interval_bytes += frame.data.size();
                ++interval_frames;
            } catch (...) {
                capture.queue(index);
                throw;
            }
            capture.queue(index);

            const uint64_t now = wd::monotonic_time_us();
            if (now - interval_start >= 1000000) {
                const double seconds = (now - interval_start) / 1000000.0;
                std::cout << "TX " << interval_frames / seconds << " fps, "
                          << interval_bytes * 8.0 / seconds / 1000000.0 << " Mbit/s, frame="
                          << frame_id << "\n";
                interval_start = now;
                interval_bytes = 0;
                interval_frames = 0;
            }
        }
        std::cout << "TX 已停止\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wd_tx: " << error.what() << "\n";
        return 1;
    }
}
