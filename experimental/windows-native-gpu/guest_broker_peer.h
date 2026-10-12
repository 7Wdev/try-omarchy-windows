// SPDX-License-Identifier: MIT
// Guest broker ingress and process lifetime. No GPU handle/offset authorization.
#pragma once
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77 // Linux UAPI; require kernel support, never use PID lookup.
#endif
#ifndef SO_PASSPIDFD
#define SO_PASSPIDFD 76
#endif

namespace guest_broker {
constexpr std::uint32_t Magic = 0x4b524257; // WBRK, little endian
constexpr std::uint16_t Version = 1;
constexpr std::size_t MaxPayload = 4096, MaxFds = 8;
enum class Operation : std::uint16_t { Hello=1, DriverCall, AcquireView, ReleaseView, ResolveObject };
struct Frame {
    std::uint32_t magic = Magic;
    std::uint16_t version = Version;
    Operation operation = Operation::Hello;
    std::uint32_t bytes = 0, fds = 0;
    std::uint64_t sequence = 1, reserved = 0;
};
static_assert(sizeof(Frame) == 32 && offsetof(Frame, sequence) == 16, "Fixed broker ABI");
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Broker wire encoding requires little endian
#endif

class Fd {
    int value = -1;
public:
    explicit Fd(int fd = -1) noexcept : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { if (value >= 0) ::close(value); value = std::exchange(other.value, -1); }
        return *this;
    }
    int get() const noexcept { return value; }
};
struct Message { Frame frame; std::vector<std::uint8_t> payload; std::vector<Fd> fds; };
enum class ReceiveState { Idle, Packet, Disconnected };
struct Received { ReceiveState state; Message message; };

class Peer {
    Fd channel, process;
    ucred identity{};
    std::uint64_t nextSequence = 1;
    bool sequenceExhausted = false;
    [[noreturn]] static void error(int code, const char* text) {
        throw std::system_error(code, std::generic_category(), text);
    }
    template<class T> static T option(int fd, int name) {
        T value{}; socklen_t bytes = sizeof(value);
        if (getsockopt(fd, SOL_SOCKET, name, &value, &bytes)) error(errno, "Broker socket option");
        if (bytes != sizeof(value)) error(EPROTO, "Broker socket option size");
        return value;
    }
    [[noreturn]] void reject(int code, const char* text) { disconnect(); error(code, text); }
public:
    // Ownership is transferred even if construction fails. The listener must
    // enable SO_PASSCRED before accept; every received packet is authenticated.
    Peer(Fd accepted, uid_t permittedUid, gid_t permittedGid) : channel(std::move(accepted)) {
        if (option<int>(channel.get(), SO_DOMAIN) != AF_UNIX ||
            option<int>(channel.get(), SO_TYPE) != SOCK_SEQPACKET ||
            option<int>(channel.get(), SO_ACCEPTCONN) != 0 ||
            option<int>(channel.get(), SO_PASSCRED) != 1 ||
            option<int>(channel.get(), SO_PASSPIDFD) != 0)
            error(EPROTO, "Broker requires a credential-enabled accepted Unix seqpacket socket");
        identity = option<ucred>(channel.get(), SO_PEERCRED);
        if (identity.pid <= 0 || identity.uid != permittedUid || identity.gid != permittedGid)
            error(EPERM, "Broker peer identity denied");
        // This obtains the socket's pinned kernel process identity. Opening a
        // numeric PID here would create a connect/exit/PID-reuse race.
        process = Fd(option<int>(channel.get(), SO_PEERPIDFD));
        if (process.get() < 0) error(EPROTO, "Invalid broker process descriptor");
        if (fcntl(process.get(), F_SETFD, FD_CLOEXEC) ||
            fcntl(channel.get(), F_SETFD, FD_CLOEXEC)) error(errno, "Broker descriptor flags");
        if (processExited()) error(ESRCH, "Broker peer already exited");
    }
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    pid_t pid() const noexcept { return identity.pid; }
    bool connected() const noexcept { return channel.get() >= 0; }
    void disconnect() noexcept { channel = Fd(); } // Does not drop process identity.
    bool processExited() const {
        pollfd event{process.get(), POLLIN, 0};
        int result;
        do { result = poll(&event, 1, 0); } while (result < 0 && errno == EINTR);
        if (result < 0) error(errno, "Broker process poll");
        if (event.revents & (POLLNVAL | POLLERR)) error(EIO, "Broker process descriptor failed");
        return (event.revents & (POLLIN | POLLHUP)) != 0;
    }
    Received receive() {
        if (!connected()) return {ReceiveState::Disconnected, {}};
        std::array<std::uint8_t, sizeof(Frame) + MaxPayload> packet{};
        union Control {
            cmsghdr alignment;
            char data[CMSG_SPACE(sizeof(ucred)) + CMSG_SPACE(MaxFds * sizeof(int))];
        } control{};
        iovec data{packet.data(), packet.size()};
        msghdr input{}; input.msg_iov = &data; input.msg_iovlen = 1;
        input.msg_control = control.data; input.msg_controllen = sizeof(control.data);
        Message result;
        // Reserve before receiving: allocation failure must not strand kernel-
        // installed SCM_RIGHTS descriptors. The control buffer bounds this count.
        result.fds.reserve(sizeof(control.data) / sizeof(int));
        const auto bytes = recvmsg(channel.get(), &input, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {ReceiveState::Idle, {}};
            reject(errno, "Broker receive failed");
        }
        bool credentialsFound = false, malformed = false;
        ucred sender{};
        // Adopt every delivered descriptor before any rejection, including a
        // truncated/empty packet. RAII closes them on every error path.
        for (auto* h = CMSG_FIRSTHDR(&input); h; h = CMSG_NXTHDR(&input, h)) {
            if (h->cmsg_level != SOL_SOCKET || h->cmsg_len < CMSG_LEN(0)) { malformed = true; continue; }
            const auto count = h->cmsg_len - CMSG_LEN(0);
            if (h->cmsg_type == SCM_RIGHTS) {
                if (count % sizeof(int)) malformed = true;
                for (std::size_t i = 0; i + sizeof(int) <= count; i += sizeof(int)) {
                    int fd = -1; std::memcpy(&fd, CMSG_DATA(h) + i, sizeof(fd));
                    result.fds.emplace_back(fd);
                }
            } else if (h->cmsg_type == 4 /* Linux SCM_PIDFD */ && count == sizeof(int)) {
                int fd = -1; std::memcpy(&fd, CMSG_DATA(h), sizeof(fd));
                result.fds.emplace_back(fd); malformed = true;
            } else if (h->cmsg_type == SCM_CREDENTIALS && count == sizeof(ucred) && !credentialsFound) {
                std::memcpy(&sender, CMSG_DATA(h), sizeof(sender)); credentialsFound = true;
            } else malformed = true;
        }
        if (!bytes) { disconnect(); return {ReceiveState::Disconnected, {}}; }
        if (malformed || (input.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || result.fds.size() > MaxFds)
            reject(EPROTO, "Malformed broker ancillary data or truncated packet");
        if (!credentialsFound || sender.pid != identity.pid || sender.uid != identity.uid || sender.gid != identity.gid)
            reject(EPERM, "Broker per-message process identity changed");
        if (processExited()) reject(ESRCH, "Broker request from exited process");
        if (static_cast<std::size_t>(bytes) < sizeof(Frame)) reject(EPROTO, "Short broker frame");
        std::memcpy(&result.frame, packet.data(), sizeof(Frame));
        const auto& f = result.frame;
        if (f.magic != Magic || f.version != Version || f.reserved || f.bytes > MaxPayload ||
            sizeof(Frame) + f.bytes != static_cast<std::size_t>(bytes) || f.fds != result.fds.size() ||
            f.operation < Operation::Hello || f.operation > Operation::ResolveObject ||
            sequenceExhausted || f.sequence != nextSequence)
            reject(EPROTO, "Invalid broker version, bounds, operation or sequence");
        result.payload.assign(packet.begin() + sizeof(Frame), packet.begin() + bytes);
        if (nextSequence == std::numeric_limits<std::uint64_t>::max()) sequenceExhausted = true;
        else ++nextSequence;
        return {ReceiveState::Packet, std::move(result)};
    }
};
// EOF/protocol rejection forbids more work but is not process exit. The broker
// must retain per-process CPU/queue backing until processExited(), and must
// separately retain shared NT references until the last capability FD closes.
// Destroying a Peer never authorizes releasing either resource class.
} // namespace guest_broker
