// SPDX-License-Identifier: MIT
// Linux QEMU guest client. Tests driver control operations, not rendering.
#include "driver_wire.h"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <iostream>
#include <stdexcept>
using namespace driver_bridge;
struct Port {
    int fd;
    explicit Port(const char* path) : fd(open(path, O_RDWR | O_CLOEXEC)) {
        if (fd < 0) throw std::runtime_error("Cannot open virtio serial port");
    }
    ~Port() { close(fd); }
    void transfer(void* data, std::size_t size, bool reading) {
        auto p = static_cast<std::uint8_t*>(data);
        while (size) {
            pollfd ready{fd, static_cast<short>(reading ? POLLIN : POLLOUT), 0};
            const int result = poll(&ready, 1, 10000);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0 || (ready.revents & (POLLERR | POLLNVAL))) throw std::runtime_error("Port timed out or failed");
            const auto n = reading ? read(fd, p, size) : write(fd, p, size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("Port disconnected");
            p += n; size -= static_cast<std::size_t>(n);
        }
    }
    std::vector<std::uint8_t> exchange(std::vector<std::uint8_t> packet) {
        auto length = static_cast<std::uint32_t>(packet.size()); transfer(&length, 4, false);
        transfer(packet.data(), packet.size(), false); transfer(&length, 4, true);
        if (length < sizeof(Header) || length > native_gpu::MaxPacket) throw std::runtime_error("Invalid reply length");
        packet.resize(length); transfer(packet.data(), packet.size(), true); return packet;
    }
};
static Header unpack(const std::vector<std::uint8_t>& packet) {
    Header h{}; std::memcpy(&h, packet.data(), sizeof h);
    if (h.padding) throw std::runtime_error("Invalid response padding");
    return h;
}
static std::uint32_t operation(Port& port, Op op, std::uint32_t handle = 0) {
    const auto packet = port.exchange(request(op, handle)); const auto h = unpack(packet);
    if (h.type != static_cast<std::uint32_t>(op) || h.status || packet.size() != sizeof(Header) + sizeof(Reply))
        throw std::runtime_error("Driver operation protocol failure");
    Reply r{}; std::memcpy(&r, packet.data() + sizeof(Header), sizeof r);
    if (r.ntstatus < 0 || r.reserved) throw std::runtime_error("Windows driver returned failure");
    if (op == Op::QueryDriverVersion) std::cout << "WDDM enum=" << r.value << '\n';
    return h.handle;
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: guest-probe /dev/vport0p1");
        Port port(argv[1]); const auto helloPacket = port.exchange(hello());
        const auto h = unpack(helloPacket);
        if (h.status || h.type != static_cast<std::uint32_t>(Op::Hello) || helloPacket.size() != 32)
            throw std::runtime_error("Handshake failed");
        native_gpu::Capabilities caps{}; std::memcpy(&caps, helloPacket.data() + sizeof(Header), sizeof caps);
        if (caps.version != Version || caps.vendor != 0x10de || caps.flags != 3) throw std::runtime_error("Unexpected backend capabilities");
        std::cout << "BRIDGE_VENDOR=" << caps.vendor << " BRIDGE_DEVICE=" << caps.device << '\n';
        for (unsigned cycle = 0; cycle < 5; ++cycle) {
            const auto adapter = operation(port, Op::OpenAdapter);
            operation(port, Op::QueryDriverVersion, adapter);
            const auto device = operation(port, Op::CreateDevice, adapter);
            const auto queue = operation(port, Op::CreatePagingQueue, device);
            operation(port, Op::ReadPagingFence, queue);
            const auto busy = unpack(port.exchange(request(Op::DestroyDevice, device)));
            if (busy.status != -16) throw std::runtime_error("Live child ownership check failed");
            operation(port, Op::DestroyPagingQueue, queue); operation(port, Op::DestroyDevice, device);
            operation(port, Op::CloseAdapter, adapter);
            const auto stale = unpack(port.exchange(request(Op::QueryDriverVersion, adapter)));
            if (stale.status != -9) throw std::runtime_error("Stale handle check failed");
        }
        // Leave a live hierarchy to verify cleanup when the VM disconnects.
        const auto abandonedAdapter = operation(port, Op::OpenAdapter);
        const auto abandonedDevice = operation(port, Op::CreateDevice, abandonedAdapter);
        operation(port, Op::CreatePagingQueue, abandonedDevice);
        std::cout << "PASS: QEMU guest WDDM control bridge, 5 lifecycle cycles; guest rendering=false\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
