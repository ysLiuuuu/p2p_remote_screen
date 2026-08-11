#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wd {

constexpr uint16_t kFlagKeyFrame = 1u << 0;

struct EncodedFrame {
    uint32_t id = 0;
    uint16_t flags = 0;
    uint64_t timestamp_us = 0;
    std::vector<uint8_t> data;
};

class UdpFrameSender {
public:
    UdpFrameSender(const std::string& host, uint16_t port);
    ~UdpFrameSender();
    UdpFrameSender(const UdpFrameSender&) = delete;
    UdpFrameSender& operator=(const UdpFrameSender&) = delete;

    bool valid() const { return fd_ >= 0; }
    const std::string& error() const { return error_; }
    bool send(const EncodedFrame& frame);

private:
    int fd_ = -1;
    std::string error_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class UdpFrameReceiver {
public:
    UdpFrameReceiver(const std::string& bind_address, uint16_t port);
    ~UdpFrameReceiver();
    UdpFrameReceiver(const UdpFrameReceiver&) = delete;
    UdpFrameReceiver& operator=(const UdpFrameReceiver&) = delete;

    bool valid() const { return fd_ >= 0; }
    const std::string& error() const { return error_; }
    bool receive(EncodedFrame& frame, int timeout_ms);
    uint64_t dropped_frames() const;

private:
    int fd_ = -1;
    std::string error_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

uint64_t monotonic_time_us();

}  // namespace wd
