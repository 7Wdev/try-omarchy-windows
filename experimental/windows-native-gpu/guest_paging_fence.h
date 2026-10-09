// SPDX-License-Identifier: MIT
// Process-local diagnostic mapping of the private QEMU read-only PCI hub.
#pragma once
#include "driver_wire.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
#include <map>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>

namespace guest_paging {
class Fences {
    struct File {
        int fd = -1;
        File() = default;
        File(const File&) = delete;
        ~File() { if (fd >= 0) syscall(SYS_close, fd); }
        void open(const std::string& path, int flags) {
            if (fd >= 0) syscall(SYS_close, fd);
            fd = static_cast<int>(syscall(SYS_openat, AT_FDCWD, path.c_str(), flags, 0));
        }
    };
    File config, resource;
    struct Mapping { void* page; volatile std::uint64_t* fence; };
    std::map<std::uint32_t, Mapping> mappings;
    std::uint64_t reads = 0;
    unsigned created = 0;
    static constexpr std::uint32_t Identity = 0x11fd1234, Magic = 0x31484e46;
    std::uint32_t word(unsigned offset) const {
        std::uint32_t value{};
        if (pread(config.fd, &value, sizeof value, offset) != sizeof value) throw std::runtime_error("Fence PCI config read failed");
        return value;
    }
    void discover() {
        const char* directory = "/sys/bus/pci/devices";
        auto devices = opendir(directory);
        if (!devices) throw std::runtime_error("Fence PCI inventory unavailable");
        std::string found;
        unsigned count = 0;
        while (auto entry = readdir(devices)) {
            if (entry->d_name[0] == '.') continue;
            if (++count > 256) { closedir(devices); throw std::runtime_error("Fence PCI inventory bound exceeded"); }
            const auto path = std::string(directory) + '/' + entry->d_name;
            File candidate; candidate.open(path + "/config", O_RDONLY | O_CLOEXEC);
            std::uint32_t identity{};
            if (candidate.fd >= 0 && pread(candidate.fd, &identity, sizeof identity, 0) == sizeof identity && identity == Identity) {
                if (!found.empty()) { closedir(devices); throw std::runtime_error("Multiple private fence hubs"); }
                found = path;
            }
        }
        closedir(devices);
        if (found.empty()) throw std::runtime_error("Private fence hub absent");
        config.open(found + "/config", O_RDONLY | O_CLOEXEC);
        if (word(0) != Identity || word(0x40) != Magic || word(0x44) != 64) throw std::runtime_error("Fence hub layout mismatch");
        File enable; enable.open(found + "/enable", O_WRONLY | O_CLOEXEC);
        if (enable.fd < 0 || write(enable.fd, "1", 1) != 1) throw std::runtime_error("Cannot enable private fence hub");
        resource.open(found + "/resource0", O_RDONLY | O_CLOEXEC | O_SYNC);
        if (resource.fd < 0) throw std::runtime_error("Fence BAR unavailable");
    }
public:
    Fences() = default;
    Fences(const Fences&) = delete;
    ~Fences() { for (const auto& mapping : mappings) munmap(mapping.second.page, 4096); }
    void* map(std::uint32_t queue, std::uint64_t offset, const char* category = "paging") {
        if (!queue || offset % 8 || offset >= driver_bridge::FenceApertureBytes || mappings.count(queue) || mappings.size() >= 64)
            throw std::runtime_error("Invalid guest fence identity or offset");
        if (resource.fd < 0) discover();
        const auto before = word(0x48);
        auto page = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, resource.fd, static_cast<off_t>(offset & ~4095ull));
        if (page == MAP_FAILED) throw std::runtime_error("Guest fence mapping failed");
        auto fence = reinterpret_cast<volatile std::uint64_t*>(static_cast<char*>(page) + (offset & 4095));
        // This is a diagnostic, not a polling API. Prove these reads bypass
        // the QEMU MMIO emulation before exposing the CPU fence to the UMD.
        std::uint64_t observed = 0;
        for (unsigned n = 0; n < 10000; ++n) observed = *fence;
        try {
            if (word(0x48) != before) throw std::runtime_error("Guest fence reads were emulated");
            mappings.emplace(queue, Mapping{page, fence});
        } catch (...) { munmap(page, 4096); throw; }
        reads += 10000; ++created;
        std::fprintf(stderr, "LINUX_BRIDGE %sFenceMapped=true direct=true loads=10000 offset=%llu value=%llu\n",
                     category, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(observed));
        return const_cast<std::uint64_t*>(fence);
    }
    void verifyRetired(std::uint32_t queue, std::uint64_t target, const char* operation) {
        const auto found = mappings.find(queue);
        if (found == mappings.end()) throw std::runtime_error("Mapped guest paging fence absent");
        const auto before = word(0x48);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t observed = 0;
        for (unsigned n = 0; n < 10000; ++n) observed = *found->second.fence;
        if (observed == UINT64_MAX || observed < target || word(0x48) != before) throw std::runtime_error("Guest fence did not directly observe paging retirement");
        reads += 10000;
        std::fprintf(stderr, "LINUX_BRIDGE pagingFenceRetired=true direct=true loads=10000 target=%llu observed=%llu operation=%s\n",
                     static_cast<unsigned long long>(target), static_cast<unsigned long long>(observed), operation);
    }
    bool waitRetired(std::uint32_t object, std::uint64_t target, const char* owner) {
        const auto found = mappings.find(object);
        if (found == mappings.end() || target == UINT64_MAX) throw std::runtime_error("Invalid mapped CPU wait fence");
        const auto before = word(0x48);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::uint64_t observed = 0;
        do {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            observed = *found->second.fence; ++reads;
            if (observed == UINT64_MAX) throw std::runtime_error("Mapped CPU wait observed device loss");
            if (observed >= target) break;
            usleep(100);
        } while (std::chrono::steady_clock::now() < deadline);
        if (word(0x48) != before) throw std::runtime_error("CPU wait used emulated fence reads");
        if (observed < target) return false;
        std::fprintf(stderr, "LINUX_BRIDGE cpuFenceWaitRetired=true direct=true target=%llu observed=%llu owner=%s\n",
                     static_cast<unsigned long long>(target), static_cast<unsigned long long>(observed), owner);
        return true;
    }
    void unmap(std::uint32_t queue) {
        const auto found = mappings.find(queue);
        if (found == mappings.end()) throw std::runtime_error("Guest paging queue absent");
        if (munmap(found->second.page, 4096)) throw std::runtime_error("Guest fence unmap failed");
        mappings.erase(found);
        std::fprintf(stderr, "LINUX_BRIDGE guestFenceUnmapped=true\n");
    }
    unsigned total() const { return created; }
    bool contains(std::uint32_t queue) const { return mappings.count(queue) != 0; }
    std::uint64_t directLoads() const { return reads; }
};
} // namespace guest_paging
