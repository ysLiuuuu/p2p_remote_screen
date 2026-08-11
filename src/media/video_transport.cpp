#include "video_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <unordered_map>

namespace wd {
namespace {

constexpr uint32_t kMagic = 0x5744484d;  // "WDHM"
constexpr uint16_t kVersion = 1;
constexpr size_t kMaxPayload = 1320;
constexpr uint32_t kMaxFrameSize = 4 * 1024 * 1024;
constexpr size_t kMaxAssemblies = 8;

#pragma pack(push, 1)
struct WireHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t frame_id;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint16_t payload_size;
    uint16_t reserved;
    uint32_t frame_size;
    uint32_t timestamp_hi;
    uint32_t timestamp_lo;
};

#pragma pack(pop)

static_assert(sizeof(WireHeader) == 32, "unexpected transport header size");

struct Assembly {
    uint16_t flags = 0;
    uint16_t fragment_count = 0;
    uint32_t received_count = 0;
    uint32_t frame_size = 0;
    uint64_t timestamp_us = 0;
    uint64_t last_update_us = 0;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> received;
};

bool newer(uint32_t value, uint32_t reference) {
    return static_cast<int32_t>(value - reference) > 0;
}

}  // namespace

struct UdpFrameSender::Impl {
    sockaddr_in peer{};
};

UdpFrameSender::UdpFrameSender(const std::string& host, uint16_t port)
    : impl_(std::make_unique<Impl>()) {
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        error_ = std::strerror(errno);
        return;
    }
    int send_buffer = 4 * 1024 * 1024;
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    impl_->peer.sin_family = AF_INET;
    impl_->peer.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &impl_->peer.sin_addr) != 1) {
        error_ = "invalid destination IPv4 address: " + host;
        close(fd_);
        fd_ = -1;
    }
}

UdpFrameSender::~UdpFrameSender() {
    if (fd_ >= 0) close(fd_);
}

bool UdpFrameSender::send(const EncodedFrame& frame) {
    if (fd_ < 0 || frame.data.empty() || frame.data.size() > kMaxFrameSize) return false;
    const size_t count = (frame.data.size() + kMaxPayload - 1) / kMaxPayload;
    if (count == 0 || count > UINT16_MAX) return false;

    for (size_t index = 0; index < count; ++index) {
        const size_t offset = index * kMaxPayload;
        const size_t payload_size = std::min(kMaxPayload, frame.data.size() - offset);
        WireHeader header{};
        header.magic = htonl(kMagic);
        header.version = htons(kVersion);
        header.flags = htons(frame.flags);
        header.frame_id = htonl(frame.id);
        header.fragment_index = htons(static_cast<uint16_t>(index));
        header.fragment_count = htons(static_cast<uint16_t>(count));
        header.payload_size = htons(static_cast<uint16_t>(payload_size));
        header.frame_size = htonl(static_cast<uint32_t>(frame.data.size()));
        header.timestamp_hi = htonl(static_cast<uint32_t>(frame.timestamp_us >> 32));
        header.timestamp_lo = htonl(static_cast<uint32_t>(frame.timestamp_us));

        iovec vectors[2] = {
            {&header, sizeof(header)},
            {const_cast<uint8_t*>(frame.data.data() + offset), payload_size},
        };
        msghdr message{};
        message.msg_name = &impl_->peer;
        message.msg_namelen = sizeof(impl_->peer);
        message.msg_iov = vectors;
        message.msg_iovlen = 2;
        ssize_t sent;
        do {
            sent = sendmsg(fd_, &message, 0);
        } while (sent < 0 && errno == EINTR);
        if (sent != static_cast<ssize_t>(sizeof(header) + payload_size)) {
            error_ = std::string("sendmsg: ") + std::strerror(errno);
            return false;
        }
    }
    return true;
}

struct UdpFrameReceiver::Impl {
    std::unordered_map<uint32_t, Assembly> assemblies;
    uint32_t last_delivered = 0;
    bool delivered_any = false;
    uint64_t dropped = 0;
};

UdpFrameReceiver::UdpFrameReceiver(const std::string& bind_address, uint16_t port)
    : impl_(std::make_unique<Impl>()) {
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        error_ = std::strerror(errno);
        return;
    }
    int reuse = 1;
    int receive_buffer = 8 * 1024 * 1024;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
#ifdef SO_RCVBUFFORCE
    // RX 以 root 运行时绕过 net.core.rmem_max，吸收 IDR 帧的瞬时 UDP 突发。
    int actual_buffer = 0;
    socklen_t actual_length = sizeof(actual_buffer);
    if (getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &actual_buffer, &actual_length) == 0 &&
        actual_buffer < receive_buffer) {
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUFFORCE, &receive_buffer, sizeof(receive_buffer));
    }
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        error_ = "invalid bind IPv4 address: " + bind_address;
    } else if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error_ = std::string("bind: ") + std::strerror(errno);
    } else {
        return;
    }
    close(fd_);
    fd_ = -1;
}

UdpFrameReceiver::~UdpFrameReceiver() {
    if (fd_ >= 0) close(fd_);
}

bool UdpFrameReceiver::receive(EncodedFrame& frame, int timeout_ms) {
    if (fd_ < 0) return false;
    const uint64_t deadline = monotonic_time_us() + static_cast<uint64_t>(std::max(0, timeout_ms)) * 1000;
    std::vector<uint8_t> datagram(sizeof(WireHeader) + kMaxPayload);

    while (true) {
        const uint64_t now = monotonic_time_us();
        if (now >= deadline) return false;
        pollfd descriptor{fd_, POLLIN, 0};
        int wait_ms = static_cast<int>(std::max<uint64_t>(1, (deadline - now + 999) / 1000));
        int result;
        do {
            result = poll(&descriptor, 1, wait_ms);
        } while (result < 0 && errno == EINTR);
        if (result <= 0) return false;

        ssize_t size = recv(fd_, datagram.data(), datagram.size(), 0);
        if (size < static_cast<ssize_t>(sizeof(WireHeader))) continue;
        WireHeader wire{};
        std::memcpy(&wire, datagram.data(), sizeof(wire));
        const uint32_t magic = ntohl(wire.magic);
        const uint16_t version = ntohs(wire.version);
        const uint16_t flags = ntohs(wire.flags);
        const uint32_t id = ntohl(wire.frame_id);
        const uint16_t fragment_index = ntohs(wire.fragment_index);
        const uint16_t fragment_count = ntohs(wire.fragment_count);
        const uint16_t payload_size = ntohs(wire.payload_size);
        const uint32_t frame_size = ntohl(wire.frame_size);
        const uint64_t timestamp = (static_cast<uint64_t>(ntohl(wire.timestamp_hi)) << 32) |
                                   ntohl(wire.timestamp_lo);

        if (magic != kMagic || version != kVersion || fragment_count == 0 ||
            fragment_index >= fragment_count || payload_size > kMaxPayload ||
            sizeof(WireHeader) + payload_size != static_cast<size_t>(size) ||
            frame_size == 0 || frame_size > kMaxFrameSize ||
            static_cast<size_t>(fragment_count - 1) * kMaxPayload >= frame_size) {
            continue;
        }
        if (impl_->delivered_any && !newer(id, impl_->last_delivered)) continue;

        auto [it, inserted] = impl_->assemblies.try_emplace(id);
        Assembly& assembly = it->second;
        if (inserted) {
            assembly.flags = flags;
            assembly.fragment_count = fragment_count;
            assembly.frame_size = frame_size;
            assembly.timestamp_us = timestamp;
            assembly.bytes.resize(frame_size);
            assembly.received.assign(fragment_count, 0);
        }
        if (assembly.fragment_count != fragment_count || assembly.frame_size != frame_size ||
            assembly.flags != flags || assembly.timestamp_us != timestamp) {
            impl_->assemblies.erase(it);
            ++impl_->dropped;
            continue;
        }
        const size_t offset = static_cast<size_t>(fragment_index) * kMaxPayload;
        if (offset + payload_size > assembly.bytes.size()) continue;
        if (!assembly.received[fragment_index]) {
            std::memcpy(assembly.bytes.data() + offset,
                        datagram.data() + sizeof(WireHeader), payload_size);
            assembly.received[fragment_index] = 1;
            ++assembly.received_count;
        }
        assembly.last_update_us = monotonic_time_us();

        if (assembly.received_count == assembly.fragment_count) {
            frame.id = id;
            frame.flags = assembly.flags;
            frame.timestamp_us = assembly.timestamp_us;
            frame.data = std::move(assembly.bytes);
            impl_->assemblies.erase(it);
            impl_->last_delivered = id;
            impl_->delivered_any = true;
            for (auto old = impl_->assemblies.begin(); old != impl_->assemblies.end();) {
                if (!newer(old->first, id)) {
                    old = impl_->assemblies.erase(old);
                    ++impl_->dropped;
                } else {
                    ++old;
                }
            }
            return true;
        }

        const uint64_t cleanup_now = monotonic_time_us();
        for (auto old = impl_->assemblies.begin(); old != impl_->assemblies.end();) {
            if (cleanup_now - old->second.last_update_us > 250000 ||
                impl_->assemblies.size() > kMaxAssemblies) {
                old = impl_->assemblies.erase(old);
                ++impl_->dropped;
            } else {
                ++old;
            }
        }
    }
}

uint64_t UdpFrameReceiver::dropped_frames() const { return impl_->dropped; }

uint64_t monotonic_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace wd
