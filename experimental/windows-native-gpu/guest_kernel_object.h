// SPDX-License-Identifier: MIT
// Kernel-authenticated session tickets for native NT fence references.
#pragma once
#include "guest-kernel/wddm_aperture.h"
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace guest_kernel {
class Objects {
    int control = -1;
public:
    Objects() = default;
    Objects(const Objects&) = delete;
    ~Objects() { if (control >= 0) syscall(SYS_close, control); }
    bool available() {
        if (control >= 0) return true;
        const auto value = std::getenv("WDDM_BRIDGE_KERNEL_APERTURE");
        if (!value || std::strcmp(value, "1")) { errno = ENOSYS; return false; }
        const auto fd = static_cast<int>(syscall(SYS_openat, AT_FDCWD, "/dev/wddm-aperture", O_RDWR | O_CLOEXEC, 0));
        if (fd < 0) return false;
        wddm_object_stats stats{}; stats.version = WDDM_APERTURE_VERSION;
        const auto result = syscall(SYS_ioctl, fd, WDDM_OBJECT_STATS, &stats);
        const auto error = errno;
        if (result || stats.version != WDDM_APERTURE_VERSION || stats.reserved || stats.held > WDDM_OBJECT_MAX || stats.active > stats.held) {
            syscall(SYS_close, fd); errno = result ? error : EPROTO; return false;
        }
        control = fd;
        return true;
    }
    int create(std::uint32_t ticket) {
        if (!available()) return -1;
        wddm_object_create arg{}; arg.version = WDDM_APERTURE_VERSION; arg.kind = WDDM_OBJECT_SYNC; arg.ticket = ticket;
        if (syscall(SYS_ioctl, control, WDDM_OBJECT_CREATE, &arg)) return -1;
        if (arg.fd < 0 || !arg.cookie || arg.ticket != ticket) { errno = EPROTO; return -1; }
        std::fprintf(stderr, "LINUX_BRIDGE kernelObjectExported=true kind=sync ticket=%u cookie=%llu\n", ticket, static_cast<unsigned long long>(arg.cookie));
        return arg.fd;
    }
    bool resolve(int fd, std::uint32_t& ticket) {
        if (!available()) return false;
        wddm_object_resolve arg{}; arg.version = WDDM_APERTURE_VERSION; arg.kind = WDDM_OBJECT_SYNC; arg.fd = fd;
        if (syscall(SYS_ioctl, control, WDDM_OBJECT_RESOLVE, &arg)) return false;
        if (!arg.ticket || !arg.cookie || arg.reserved || arg.kind != WDDM_OBJECT_SYNC) { errno = EPROTO; return false; }
        ticket = arg.ticket;
        return true;
    }
    int reap(std::uint32_t& ticket) {
        if (control < 0) return 0;
        wddm_object_reap arg{}; arg.version = WDDM_APERTURE_VERSION;
        if (syscall(SYS_ioctl, control, WDDM_OBJECT_REAP, &arg)) return errno == EAGAIN ? 0 : -1;
        if (!arg.ticket || !arg.cookie || arg.kind != WDDM_OBJECT_SYNC || arg.reserved) { errno = EPROTO; return -1; }
        ticket = arg.ticket;
        return 1;
    }
};
static_assert(sizeof(wddm_object_create) == 40 && sizeof(wddm_object_resolve) == 32 &&
              sizeof(wddm_object_reap) == 24 && sizeof(wddm_object_stats) == 40, "Fixed object FD ABI");
}
