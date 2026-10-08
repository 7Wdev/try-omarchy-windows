// SPDX-License-Identifier: MIT
#include "driver_wire.h"
#include "adapter_query_client.h"
#include <iostream>
#include <stdexcept>
using namespace driver_bridge;
static void require(bool good) { if (!good) throw std::runtime_error("driver wire regression"); }
static Header header(const std::vector<std::uint8_t>& p) {
    require(p.size() >= sizeof(Header)); Header h{}; std::memcpy(&h, p.data(), sizeof h); return h;
}
struct Fake : Driver {
    int calls = 0; std::uint32_t next = 500; bool fail = false, shortRead = false, badContextReply = false;
    bool contextsEnabled = true;
    bool queriesEnabled = true, badQueryReply = false;
    bool guestPagingEnabled = false;
    int badPagingReply = 0;
    bool vendorEnabled = false;
    bool gpuVaEnabled = false;
    bool residencyEnabled = false;
    int badResidentReply = 0;
    unsigned residentCalls = 0;
    std::vector<std::uint32_t> lastResidentPriorities, lastResidentHandles;
    int badGpuVaReply = 0;
    int badVendorReply = 0;
    std::map<std::uint32_t, std::uint32_t> vendorOwners;
    std::map<std::uint32_t, std::vector<std::uint8_t>> buffers;
    std::vector<Kind> destroyed;
    native_gpu::Capabilities capabilities() const override {
        return {1, 31u | (contextsEnabled ? ContextCapability : 0u) | (queriesEnabled ? QueryCapability : 0u) |
                (guestPagingEnabled ? GuestPagingCapability : 0u) | (vendorEnabled ? VendorAllocationCapability : 0u) |
                (gpuVaEnabled ? VendorGpuVaCapability : 0u) | (residencyEnabled ? VendorResidencyCapability : 0u), 0x10de, 123};
    }
    Result created() { ++calls; return {fail ? -123 : 0, ++next, 0}; }
    Result openAdapter() override { return created(); }
    Result queryVersion(std::uint32_t h) override { require(h > 500); ++calls; return {0, 0, 3200}; }
    Result queryAdapter(std::uint32_t h, QueryDesc desc, std::vector<std::uint8_t>& data) override {
        require(h > 500 && validQuery(desc) && data.size() == desc.bytes); ++calls;
        for (auto& byte : data) byte ^= 255;
        if (badQueryReply) data.pop_back();
        return {fail ? -123 : 0, 0, desc.bytes};
    }
    Result createDevice(std::uint32_t h) override { require(h > 500); return created(); }
    Result createContext(std::uint32_t h, ContextDesc desc, std::vector<std::uint8_t>& data) override {
        require(h > 500 && validContext(desc) && data.size() == desc.privateBytes);
        if (!data.empty()) data[0] ^= 255;
        if (badContextReply) data.push_back(0);
        return created();
    }
    Result createPagingQueue(std::uint32_t h) override { require(h > 500); return created(); }
    Result createVendorAllocation(std::uint32_t device, VendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        require(device > 500 && validVendorAllocation(desc) && data.size() == desc.privateBytes);
        ++calls; data[0] ^= 255;
        if (badVendorReply == 1) data.pop_back();
        if (fail) return {-123, 0, 0};
        const auto handle = ++next; vendorOwners.emplace(handle, device);
        return {0, badVendorReply == 2 ? 0u : handle, badVendorReply == 3 ? 1ull : 0ull};
    }
    Result destroyVendorAllocations(std::uint32_t device, const std::vector<std::uint32_t>& handles) override {
        require(!handles.empty()); ++calls;
        for (const auto handle : handles) require(vendorOwners.at(handle) == device);
        if (fail) return {-123, 0, 0};
        for (const auto handle : handles) { vendorOwners.erase(handle); destroyed.push_back(Kind::VendorAllocation); }
        return {0, 0, 0};
    }
    GpuVaResult mapVendorAllocation(std::uint32_t allocation, std::uint32_t queue, GpuVaDesc desc) override {
        require(vendorOwners.count(allocation) && queue > 500 && validGpuVa(desc)); ++calls;
        if (fail) return {{-123, 0, 0}, 0};
        const auto address = desc.base ? desc.base : (desc.minimum ? desc.minimum : 65536ull) + (allocation - 500) * 1048576ull;
        return {{badGpuVaReply == 4 ? 1 : 0x103, badGpuVaReply == 5 ? 777u : 0u,
                 badGpuVaReply == 1 ? 0ull : badGpuVaReply == 2 ? address + 1 : badGpuVaReply == 3 ? MaxGpuAddress : address}, 7019};
    }
    ResidentResult makeVendorResident(std::uint32_t queue, ResidentDesc desc, const std::vector<std::uint32_t>& handles,
                                     const std::vector<std::uint32_t>& priorities) override {
        require(queue > 500 && validResident(desc) && handles.size() == desc.count && priorities.size() == (desc.priorities ? desc.count : 0));
        for (const auto handle : handles) require(vendorOwners.count(handle));
        ++calls; ++residentCalls; lastResidentPriorities = priorities; lastResidentHandles = handles;
        if (fail) return {{static_cast<std::int32_t>(0xc0000017u), 0, 7009}, {desc.count - 1, 0, 65536}};
        return {{badResidentReply == 1 ? 1 : 259, badResidentReply == 2 ? 777u : 0u, 7011},
                {badResidentReply == 3 ? desc.count + 1 : desc.count, badResidentReply == 4 ? 1u : 0u, 0}};
    }
    GuestPagingResult createGuestPagingQueue(std::uint32_t h) override {
        require(h > 500);
        auto queue = created();
        const auto sync = badPagingReply == 1 ? 0 : ++next;
        if (badPagingReply == 4) queue.value = UINT64_MAX;
        return {queue, sync, badPagingReply == 2 ? 1ull : badPagingReply == 3 ? FenceApertureBytes : 8192ull};
    }
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
        if (k == Kind::PagingSync) return {0, 0, 0}; // borrowed, queue destroys the native object
        if (k == Kind::VendorAllocation) return destroyVendorAllocations(vendorOwners.at(h), {h});
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
    Fake contexts;
    {
        Session s(contexts);
        auto sync = request(Op::CreateContext, 2, ContextDesc{0, 0, 8, 0, 0, 0});
        require(header(s.dispatch(sync)).status == -71);
        s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        require(d == 2);
        auto before = contexts.calls;
        contexts.contextsEnabled = false;
        require(header(s.dispatch(sync)).status == -95 && contexts.calls == before);
        contexts.contextsEnabled = true;
        require(header(s.dispatch(request(Op::CreateContext, a, ContextDesc{0, 0, 8, 0, 0, 0}))).status == -9);
        for (const auto desc : {ContextDesc{0, 1, 4, 12, 0, 0}, ContextDesc{0, 0, 8, 0, 0, 1},
                               ContextDesc{0, 1, 16, 12, MaxContextPrivateBytes + 1, 0},
                               ContextDesc{64, 1, 16, 12, 1, 0}, ContextDesc{0, 1, 16, 12, 1, 0}})
            require(header(s.dispatch(request(Op::CreateContext, d, desc))).status == -22);
        require(contexts.calls == before);
        auto c = header(s.dispatch(sync)).handle; require(c != 0);
        require(header(s.dispatch(request(Op::DestroyDevice, d))).status == -16);
        require(header(s.dispatch(request(Op::CreatePagingQueue, c))).status == -9);
        auto p = request(Op::CreateContext, d, ContextDesc{0, 1, 16, 12, 3, 0});
        p.insert(p.end(), {11, 22, 33});
        const auto output = s.dispatch(p);
        require(output.size() == sizeof(Header) + sizeof(Reply) + 3 && output.back() == 33);
        require(output[sizeof(Header) + sizeof(Reply)] == (11 ^ 255));
        contexts.fail = true;
        const auto failed = s.dispatch(p);
        Reply body{}; std::memcpy(&body, failed.data() + sizeof(Header), sizeof body);
        require(header(failed).handle == 0 && body.ntstatus == -123 && failed.size() == sizeof(Header) + sizeof(Reply));
        contexts.fail = false; contexts.badContextReply = true;
        require(header(s.dispatch(p)).status == -5 && contexts.destroyed.back() == Kind::Context);
        contexts.badContextReply = false;
        require(header(s.dispatch(request(Op::DestroyContext, c, std::uint32_t{0}))).status == -22);
        require(header(s.dispatch(request(Op::DestroyContext, c))).status == 0);
        before = contexts.calls;
        require(header(s.dispatch(request(Op::DestroyContext, c))).status == -9 && contexts.calls == before);
        // Leave the second context alive: disconnect destroys it before device.
    }
    require(contexts.destroyed == std::vector<Kind>{Kind::Context, Kind::Context, Kind::Context, Kind::Device, Kind::Adapter});
    Fake adapterQueries;
    {
        Session s(adapterQueries);
        const QueryDesc desc{0, 50616, 0, 0};
        require(header(s.dispatch(request(Op::BeginAdapterQuery, 1, desc))).status == -71);
        s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        const auto before = adapterQueries.calls;
        adapterQueries.queriesEnabled = false;
        require(header(s.dispatch(request(Op::BeginAdapterQuery, a, desc))).status == -95);
        adapterQueries.queriesEnabled = true;
        require(header(s.dispatch(request(Op::BeginAdapterQuery, device, desc))).status == -9);
        for (const auto invalid : {QueryDesc{0, 0, 0, 0}, QueryDesc{0, MaxQueryBytes + 1, 0, 0},
                QueryDesc{0, 4, 1, 0}, QueryDesc{0, 4, 0, 1}, QueryDesc{13, 8, 0, 0},
                QueryDesc{7, 4, 0, 0}, QueryDesc{41, 24, 0, 0}, QueryDesc{48, 552, 0, 0}})
            require(header(s.dispatch(request(Op::BeginAdapterQuery, a, invalid))).status == -22);
        require(adapterQueries.calls == before);
        require(header(s.dispatch(request(Op::BeginAdapterQuery, a, desc))).status == 0);
        require(header(s.dispatch(request(Op::BeginAdapterQuery, a, desc))).status == -16);
        require(header(s.dispatch(request(Op::CloseAdapter, a))).status == -16);
        require(header(s.dispatch(request(Op::RunAdapterQuery, a))).status == -71);
        require(header(s.dispatch(request(Op::ReadAdapterQuery, a, Range{0, 4}))).status == -71);
        auto hole = request(Op::WriteAdapterQuery, a, Range{1, 1}); hole.push_back(9);
        require(header(s.dispatch(hole)).status == -71);
        for (std::uint32_t offset = 0; offset < desc.bytes;) {
            const auto count = std::min(MaxChunk, desc.bytes - offset);
            auto input = request(Op::WriteAdapterQuery, a, Range{offset, count});
            for (std::uint32_t i = 0; i < count; ++i) input.push_back(static_cast<std::uint8_t>((offset + i) % 251));
            require(header(s.dispatch(input)).status == 0);
            require(header(s.dispatch(input)).status == -71); // overlap/retry
            offset += count;
        }
        auto output = s.dispatch(request(Op::RunAdapterQuery, a));
        Reply result{}; std::memcpy(&result, output.data() + sizeof(Header), sizeof result);
        require(header(output).status == 0 && result.ntstatus == 0 && result.value == desc.bytes);
        require(adapterQueries.calls == before + 1);
        require(header(s.dispatch(request(Op::RunAdapterQuery, a))).status == -71);
        hole = request(Op::WriteAdapterQuery, a, Range{0, 1}); hole.push_back(9);
        require(header(s.dispatch(hole)).status == -71);
        for (std::uint32_t offset = 0; offset < desc.bytes;) {
            const auto count = std::min(MaxChunk, desc.bytes - offset);
            output = s.dispatch(request(Op::ReadAdapterQuery, a, Range{offset, count}));
            require(output.size() == sizeof(Header) + sizeof(Reply) + count && output.size() <= native_gpu::MaxPacket);
            for (std::uint32_t i = 0; i < count; ++i)
                require(output[sizeof(Header) + sizeof(Reply) + i] == static_cast<std::uint8_t>(((offset + i) % 251) ^ 255));
            offset += count;
        }
        require(header(s.dispatch(request(Op::ReadAdapterQuery, a, Range{UINT32_MAX, 4}))).status == -22);
        require(header(s.dispatch(request(Op::EndAdapterQuery, a, std::uint32_t{0}))).status == -22);
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == 0);
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == -2);
        // Cancellation frees staging memory before adapter teardown.
        const auto b = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto c = header(s.dispatch(request(Op::OpenAdapter))).handle;
        for (const auto id : {a, b})
            require(header(s.dispatch(request(Op::BeginAdapterQuery, id, QueryDesc{0, MaxQueryBytes, 0, 0}))).status == 0);
        require(header(s.dispatch(request(Op::BeginAdapterQuery, c, desc))).status == -24);
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == 0);
        require(header(s.dispatch(request(Op::BeginAdapterQuery, c, desc))).status == 0);
        require(header(s.dispatch(request(Op::CloseAdapter, c))).status == -16);
        require(header(s.dispatch(request(Op::EndAdapterQuery, c))).status == 0);
        auto prepare = [&] {
            require(header(s.dispatch(request(Op::BeginAdapterQuery, a, QueryDesc{13, 4, 0, 0}))).status == 0);
            auto input = request(Op::WriteAdapterQuery, a, Range{0, 4}); input.insert(input.end(), {1, 2, 3, 4});
            require(header(s.dispatch(input)).status == 0);
        };
        prepare(); adapterQueries.fail = true;
        output = s.dispatch(request(Op::RunAdapterQuery, a));
        std::memcpy(&result, output.data() + sizeof(Header), sizeof result);
        require(result.ntstatus == -123);
        output = s.dispatch(request(Op::ReadAdapterQuery, a, Range{0, 4}));
        std::memcpy(&result, output.data() + sizeof(Header), sizeof result);
        require(result.ntstatus == -123 && output.back() == (4 ^ 255));
        adapterQueries.fail = false;
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == 0);
        prepare(); adapterQueries.badQueryReply = true;
        require(header(s.dispatch(request(Op::RunAdapterQuery, a))).status == -5);
        adapterQueries.badQueryReply = false;
        prepare(); // malformed native reply discarded the staging object
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == 0);
        require(header(s.dispatch(request(Op::DestroyDevice, device))).status == 0);
        require(header(s.dispatch(request(Op::CloseAdapter, a))).status == 0);
        // b deliberately retains an unfinished 64 KiB query on disconnect.
    }
    require(adapterQueries.destroyed == std::vector<Kind>{Kind::Device, Kind::Adapter, Kind::Adapter, Kind::Adapter});
    Fake queryClient;
    {
        Session s(queryClient); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        std::vector<std::uint8_t> input(50616);
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint8_t>(i % 251);
        auto exchange = [&](const std::vector<std::uint8_t>& packet) { return s.dispatch(packet); };
        const auto result = adapterQuery(exchange, a, 0, input);
        require(result.ntstatus == 0 && result.data.size() == input.size());
        for (std::size_t i = 0; i < input.size(); ++i) require(result.data[i] == (input[i] ^ 255));
        queryClient.fail = true;
        require(adapterQuery(exchange, a, 0, input).ntstatus == -123);
        queryClient.fail = false;
        bool corrupted = false;
        auto corrupt = [&](const std::vector<std::uint8_t>& packet) {
            auto output = s.dispatch(packet);
            if (header(packet).type == static_cast<std::uint32_t>(Op::ReadAdapterQuery)) output.pop_back();
            return output;
        };
        try { adapterQuery(corrupt, a, 0, input); } catch (const QueryProtocolError&) { corrupted = true; }
        require(corrupted);
        // A malformed reply cancels the staging object; a fresh query works.
        require(adapterQuery(exchange, a, 13, {0, 0, 0, 0}).ntstatus == 0);
        require(header(s.dispatch(request(Op::CloseAdapter, a))).status == 0);
    }
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
    Fake paging;
    {
        Session s(paging);
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue))).status == -71);
        s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        auto before = paging.calls;
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).status == -95 && paging.calls == before);
        paging.guestPagingEnabled = true;
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, a))).status == -9);
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d, unsigned{0}))).status == -22);
        require(paging.calls == before);
        for (int invalid = 1; invalid <= 4; ++invalid) {
            paging.badPagingReply = invalid;
            const auto destroys = paging.destroyed.size();
            require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).status == -5);
            require(paging.destroyed.size() == destroys + 1 && paging.destroyed.back() == Kind::PagingQueue);
        }
        paging.badPagingReply = 0; paging.fail = true;
        auto packet = s.dispatch(request(Op::CreateGuestPagingQueue, d));
        Reply nt{}; std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(packet.size() == sizeof(Header) + sizeof(Reply) && !header(packet).handle && nt.ntstatus == -123);
        paging.fail = false;
        packet = s.dispatch(request(Op::CreateGuestPagingQueue, d));
        const auto queue = header(packet).handle;
        PagingReply info{}; require(packet.size() == sizeof(Header) + sizeof(Reply) + sizeof info);
        std::memcpy(&info, packet.data() + sizeof(Header) + sizeof(Reply), sizeof info);
        require(queue == 3 && info.sync == 4 && !info.reserved && info.offset == 8192);
        before = paging.calls;
        require(header(s.dispatch(request(Op::ReadPagingFence, info.sync))).status == -9);
        require(header(s.dispatch(request(Op::DestroyPagingQueue, info.sync))).status == -9);
        require(header(s.dispatch(request(Op::DestroyDevice, d))).status == -16 && paging.calls == before);
        paging.fail = true;
        packet = s.dispatch(request(Op::DestroyPagingQueue, queue));
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); require(nt.ntstatus == -123);
        require(header(s.dispatch(request(Op::DestroyDevice, d))).status == -16);
        paging.fail = false;
        require(header(s.dispatch(request(Op::ReadPagingFence, queue))).status == 0);
        const auto destroys = paging.destroyed.size();
        require(header(s.dispatch(request(Op::DestroyPagingQueue, queue))).status == 0);
        require(paging.destroyed.size() == destroys + 1); // no separate native sync destruction
        require(header(s.dispatch(request(Op::ReadPagingFence, queue))).status == -9);
        require(header(s.dispatch(request(Op::DestroyPagingQueue, info.sync))).status == -9);
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).handle > info.sync);
    }
    require(paging.destroyed[paging.destroyed.size() - 3] == Kind::PagingQueue);
    require(paging.destroyed.back() == Kind::Adapter);
    Fake vendor;
    {
        Session s(vendor);
        const VendorAllocationDesc desc{4, 0x78100000, 0, 586, 0, 0};
        auto create = [&](std::uint32_t device, VendorAllocationDesc input = VendorAllocationDesc{4, 0x78100000, 0, 586, 0, 0}) {
            auto packet = request(Op::CreateVendorAllocation, device, input); packet.insert(packet.end(), input.privateBytes, 37); return packet;
        };
        auto release = [&](std::uint32_t device, std::vector<std::uint32_t> ids) {
            auto packet = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{static_cast<std::uint32_t>(ids.size()), 0});
            const auto bytes = reinterpret_cast<const std::uint8_t*>(ids.data());
            if (!ids.empty()) packet.insert(packet.end(), bytes, bytes + ids.size() * sizeof(ids[0]));
            return packet;
        };
        require(header(s.dispatch(create(2))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        auto before = vendor.calls;
        require(header(s.dispatch(create(device))).status == -95 && vendor.calls == before);
        vendor.vendorEnabled = true;
        require(header(s.dispatch(create(adapter))).status == -9);
        for (const auto input : {VendorAllocationDesc{5, desc.priority, 0, 586, 0, 0}, VendorAllocationDesc{4, 0xa0000000, 0, 586, 0, 0},
                                 VendorAllocationDesc{4, desc.priority, 1, 586, 0, 0}, VendorAllocationDesc{4, desc.priority, 0, 0, 0, 0},
                                 VendorAllocationDesc{4, desc.priority, 0, MaxVendorPrivateBytes + 1, 0, 0},
                                 VendorAllocationDesc{4, desc.priority, 0, 586, 1, 0}, VendorAllocationDesc{4, desc.priority, 0, 586, 0, 1}})
            require(header(s.dispatch(create(device, input))).status == -22);
        auto shortInput = create(device); shortInput.pop_back(); require(header(s.dispatch(shortInput)).status == -22);
        require(vendor.calls == before);
        vendor.fail = true;
        auto packet = s.dispatch(create(device)); Reply nt{}; std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(!header(packet).handle && !header(packet).status && nt.ntstatus == -123 && packet.back() == 37 &&
                packet[sizeof(Header) + sizeof(Reply)] == (37 ^ 255) && packet.size() == sizeof(Header) + sizeof(Reply) + desc.privateBytes);
        vendor.fail = false;
        for (const auto invalid : {1, 3}) { // malformed native in/out and GPU address must clean the native object
            vendor.badVendorReply = invalid;
            require(header(s.dispatch(create(device))).status == -5 && vendor.vendorOwners.empty());
        }
        vendor.badVendorReply = 0;
        const auto first = header(s.dispatch(create(device))).handle;
        require(first == 3); // failed allocations never consume successful object IDs
        const auto second = header(s.dispatch(create(device))).handle;
        const auto other = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        before = vendor.calls;
        require(header(s.dispatch(request(Op::ReadAllocation, first, Range{0, 4}))).status == -9);
        require(header(s.dispatch(request(Op::DestroyAllocation, first))).status == -9);
        require(header(s.dispatch(release(device, {}))).status == -22);
        require(header(s.dispatch(release(device, {first, first}))).status == -22);
        require(header(s.dispatch(release(other, {first}))).status == -9);
        require(header(s.dispatch(release(device, {first, device}))).status == -9);
        require(header(s.dispatch(request(Op::DestroyDevice, device))).status == -16 && vendor.calls == before);
        vendor.fail = true;
        packet = s.dispatch(release(device, {first, second})); std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(nt.ntstatus == -123 && vendor.vendorOwners.size() == 2);
        vendor.fail = false;
        require(header(s.dispatch(release(device, {first, second}))).status == 0 && vendor.vendorOwners.empty());
        require(header(s.dispatch(release(device, {first}))).status == -9);
        for (std::size_t n = 0; n < MaxVendorAllocations; ++n) require(header(s.dispatch(create(device))).handle != 0);
        before = vendor.calls;
        require(header(s.dispatch(create(device))).status == -24 && vendor.calls == before);
    }
    require(vendor.vendorOwners.empty() && vendor.destroyed.back() == Kind::Adapter);
    Fake gpuVa; gpuVa.vendorEnabled = true;
    {
        Session s(gpuVa); const GpuVaDesc input{3, 0, 0, 67108864, 1099511627776ull, 0, 16, 1, 0};
        require(header(s.dispatch(request(Op::MapVendorAllocation, 4, input))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        auto create = [&] {
            auto packet = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{4, 0x78100000, 0, 586, 0, 0});
            packet.insert(packet.end(), 586, 0); return header(s.dispatch(packet)).handle;
        };
        auto release = [&](std::uint32_t id) {
            auto packet = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{1, 0});
            const auto start = packet.size(); packet.resize(start + 4); std::memcpy(packet.data() + start, &id, 4);
            return s.dispatch(packet);
        };
        const auto allocation = create(); require(queue == 3 && allocation == 4);
        auto before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, input))).status == -95 && gpuVa.calls == before);
        gpuVa.gpuVaEnabled = true;
        for (unsigned n = 0; n < 12; ++n) {
            auto invalid = input;
            switch (n) {
                case 0: invalid.queue = 0; break;
                case 1: invalid.reserved = 1; break;
                case 2: invalid.sizePages = 0; break;
                case 3: invalid.sizePages = MaxVendorMapPages + 1; break;
                case 4: invalid.offsetPages = UINT64_MAX; break;
                case 5: invalid.protection = 16; break;
                case 6: invalid.driverProtection = 1; break;
                case 7: invalid.base = MaxGpuAddress - 4096; break;
                case 8: invalid.minimum = 1; break;
                case 9: invalid.maximum = MaxGpuAddress + 4096; break;
                case 10: invalid.maximum = invalid.minimum; break;
                default: invalid.base = 1; break;
            }
            require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, invalid))).status == -22);
        }
        auto other = input; other.queue = device;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, other))).status == -9);
        require(header(s.dispatch(request(Op::MapVendorAllocation, queue, input))).status == -9);
        const auto device2 = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        other.queue = header(s.dispatch(request(Op::CreatePagingQueue, device2))).handle;
        before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, other))).status == -9 && gpuVa.calls == before);
        gpuVa.fail = true;
        auto packet = s.dispatch(request(Op::MapVendorAllocation, allocation, input)); Reply nt{}; GpuVaReply fence{};
        require(packet.size() == sizeof(Header) + sizeof(Reply) + sizeof(GpuVaReply));
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); std::memcpy(&fence, packet.data() + sizeof(Header) + sizeof(Reply), sizeof fence);
        require(nt.ntstatus == -123 && !nt.value && !fence.fence && header(packet).handle == allocation);
        gpuVa.fail = false;
        for (int invalid = 1; invalid <= 5; ++invalid) {
            gpuVa.badGpuVaReply = invalid;
            require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, input))).status == -5);
        }
        gpuVa.badGpuVaReply = 0;
        packet = s.dispatch(request(Op::MapVendorAllocation, allocation, input));
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); std::memcpy(&fence, packet.data() + sizeof(Header) + sizeof(Reply), sizeof fence);
        require(!header(packet).status && nt.ntstatus == 0x103 && validGpuVaOutput(input, nt.value) && fence.fence == 7019);
        const auto competing = create(); auto collision = input; collision.base = nt.value;
        before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, competing, collision))).status == -16 && gpuVa.calls == before);
        require(header(release(competing)).status == 0);
        before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, input))).status == -16 && gpuVa.calls == before);
        gpuVa.fail = true; release(allocation); gpuVa.fail = false;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, input))).status == -16);
        require(header(release(allocation)).status == 0);
        for (std::size_t n = 0; n < MaxVendorAllocations; ++n) {
            const auto id = create(); auto large = input; large.sizePages = MaxVendorMapPages;
            require(header(s.dispatch(request(Op::MapVendorAllocation, id, large))).status == 0);
        }
        // Disconnect releases all mapped native allocations before their devices.
    }
    require(gpuVa.vendorOwners.empty());
    Fake resident; resident.vendorEnabled = true;
    {
        Session s(resident);
        auto make = [&](std::uint32_t queue, const std::vector<std::uint32_t>& ids, std::uint32_t flags = 1,
                        const std::vector<std::uint32_t>& priorities = std::vector<std::uint32_t>{}) {
            auto packet = request(Op::MakeVendorResident, queue, ResidentDesc{static_cast<std::uint32_t>(ids.size()), flags, priorities.empty() ? 0u : 1u, 0});
            for (const auto& values : {ids, priorities}) {
                const auto start = packet.size(); packet.resize(start + values.size() * 4);
                if (!values.empty()) std::memcpy(packet.data() + start, values.data(), values.size() * 4);
            }
            return packet;
        };
        auto create = [&](std::uint32_t device) {
            auto packet = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{4, 0x78100000, 0, 586, 0, 0});
            packet.insert(packet.end(), 586, 0); return header(s.dispatch(packet)).handle;
        };
        auto release = [&](std::uint32_t device, std::uint32_t id) {
            auto packet = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{1, 0});
            const auto start = packet.size(); packet.resize(start + 4); std::memcpy(packet.data() + start, &id, 4);
            return s.dispatch(packet);
        };
        require(header(s.dispatch(make(3, {4}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        const auto first = create(device), second = create(device);
        auto before = resident.calls;
        require(header(s.dispatch(make(queue, {first}))).status == -95 && resident.calls == before);
        resident.residencyEnabled = true;
        require(header(s.dispatch(make(queue, {}))).status == -22);
        require(header(s.dispatch(make(queue, std::vector<std::uint32_t>(17, first)))).status == -22);
        require(header(s.dispatch(make(queue, {first}, 2))).status == -22);
        require(header(s.dispatch(make(queue, {first}, 4))).status == -22);
        require(header(s.dispatch(make(queue, {first, first}))).status == -22);
        auto truncated = make(queue, {first}, 1, {0x78100000}); truncated.pop_back();
        require(header(s.dispatch(truncated)).status == -22);
        require(header(s.dispatch(make(queue, {first, second}, 1, {0x78100000}))).status == -22);
        auto invalid = make(queue, {first}); invalid[28] = 1; // reserved field
        require(header(s.dispatch(invalid)).status == -22);
        invalid = make(queue, {first}); invalid[24] = 2; // priority-present is boolean
        require(header(s.dispatch(invalid)).status == -22);
        require(header(s.dispatch(make(device, {first}))).status == -9);
        require(header(s.dispatch(make(queue, {device}))).status == -9);
        require(header(s.dispatch(make(queue, {999}))).status == -9 && resident.calls == before);
        const auto other = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto foreign = create(other);
        before = resident.calls;
        require(header(s.dispatch(make(queue, {first, foreign}))).status == -9 && resident.calls == before);
        resident.fail = true;
        auto packet = s.dispatch(make(queue, {first, second}, 1, {0x78100000, 0x78000000}));
        Reply nt{}; ResidentReply out{};
        require(packet.size() == sizeof(Header) + sizeof(Reply) + sizeof(ResidentReply) && header(packet).handle == queue && !header(packet).status);
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); std::memcpy(&out, packet.data() + sizeof(Header) + sizeof(Reply), sizeof out);
        require(nt.ntstatus == static_cast<std::int32_t>(0xc0000017u) && nt.value == 7009 && out.count == 1 && out.bytesToTrim == 65536);
        require(resident.lastResidentHandles.size() == 2 && resident.lastResidentHandles[0] > 500 &&
                resident.lastResidentPriorities == std::vector<std::uint32_t>{0x78100000, 0x78000000});
        resident.fail = false;
        for (int n = 1; n <= 4; ++n) {
            resident.badResidentReply = n; require(header(s.dispatch(make(queue, {first}))).status == -5);
        }
        resident.badResidentReply = 0;
        packet = s.dispatch(make(queue, {first}, 0));
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); std::memcpy(&out, packet.data() + sizeof(Header) + sizeof(Reply), sizeof out);
        require(!header(packet).status && nt.ntstatus == 259 && nt.value == 7011 && out.count == 1 && !out.reserved && !out.bytesToTrim);
        // Failed native destruction retains ownership and admits another call.
        resident.fail = true; release(device, first); resident.fail = false;
        require(header(s.dispatch(make(queue, {first}))).status == 0);
        require(header(release(device, first)).status == 0);
        before = resident.calls;
        require(header(s.dispatch(make(queue, {first}))).status == -9 && resident.calls == before);
        const auto capped = create(device);
        for (unsigned n = 0; n < MaxVendorResidencyAttempts; ++n)
            require(header(s.dispatch(make(queue, {capped}))).status == 0);
        before = resident.calls;
        require(header(s.dispatch(make(queue, {capped}))).status == -24 && resident.calls == before);
        // EOF releases all allocations, including those with successful or failed residency.
    }
    require(resident.vendorOwners.empty());
    Fake pagingQuota; pagingQuota.guestPagingEnabled = true;
    {
        Session s(pagingQuota); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        for (std::size_t n = 0; n < (MaxObjects - 2) / 2; ++n)
            require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).handle != 0);
        const auto before = pagingQuota.calls;
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).status == -24 && pagingQuota.calls == before);
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
    std::cout << "PASS: WDDM wire ownership, allocation bounds/budget, chunked queries, NTSTATUS, cleanup, ABI rejection\n";
}
