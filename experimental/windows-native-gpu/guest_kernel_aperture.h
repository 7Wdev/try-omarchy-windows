// SPDX-License-Identifier: MIT
// Explicit guest-kernel path. Never falls back to raw sysfs BAR access.
#pragma once
#include "guest-kernel/wddm_aperture.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace guest_kernel {
class Aperture {
    int control = -1;
    struct View { std::uint64_t cookie; std::size_t bytes; };
    std::map<void*, View> views;
    static void failed(const char* operation) {
        throw std::runtime_error(std::string("Kernel aperture ") + operation + ": " + std::strerror(errno));
    }
    void open() {
        if (control >= 0) return;
        control = static_cast<int>(syscall(SYS_openat, AT_FDCWD, "/dev/wddm-aperture", O_RDWR | O_CLOEXEC, 0));
        if (control < 0) failed("open");
    }
    void revoke(std::uint64_t cookie) {
        wddm_aperture_revoke arg{WDDM_APERTURE_VERSION, 0, cookie};
        if (syscall(SYS_ioctl, control, WDDM_APERTURE_REVOKE, &arg)) failed("revoke");
    }
public:
    Aperture() = default;
    Aperture(const Aperture&) = delete;
    ~Aperture() {
        for (const auto& entry : views) munmap(entry.first, entry.second.bytes);
        // Closing the controller revokes all of its remaining FDs and VMAs.
        if (control >= 0) syscall(SYS_close, control);
    }
    bool enabled() const {
        const auto value = std::getenv("WDDM_BRIDGE_KERNEL_APERTURE");
        if (!value || !*value) return false;
        if (std::strcmp(value, "1")) throw std::runtime_error("Invalid kernel aperture opt-in");
        return true;
    }
    wddm_aperture_info info() {
        open();
        wddm_aperture_info value{}; value.version = WDDM_APERTURE_VERSION;
        if (syscall(SYS_ioctl, control, WDDM_APERTURE_INFO, &value)) failed("info");
        if (value.version != WDDM_APERTURE_VERSION ||
            (value.fence_slots != 64 && value.fence_slots != 128) ||
            value.allocation_slots < 16 || value.allocation_slots > 128 ||
            (value.allocation_slots & (value.allocation_slots - 1)) ||
            value.cpu_slot_bytes != 1048576 || value.fence_bytes != value.fence_slots * 4096ull ||
            value.allocation_bytes != value.allocation_slots * value.cpu_slot_bytes ||
            value.active_leases > WDDM_APERTURE_MAX_LEASES || value.active_cpu_bytes > WDDM_APERTURE_CPU_LIMIT)
            throw std::runtime_error("Kernel aperture layout mismatch");
        return value;
    }
    void* map(std::uint32_t kind, std::uint64_t offset, std::size_t bytes, std::uint64_t generation = 0) {
        open();
        wddm_aperture_create arg{};
        arg.version = WDDM_APERTURE_VERSION; arg.kind = kind;
        arg.offset = offset; arg.bytes = bytes; arg.host_generation = generation;
        arg.flags = kind == WDDM_APERTURE_ALLOCATION ? WDDM_APERTURE_WRITE : 0;
        if (syscall(SYS_ioctl, control, WDDM_APERTURE_CREATE, &arg)) failed("create");
        if (!arg.cookie || arg.fd < 0) throw std::runtime_error("Kernel aperture lease response mismatch");
        auto data = mmap(nullptr, bytes, PROT_READ | (arg.flags ? PROT_WRITE : 0), MAP_SHARED, arg.fd, 0);
        const auto saved = errno;
        syscall(SYS_close, arg.fd);
        if (data == MAP_FAILED) { revoke(arg.cookie); errno = saved; failed("mmap"); }
        try {
            if (!views.emplace(data, View{arg.cookie, bytes}).second)
                throw std::runtime_error("Overlapping kernel aperture virtual mappings");
        } catch (...) { munmap(data, bytes); revoke(arg.cookie); throw; }
        std::fprintf(stderr, "LINUX_BRIDGE kernelApertureLease=true kind=%u bytes=%zu cookie=%llu\n", kind, bytes,
                     static_cast<unsigned long long>(arg.cookie));
        return data;
    }
    void unmap(void* data, std::size_t bytes) {
        const auto found = views.find(data);
        if (found == views.end() || found->second.bytes != bytes)
            throw std::runtime_error("Kernel aperture mapping absent");
        if (munmap(data, bytes)) failed("munmap");
        revoke(found->second.cookie);
        views.erase(found);
        std::fprintf(stderr, "LINUX_BRIDGE kernelApertureRevoked=true\n");
    }
};
static_assert(sizeof(wddm_aperture_info) == 72 && sizeof(wddm_aperture_create) == 64 &&
              sizeof(wddm_aperture_revoke) == 16, "Fixed kernel aperture ABI");
} // namespace guest_kernel
