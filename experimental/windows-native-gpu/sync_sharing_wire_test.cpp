// SPDX-License-Identifier: MIT
#include "driver_wire.h"
#include <iostream>
#include <set>
using namespace driver_bridge;
static unsigned checks = 0;
static void require(bool good) { ++checks; if (!good) throw std::runtime_error("Shared sync wire regression"); }
static Header header(const std::vector<std::uint8_t>& data) {
    require(data.size() >= sizeof(Header)); Header h{}; memcpy(&h, data.data(), sizeof h); return h;
}
struct Fake : Driver {
    std::uint32_t next = 1000;
    unsigned exports = 0, imports = 0, calls = 0;
    bool supported = true, fail = false;
    int badExport = 0, badImport = 0;
    std::map<std::uint32_t, Kind> live;
    Result create(Kind kind) { ++calls; const auto id = ++next; live.emplace(id, kind); return {0, id, 0}; }
    native_gpu::Capabilities capabilities() const override { return {Version, 7 | SyncCapability, 0x10de, 1}; }
    SyncSharingProfile beginSyncSharing() override { ++calls; return supported ? GuestSyncSharingProfile : SyncSharingProfile{}; }
    Result openAdapter() override { return create(Kind::Adapter); }
    Result queryVersion(std::uint32_t) override { return {0, 0, 31}; }
    Result queryAdapter(std::uint32_t, QueryDesc, std::vector<std::uint8_t>&) override { return {-1, 0, 0}; }
    Result createDevice(std::uint32_t) override { return create(Kind::Device); }
    Result createContext(std::uint32_t, ContextDesc, std::vector<std::uint8_t>&) override { return {-1, 0, 0}; }
    SyncResult createSync(std::uint32_t, SyncDesc d) override {
        auto r = create(Kind::Sync); return {r, {4096, d.flags & 128 ? 0ull : 65536ull}};
    }
    Result exportSync(std::uint32_t id) override {
        require(live.at(id) == Kind::Sync); ++exports;
        if (fail) return {-123, 0, 0};
        auto r = create(Kind::SharedSync);
        if (badExport == 1) r.ntstatus = 259;
        if (badExport == 2) r.value = 1;
        return r;
    }
    SyncResult importSync(std::uint32_t id, SharedSyncOpenDesc d) override {
        require(live.at(id) == Kind::SharedSync && live.at(d.device) == Kind::Device); ++imports;
        if (fail) return {{-123, 0, 0}, {0, 0}};
        auto r = create(Kind::Sync); SyncReply output{8192, d.flags & 128 ? 0ull : 131072ull};
        if (badImport == 1) r.ntstatus = 259;
        if (badImport == 2) r.value = 1;
        if (badImport == 3) output.offset = FenceApertureBytes;
        if (badImport == 4) output.offset = 1;
        if (badImport == 5) output.gpuAddress = 0;
        if (badImport == 6) output.gpuAddress = MaxGpuAddress;
        return {r, output};
    }
    Result createPagingQueue(std::uint32_t) override { return {-1, 0, 0}; }
    Result readPagingFence(std::uint32_t) override { return {-1, 0, 0}; }
    Result createAllocation(std::uint32_t, std::uint32_t) override { return {-1, 0, 0}; }
    Result createSharedAllocation(std::uint32_t, GuestRange) override { return {-1, 0, 0}; }
    Result copySharedAllocation(std::uint32_t, std::uint32_t, CopyRange) override { return {-1, 0, 0}; }
    Result writeAllocation(std::uint32_t, Range, const std::uint8_t*) override { return {-1, 0, 0}; }
    Result readAllocation(std::uint32_t, Range, std::vector<std::uint8_t>&) override { return {-1, 0, 0}; }
    Result makeResident(std::uint32_t, std::uint32_t) override { return {-1, 0, 0}; }
    Result mapAllocation(std::uint32_t, std::uint32_t) override { return {-1, 0, 0}; }
    Result queryResidency(std::uint32_t) override { return {-1, 0, 0}; }
    Result destroy(Kind kind, std::uint32_t id) override {
        require(live.at(id) == kind); live.erase(id); return {0, 0, 0};
    }
};
static void negotiate(Session& s) { require(header(s.dispatch(request(Op::Hello, 0, Version))).status == 0); }
static void begin(Session& s) {
    const auto out = s.dispatch(request(Op::BeginSyncSharing, 0));
    require(header(out).status == 0 && out.size() == sizeof(Header) + sizeof(SyncSharingProfile));
    SyncSharingProfile p{}; memcpy(&p, out.data() + sizeof(Header), sizeof p); require(validSyncSharingProfile(p));
}
static std::uint32_t add(Session& s, Op op, std::uint32_t parent) {
    const auto h = header(s.dispatch(request(op, parent))); require(h.status == 0 && h.handle); return h.handle;
}
static std::uint32_t sync(Session& s, std::uint32_t device, std::uint32_t flags = 3) {
    const auto h = header(s.dispatch(request(Op::CreateSync, device, SyncDesc{5, flags, 1, 0, 0})));
    require(h.status == 0 && h.handle); return h.handle;
}
int main() {
    try {
        Fake fake;
        {
            Session s(fake);
            require(header(s.dispatch(request(Op::BeginSyncSharing, 0))).status == -71);
            negotiate(s);
            fake.supported = false; require(header(s.dispatch(request(Op::BeginSyncSharing, 0))).status == -95);
            fake.supported = true;
            auto bad = request(Op::BeginSyncSharing, 0); bad.push_back(0);
            require(header(s.dispatch(bad)).status == -22);
            require(header(s.dispatch(request(Op::BeginSyncSharing, 1))).status == -22);
            const auto adapter = add(s, Op::OpenAdapter, 0), sourceDevice = add(s, Op::CreateDevice, adapter);
            const auto device = add(s, Op::CreateDevice, adapter);
            require(header(s.dispatch(request(Op::CreateSync, device, SyncDesc{5, 3, 1, 0, 0}))).status == -71);
            begin(s); require(header(s.dispatch(request(Op::BeginSyncSharing, 0))).status == -16);
            const auto source = sync(s, sourceDevice), notShared = sync(s, device, 0);
            require(header(s.dispatch(request(Op::ExportSync, notShared))).status == -22);
            require(header(s.dispatch(request(Op::ExportSync, device))).status == -9);
            for (int n = 1; n <= 2; ++n) {
                fake.badExport = n;
                const auto before = fake.live.size();
                require(header(s.dispatch(request(Op::ExportSync, source))).status == -5 && fake.live.size() == before);
            }
            fake.badExport = 0; fake.fail = true;
            auto failed = s.dispatch(request(Op::ExportSync, source));
            require(header(failed).status == 0 && !header(failed).handle);
            Reply result{}; memcpy(&result, failed.data() + sizeof(Header), sizeof result); require(result.ntstatus == -123);
            fake.fail = false;
            const auto ticket = add(s, Op::ExportSync, source);
            require(header(s.dispatch(request(Op::DestroySync, source))).status == 0);
            require(header(s.dispatch(request(Op::DestroyDevice, sourceDevice))).status == 0);
            require(header(s.dispatch(request(Op::CloseAdapter, adapter))).status == -16);
            const auto otherAdapter = add(s, Op::OpenAdapter, 0), otherDevice = add(s, Op::CreateDevice, otherAdapter);
            const auto beforeCalls = fake.imports;
            for (auto desc : {SharedSyncOpenDesc{0,0,1,0}, SharedSyncOpenDesc{device,1,1,0},
                              SharedSyncOpenDesc{device,0,2,0}, SharedSyncOpenDesc{device,0,1,1}})
                require(header(s.dispatch(request(Op::ImportSync, ticket, desc))).status == -22);
            require(header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{otherDevice,0,1,0}))).status == -9);
            require(header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{adapter,0,1,0}))).status == -9);
            require(fake.imports == beforeCalls);
            for (int n = 1; n <= 6; ++n) {
                fake.badImport = n; const auto before = fake.live.size();
                require(header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{device,0,1,0}))).status == -5 && fake.live.size() == before);
            }
            fake.badImport = 0;
            const auto imported = header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{device,0,1,0}))).handle;
            require(imported);
            const auto reexported = add(s, Op::ExportSync, imported);
            require(header(s.dispatch(request(Op::CloseSharedSync, ticket))).status == 0);
            require(header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{device,0,1,0}))).status == -9);
            require(header(s.dispatch(request(Op::CloseSharedSync, ticket))).status == -9);
            require(header(s.dispatch(request(Op::DestroySync, imported))).status == 0);
            require(header(s.dispatch(request(Op::CloseSharedSync, reexported))).status == 0);
            const auto remaining = sync(s, device);
            std::vector<std::uint32_t> tickets;
            for (std::size_t n = 0; n < MaxSharedSyncHandles; ++n) tickets.push_back(add(s, Op::ExportSync, remaining));
            const auto exports = fake.exports;
            require(header(s.dispatch(request(Op::ExportSync, remaining))).status == -24 && fake.exports == exports);
            // Disconnect must close every remaining NT reference and KMT object.
        }
        require(fake.live.empty());
        std::cout << "PASS: " << checks << " shared-fence protocol checks, scoped tickets, cross-device ownership, quotas, malformed replies and cleanup\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
