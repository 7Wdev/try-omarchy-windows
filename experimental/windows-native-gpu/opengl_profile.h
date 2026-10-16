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
// Version 5 replaces lifetime command/request caps with bounded live work.
// Zero is an exact, negotiated rolling budget, never a legacy-profile wildcard.
// Memory-byte, pending-work and aperture limits remain independently enforced.
constexpr OpenGlProfile GuestOpenGlDesktopProfile{5, 0, 0, 64, 16ull * 1024 * 1024 * 1024, 0, 1024};
// A real 3840x2160 compositor crosses the v5 256 MiB GPU-VA alias-map
// budget while importing its output, then its reported-usage guard. Version 6
// bounds both GPU budgets at 512 MiB;
// CPU maps, live objects, pending work and all older profiles stay bounded.
constexpr OpenGlProfile GuestOpenGlDesktop4kProfile{6, 0, 0, 64, 16ull * 1024 * 1024 * 1024, 0, 1024};
constexpr std::uint32_t OpenGlDesktop4kGpuMappedBytes = 512u * 1024 * 1024;
constexpr std::uint64_t OpenGlDesktop4kReportedGpuBytes = 512ull * 1024 * 1024;
constexpr std::uint32_t OpenGlDesktopVendorAllocations = 512;
inline bool lifetimeBudgetAvailable(std::uint64_t used,std::uint32_t limit) {
    return used<UINT64_MAX && (!limit || used<limit);
}
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
inline bool validOpenGlDesktopProfile(OpenGlProfile p) {
    return p.version==5 && !p.frames && !p.commandLimit && p.reservationLimit==64 &&
           p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
inline bool validOpenGlDesktop4kProfile(OpenGlProfile p) {
    return p.version==6 && !p.frames && !p.commandLimit && p.reservationLimit==64 &&
           p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
}
