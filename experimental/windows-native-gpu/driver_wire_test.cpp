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
    int badContextPriorityReply = 0;
    Result setContextInProcessPriority(std::uint32_t context, std::int32_t priority) override {
        require(context > 500 && validContextPriority(priority)); ++calls;
        return {fail ? -123 : badContextPriorityReply == 1 ? 259 : 0,
                badContextPriorityReply == 2 ? 501u : 0u, badContextPriorityReply == 3 ? 1ull : 0ull};
    }
    bool queriesEnabled = true, badQueryReply = false;
    bool guestPagingEnabled = false;
    int badPagingReply = 0;
    bool vendorEnabled = false;
    int badVendorDestroyReply = 0;
    bool gpuVaEnabled = false;
    bool residencyEnabled = false;
    bool cpuEnabled = false;
    bool translationEnabled = false;
    int badTranslationReply = 0;
    std::uint32_t translationOverride = 0;
    std::vector<std::uint32_t> lastTranslationHandles;
    bool hwQueuesEnabled = false;
    bool retirementEnabled = false;
    bool reservationEnabled = false;
    bool gpuStateEnabled = false;
    int badGpuStateReply = 0;
    GpuVaDesc lastGpuStateDesc{};
    int badReservationReply = 0, badReservationDestroyReply = 0;
    std::map<std::uint32_t, std::uint32_t> reservationParents;
    int badHwQueueReply = 0;
    std::map<std::uint32_t, std::uint32_t> hwQueueParents;
    bool syncEnabled = false;
    int badSyncReply = 0;
    SyncDesc lastSyncDesc{};
    std::map<std::uint32_t, std::uint32_t> syncParents;
    bool submitEnabled = false;
    int badSubmitReply = 0;
    unsigned submitCalls = 0;
    unsigned cpuLocks = 0, cpuUnlocks = 0;
    int badCpuReply = 0;
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
                (gpuVaEnabled ? VendorGpuVaCapability : 0u) | (residencyEnabled ? VendorResidencyCapability : 0u) |
                (cpuEnabled ? VendorCpuCapability : 0u) | (translationEnabled ? VendorTranslationCapability : 0u) |
                (hwQueuesEnabled ? HwQueueCapability : 0u) | (syncEnabled ? SyncCapability : 0u) |
                (submitEnabled ? HwSubmitCapability : 0u) | (retirementEnabled ? VendorRetirementCapability : 0u) |
                (reservationEnabled ? GpuReservationCapability : 0u) | (gpuStateEnabled ? GpuStateCapability : 0u), 0x10de, 123};
    }
    GpuVaResult mapGpuState(std::uint32_t reservation, std::uint32_t queue, GpuVaDesc desc) override {
        require(reservationParents.count(reservation) && queue > 500 && validGpuState(desc)); ++calls;
        lastGpuStateDesc = desc;
        if (fail) return {{-123, 0, badGpuStateReply == 5 ? desc.base : 0}, badGpuStateReply == 6 ? 42ull : 0ull};
        return {{badGpuStateReply == 1 ? 1 : 259, badGpuStateReply == 2 ? 777u : 0u,
                 badGpuStateReply == 3 ? 0ull : badGpuStateReply == 4 ? desc.base + 4096 : desc.base}, 7019};
    }
    Result created() { ++calls; return {fail ? -123 : 0, ++next, 0}; }
    Result reserveGpuAddress(std::uint32_t adapter, GpuReservationDesc desc) override {
        require(adapter > 500 && validGpuReservation(desc)); ++calls;
        if (fail && badReservationReply != 10) return {-123, 0, badReservationReply == 9 ? 65536ull : 0ull};
        const auto id = badReservationReply == 2 ? 0u : ++next;
        if (id) reservationParents.emplace(id, adapter);
        const auto address = desc.base ? desc.base : desc.minimum + (next - 500) * MaxGpuReservationBytes;
        return {fail ? -123 : badReservationReply == 1 ? 259 : 0, id,
                badReservationReply == 3 ? 0ull : badReservationReply == 4 ? address + 1 :
                badReservationReply == 5 ? MaxGpuAddress : badReservationReply == 6 ? desc.minimum - 65536 :
                badReservationReply == 7 ? desc.maximum : badReservationReply == 8 ? address + 65536 : address};
    }
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
        if (badVendorDestroyReply) return {badVendorDestroyReply == 1 ? 259 : 0,
                                          badVendorDestroyReply == 2 ? 777u : 0u,
                                          badVendorDestroyReply == 3 ? 1ull : 0ull};
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
    VendorCpuResult lockVendorAllocation(std::uint32_t allocation, std::uint32_t device) override {
        require(allocation > 500 && device > 500 && vendorOwners.at(allocation) == device); ++calls; ++cpuLocks;
        if (fail) return {{-123, 0, 0}, {0, 0, 0}};
        return {{badCpuReply == 1 ? 259 : 0, badCpuReply == 2 ? 777u : 0u,
                 badCpuReply == 3 ? 1ull : badCpuReply == 4 ? VendorCpuApertureBytes : 0ull},
                {badCpuReply == 5 ? 0u : badCpuReply == 6 ? 4097u : badCpuReply == 7 ? MaxVendorCpuMappingBytes + 4096u :
                 badCpuReply == 8 ? 8192u : 65536u, badCpuReply == 9 ? 1u : 0u, badCpuReply == 10 ? 0ull : 1ull}};
    }
    Result unlockVendorAllocation(std::uint32_t allocation, std::uint32_t device) override {
        require(allocation > 500 && device > 500 && vendorOwners.at(allocation) == device); ++calls; ++cpuUnlocks;
        return {fail ? -123 : badCpuReply == 1 ? 259 : 0, badCpuReply == 2 ? 777u : 0u, badCpuReply == 3 ? 1ull : 0ull};
    }
    Result translateVendorAllocation(std::uint32_t allocation, std::uint32_t device, std::uint32_t adapter) override {
        require(adapter > 500 && vendorOwners.at(allocation) == device); ++calls;
        lastTranslationHandles = {allocation, device, adapter};
        if (fail) return {-123, 0, badTranslationReply == 5 ? 1ull : 0ull};
        return {badTranslationReply == 3 ? 259 : 0, badTranslationReply == 4 ? 777u : 0u,
                badTranslationReply == 1 ? 0ull : badTranslationReply == 2 ? 1ull << 32 :
                translationOverride ? translationOverride : allocation + 0x10000000ull};
    }
    HwQueueResult createHwQueue(std::uint32_t context, HwQueueDesc desc, std::vector<std::uint8_t>& data) override {
        require(context > 500 && validHwQueue(desc) && data.size() == desc.privateBytes); ++calls;
        data[0] ^= 255;
        if (badHwQueueReply == 11) data.pop_back();
        if (fail) return {{-123, 0, 0}, badHwQueueReply == 12 ? 777u : 0u, 0, 0};
        const auto queue = badHwQueueReply == 2 ? 0u : ++next;
        if (queue) hwQueueParents.emplace(queue, context);
        return {{badHwQueueReply == 1 ? 259 : 0, queue, badHwQueueReply == 10 ? 1ull : 0ull},
                badHwQueueReply == 3 ? 0u : badHwQueueReply == 4 ? queue : ++next,
                badHwQueueReply == 5 ? 1ull : badHwQueueReply == 6 ? FenceApertureBytes : 8192ull,
                badHwQueueReply == 7 ? 0ull : badHwQueueReply == 8 ? 65537ull : badHwQueueReply == 9 ? MaxGpuAddress : 65536ull};
    }
    Result submitHwQueue(std::uint32_t queue, HwSubmitDesc desc, const std::vector<std::uint8_t>& data) override {
        require(hwQueueParents.count(queue) && validHwSubmit(desc) && data == std::vector<std::uint8_t>{37, 38, 39, 40});
        ++calls; ++submitCalls;
        if (fail) return {-123, 0, badSubmitReply == 5 ? 1ull : 0ull};
        return {badSubmitReply == 1 ? 259 : 0, badSubmitReply == 2 ? 777u : 0u,
                badSubmitReply == 3 ? desc.fence - 1 : badSubmitReply == 4 ? UINT64_MAX : desc.fence};
    }
    SyncResult createSync(std::uint32_t device, SyncDesc desc) override {
        require(device > 500 && validSync(desc)); ++calls; lastSyncDesc = desc;
        if (fail) return {{-123, 0, 0}, {badSyncReply == 10 ? 1ull : 0ull, 0}};
        const auto handle = badSyncReply == 2 ? 0u : ++next;
        if (handle) syncParents.emplace(handle, device);
        return {{badSyncReply == 1 ? 259 : 0, handle, badSyncReply == 3 ? 1ull : 0ull},
                {desc.type == 1 && badSyncReply != 9 ? 0ull : badSyncReply == 4 ? 1ull : badSyncReply == 5 ? FenceApertureBytes : 8192ull,
                 badSyncReply == 11 ? 65536ull : desc.type == 1 || desc.flags == NoGpuAccessSyncFlag || badSyncReply == 6 ? 0ull :
                     badSyncReply == 7 ? 65537ull : badSyncReply == 8 ? MaxGpuAddress : 65536ull}};
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
        if (k == Kind::PagingSync || k == Kind::HwQueueSync) return {0, 0, 0}; // borrowed
        if (k == Kind::VendorAllocation) return destroyVendorAllocations(vendorOwners.at(h), {h});
        if (k == Kind::GpuReservation && badReservationDestroyReply)
            return {badReservationDestroyReply == 1 ? 259 : 0, badReservationDestroyReply == 2 ? 777u : 0u,
                    badReservationDestroyReply == 3 ? 1ull : 0ull};
        if (!fail) {
            destroyed.push_back(k);
            if (k == Kind::Allocation) require(buffers.erase(h) == 1);
            if (k == Kind::HwQueue) require(hwQueueParents.erase(h) == 1);
            if (k == Kind::Sync) require(syncParents.erase(h) == 1);
            if (k == Kind::GpuReservation) require(reservationParents.erase(h) == 1);
        }
        return {fail ? -123 : 0, 0, 0};
    }
};
int main() {
    {
        GpuStateRanges ranges; GpuStateRanges::Plan plan;
        const auto page = 4096ull, base = 67108864ull;
        require(ranges.prepare(1, base, 4 * page, 5, plan)); ranges.commit(plan);
        require(ranges.prepare(1, base + page, 2 * page, 8, plan)); ranges.commit(plan);
        require(ranges.size() == 3 && ranges.bytes() == 4 * page);
        require(!ranges.prepare(2, base + page, page, 5, plan) && ranges.size() == 3);
        require(ranges.prepare(1, base + page, 2 * page, 5, plan)); ranges.commit(plan);
        require(ranges.size() == 1 && ranges.bytes() == 4 * page);
        require(ranges.prepareRemove(base + page, 2 * page, plan)); ranges.commit(plan);
        require(ranges.size() == 2 && ranges.bytes() == 2 * page);
        ranges.release(2); require(ranges.size() == 2); ranges.release(1); require(ranges.empty());
        require(ranges.prepare(1, base, GpuStateRanges::ByteLimit, 4, plan)); ranges.commit(plan);
        require(!ranges.prepare(1, base + GpuStateRanges::ByteLimit, page, 4, plan));
        require(ranges.size() == 1 && ranges.bytes() == GpuStateRanges::ByteLimit);
        ranges.release(1);
        for (std::size_t i = 0; i < GpuStateRanges::Limit; ++i) {
            require(ranges.prepare(1, base + 2 * i * page, page, 4, plan)); ranges.commit(plan);
        }
        require(!ranges.prepare(1, base + 2 * GpuStateRanges::Limit * page, page, 4, plan));
        require(ranges.size() == GpuStateRanges::Limit && ranges.bytes() == GpuStateRanges::Limit * page);
        ranges.release(1); require(ranges.empty());
    }
    Fake state; state.reservationEnabled = state.vendorEnabled = state.gpuVaEnabled = true;
    {
        Session s(state);
        GpuVaDesc desc{3, 0, 67108864, 0, 0, 0, 16, 5, 0};
        require(header(s.dispatch(request(Op::MapGpuState, 4, desc))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        desc.queue = queue;
        const auto owned = header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{desc.base, 0, 0, 65536}))).handle;
        auto before = state.calls;
        require(header(s.dispatch(request(Op::MapGpuState, owned, desc))).status == -95 && state.calls == before);
        state.gpuStateEnabled = true;
        for (unsigned n = 0; n < 17; ++n) {
            auto bad = desc;
            switch (n) {
                case 0: bad.base = 0; break;
                case 1: bad.base++; break;
                case 2: bad.offsetPages = 1; break;
                case 3: bad.protection = 12; break;
                case 4: bad.protection = 16; break;
                case 5: bad.protection = 1; break;
                case 6: bad.protection = UINT64_MAX; break;
                case 7: bad.sizePages = 0; break;
                case 8: bad.sizePages = MaxGpuReservationBytes / 4096 + 1; break;
                case 9: bad.base = MaxGpuAddress - 4096; break;
                case 10: bad.reserved = 1; break;
                case 11: bad.driverProtection = 1; break;
                case 12: bad.minimum = 1; break;
                case 13: bad.maximum = MaxGpuAddress + 4096; break;
                case 14: bad.minimum = MaxGpuAddress; break;
                case 15: bad.maximum = 1; break;
                case 16: bad.queue = 0; break;
            }
            require(header(s.dispatch(request(Op::MapGpuState, owned, bad))).status == -22 && state.calls == before);
        }
        auto shortPacket = request(Op::MapGpuState, owned, desc); shortPacket.pop_back();
        require(header(s.dispatch(shortPacket)).status == -22);
        auto foreign = desc; foreign.base += 65536;
        require(header(s.dispatch(request(Op::MapGpuState, owned, foreign))).status == -9);
        require(header(s.dispatch(request(Op::MapGpuState, device, desc))).status == -9);
        require(header(s.dispatch(request(Op::MapGpuState, 501, desc))).status == -9);
        const auto other = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto otherDevice = header(s.dispatch(request(Op::CreateDevice, other))).handle;
        foreign = desc; foreign.queue = header(s.dispatch(request(Op::CreatePagingQueue, otherDevice))).handle;
        before = state.calls;
        require(header(s.dispatch(request(Op::MapGpuState, owned, foreign))).status == -9 && state.calls == before);
        for (int n = 1; n <= 4; ++n) {
            state.badGpuStateReply = n;
            require(header(s.dispatch(request(Op::MapGpuState, owned, desc))).status == -5);
        }
        state.fail = true;
        for (int n = 5; n <= 6; ++n) {
            state.badGpuStateReply = n;
            require(header(s.dispatch(request(Op::MapGpuState, owned, desc))).status == -5);
        }
        state.badGpuStateReply = 0;
        auto packet = s.dispatch(request(Op::MapGpuState, owned, desc)); Reply nt{}; GpuVaReply fence{};
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(!header(packet).status && nt.ntstatus == -123 && !nt.value);
        state.fail = false;
        for (const auto protection : {4ull, 5ull, 6ull, 7ull, 8ull, 9ull, 10ull, 11ull}) {
            desc.protection = protection; packet = s.dispatch(request(Op::MapGpuState, owned, desc));
            std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
            std::memcpy(&fence, packet.data() + sizeof(Header) + sizeof(Reply), sizeof fence);
            require(!header(packet).status && header(packet).handle == owned && nt.ntstatus == 259 && nt.value == desc.base && fence.fence == 7019);
        }
        auto allocationPacket = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 4, 0, 0});
        allocationPacket.insert(allocationPacket.end(), 4, 0);
        const auto allocation = header(s.dispatch(allocationPacket)).handle;
        auto allocationDesc = desc; allocationDesc.protection = 1;
        require(!header(s.dispatch(request(Op::MapVendorAllocation, allocation, allocationDesc))).status);
        before = state.calls;
        auto partial = desc; partial.sizePages = 8;
        require(header(s.dispatch(request(Op::MapGpuState, owned, partial))).status == -16 && state.calls == before);
        state.fail = true;
        packet = s.dispatch(request(Op::MapGpuState, owned, desc)); std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(!header(packet).status && nt.ntstatus == -123);
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, allocationDesc))).status == -16);
        state.fail = false;
        require(!header(s.dispatch(request(Op::MapGpuState, owned, desc))).status);
        // Allocation and its CPU/residency ownership survive the page-table
        // replacement. A fresh mapping can replace those state pages again.
        require(!header(s.dispatch(request(Op::MapVendorAllocation, allocation, allocationDesc))).status && state.vendorOwners.size() == 1);
        require(!header(s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0}))).status);
        require(header(s.dispatch(request(Op::MapGpuState, owned, desc))).status == -9);
    }
    require(state.reservationParents.empty() && state.vendorOwners.empty());
    const GpuReservationDesc reservationInput{0, 67108864, 1ull << 40, 65536};
    Fake reservation;
    {
        Session s(reservation);
        require(header(s.dispatch(request(Op::ReserveGpuAddress, 1, reservationInput))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto other = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        auto before = reservation.calls;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -95 && reservation.calls == before);
        reservation.reservationEnabled = true;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, device, reservationInput))).status == -9);
        require(header(s.dispatch(request(Op::ReserveGpuAddress, 501, reservationInput))).status == -9);
        for (unsigned n = 0; n < 11; ++n) {
            auto bad = reservationInput;
            switch (n) {
                case 0: bad.bytes = 0; break;
                case 1: ++bad.bytes; break;
                case 2: bad.bytes = MaxGpuReservationBytes + 65536; break;
                case 3: bad.base = 1; break;
                case 4: ++bad.minimum; break;
                case 5: ++bad.maximum; break;
                case 6: bad.base = MaxGpuAddress; break;
                case 7: bad.minimum = MaxGpuAddress; break;
                case 8: bad.maximum = MaxGpuAddress + 65536; break;
                case 9: bad.maximum = bad.minimum; break;
                default: bad.base = MaxGpuAddress - 65536; bad.bytes = 131072; break;
            }
            require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, bad))).status == -22 && reservation.calls == before);
        }
        auto shortPacket = request(Op::ReserveGpuAddress, adapter, reservationInput); shortPacket.pop_back();
        require(header(s.dispatch(shortPacket)).status == -22);
        reservation.fail = true;
        auto failure = s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput)); Reply nt{};
        std::memcpy(&nt, failure.data() + sizeof(Header), sizeof nt);
        require(!header(failure).status && !header(failure).handle && nt.ntstatus == -123 && !nt.value);
        reservation.badReservationReply = 9;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -5);
        reservation.fail = false;
        for (int n = 1; n <= 7; ++n) {
            reservation.badReservationReply = n;
            require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -5 && reservation.reservationParents.empty());
        }
        reservation.badReservationReply = 8;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{67108864, 0, 0, 65536}))).status == -5);
        reservation.badReservationReply = 0;
        const auto reservedPacket = s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput));
        const auto reserved = header(reservedPacket).handle;
        std::memcpy(&nt, reservedPacket.data() + sizeof(Header), sizeof nt);
        require(reserved && reserved < 500 && nt.ntstatus == 0 && validGpuReservationOutput(reservationInput, nt.value));
        before = reservation.calls;
        require(header(s.dispatch(request(Op::CloseAdapter, adapter))).status == -16);
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{nt.value, 0, 0, 65536}))).status == -16);
        require(header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{other, 0}))).status == -9);
        require(header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 1}))).status == -22);
        require(header(s.dispatch(request(Op::FreeGpuReservation, device, FreeGpuReservationDesc{adapter, 0}))).status == -9 && reservation.calls == before);
        for (int n = 1; n <= 3; ++n) {
            reservation.badReservationDestroyReply = n;
            require(header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 0}))).status == -5 && reservation.reservationParents.size() == 1);
        }
        reservation.badReservationDestroyReply = 0; reservation.fail = true;
        failure = s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 0}));
        std::memcpy(&nt, failure.data() + sizeof(Header), sizeof nt);
        require(!header(failure).status && nt.ntstatus == -123 && reservation.reservationParents.size() == 1);
        reservation.fail = false;
        require(!header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 0}))).status);
        before = reservation.calls;
        require(header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 0}))).status == -9 && reservation.calls == before);
        for (std::size_t n = 0; n < MaxGpuReservations; ++n)
            require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).handle != 0);
        before = reservation.calls;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -24 && reservation.calls == before);
    }
    require(reservation.reservationParents.empty() && reservation.destroyed.back() == Kind::Adapter);
    Fake reservationBudget; reservationBudget.reservationEnabled = true;
    {
        Session s(reservationBudget); s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        auto maximumRange = reservationInput; maximumRange.bytes = MaxGpuReservationBytes;
        for (unsigned n = 0; n < 4; ++n) require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, maximumRange))).handle != 0);
        const auto before = reservationBudget.calls;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -24 && reservationBudget.calls == before);
    }
    Fake reservationOverlap; reservationOverlap.reservationEnabled = reservationOverlap.vendorEnabled = reservationOverlap.gpuVaEnabled = true;
    {
        Session s(reservationOverlap); s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        auto allocationPacket = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 4, 0, 0});
        allocationPacket.insert(allocationPacket.end(), 4, 0);
        const auto allocation = header(s.dispatch(allocationPacket)).handle;
        const auto owned = header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{67108864, 0, 0, 65536}))).handle;
        require(owned && !header(s.dispatch(request(Op::MapVendorAllocation, allocation, GpuVaDesc{queue, 0, 67108864, 0, 0, 0, 32, 1, 0}))).status);
        const auto before = reservationOverlap.calls;
        require(header(s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0}))).status == -16 && reservationOverlap.calls == before);
        // The reservation was created last; disconnect still destroys its
        // mapped allocation before releasing the address range and adapter.
    }
    require(reservationOverlap.reservationParents.empty() && reservationOverlap.vendorOwners.empty());
    const auto allocationDestroyed = std::find(reservationOverlap.destroyed.begin(), reservationOverlap.destroyed.end(), Kind::VendorAllocation);
    const auto rangeDestroyed = std::find(reservationOverlap.destroyed.begin(), reservationOverlap.destroyed.end(), Kind::GpuReservation);
    require(allocationDestroyed < rangeDestroyed && rangeDestroyed < reservationOverlap.destroyed.end() - 1 && reservationOverlap.destroyed.back() == Kind::Adapter);
    Fake reservationMapped; reservationMapped.reservationEnabled = reservationMapped.vendorEnabled = reservationMapped.gpuVaEnabled = true;
    {
        Session s(reservationMapped); s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        auto allocationPacket = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 4, 0, 0});
        allocationPacket.insert(allocationPacket.end(), 4, 0);
        const auto allocation = header(s.dispatch(allocationPacket)).handle;
        const auto owned = header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{67108864, 0, 0, 65536}))).handle;
        const auto mapping = request(Op::MapVendorAllocation, allocation, GpuVaDesc{queue, 0, 67108864, 0, 0, 0, 16, 1, 0});
        require(owned && !header(s.dispatch(mapping)).status);
        reservationMapped.fail = true;
        auto failure = s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0})); Reply nt{};
        std::memcpy(&nt, failure.data() + sizeof(Header), sizeof nt);
        require(!header(failure).status && nt.ntstatus == -123);
        const auto before = reservationMapped.calls;
        require(header(s.dispatch(mapping)).status == -16 && reservationMapped.calls == before);
        reservationMapped.fail = false;
        require(!header(s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0}))).status);
        // Successful release invalidates the GPU mapping while retaining the
        // allocation itself. It can then receive a fresh GPU mapping.
        require(!header(s.dispatch(mapping)).status && reservationMapped.vendorOwners.size() == 1);
        require(header(s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0}))).status == -9);
    }
    require(reservationMapped.reservationParents.empty() && reservationMapped.vendorOwners.empty());
    require(validVendorCpuSlots(16) && validVendorCpuSlots(32) && validVendorCpuSlots(64));
    for (const auto slots : {0u, 1u, 8u, 15u, 17u, 31u, 33u, 63u, 65u, UINT32_MAX}) require(!validVendorCpuSlots(slots));
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
        require(validVendorAllocation(VendorAllocationDesc{4, 0xa0000000, 0, 586, 0, 0}) &&
                validVendorAllocation(VendorAllocationDesc{4, 0xc8000000, 0, 586, 0, 0}) &&
                validVendorAllocation(VendorAllocationDesc{4, desc.priority, UninitializedDisplaySource, 586, 0, 0}));
        for (const auto input : {VendorAllocationDesc{5, desc.priority, 0, 586, 0, 0}, VendorAllocationDesc{4, 0xc8000001, 0, 586, 0, 0},
                                 VendorAllocationDesc{5, desc.priority, UninitializedDisplaySource, 586, 0, 0},
                                 VendorAllocationDesc{4, desc.priority, UINT32_MAX - 1, 586, 0, 0},
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
        const auto second = header(s.dispatch(create(device, VendorAllocationDesc{4, desc.priority, UninitializedDisplaySource, 586, 0, 0}))).handle;
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
        for (std::size_t n = 0; n < MaxVendorMappedPages / MaxVendorMapPages; ++n) {
            const auto id = create(); auto large = input; large.sizePages = MaxVendorMapPages;
            require(header(s.dispatch(request(Op::MapVendorAllocation, id, large))).status == 0);
        }
        const auto extra = create(); auto large = input; large.sizePages = MaxVendorMapPages;
        before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, extra, large))).status == -24 && gpuVa.calls == before);
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
    Fake cpu; cpu.vendorEnabled = cpu.gpuVaEnabled = cpu.cpuEnabled = true;
    {
        Session s(cpu);
        require(header(s.dispatch(request(Op::LockVendorAllocation, 1, VendorCpuDesc{2, 0}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto other = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        auto create = [&] {
            auto packet = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 1, 0, 0});
            packet.push_back(1); return header(s.dispatch(packet)).handle;
        };
        auto mapped = [&](std::uint32_t allocation, std::uint64_t offset = 0) {
            return s.dispatch(request(Op::MapVendorAllocation, allocation, GpuVaDesc{queue, 0, 0, 65536, 1ull << 40, offset, 16, 1, 0}));
        };
        const auto allocation = create(); require(allocation != 0);
        const auto lock = request(Op::LockVendorAllocation, allocation, VendorCpuDesc{device, 0});
        const auto unlock = request(Op::UnlockVendorAllocation, allocation, VendorCpuDesc{device, 0});
        auto before = cpu.calls;
        cpu.cpuEnabled = false; require(header(s.dispatch(lock)).status == -95); cpu.cpuEnabled = true;
        require(header(s.dispatch(request(Op::LockVendorAllocation, allocation))).status == -22);
        require(header(s.dispatch(request(Op::LockVendorAllocation, allocation, VendorCpuDesc{device, 1}))).status == -22);
        require(header(s.dispatch(request(Op::LockVendorAllocation, allocation, VendorCpuDesc{0, 0}))).status == -22);
        require(header(s.dispatch(request(Op::LockVendorAllocation, allocation, VendorCpuDesc{other, 0}))).status == -9);
        require(header(s.dispatch(request(Op::LockVendorAllocation, device, VendorCpuDesc{device, 0}))).status == -9);
        require(header(s.dispatch(lock)).status == -95 && header(s.dispatch(unlock)).status == -9 && cpu.calls == before);
        require(header(mapped(allocation)).status == 0);
        const auto subrange = create(); require(header(mapped(subrange, 1)).status == 0);
        before = cpu.calls;
        require(header(s.dispatch(request(Op::LockVendorAllocation, subrange, VendorCpuDesc{device, 0}))).status == -95 && cpu.calls == before);
        cpu.fail = true;
        auto out = s.dispatch(lock); Reply reply{}; std::memcpy(&reply, out.data() + sizeof(Header), sizeof reply);
        require(header(out).status == 0 && reply.ntstatus == -123 && reply.value == 0 && out.size() == sizeof(Header) + sizeof(Reply) + sizeof(VendorCpuReply));
        cpu.fail = false;
        for (int malformedCpu = 1; malformedCpu <= 10; ++malformedCpu) {
            cpu.badCpuReply = malformedCpu; require(header(s.dispatch(lock)).status == -5);
        }
        cpu.badCpuReply = 0;
        out = s.dispatch(lock); VendorCpuReply body{};
        std::memcpy(&reply, out.data() + sizeof(Header), sizeof reply);
        std::memcpy(&body, out.data() + sizeof(Header) + sizeof reply, sizeof body);
        require(header(out).handle == allocation && reply.value == 0 && body.bytes == 65536 && body.generation == 1);
        before = cpu.calls; require(header(s.dispatch(lock)).status == -16 && cpu.calls == before);
        auto release = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{1, 0});
        const auto bytes = reinterpret_cast<const std::uint8_t*>(&allocation); release.insert(release.end(), bytes, bytes + 4);
        require(header(s.dispatch(release)).status == -16 && cpu.calls == before);
        const auto duplicate = create(); require(header(mapped(duplicate)).status == 0);
        require(header(s.dispatch(request(Op::LockVendorAllocation, duplicate, VendorCpuDesc{device, 0}))).status == -5);
        cpu.fail = true; out = s.dispatch(unlock); std::memcpy(&reply, out.data() + sizeof(Header), sizeof reply);
        require(header(out).status == 0 && reply.ntstatus == -123);
        cpu.fail = false;
        require(header(s.dispatch(lock)).status == -16);
        for (int malformedCpu = 1; malformedCpu <= 3; ++malformedCpu) {
            cpu.badCpuReply = malformedCpu; require(header(s.dispatch(unlock)).status == -5);
        }
        cpu.badCpuReply = 0;
        require(header(s.dispatch(unlock)).status == 0);
        before = cpu.calls; require(header(s.dispatch(unlock)).status == -9 && cpu.calls == before);
        require(header(s.dispatch(lock)).status == 0); // A new lease can reuse the slot after unlock.
        // EOF retains the vendor allocation for native child-before-parent destruction.
    }
    require(cpu.vendorOwners.empty() && cpu.cpuLocks > 0 && cpu.cpuUnlocks > 0);
    Fake translation; translation.vendorEnabled = translation.translationEnabled = true;
    {
        Session s(translation);
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, 3, VendorTranslationDesc{2, 1, 0, 0}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto otherAdapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto otherDevice = header(s.dispatch(request(Op::CreateDevice, otherAdapter))).handle;
        auto create = [&] {
            auto p = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 1, 0, 0});
            p.push_back(37); return header(s.dispatch(p)).handle;
        };
        const auto allocation = create(), second = create();
        const auto translate = request(Op::TranslateVendorAllocation, allocation, VendorTranslationDesc{device, adapter, 0, 0});
        const auto before = translation.calls;
        translation.translationEnabled = false; require(header(s.dispatch(translate)).status == -95);
        translation.translationEnabled = true;
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, allocation))).status == -22);
        for (const auto desc : {VendorTranslationDesc{0, adapter, 0, 0}, VendorTranslationDesc{device, 0, 0, 0},
                               VendorTranslationDesc{device, adapter, 1, 0}, VendorTranslationDesc{device, adapter, 0, 1}})
            require(header(s.dispatch(request(Op::TranslateVendorAllocation, allocation, desc))).status == -22);
        for (const auto desc : {VendorTranslationDesc{otherDevice, otherAdapter, 0, 0}, VendorTranslationDesc{device, otherAdapter, 0, 0},
                               VendorTranslationDesc{allocation, adapter, 0, 0}, VendorTranslationDesc{device, device, 0, 0}})
            require(header(s.dispatch(request(Op::TranslateVendorAllocation, allocation, desc))).status == -9);
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, device, VendorTranslationDesc{device, adapter, 0, 0}))).status == -9);
        require(translation.calls == before);
        translation.fail = true;
        auto out = s.dispatch(translate); Reply body{}; std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && header(out).handle == allocation && body.ntstatus == -123 && body.value == 0);
        translation.badTranslationReply = 5; require(header(s.dispatch(translate)).status == -5);
        translation.fail = false;
        for (int malformedTranslation = 1; malformedTranslation <= 4; ++malformedTranslation) {
            translation.badTranslationReply = malformedTranslation; require(header(s.dispatch(translate)).status == -5);
        }
        translation.badTranslationReply = 0;
        out = s.dispatch(translate); std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).handle == allocation && body.ntstatus == 0 && body.value == 0x10000000ull + allocation + 500);
        require(translation.lastTranslationHandles == std::vector<std::uint32_t>{allocation + 500, device + 500, adapter + 500});
        require(header(s.dispatch(translate)).status == 0);
        translation.translationOverride = static_cast<std::uint32_t>(body.value);
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, second, VendorTranslationDesc{device, adapter, 0, 0}))).status == -5);
        ++translation.translationOverride; require(header(s.dispatch(translate)).status == -5);
        translation.translationOverride = 0;
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, second, VendorTranslationDesc{device, adapter, 0, 0}))).status == 0);
        // The driver token never becomes a typed wire identity.
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, static_cast<std::uint32_t>(body.value), VendorTranslationDesc{device, adapter, 0, 0}))).status == -9);
    }
    require(translation.vendorOwners.empty());
    Fake hardware; hardware.hwQueuesEnabled = hardware.vendorEnabled = true;
    {
        Session s(hardware);
        require(header(s.dispatch(request(Op::CreateHwQueue, 3, HwQueueDesc{0, 4, 0, 0}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        auto contextPacket = request(Op::CreateContext, device, ContextDesc{0, 1, 16, 12, 4, 0});
        contextPacket.insert(contextPacket.end(), {1, 2, 3, 4});
        const auto context = header(s.dispatch(contextPacket)).handle;
        const auto syncContext = header(s.dispatch(request(Op::CreateContext, device, ContextDesc{0, 0, 8, 0, 0, 0}))).handle;
        auto create = [&](std::uint32_t parent, HwQueueDesc desc = {0, 4, 0, 0}) {
            auto p = request(Op::CreateHwQueue, parent, desc); p.insert(p.end(), {37, 38, 39, 40}); return p;
        };
        auto before = hardware.calls;
        hardware.hwQueuesEnabled = false; require(header(s.dispatch(create(context))).status == -95); hardware.hwQueuesEnabled = true;
        require(header(s.dispatch(create(device))).status == -9);
        require(header(s.dispatch(create(syncContext))).status == -95);
        require(header(s.dispatch(request(Op::CreateHwQueue, context))).status == -22);
        for (const auto desc : {HwQueueDesc{1, 4, 0, 0}, HwQueueDesc{2, 4, 0, 0}, HwQueueDesc{4, 4, 0, 0},
                               HwQueueDesc{0, 0, 0, 0}, HwQueueDesc{0, 4001, 0, 0}, HwQueueDesc{0, 4, 1, 0}, HwQueueDesc{0, 4, 0, 1}})
            require(header(s.dispatch(create(context, desc))).status == -22);
        require(hardware.calls == before);
        hardware.fail = true;
        auto out = s.dispatch(create(context)); Reply body{}; HwQueueReply progress{};
        std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        std::memcpy(&progress, out.data() + sizeof(Header) + sizeof body, sizeof progress);
        require(header(out).status == 0 && !header(out).handle && body.ntstatus == -123 && !progress.sync && !progress.gpuAddress && out.back() == 40);
        hardware.badHwQueueReply = 12; require(header(s.dispatch(create(context))).status == -5);
        hardware.fail = false;
        for (int malformedQueue = 1; malformedQueue <= 11; ++malformedQueue) {
            hardware.badHwQueueReply = malformedQueue; require(header(s.dispatch(create(context))).status == -5 && hardware.hwQueueParents.empty());
        }
        hardware.badHwQueueReply = 0;
        out = s.dispatch(create(context)); const auto queue = header(out).handle;
        std::memcpy(&progress, out.data() + sizeof(Header) + sizeof body, sizeof progress);
        require(queue && progress.sync && progress.sync != queue && progress.offset == 8192 && progress.gpuAddress == 65536);
        require(out[sizeof(Header) + sizeof body + sizeof progress] == (37 ^ 255));
        before = hardware.calls;
        require(header(s.dispatch(request(Op::DestroyContext, context))).status == -16);
        require(header(s.dispatch(request(Op::DestroyDevice, device))).status == -16);
        require(header(s.dispatch(request(Op::DestroyHwQueue, progress.sync))).status == -9 && hardware.calls == before);
        auto allocationPacket = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 1, 0, 0}); allocationPacket.push_back(37);
        const auto allocation = header(s.dispatch(allocationPacket)).handle;
        auto release = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{1, 0});
        const auto bytes = reinterpret_cast<const std::uint8_t*>(&allocation); release.insert(release.end(), bytes, bytes + 4);
        before = hardware.calls; require(header(s.dispatch(release)).status == -16 && hardware.calls == before);
        hardware.retirementEnabled = true;
        hardware.fail = true; out = s.dispatch(release); std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && body.ntstatus == -123 && !hardware.vendorOwners.empty());
        hardware.fail = false;
        for (int bad = 1; bad <= 3; ++bad) {
            hardware.badVendorDestroyReply = bad;
            require(header(s.dispatch(release)).status == -5 && !hardware.vendorOwners.empty());
        }
        hardware.badVendorDestroyReply = 0;
        require(header(s.dispatch(release)).status == 0 && hardware.vendorOwners.empty() && !hardware.hwQueueParents.empty());
        before = hardware.calls; require(header(s.dispatch(release)).status == -9 && hardware.calls == before);
        hardware.retirementEnabled = false;
        hardware.fail = true; out = s.dispatch(request(Op::DestroyHwQueue, queue)); std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && body.ntstatus == -123 && !hardware.hwQueueParents.empty());
        hardware.fail = false;
        require(header(s.dispatch(request(Op::DestroyHwQueue, queue))).status == 0);
        require(header(s.dispatch(request(Op::DestroyHwQueue, queue))).status == -9);
        for (std::size_t n = 0; n < MaxHwQueues; ++n) require(header(s.dispatch(create(context))).handle != 0);
        before = hardware.calls; require(header(s.dispatch(create(context))).status == -24 && hardware.calls == before);
        // An allocation created after the queues still outlives them at EOF.
        require(header(s.dispatch(allocationPacket)).handle != 0);
        hardware.destroyed.clear();
    }
    require(hardware.hwQueueParents.empty() && hardware.vendorOwners.empty() && hardware.destroyed.size() >= MaxHwQueues + 5);
    for (std::size_t n = 0; n < MaxHwQueues; ++n) require(hardware.destroyed[n] == Kind::HwQueue);
    require(hardware.destroyed[MaxHwQueues] == Kind::VendorAllocation);
    Fake synchronization; synchronization.syncEnabled = synchronization.guestPagingEnabled = true;
    {
        Session s(synchronization);
        require(header(s.dispatch(request(Op::CreateSync, 2, SyncDesc{5, 0, 0, 0, 0}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto syncPaging = s.dispatch(request(Op::CreateGuestPagingQueue, device)); PagingReply borrowed{};
        std::memcpy(&borrowed, syncPaging.data() + sizeof(Header) + sizeof(Reply), sizeof borrowed);
        const auto create = request(Op::CreateSync, device, SyncDesc{5, 0, 1, 0, 42});
        const auto before = synchronization.calls;
        synchronization.syncEnabled = false; require(header(s.dispatch(create)).status == -95); synchronization.syncEnabled = true;
        require(header(s.dispatch(request(Op::CreateSync, adapter, SyncDesc{5, 0, 0, 0, 0}))).status == -9);
        require(header(s.dispatch(request(Op::CreateSync, device))).status == -22);
        for (const auto desc : {SyncDesc{4, 0, 0, 0, 0}, SyncDesc{5, 1, 0, 0, 0}, SyncDesc{5, 0, 2, 0, 0},
                               SyncDesc{5, 0, 0, 1, 0}, SyncDesc{1, 0, 0, 0, 2}, SyncDesc{1, 0, 1, 0, 0},
                               SyncDesc{1, NoGpuAccessSyncFlag, 0, 0, 0}, SyncDesc{5, NoGpuAccessSyncFlag | 1, 0, 0, 0}})
            require(header(s.dispatch(request(Op::CreateSync, device, desc))).status == -22);
        require(header(s.dispatch(request(Op::DestroySync, borrowed.sync))).status == -9 && synchronization.calls == before);
        synchronization.fail = true;
        auto out = s.dispatch(create); Reply body{}; SyncReply view{};
        std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        std::memcpy(&view, out.data() + sizeof(Header) + sizeof body, sizeof view);
        require(header(out).status == 0 && !header(out).handle && body.ntstatus == -123 && !view.offset && !view.gpuAddress);
        synchronization.badSyncReply = 10; require(header(s.dispatch(create)).status == -5);
        synchronization.fail = false;
        for (int malformedSync = 1; malformedSync <= 8; ++malformedSync) {
            synchronization.badSyncReply = malformedSync; require(header(s.dispatch(create)).status == -5 && synchronization.syncParents.empty());
        }
        synchronization.badSyncReply = 9;
        require(header(s.dispatch(request(Op::CreateSync, device, SyncDesc{1, 0, 0, 0, 0}))).status == -5 && synchronization.syncParents.empty());
        synchronization.badSyncReply = 0;
        out = s.dispatch(create); const auto fence = header(out).handle;
        std::memcpy(&view, out.data() + sizeof(Header) + sizeof body, sizeof view);
        require(fence && view.offset == 8192 && view.gpuAddress == 65536 && synchronization.lastSyncDesc.initial == 42 && synchronization.lastSyncDesc.affinity == 1);
        const auto mutex = header(s.dispatch(request(Op::CreateSync, device, SyncDesc{1, 0, 0, 0, 1}))).handle;
        require(mutex && synchronization.lastSyncDesc.initial == 1);
        require(header(s.dispatch(request(Op::DestroyDevice, device))).status == -16);
        synchronization.fail = true;
        out = s.dispatch(request(Op::DestroySync, mutex)); std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && body.ntstatus == -123 && synchronization.syncParents.size() == 2);
        synchronization.fail = false;
        require(header(s.dispatch(request(Op::DestroySync, mutex))).status == 0);
        require(header(s.dispatch(request(Op::DestroySync, mutex))).status == -9);
        const auto cpuFenceRequest = request(Op::CreateSync, device, SyncDesc{5, NoGpuAccessSyncFlag, 0, 0, 42});
        synchronization.badSyncReply = 11;
        require(header(s.dispatch(cpuFenceRequest)).status == -5 && synchronization.syncParents.size() == 1);
        synchronization.badSyncReply = 0;
        out = s.dispatch(cpuFenceRequest); const auto cpuFence = header(out).handle;
        std::memcpy(&view, out.data() + sizeof(Header) + sizeof body, sizeof view);
        require(cpuFence && view.offset == 8192 && !view.gpuAddress && synchronization.lastSyncDesc.flags == NoGpuAccessSyncFlag);
        require(header(s.dispatch(request(Op::DestroySync, cpuFence))).status == 0);
        const auto noMaxRequest = request(Op::CreateSync, device, SyncDesc{5, NoSignalMaxValueOnTdrSyncFlag, 0, 0, 42});
        synchronization.badSyncReply = 6; // A GPU-accessible fence must have a valid GPU address.
        require(header(s.dispatch(noMaxRequest)).status == -5 && synchronization.syncParents.size() == 1);
        synchronization.badSyncReply = 0;
        out = s.dispatch(noMaxRequest); const auto noMaxFence = header(out).handle;
        std::memcpy(&view, out.data() + sizeof(Header) + sizeof body, sizeof view);
        require(noMaxFence && view.offset == 8192 && view.gpuAddress == 65536 &&
                synchronization.lastSyncDesc.flags == NoSignalMaxValueOnTdrSyncFlag && synchronization.lastSyncDesc.initial == 42);
        require(header(s.dispatch(request(Op::DestroySync, noMaxFence))).status == 0);
        for (std::uint32_t flags = 0; flags < 1024; ++flags) {
            require(validSync({5, flags, 0, 0, 0}) == (flags == 0 || flags == NoSignalMaxValueOnTdrSyncFlag || flags == NoGpuAccessSyncFlag));
            require(validSync({1, flags, 0, 0, 0}) == (flags == 0));
        }
        for (std::size_t n = 1; n < MaxSyncObjects; ++n)
            require(header(s.dispatch(request(Op::CreateSync, device, SyncDesc{1, 0, 0, 0, 0}))).handle != 0);
        require(header(s.dispatch(create)).status == -24);
    }
    require(synchronization.syncParents.empty());
    Fake submission; submission.submitEnabled = submission.hwQueuesEnabled = submission.vendorEnabled = submission.gpuVaEnabled =
                     submission.residencyEnabled = submission.cpuEnabled = true;
    {
        Session s(submission); const HwSubmitDesc validSubmit{65536, 5, 4096, 4, 0, 0};
        require(header(s.dispatch(request(Op::SubmitHwQueue, 3, validSubmit))).status == -71);
        s.dispatch(hello());
        const auto submitAdapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto submitDevice = header(s.dispatch(request(Op::CreateDevice, submitAdapter))).handle;
        auto submitContextPacket = request(Op::CreateContext, submitDevice, ContextDesc{0, 1, 16, 12, 4, 0});
        submitContextPacket.insert(submitContextPacket.end(), {1, 2, 3, 4});
        const auto submitContext = header(s.dispatch(submitContextPacket)).handle;
        auto submitQueuePacket = request(Op::CreateHwQueue, submitContext, HwQueueDesc{0, 4, 0, 0});
        submitQueuePacket.insert(submitQueuePacket.end(), {37, 38, 39, 40});
        const auto submitQueue = header(s.dispatch(submitQueuePacket)).handle;
        const auto submitPaging = header(s.dispatch(request(Op::CreatePagingQueue, submitDevice))).handle;
        auto submitAllocationPacket = request(Op::CreateVendorAllocation, submitDevice, VendorAllocationDesc{0, 0, 0, 1, 0, 0});
        submitAllocationPacket.push_back(37);
        const auto submitAllocation = header(s.dispatch(submitAllocationPacket)).handle;
        auto submitPacket = [&](std::uint32_t queue, HwSubmitDesc desc) {
            auto p = request(Op::SubmitHwQueue, queue, desc); p.insert(p.end(), {37, 38, 39, 40}); return p;
        };
        auto submitBefore = submission.calls;
        submission.submitEnabled = false;
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -95);
        submission.submitEnabled = true;
        require(header(s.dispatch(submitPacket(submitDevice, validSubmit))).status == -9);
        require(header(s.dispatch(request(Op::SubmitHwQueue, submitQueue))).status == -22);
        for (unsigned n = 0; n < 11; ++n) {
            auto invalid = validSubmit;
            switch (n) {
                case 0: invalid.address = 0; break;
                case 1: ++invalid.address; break;
                case 2: invalid.address = MaxGpuAddress - 4096; invalid.bytes = 8192; break;
                case 3: invalid.bytes = 0; break;
                case 4: ++invalid.bytes; break;
                case 5: invalid.bytes = MaxAllocation + 4096; break;
                case 6: invalid.fence = 0; break;
                case 7: invalid.fence = UINT64_MAX; break;
                case 8: invalid.privateBytes = 4001; break;
                case 9: invalid.primaries = 1; break;
                default: invalid.reserved = 1; break;
            }
            require(header(s.dispatch(submitPacket(submitQueue, invalid))).status == -22);
        }
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -16 && submission.calls == submitBefore);
        const GpuVaDesc submitMapping{submitPaging, 0, 65536, 0, 0, 0, 16, 1, 0};
        require(header(s.dispatch(request(Op::MapVendorAllocation, submitAllocation, submitMapping))).status == 0);
        auto submitResidency = request(Op::MakeVendorResident, submitPaging, ResidentDesc{1, 1, 0, 0});
        const auto submitIdBytes = reinterpret_cast<const std::uint8_t*>(&submitAllocation);
        submitResidency.insert(submitResidency.end(), submitIdBytes, submitIdBytes + 4);
        require(header(s.dispatch(submitResidency)).status == 0);
        submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -16 && submission.calls == submitBefore);
        require(header(s.dispatch(request(Op::LockVendorAllocation, submitAllocation, VendorCpuDesc{submitDevice, 0}))).status == 0);
        auto submitOutside = validSubmit; submitOutside.address = 131072;
        submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, submitOutside))).status == -9 && submission.calls == submitBefore);
        auto submitOverrun = validSubmit; submitOverrun.address += 61440; submitOverrun.bytes = 8192;
        require(header(s.dispatch(submitPacket(submitQueue, submitOverrun))).status == -9 && submission.calls == submitBefore);
        submission.fail = true; auto submitOut = s.dispatch(submitPacket(submitQueue, validSubmit)); Reply submitNt{};
        std::memcpy(&submitNt, submitOut.data() + sizeof(Header), sizeof submitNt);
        require(header(submitOut).handle == submitQueue && !header(submitOut).status && submitNt.ntstatus == -123 && !submitNt.value);
        submission.badSubmitReply = 5; require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -5);
        submission.fail = false;
        for (int malformedSubmit = 1; malformedSubmit <= 4; ++malformedSubmit) {
            submission.badSubmitReply = malformedSubmit;
            require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -5);
        }
        submission.badSubmitReply = 0;
        submitOut = s.dispatch(submitPacket(submitQueue, validSubmit));
        std::memcpy(&submitNt, submitOut.data() + sizeof(Header), sizeof submitNt);
        require(!header(submitOut).status && submitNt.ntstatus == 0 && submitNt.value == validSubmit.fence);
        submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -22 && submission.calls == submitBefore);
        auto submitNext = validSubmit;
        while (submission.submitCalls < MaxHwSubmissions) {
            ++submitNext.fence; require(header(s.dispatch(submitPacket(submitQueue, submitNext))).status == 0);
        }
        ++submitNext.fence; submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, submitNext))).status == -24 && submission.calls == submitBefore);
    }
    require(submission.hwQueueParents.empty() && submission.vendorOwners.empty());
    Fake priorityDriver;
    {
        Session s(priorityDriver);
        auto priorityPacket = request(Op::SetContextInProcessPriority, 99, std::int32_t{0});
        require(header(s.dispatch(priorityPacket)).status == -71); s.dispatch(hello());
        priorityDriver.contextsEnabled = false;
        require(header(s.dispatch(priorityPacket)).status == -95); priorityDriver.contextsEnabled = true;
        require(header(s.dispatch(priorityPacket)).status == -9);
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        auto contextPacket = request(Op::CreateContext, d, ContextDesc{0, 1, 16, 12, 4, 0});
        contextPacket.insert(contextPacket.end(), {1, 2, 3, 4});
        const auto context = header(s.dispatch(contextPacket)).handle;
        const auto before = priorityDriver.calls;
        for (const auto invalid : {INT32_MIN, -1, 2, 7, INT32_MAX})
            require(header(s.dispatch(request(Op::SetContextInProcessPriority, context, invalid))).status == -22);
        require(header(s.dispatch(request(Op::SetContextInProcessPriority, d, std::int32_t{0}))).status == -9);
        require(header(s.dispatch(request(Op::SetContextInProcessPriority, context))).status == -22);
        priorityPacket = request(Op::SetContextInProcessPriority, context, std::int32_t{0}); priorityPacket.push_back(0);
        require(header(s.dispatch(priorityPacket)).status == -22 && priorityDriver.calls == before);
        priorityPacket.pop_back();
        priorityDriver.fail = true; auto out = s.dispatch(priorityPacket); Reply body{};
        std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && body.ntstatus == -123 && header(out).handle == context);
        priorityDriver.fail = false;
        for (int bad = 1; bad <= 3; ++bad) {
            priorityDriver.badContextPriorityReply = bad;
            require(header(s.dispatch(priorityPacket)).status == -5);
        }
        priorityDriver.badContextPriorityReply = 0;
        require(header(s.dispatch(priorityPacket)).status == 0);
        require(header(s.dispatch(request(Op::SetContextInProcessPriority, context, std::int32_t{1}))).status == 0);
        require(header(s.dispatch(request(Op::DestroyContext, context))).status == 0);
        const auto afterDestroy = priorityDriver.calls;
        require(header(s.dispatch(priorityPacket)).status == -9 && priorityDriver.calls == afterDestroy);
    }
    Fake pagingQuota; pagingQuota.guestPagingEnabled = true;
    {
        Session s(pagingQuota); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        for (std::size_t n = 0; n < (MaxObjects - 2) / 2; ++n)
            require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).handle != 0);
        require(s.objectCount() == MaxObjects);
        const auto before = pagingQuota.calls;
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).status == -24 && pagingQuota.calls == before);
    }
    // A two-handle queue cannot consume the last singleton slot. Retiring
    // that singleton opens exactly two slots, without lifting the aggregate cap.
    {
        Session s(pagingQuota); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        for (std::size_t n = 0; n < (MaxObjects - 4) / 2; ++n)
            require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).handle != 0);
        const auto spare = header(s.dispatch(request(Op::OpenAdapter))).handle;
        require(spare && s.objectCount() == MaxObjects - 1);
        const auto before = pagingQuota.calls;
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).status == -24 && pagingQuota.calls == before);
        require(header(s.dispatch(request(Op::CloseAdapter, spare))).status == 0 && s.objectCount() == MaxObjects - 2);
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).handle != 0 && s.objectCount() == MaxObjects);
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
