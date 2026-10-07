// SPDX-License-Identifier: MIT
// Independent, deliberately small WDDM experiment, not the /dev/dxg or RM ABI.
#pragma once
#include "wire.h"
#include <map>

namespace driver_bridge {
using native_gpu::Header;
constexpr std::uint32_t Version = 1;
constexpr std::size_t MaxObjects = 64;
constexpr std::uint32_t MaxAllocation = 1024 * 1024;
constexpr std::uint32_t MaxAllocatedBytes = 16 * 1024 * 1024;
constexpr std::uint32_t MaxChunk = 4064;
enum class Op : std::uint32_t {
    Hello = 0x2000, OpenAdapter, QueryDriverVersion, CloseAdapter,
    CreateDevice, DestroyDevice, CreatePagingQueue, ReadPagingFence, DestroyPagingQueue,
    CreateAllocation = 0x2010, WriteAllocation, ReadAllocation, MakeResident,
    MapAllocation, QueryResidency, DestroyAllocation
};
enum class Kind { Adapter, Device, PagingQueue, Allocation };
struct Range { std::uint32_t offset; std::uint32_t size; };
static_assert(sizeof(Range) == 8, "fixed transfer layout");
struct Result { std::int32_t ntstatus; std::uint32_t nativeHandle; std::uint64_t value; };
// Native handles and pointers never cross the wire. NTSTATUS is preserved in
// this fixed payload; Header.status is a negative errno for protocol errors.
struct Reply { std::int32_t ntstatus; std::uint32_t reserved; std::uint64_t value; };
static_assert(sizeof(Reply) == 16, "fixed reply layout");
class Driver {
public:
    virtual ~Driver() = default;
    virtual native_gpu::Capabilities capabilities() const = 0;
    virtual Result openAdapter() = 0;
    virtual Result queryVersion(std::uint32_t adapter) = 0;
    virtual Result createDevice(std::uint32_t adapter) = 0;
    virtual Result createPagingQueue(std::uint32_t device) = 0;
    virtual Result readPagingFence(std::uint32_t queue) = 0;
    virtual Result createAllocation(std::uint32_t device, std::uint32_t size) = 0;
    virtual Result writeAllocation(std::uint32_t allocation, Range range, const std::uint8_t* data) = 0;
    virtual Result readAllocation(std::uint32_t allocation, Range range, std::vector<std::uint8_t>& data) = 0;
    virtual Result makeResident(std::uint32_t allocation, std::uint32_t queue) = 0;
    virtual Result mapAllocation(std::uint32_t allocation, std::uint32_t queue) = 0;
    virtual Result queryResidency(std::uint32_t allocation) = 0;
    virtual Result destroy(Kind kind, std::uint32_t handle) = 0;
};
class Session {
    struct Object { Kind kind; std::uint32_t parent; std::uint32_t nativeHandle; std::uint32_t size; };
    Driver& driver;
    std::map<std::uint32_t, Object> objects;
    std::uint32_t nextId = 1;
    std::uint32_t allocatedBytes = 0;
    bool negotiated = false;
    static std::vector<std::uint8_t> reply(Header h, std::int32_t error,
                                         std::uint32_t id = 0, const Result* result = nullptr) {
        h.handle = id; h.status = error; h.padding = 0;
        std::vector<std::uint8_t> out(sizeof h + (result ? sizeof(Reply) : 0));
        std::memcpy(out.data(), &h, sizeof h);
        if (result) {
            const Reply body{result->ntstatus, 0, result->value};
            std::memcpy(out.data() + sizeof h, &body, sizeof body);
        }
        return out;
    }
    std::vector<std::uint8_t> insert(Header h, Kind kind, std::uint32_t parent, Result result, std::uint32_t size = 0) {
        if (result.ntstatus < 0) return reply(h, 0, 0, &result);
        if (!result.nativeHandle) return reply(h, -5);
        const auto id = nextId++;
        objects.emplace(id, Object{kind, parent, result.nativeHandle, size});
        allocatedBytes += size;
        return reply(h, 0, id, &result);
    }
public:
    explicit Session(Driver& d) : driver(d) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // A disconnect releases child objects before their parents. The Windows
    // worker is also one process per connection, so OS teardown is a backstop.
    ~Session() {
        for (auto i = objects.rbegin(); i != objects.rend(); ++i)
            driver.destroy(i->second.kind, i->second.nativeHandle);
    }
    std::vector<std::uint8_t> dispatch(const std::vector<std::uint8_t>& packet) {
        if (packet.size() < sizeof(Header)) return {};
        Header h{}; std::memcpy(&h, packet.data(), sizeof h);
        if (packet.size() > native_gpu::MaxPacket || h.status || h.padding) return reply(h, -22);
        const auto op = static_cast<Op>(h.type);
        if (op == Op::Hello) {
            if (h.handle || packet.size() != sizeof h + 4) return reply(h, -22);
            if (!objects.empty()) return reply(h, -16);
            std::uint32_t version{}; std::memcpy(&version, packet.data() + sizeof h, 4);
            negotiated = false;
            if (version != Version) return reply(h, -93);
            const auto caps = driver.capabilities();
            if (caps.version != Version) return reply(h, -93);
            negotiated = true;
            auto out = reply(h, 0); out.resize(sizeof h + sizeof caps);
            std::memcpy(out.data() + sizeof h, &caps, sizeof caps);
            return out;
        }
        const bool memoryOp = op >= Op::CreateAllocation && op <= Op::DestroyAllocation;
        if (!memoryOp && (op < Op::OpenAdapter || op > Op::DestroyPagingQueue)) return reply(h, -95);
        const bool scalar = op == Op::CreateAllocation || op == Op::MakeResident || op == Op::MapAllocation;
        const bool transfer = op == Op::WriteAllocation || op == Op::ReadAllocation;
        if ((!scalar && !transfer && packet.size() != sizeof h) ||
            (scalar && packet.size() != sizeof h + 4) ||
            (transfer && packet.size() < sizeof h + sizeof(Range))) return reply(h, -22);
        if (!negotiated) return reply(h, -71);
        if (op == Op::OpenAdapter) {
            if (h.handle) return reply(h, -22);
            if (objects.size() >= MaxObjects || nextId == UINT32_MAX) return reply(h, -24);
            return insert(h, Kind::Adapter, 0, driver.openAdapter());
        }
        const auto entry = objects.find(h.handle);
        if (entry == objects.end()) return reply(h, -9);
        const auto& object = entry->second;
        const bool adapterOp = op == Op::QueryDriverVersion || op == Op::CloseAdapter || op == Op::CreateDevice;
        const bool deviceOp = op == Op::DestroyDevice || op == Op::CreatePagingQueue || op == Op::CreateAllocation;
        const auto required = adapterOp ? Kind::Adapter : deviceOp ? Kind::Device : memoryOp ? Kind::Allocation : Kind::PagingQueue;
        if (object.kind != required) return reply(h, -9);
        if (op == Op::CreateAllocation) {
            std::uint32_t size{}; std::memcpy(&size, packet.data() + sizeof h, 4);
            if (!size || size % 4096 || size > MaxAllocation) return reply(h, -22);
            if (objects.size() >= MaxObjects || nextId == UINT32_MAX || size > MaxAllocatedBytes - allocatedBytes)
                return reply(h, -24);
            return insert(h, Kind::Allocation, h.handle, driver.createAllocation(object.nativeHandle, size), size);
        }
        if (transfer) {
            Range range{}; std::memcpy(&range, packet.data() + sizeof h, sizeof range);
            const auto expected = sizeof h + sizeof range + (op == Op::WriteAllocation ? range.size : 0);
            if (!range.size || range.size > MaxChunk || packet.size() != expected ||
                range.size > object.size || range.offset > object.size - range.size) return reply(h, -22);
            std::vector<std::uint8_t> data;
            const auto result = op == Op::WriteAllocation
                ? driver.writeAllocation(object.nativeHandle, range, packet.data() + sizeof h + sizeof range)
                : driver.readAllocation(object.nativeHandle, range, data);
            auto out = reply(h, 0, h.handle, &result);
            if (result.ntstatus >= 0 && op == Op::ReadAllocation) {
                if (data.size() != range.size) return reply(h, -5);
                out.insert(out.end(), data.begin(), data.end());
            }
            return out;
        }
        if (op == Op::MakeResident || op == Op::MapAllocation) {
            std::uint32_t queueId{}; std::memcpy(&queueId, packet.data() + sizeof h, 4);
            const auto queue = objects.find(queueId);
            if (queue == objects.end() || queue->second.kind != Kind::PagingQueue || queue->second.parent != object.parent)
                return reply(h, -9);
            const auto result = op == Op::MakeResident ? driver.makeResident(object.nativeHandle, queue->second.nativeHandle)
                                                     : driver.mapAllocation(object.nativeHandle, queue->second.nativeHandle);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::QueryResidency) {
            const auto result = driver.queryResidency(object.nativeHandle); return reply(h, 0, h.handle, &result);
        }
        if (op == Op::QueryDriverVersion || op == Op::ReadPagingFence) {
            const auto result = op == Op::QueryDriverVersion ? driver.queryVersion(object.nativeHandle)
                                                            : driver.readPagingFence(object.nativeHandle);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::CreateDevice || op == Op::CreatePagingQueue) {
            if (objects.size() >= MaxObjects || nextId == UINT32_MAX) return reply(h, -24);
            const auto result = op == Op::CreateDevice ? driver.createDevice(object.nativeHandle)
                                                       : driver.createPagingQueue(object.nativeHandle);
            return insert(h, op == Op::CreateDevice ? Kind::Device : Kind::PagingQueue, h.handle, result);
        }
        for (const auto& child : objects)
            if (child.second.parent == h.handle) return reply(h, -16);
        const auto result = driver.destroy(object.kind, object.nativeHandle);
        if (result.ntstatus >= 0) { allocatedBytes -= object.size; objects.erase(entry); }
        return reply(h, 0, h.handle, &result);
    }
};
inline std::vector<std::uint8_t> request(Op operation, std::uint32_t handle = 0) {
    const Header h{static_cast<std::uint32_t>(operation), handle, 0, 0};
    std::vector<std::uint8_t> out(sizeof h); std::memcpy(out.data(), &h, sizeof h); return out;
}
inline std::vector<std::uint8_t> hello(std::uint32_t version = Version) {
    auto out = request(Op::Hello); out.resize(sizeof(Header) + 4);
    std::memcpy(out.data() + sizeof(Header), &version, 4); return out;
}
template<class T> inline std::vector<std::uint8_t> request(Op operation, std::uint32_t handle, const T& body) {
    auto out = request(operation, handle); out.resize(sizeof(Header) + sizeof body);
    std::memcpy(out.data() + sizeof(Header), &body, sizeof body); return out;
}
} // namespace driver_bridge
