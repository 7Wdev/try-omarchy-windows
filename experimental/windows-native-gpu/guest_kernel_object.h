// SPDX-License-Identifier: MIT
// Kernel-authenticated, typed session tickets for native NT object references.
#pragma once
#include "guest-kernel/wddm_aperture.h"
#include "guest_broker_client.h"
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
    bool remoteStarted = false;
public:
    Objects() = default;
    Objects(const Objects&) = delete;
    ~Objects() { if (control >= 0) syscall(SYS_close, control); }
    bool available() {
        if (guest_broker::clientEnabled()) {
            if (remoteStarted) return true;
            try {
                const auto stats=guest_broker::client().objectInfo();
                if (stats.version!=WDDM_APERTURE_VERSION || stats.reserved || stats.held>WDDM_OBJECT_MAX || stats.active>stats.held) {
                    errno=EPROTO; return false;
                }
                remoteStarted=true; return true;
            } catch (const std::system_error& error) { errno=error.code().value(); return false; }
        }
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
    int create(std::uint32_t ticket, std::uint32_t kind = WDDM_OBJECT_SYNC) {
        if (kind != WDDM_OBJECT_SYNC && kind != WDDM_OBJECT_RESOURCE) { errno = EINVAL; return -1; }
        if (!available()) return -1;
        if (guest_broker::clientEnabled()) {
            try {
                auto reply=guest_broker::client().createObject(ticket,kind);
                const auto ref=guest_broker::decode<guest_broker::ObjectRef>(reply.payload);
                std::fprintf(stderr,"LINUX_BRIDGE kernelObjectExported=true kind=%s ticket=%u cookie=%llu\n",kind==WDDM_OBJECT_SYNC ? "sync" : "resource",ticket,static_cast<unsigned long long>(ref.cookie));
                return reply.fds[0].release();
            } catch (const std::system_error& error) { errno=error.code().value(); return -1; }
        }
        wddm_object_create arg{}; arg.version = WDDM_APERTURE_VERSION; arg.kind = kind; arg.ticket = ticket;
        if (syscall(SYS_ioctl, control, WDDM_OBJECT_CREATE, &arg)) return -1;
        if (arg.fd < 0 || !arg.cookie || arg.ticket != ticket) { errno = EPROTO; return -1; }
        std::fprintf(stderr, "LINUX_BRIDGE kernelObjectExported=true kind=%s ticket=%u cookie=%llu\n", kind == WDDM_OBJECT_SYNC ? "sync" : "resource", ticket, static_cast<unsigned long long>(arg.cookie));
        return arg.fd;
    }
    bool resolve(int fd, std::uint32_t& ticket, std::uint32_t kind = WDDM_OBJECT_SYNC) {
        if (kind != WDDM_OBJECT_SYNC && kind != WDDM_OBJECT_RESOURCE) { errno = EINVAL; return false; }
        if (!available()) return false;
        if (guest_broker::clientEnabled()) {
            try { ticket=guest_broker::client().resolveObject(fd,kind).ticket; return true; }
            catch (const std::system_error& error) { errno=error.code().value(); return false; }
        }
        wddm_object_resolve arg{}; arg.version = WDDM_APERTURE_VERSION; arg.kind = kind; arg.fd = fd;
        if (syscall(SYS_ioctl, control, WDDM_OBJECT_RESOLVE, &arg)) return false;
        if (!arg.ticket || !arg.cookie || arg.reserved || arg.kind != kind) { errno = EPROTO; return false; }
        ticket = arg.ticket;
        return true;
    }
    int reap(std::uint32_t& ticket, std::uint32_t* kind = nullptr) {
        if (guest_broker::clientEnabled()) {
            if (!remoteStarted) return 0;
            try {
                const auto ref=guest_broker::client().reapObject();
                if (!ref.ticket) return 0;
                ticket=ref.ticket; if (kind) *kind=ref.kind; return 1;
            } catch (const std::system_error& error) { errno=error.code().value(); return -1; }
        }
        if (control < 0) return 0;
        wddm_object_reap arg{}; arg.version = WDDM_APERTURE_VERSION;
        if (syscall(SYS_ioctl, control, WDDM_OBJECT_REAP, &arg)) return errno == EAGAIN ? 0 : -1;
        if (!arg.ticket || !arg.cookie || (arg.kind != WDDM_OBJECT_SYNC && arg.kind != WDDM_OBJECT_RESOURCE) || arg.reserved) { errno = EPROTO; return -1; }
        ticket = arg.ticket;
        if (kind) *kind = arg.kind;
        return 1;
    }
};
static_assert(sizeof(wddm_object_create) == 40 && sizeof(wddm_object_resolve) == 32 &&
              sizeof(wddm_object_reap) == 24 && sizeof(wddm_object_stats) == 40, "Fixed object FD ABI");
}
