// SPDX-License-Identifier: MIT
// Diagnostic process-local views of owned native NVIDIA CPU allocation locks.
#pragma once
#include "driver_wire.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace guest_allocation {
class Memory {
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
    struct Mapping { void* data; std::uint32_t bytes, references; std::uint64_t offset, generation; };
    std::map<std::uint32_t, Mapping> mappings;
    unsigned created = 0, unmapped = 0;
    std::uint64_t loads = 0;
    static constexpr std::uint32_t Identity = 0x11fc1234, Magic = 0x31484c41;
    std::uint32_t word(unsigned offset) const {
        std::uint32_t value{};
        if (pread(config.fd, &value, sizeof value, offset) != sizeof value) throw std::runtime_error("Allocation PCI config unavailable");
        return value;
    }
    void discover() {
        const char* directory = "/sys/bus/pci/devices";
        auto devices = opendir(directory);
        if (!devices) throw std::runtime_error("Allocation PCI inventory unavailable");
        std::string found;
        unsigned count = 0;
        while (auto entry = readdir(devices)) {
            if (entry->d_name[0] == '.') continue;
            if (++count > 256) { closedir(devices); throw std::runtime_error("Allocation PCI inventory bound exceeded"); }
            const auto path = std::string(directory) + '/' + entry->d_name;
            File candidate; candidate.open(path + "/config", O_RDONLY | O_CLOEXEC);
            std::uint32_t identity{};
            if (candidate.fd >= 0 && pread(candidate.fd, &identity, sizeof identity, 0) == sizeof identity && identity == Identity) {
                if (!found.empty()) { closedir(devices); throw std::runtime_error("Multiple allocation hubs"); }
                found = path;
            }
        }
        closedir(devices);
        if (found.empty()) throw std::runtime_error("Allocation hub absent");
        config.open(found + "/config", O_RDONLY | O_CLOEXEC);
        if (word(0) != Identity || word(0x40) != Magic || word(0x44) != 16 || word(0x48) != driver_bridge::VendorCpuSlotBytes)
            throw std::runtime_error("Allocation hub layout mismatch");
        File enable; enable.open(found + "/enable", O_WRONLY | O_CLOEXEC);
        if (enable.fd < 0 || write(enable.fd, "1", 1) != 1) throw std::runtime_error("Cannot enable allocation hub");
        resource.open(found + "/resource0", O_RDWR | O_CLOEXEC | O_SYNC);
        if (resource.fd < 0) throw std::runtime_error("Allocation BAR unavailable");
    }
public:
    Memory() = default;
    Memory(const Memory&) = delete;
    ~Memory() { for (const auto& item : mappings) munmap(item.second.data, item.second.bytes); }
    bool contains(std::uint32_t allocation) const { return mappings.count(allocation) != 0; }
    void* retain(std::uint32_t allocation) {
        auto& entry = mappings.at(allocation);
        if (entry.references >= 64) throw std::runtime_error("Allocation lock reference bound exceeded");
        ++entry.references; return entry.data;
    }
    void* map(std::uint32_t allocation, std::uint64_t offset, driver_bridge::VendorCpuReply reply) {
        if (!allocation || !driver_bridge::validVendorCpuReply(offset, reply) || mappings.count(allocation) || mappings.size() >= 16)
            throw std::runtime_error("Invalid allocation CPU lease");
        for (const auto& item : mappings) if (item.second.offset == offset) throw std::runtime_error("Duplicate allocation CPU slot");
        if (resource.fd < 0) discover();
        const auto beforeRead = word(0x4c), beforeWrite = word(0x50);
        auto data = mmap(nullptr, reply.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, resource.fd, static_cast<off_t>(offset));
        if (data == MAP_FAILED) throw std::runtime_error("Allocation CPU view failed");
        const auto first = static_cast<volatile const std::uint64_t*>(data);
        for (unsigned n = 0; n < 10000; ++n) { const auto observed = *first; (void)observed; }
        try {
            if (word(0x4c) != beforeRead || word(0x50) != beforeWrite) throw std::runtime_error("Allocation CPU reads were emulated");
            mappings.emplace(allocation, Mapping{data, reply.bytes, 1, offset, reply.generation});
        } catch (...) { munmap(data, reply.bytes); throw; }
        loads += 10000; ++created;
        std::fprintf(stderr, "LINUX_BRIDGE allocationCpuMapped=true direct=true loads=10000 bytes=%u offset=%llu generation=%llu\n",
                     reply.bytes, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(reply.generation));
        return data;
    }
    // False means another process-local reference still owns the same view.
    bool release(std::uint32_t allocation, bool force = false) {
        const auto entry = mappings.find(allocation);
        if (entry == mappings.end()) return true;
        if (!force && entry->second.references > 1) { --entry->second.references; return false; }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto storeTest = std::getenv("WDDM_BRIDGE_CPU_STORE_TEST");
        if (storeTest && !std::strcmp(storeTest, "1")) {
            auto first = static_cast<volatile std::uint64_t*>(entry->second.data);
            auto last = reinterpret_cast<volatile std::uint64_t*>(static_cast<char*>(entry->second.data) + entry->second.bytes - 8);
            *first = driver_bridge::CpuStoreFirstMarker; *last = driver_bridge::CpuStoreLastMarker;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (*first != driver_bridge::CpuStoreFirstMarker || *last != driver_bridge::CpuStoreLastMarker)
                throw std::runtime_error("Allocation diagnostic stores did not read back");
            std::fprintf(stderr, "LINUX_BRIDGE allocationCpuStoreTest=true firstLastReadback=true bytes=%u\n", entry->second.bytes);
        }
        if (word(0x4c) || word(0x50)) throw std::runtime_error("Allocation CPU access used MMIO emulation");
        if (munmap(entry->second.data, entry->second.bytes)) throw std::runtime_error("Allocation CPU unmap failed");
        mappings.erase(entry); ++unmapped;
        std::fprintf(stderr, "LINUX_BRIDGE allocationCpuUnmapped=true direct=true mmioReads=0 mmioWrites=0\n");
        return true;
    }
    unsigned total() const { return created; }
    unsigned released() const { return unmapped; }
    std::uint64_t directLoads() const { return loads; }
};
} // namespace guest_allocation
