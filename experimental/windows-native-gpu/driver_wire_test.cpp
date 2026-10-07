// SPDX-License-Identifier: MIT
#include "driver_wire.h"
#include <iostream>
#include <stdexcept>
using namespace driver_bridge;
static void require(bool good) { if (!good) throw std::runtime_error("driver wire regression"); }
static Header header(const std::vector<std::uint8_t>& p) {
    require(p.size() >= sizeof(Header)); Header h{}; std::memcpy(&h, p.data(), sizeof h); return h;
}
struct Fake : Driver {
    int calls = 0; std::uint32_t next = 500; bool fail = false, shortRead = false;
    std::map<std::uint32_t, std::vector<std::uint8_t>> buffers;
    std::vector<Kind> destroyed;
    native_gpu::Capabilities capabilities() const override { return {1, 31, 0x10de, 123}; }
    Result created() { ++calls; return {fail ? -123 : 0, ++next, 0}; }
    Result openAdapter() override { return created(); }
    Result queryVersion(std::uint32_t h) override { require(h > 500); ++calls; return {0, 0, 3200}; }
    Result createDevice(std::uint32_t h) override { require(h > 500); return created(); }
    Result createPagingQueue(std::uint32_t h) override { require(h > 500); return created(); }
    Result readPagingFence(std::uint32_t h) override { require(h > 500); ++calls; return {0, 0, 42}; }
    Result createAllocation(std::uint32_t h, std::uint32_t size) override {
        require(h > 500); auto result = created();
        if (result.ntstatus >= 0) buffers.emplace(result.nativeHandle, std::vector<std::uint8_t>(size));
        result.value = size; return result;
    }
    Result createSharedAllocation(std::uint32_t h, GuestRange range) override { return createAllocation(h, range.size); }
    Result copySharedAllocation(std::uint32_t destination, std::uint32_t source, CopyRange range) override {
        auto& to = buffers.at(destination); const auto& from = buffers.at(source); ++calls;
        require(destination != source && range.size && range.size <= to.size() && range.size <= from.size());
        require(range.sourceOffset <= from.size() - range.size && range.destinationOffset <= to.size() - range.size);
        std::memcpy(to.data() + range.destinationOffset, from.data() + range.sourceOffset, range.size);
        return {0, 0, range.size};
    }
    Result writeAllocation(std::uint32_t h, Range range, const std::uint8_t* data) override {
        auto& buffer = buffers.at(h); ++calls;
        require(range.size <= buffer.size() && range.offset <= buffer.size() - range.size);
        std::memcpy(buffer.data() + range.offset, data, range.size); return {0, 0, range.size};
    }
    Result readAllocation(std::uint32_t h, Range range, std::vector<std::uint8_t>& data) override {
        const auto& buffer = buffers.at(h); ++calls;
        require(range.size <= buffer.size() && range.offset <= buffer.size() - range.size);
        data.assign(buffer.begin() + range.offset, buffer.begin() + range.offset + range.size);
        if (shortRead) data.pop_back();
        return {0, 0, range.size};
    }
    Result makeResident(std::uint32_t h, std::uint32_t q) override {
        require(buffers.count(h) && q > 500); ++calls; return {0, 0, 42};
    }
    Result mapAllocation(std::uint32_t h, std::uint32_t q) override {
        require(buffers.count(h) && q > 500); ++calls; return {0, 0, 0x123456789ull};
    }
    Result queryResidency(std::uint32_t h) override { require(buffers.count(h)); ++calls; return {0, 0, 1}; }
    Result destroy(Kind k, std::uint32_t h) override {
        require(h > 500); ++calls;
        if (!fail) { destroyed.push_back(k); if (k == Kind::Allocation) require(buffers.erase(h) == 1); }
        return {fail ? -123 : 0, 0, 0};
    }
};
int main() {
    Fake driver;
    {
        Session s(driver);
        require(header(s.dispatch(request(Op::OpenAdapter))).status == -71 && driver.calls == 0);
        require(header(s.dispatch(hello(2))).status == -93);
        require(header(s.dispatch(hello())).status == 0);
        auto a = header(s.dispatch(request(Op::OpenAdapter)));
        require(a.status == 0 && a.handle == 1); // never the native handle 501
        auto d = header(s.dispatch(request(Op::CreateDevice, a.handle)));
        auto q = header(s.dispatch(request(Op::CreatePagingQueue, d.handle)));
        require(d.handle == 2 && q.handle == 3);
        const auto before = driver.calls;
        require(header(s.dispatch(request(Op::CloseAdapter, a.handle))).status == -16);
        require(header(s.dispatch(request(Op::DestroyDevice, d.handle))).status == -16);
        require(header(s.dispatch(request(Op::QueryDriverVersion, q.handle))).status == -9);
        require(header(s.dispatch(request(Op::CreateDevice, 501))).status == -9);
        require(header(s.dispatch(hello())).status == -16 && driver.calls == before);
        auto p = s.dispatch(request(Op::ReadPagingFence, q.handle));
        Reply r{}; std::memcpy(&r, p.data() + sizeof(Header), sizeof r);
        require(header(p).status == 0 && r.ntstatus == 0 && r.reserved == 0 && r.value == 42);
        Session isolated(driver); isolated.dispatch(hello());
        require(header(isolated.dispatch(request(Op::CreateDevice, a.handle))).status == -9);
        driver.fail = true;
        p = s.dispatch(request(Op::DestroyPagingQueue, q.handle));
        std::memcpy(&r, p.data() + sizeof(Header), sizeof r);
        require(header(p).status == 0 && r.ntstatus == -123);
        driver.fail = false;
        require(header(s.dispatch(request(Op::ReadPagingFence, q.handle))).status == 0);
        // Disconnect without explicit close: reverse child-before-parent cleanup.
    }
    require(driver.destroyed == std::vector<Kind>{Kind::PagingQueue, Kind::Device, Kind::Adapter});
    Fake memory;
    {
        Session s(memory); s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        const auto otherDevice = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto otherQueue = header(s.dispatch(request(Op::CreatePagingQueue, otherDevice))).handle;
        const auto allocation = header(s.dispatch(request(Op::CreateAllocation, device, std::uint32_t{4096}))).handle;
        require(allocation && header(s.dispatch(request(Op::DestroyDevice, device))).status == -16);
        auto before = memory.calls;
        require(header(s.dispatch(request(Op::MakeResident, allocation, otherQueue))).status == -9);
        require(header(s.dispatch(request(Op::MapAllocation, allocation, device))).status == -9);
        require(header(s.dispatch(request(Op::ReadAllocation, queue, Range{0, 4}))).status == -9);
        for (const auto range : {Range{4095, 2}, Range{UINT32_MAX, 2}, Range{0, MaxChunk + 1}, Range{0, 0}})
            require(header(s.dispatch(request(Op::ReadAllocation, allocation, range))).status == -22);
        for (const auto size : {0u, 1u, MaxAllocation + 4096u})
            require(header(s.dispatch(request(Op::CreateAllocation, device, size))).status == -22);
        auto packet = request(Op::WriteAllocation, allocation, Range{0, 4});
        require(header(s.dispatch(packet)).status == -22 && memory.calls == before);
        packet.insert(packet.end(), {10, 20, 30, 40});
        require(header(s.dispatch(packet)).status == 0);
        packet = s.dispatch(request(Op::ReadAllocation, allocation, Range{0, 4}));
        require(packet.size() == sizeof(Header) + sizeof(Reply) + 4);
        require(std::vector<std::uint8_t>(packet.end() - 4, packet.end()) == std::vector<std::uint8_t>{10, 20, 30, 40});
        memory.shortRead = true;
        require(header(s.dispatch(request(Op::ReadAllocation, allocation, Range{0, 4}))).status == -5);
        memory.shortRead = false;
        require(header(s.dispatch(request(Op::MakeResident, allocation, queue))).status == 0);
        packet = s.dispatch(request(Op::MapAllocation, allocation, queue));
        Reply result{}; std::memcpy(&result, packet.data() + sizeof(Header), sizeof result);
        require(result.value == 0x123456789ull);
        memory.fail = true;
        packet = s.dispatch(request(Op::DestroyAllocation, allocation));
        std::memcpy(&result, packet.data() + sizeof(Header), sizeof result); require(result.ntstatus == -123);
        memory.fail = false;
        require(header(s.dispatch(request(Op::DestroyAllocation, allocation))).status == 0);
        require(header(s.dispatch(request(Op::QueryResidency, allocation))).status == -9);
        // New allocation retains a larger monotonic ID. Leave it for disconnect cleanup.
        require(header(s.dispatch(request(Op::CreateAllocation, device, std::uint32_t{4096}))).handle > allocation);
        const auto shared = header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0x100000, 4096, 0}))).handle;
        require(shared != 0);
        before = memory.calls;
        require(header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0x100000, 4096, 0}))).status == -16);
        require(header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{UINT64_MAX - 4095, 8192, 0}))).status == -22);
        require(header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{1, 4096, 0}))).status == -22);
        require(header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0, 4096, 1}))).status == -22);
        require(memory.calls == before);
        const auto from = header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0x200000, 65536, 0}))).handle;
        const auto to = header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0x210000, 65536, 0}))).handle;
        const auto foreign = header(s.dispatch(request(Op::CreateSharedAllocation, otherDevice, GuestRange{0x220000, 65536, 0}))).handle;
        before = memory.calls;
        for (const auto copy : {CopyRange{from, UINT32_MAX, 0, 1}, CopyRange{from, 0, 65535, 2},
                                CopyRange{from, 0, 0, 0}, CopyRange{to, 0, 0, 4}})
            require(header(s.dispatch(request(Op::CopySharedAllocation, to, copy))).status == -22);
        require(header(s.dispatch(request(Op::CopySharedAllocation, to, CopyRange{foreign, 0, 0, 4}))).status == -9);
        require(header(s.dispatch(request(Op::CopySharedAllocation, to, CopyRange{allocation, 0, 0, 4}))).status == -9);
        require(header(s.dispatch(request(Op::CopySharedAllocation, to, CopyRange{queue, 0, 0, 4}))).status == -9);
        require(header(s.dispatch(request(Op::CopySharedAllocation, shared, CopyRange{from, 0, 0, 4}))).status == -22);
        require(memory.calls == before);
        packet = request(Op::WriteAllocation, from, Range{3, 4}); packet.insert(packet.end(), {91, 23, 201, 45});
        require(header(s.dispatch(packet)).status == 0);
        require(header(s.dispatch(request(Op::CopySharedAllocation, to, CopyRange{from, 3, 7, 4}))).status == 0);
        packet = s.dispatch(request(Op::ReadAllocation, to, Range{7, 4}));
        require(std::vector<std::uint8_t>(packet.end() - 4, packet.end()) == std::vector<std::uint8_t>{91, 23, 201, 45});
    }
    require(memory.buffers.empty() && memory.destroyed.back() == Kind::Adapter);
    Fake budget;
    {
        Session s(budget); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        budget.fail = true;
        auto packet = s.dispatch(request(Op::CreateAllocation, d, MaxAllocation));
        Reply result{}; std::memcpy(&result, packet.data() + sizeof(Header), sizeof result);
        require(result.ntstatus == -123 && header(packet).handle == 0);
        budget.fail = false;
        std::uint32_t last = 0;
        for (std::uint32_t size = 0; size < MaxAllocatedBytes; size += MaxAllocation) {
            last = header(s.dispatch(request(Op::CreateAllocation, d, MaxAllocation))).handle; require(last != 0);
        }
        const auto before = budget.calls;
        require(header(s.dispatch(request(Op::CreateAllocation, d, std::uint32_t{4096}))).status == -24 && budget.calls == before);
        require(header(s.dispatch(request(Op::DestroyAllocation, last))).status == 0);
        require(header(s.dispatch(request(Op::CreateAllocation, d, MaxAllocation))).status == 0);
    }
    Fake bounded;
    {
        Session s(bounded); s.dispatch(hello());
        for (std::size_t i = 0; i < MaxObjects; ++i) require(header(s.dispatch(request(Op::OpenAdapter))).status == 0);
        const auto before = bounded.calls;
        require(header(s.dispatch(request(Op::OpenAdapter))).status == -24 && bounded.calls == before);
    }
    Fake malformed; Session s(malformed); s.dispatch(hello());
    auto p = request(Op::OpenAdapter); p.push_back(0);
    require(header(s.dispatch(p)).status == -22);
    p = request(Op::OpenAdapter); p[8] = 1; require(header(s.dispatch(p)).status == -22);
    p = request(Op::OpenAdapter); p[12] = 1; require(header(s.dispatch(p)).status == -22);
    for (std::uint32_t op = 1; op <= 8; ++op) {
        p = request(static_cast<Op>(op)); require(header(s.dispatch(p)).status == -95);
    }
    require(s.dispatch({1, 2, 3}).empty() && malformed.calls == 0);
    require(header(s.dispatch(hello(2))).status == -93);
    require(header(s.dispatch(request(Op::OpenAdapter))).status == -71);
    std::cout << "PASS: WDDM wire ownership, allocation bounds/budget, NTSTATUS, cleanup, ABI rejection\n";
}
