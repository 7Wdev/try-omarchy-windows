// SPDX-License-Identifier: MIT
// Process-owned authenticated broker channel; no privileged controller FD.
#pragma once
#include "guest_broker_peer.h"
#include "guest-kernel/wddm_aperture.h"
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sys/un.h>

namespace guest_broker {
struct ViewRequest {
    std::uint32_t kind, reserved;
    std::uint64_t offset, bytes, generation;
};
struct ViewReply { std::uint64_t cookie; };
static_assert(sizeof(ViewRequest) == 32 && sizeof(ViewReply) == 8, "Fixed broker view ABI");
template<class T> std::vector<std::uint8_t> encode(const T& value) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
    return {data, data + sizeof(value)};
}
template<class T> T decode(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != sizeof(T)) throw std::system_error(EPROTO, std::generic_category(), "Broker payload size");
    T value{}; std::memcpy(&value, bytes.data(), sizeof(value)); return value;
}
inline bool clientEnabled() {
    const auto* path = std::getenv("WDDM_BRIDGE_BROKER_SOCKET");
    return path && *path;
}
class Client {
    std::mutex guard;
    std::unique_ptr<Peer> peer;
    std::uint64_t sequence = 0;
    bool stopped = false;
    [[noreturn]] static void failed(int code, const char* message) {
        throw std::system_error(code, std::generic_category(), message);
    }
    Message requestUnlocked(Operation operation, const std::vector<std::uint8_t>& payload) {
        if (!peer || !peer->connected() || sequence == UINT64_MAX) failed(ESHUTDOWN, "Broker channel unavailable");
        const auto current = ++sequence;
        peer->send(operation, current, payload);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            auto result = peer->receive();
            if (result.state == ReceiveState::Disconnected) failed(EPIPE, "Broker disconnected");
            if (result.state == ReceiveState::Packet) {
                auto& message = result.message;
                if (message.frame.operation != operation || message.frame.sequence != current)
                    failed(EPROTO, "Mismatched broker response");
                if (message.frame.status) failed(message.frame.status, "Broker operation rejected");
                return std::move(message);
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (ms <= 0) failed(ETIMEDOUT, "Broker response deadline exceeded");
            peer->waitReadable(static_cast<int>(ms));
        }
    }
    void connectUnlocked() {
        if (stopped) failed(ESHUTDOWN, "Broker connection stopped");
        if (peer) return;
        const auto* path = std::getenv("WDDM_BRIDGE_BROKER_SOCKET");
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        if (!path || path[0] != '/' || std::strlen(path) >= sizeof(address.sun_path))
            failed(EINVAL, "Broker socket must have a bounded absolute path");
        std::memcpy(address.sun_path, path, std::strlen(path) + 1);
        Fd channel(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        if (channel.get() < 0) failed(errno, "Broker socket creation failed");
        const int enabled = 1;
        if (setsockopt(channel.get(), SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) ||
            connect(channel.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)))
            failed(errno, "Broker connect failed");
        peer = std::make_unique<Peer>(std::move(channel), 0, 0, true);
        auto hello = requestUnlocked(Operation::Hello, {});
        if (!hello.fds.empty()) failed(EPROTO, "Broker hello carried descriptors");
        (void)decode<wddm_aperture_info>(hello.payload);
    }
public:
    Message request(Operation operation, const std::vector<std::uint8_t>& payload = {}) {
        std::lock_guard<std::mutex> lock(guard);
        try { connectUnlocked(); return requestUnlocked(operation, payload); }
        catch (...) { stopped = true; if (peer) peer->disconnect(); throw; }
    }
    std::vector<std::uint8_t> driver(const std::vector<std::uint8_t>& packet) {
        auto reply = request(Operation::DriverCall, packet);
        if (!reply.fds.empty()) { disconnect(); failed(EPROTO, "Driver reply carried unexpected descriptors"); }
        return std::move(reply.payload);
    }
    wddm_aperture_info info() {
        auto reply = request(Operation::Hello);
        if (!reply.fds.empty()) { disconnect(); failed(EPROTO, "Layout reply carried descriptors"); }
        return decode<wddm_aperture_info>(reply.payload);
    }
    Message acquire(ViewRequest view) {
        auto reply = request(Operation::AcquireView, encode(view));
        if (reply.fds.size() != 1 || !decode<ViewReply>(reply.payload).cookie) {
            disconnect(); failed(EPROTO, "Broker view response mismatch");
        }
        return reply;
    }
    void release(std::uint64_t cookie) {
        auto reply = request(Operation::ReleaseView, encode(ViewReply{cookie}));
        if (!reply.fds.empty() || !reply.payload.empty()) { disconnect(); failed(EPROTO, "Broker release response mismatch"); }
    }
    void disconnect() {
        std::lock_guard<std::mutex> lock(guard);
        stopped = true; if (peer) peer->disconnect();
    }
};
inline Client& client() {
    // Keep the channel identity through all driver DSO finalizers. Explicit
    // shutdown closes only the channel; the server waits for kernel exit.
    static auto* instance = new Client;
    return *instance;
}
} // namespace guest_broker
