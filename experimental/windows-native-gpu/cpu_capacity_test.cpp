// SPDX-License-Identifier: MIT
// Reproduce virtual-slot exhaustion with small UMD views plus a full 4K map.
#include "cpu_aperture.h"
#include <iostream>
#include <vector>
static void require(bool value) { if (!value) throw std::runtime_error("CPU virtual capacity regression"); }
int main() {
    using namespace driver_bridge;
    static_assert(gpu_diagnostic::HighResolution, "This regression needs the explicit 4K profile");
    static_assert(ExpandedVendorCpuBytes == 96u * 1024 * 1024, "Do not raise the physical byte budget");
    static_assert(MaxVendorCpuMappingBytes == 64u * 1024 * 1024, "Keep the per-view bound");
    constexpr std::uint32_t imageBytes = 3840u * 2160 * 4;
    unsigned stops = 0;
    auto stop = [&] { ++stops; };
    auto map = [](std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t) {};
    auto unmap = [](std::uint32_t, std::uint64_t) {};
    for (const auto slots : {128u, 256u}) {
        driver_cpu::Aperture aperture(slots);
        std::vector<driver_cpu::Lease> small;
        for (unsigned n = 0; n < 100; ++n) small.push_back(aperture.map(4096, map, stop));
        require(aperture.bytes() == 100 * 4096 && aperture.chunks() == 100);
        require(vendorCpuBudgetFits(aperture.bytes(), imageBytes, ExpandedVendorCpuCapability));
        require(aperture.canMap(imageBytes) == (slots == 256));
        if (slots == 256) {
            const auto image = aperture.map(imageBytes, map, stop);
            require(image.slot == 100 && aperture.chunks() == 132);
            require(aperture.bytes() == imageBytes + 100 * 4096);
            require(validVendorCpuReply(image.offset, {image.bytes, 0, image.generation}));
            aperture.release(image, unmap, stop);
            const auto reused = aperture.map(imageBytes, map, stop);
            require(reused.slot == image.slot && reused.generation > image.generation);
            aperture.release(reused, unmap, stop);
        }
        for (const auto view : small) aperture.release(view, unmap, stop);
        require(!aperture.bytes() && !aperture.groups() && aperture.chunkMaps() == aperture.chunkUnmaps());
    }
    require(!vendorCpuBudgetFits(ExpandedVendorCpuBytes - 4096, 8192, ExpandedVendorCpuCapability));
    require(!validVendorCpuSlots(255) && !validVendorCpuSlots(257) && !validVendorCpuSlots(512));
    require(!stops);
    std::cout << "PASS: 256 virtual slots fit full 4K map beside small views; unchanged 96 MiB CPU budget\n";
}
