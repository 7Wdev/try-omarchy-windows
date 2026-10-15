// SPDX-License-Identifier: MIT
// Explicit, bounded Mesa lab contracts. Older session budgets stay unchanged.
#pragma once
#include <cstdint>
namespace driver_bridge {
struct OpenGlProfile {
    std::uint32_t version, frames, commandLimit, reservationLimit;
    std::uint64_t reservedBytes;
    // Version 1 reserves the final word as zero; later versions carry their cap.
    std::uint32_t requestLimit, objectLimit;
};
static_assert(sizeof(OpenGlProfile) == 32, "fixed OpenGL lab negotiation");
constexpr OpenGlProfile GuestOpenGlProfile{1, 2, 64, 16, 16ull * 1024 * 1024 * 1024, 10000, 0};
constexpr OpenGlProfile GuestOpenGlFenceProfile{2, 2, 64, 16, 16ull * 1024 * 1024 * 1024, 10000, 384};
// A compositor owns multiple Mesa screens (allocator and renderer). Its startup
// exceeds the two-frame probe's 96 sync objects. Version 3 is an explicit
// initialization profile, not a sustained desktop or performance guarantee.
constexpr OpenGlProfile GuestOpenGlCompositorProfile{3, 2, 256, 16, 16ull * 1024 * 1024 * 1024, 50000, 512};
// The real compositor's first draw creates additional small GPUVA reservations.
// Preserve version 3; rendering capacity is a distinct, exact negotiation.
constexpr OpenGlProfile GuestOpenGlCompositorRenderProfile{4, 2, 256, 32, 16ull * 1024 * 1024 * 1024, 50000, 768};
constexpr std::uint32_t OpenGlCompositorSyncObjects = 192;
constexpr std::uint32_t OpenGlCompositorVendorAllocations = 256;
constexpr std::uint32_t OpenGlCompositorHwSignals = GuestOpenGlCompositorProfile.commandLimit;
constexpr std::uint32_t OpenGlFenceHwQueues = 16;
constexpr std::uint32_t OpenGlFenceVendorAllocations = 192;
// Lifetime signals must cover the profile's command budget. Pending native
// signals retain the independent MaxHwQueueSignals (16) concurrency bound.
constexpr std::uint32_t OpenGlFenceHwSignals = GuestOpenGlFenceProfile.commandLimit;
inline bool validOpenGlProfile(OpenGlProfile p) {
    return p.version == 1 && p.frames == 2 && p.commandLimit == 64 && p.reservationLimit == 16 &&
           p.reservedBytes == 16ull * 1024 * 1024 * 1024 && p.requestLimit == 10000 && !p.objectLimit;
}
inline bool validOpenGlFenceProfile(OpenGlProfile p) {
    return p.version == 2 && p.frames == 2 && p.commandLimit == 64 && p.reservationLimit == 16 &&
           p.reservedBytes == 16ull * 1024 * 1024 * 1024 && p.requestLimit == 10000 && p.objectLimit == 384;
}
inline bool validOpenGlCompositorProfile(OpenGlProfile p) {
    return p.version == 3 && p.frames == 2 && p.commandLimit == 256 && p.reservationLimit == 16 &&
           p.reservedBytes == 16ull * 1024 * 1024 * 1024 && p.requestLimit == 50000 && p.objectLimit == 512;
}
inline bool validOpenGlCompositorRenderProfile(OpenGlProfile p) {
    return p.version == 4 && p.frames == 2 && p.commandLimit == 256 && p.reservationLimit == 32 &&
           p.reservedBytes == 16ull * 1024 * 1024 * 1024 && p.requestLimit == 50000 && p.objectLimit == 768;
}
}
