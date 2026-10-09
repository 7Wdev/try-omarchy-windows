// SPDX-License-Identifier: MIT
#include "cpu_aperture.h"
#include <iostream>
#include <tuple>
#include <vector>
using driver_cpu::Aperture;
using driver_cpu::Lease;
static void require(bool good) { if (!good) throw std::runtime_error("CPU aperture regression"); }
template<class F> static void rejected(F fn) { bool threw = false; try { fn(); } catch (const std::runtime_error&) { threw = true; } require(threw); }
int main() {
    constexpr std::uint32_t stride = 1048576, largeBytes = 272 * 4096;
    using Chunk = std::tuple<std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t>;
    std::vector<Chunk> mapped;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> unmapped;
    auto map = [&](std::uint32_t slot, std::uint64_t offset, std::uint32_t bytes, std::uint64_t generation) { mapped.emplace_back(slot, offset, bytes, generation); };
    auto unmap = [&](std::uint32_t slot, std::uint64_t generation) { unmapped.emplace_back(slot, generation); };
    unsigned stopped = 0; auto stop = [&] { ++stopped; };
    Aperture a(16);
    require(!a.canMap(0) && !a.canMap(4097) && !a.canMap(4 * stride + 4096));
    rejected([&] { a.map(0, map, stop); }); require(mapped.empty() && !stopped);
    const auto large = a.map(largeBytes, map, stop);
    require(large.slot == 0 && large.offset == 0 && large.generation == 1 && large.bytes == largeBytes);
    require(mapped == std::vector<Chunk>{{0, 0, stride, 1}, {1, stride, 65536, 2}});
    require(a.groups() == 1 && a.chunks() == 2 && a.bytes() == largeBytes);
    const auto small = a.map(4096, map, stop); require(small.slot == 2 && small.generation == 3);
    auto bad = large; bad.bytes = stride; rejected([&] { a.release(bad, unmap, stop); });
    bad = large; ++bad.generation; rejected([&] { a.release(bad, unmap, stop); });
    bad = large; ++bad.slot; bad.offset += stride; rejected([&] { a.release(bad, unmap, stop); });
    require(unmapped.empty() && !stopped && a.groups() == 2);
    a.release(large, unmap, stop);
    require(unmapped == std::vector<std::pair<std::uint32_t, std::uint64_t>>{{1, 2}, {0, 1}});
    require(a.groups() == 1 && a.chunks() == 1 && a.bytes() == 4096);
    rejected([&] { a.release(large, unmap, stop); });
    a.release(small, unmap, stop); require(a.groups() == 0 && a.chunks() == 0 && a.bytes() == 0);
    const auto maximum = a.map(4 * stride, map, stop);
    require(maximum.slot == 0 && maximum.generation == 4 && a.chunks() == 4);
    a.release(maximum, unmap, stop); require(a.maps() == 3 && a.unmaps() == 3 && a.chunkMaps() == 7 && a.chunkUnmaps() == 7);
    require(driver_bridge::validVendorCpuReply(0, {largeBytes, 0, 1}));
    require(!driver_bridge::validVendorCpuReply(driver_bridge::VendorCpuApertureBytes - stride, {largeBytes, 0, 1}));
    Aperture fragmented(16); std::vector<Lease> onePage;
    for (unsigned n = 0; n < 16; ++n) onePage.push_back(fragmented.map(4096, map, stop));
    for (unsigned n = 0; n < 16; n += 2) fragmented.release(onePage[n], unmap, stop);
    require(fragmented.canMap(4096) && !fragmented.canMap(largeBytes));
    rejected([&] { fragmented.map(largeBytes, map, stop); }); require(!stopped);
    for (unsigned n = 1; n < 16; n += 2) fragmented.release(onePage[n], unmap, stop);
    Aperture mapFailure(16);
    for (unsigned n = 0; n < 14; ++n) mapFailure.map(4096, map, stop);
    unsigned attempts = 0;
    rejected([&] { mapFailure.map(largeBytes, [&](std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t) {
        if (++attempts == 2) throw std::runtime_error("second chunk mapping failed");
    }, [&] { require(!mapFailure.canMap(4096) && mapFailure.groups() == 14); ++stopped; }); });
    require(stopped == 1 && attempts == 2 && mapFailure.groups() == 0 && mapFailure.chunks() == 0 && mapFailure.bytes() == 0);
    require(mapFailure.maps() == 14 && mapFailure.chunkMaps() == 15 && mapFailure.chunkUnmaps() == 0);
    Aperture unmapFailure(16); const auto held = unmapFailure.map(largeBytes, map, stop);
    for (unsigned n = 0; n < 14; ++n) unmapFailure.map(4096, map, stop);
    attempts = 0;
    rejected([&] { unmapFailure.release(held, [&](std::uint32_t, std::uint64_t) {
        if (++attempts == 2) throw std::runtime_error("second chunk unmapping failed");
    }, [&] { require(!unmapFailure.canMap(4096) && unmapFailure.groups() == 15); ++stopped; }); });
    require(stopped == 2 && attempts == 2 && unmapFailure.groups() == 0 && unmapFailure.chunks() == 0);
    require(unmapFailure.unmaps() == 0 && unmapFailure.chunkUnmaps() == 1);
    std::cout << "PASS: contiguous CPU spans, exact chunk ownership, fragmentation and VM-exit-first partial failure cleanup\n";
}
