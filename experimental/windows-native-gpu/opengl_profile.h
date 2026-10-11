// SPDX-License-Identifier: MIT
// Explicit two-frame Mesa lab contract. Older session budgets stay unchanged.
#pragma once
#include <cstdint>
namespace driver_bridge {
struct OpenGlProfile {
    std::uint32_t version, frames, commandLimit, reservationLimit;
    std::uint64_t reservedBytes;
    std::uint32_t requestLimit, reserved;
};
static_assert(sizeof(OpenGlProfile) == 32, "fixed OpenGL lab negotiation");
constexpr OpenGlProfile GuestOpenGlProfile{1, 2, 64, 16, 16ull * 1024 * 1024 * 1024, 10000, 0};
inline bool validOpenGlProfile(OpenGlProfile p) {
    return p.version == 1 && p.frames == 2 && p.commandLimit == 64 && p.reservationLimit == 16 &&
           p.reservedBytes == 16ull * 1024 * 1024 * 1024 && p.requestLimit == 10000 && !p.reserved;
}
}
