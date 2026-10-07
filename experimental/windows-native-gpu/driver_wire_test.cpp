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
    int calls = 0; std::uint32_t next = 500; bool fail = false;
    std::vector<Kind> destroyed;
    native_gpu::Capabilities capabilities() const override { return {1, 3, 0x10de, 123}; }
    Result created() { ++calls; return {fail ? -123 : 0, ++next, 0}; }
    Result openAdapter() override { return created(); }
    Result queryVersion(std::uint32_t h) override { require(h > 500); ++calls; return {0, 0, 3200}; }
    Result createDevice(std::uint32_t h) override { require(h > 500); return created(); }
    Result createPagingQueue(std::uint32_t h) override { require(h > 500); return created(); }
    Result readPagingFence(std::uint32_t h) override { require(h > 500); ++calls; return {0, 0, 42}; }
    Result destroy(Kind k, std::uint32_t h) override {
        require(h > 500); ++calls; if (!fail) destroyed.push_back(k); return {fail ? -123 : 0, 0, 0};
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
    std::cout << "PASS: WDDM wire ownership, quota, NTSTATUS, cleanup, ABI rejection\n";
}
