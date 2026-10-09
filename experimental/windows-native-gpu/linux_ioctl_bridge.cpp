// SPDX-License-Identifier: MIT
// Local runtime diagnostic, not a kernel driver or a usable graphics device.
// Interposes /dev/dxg only in the explicitly preloaded process. Unsupported
// calls fail here; they never fall through to the real WSL GPU device.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
#include "adapter_query_client.h"
#include "guest_paging_fence.h"
#include "guest_allocation_memory.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

extern char** environ;
namespace {
using namespace driver_bridge;
int rawClose(int fd) { return static_cast<int>(syscall(SYS_close, fd)); }
int rawOpen(const char* path, int flags) {
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, 0));
}
int ntErrno(std::int32_t status) {
    switch (static_cast<std::uint32_t>(status)) {
        case 0xc0000008: return EBADF;
        case 0xc000000d: return EINVAL;
        case 0xc0000017: return ENOMEM;
        case 0xc0000034: return ENOENT;
        case 0xc00000bb: return ENOSYS;
        case 0xc00000b5: return EAGAIN;
        default: return EIO;
    }
}
struct Error : std::runtime_error {
    int number;
    explicit Error(int error) : std::runtime_error("Linux driver bridge failed"), number(error) {}
};
void checkNt(std::int32_t status) { if (status < 0) throw Error(ntErrno(status)); }
int findNvidiaModule(struct dl_phdr_info* info, std::size_t bytes, void* data) {
    (void)bytes;
    const auto name = std::getenv("WDDM_BRIDGE_LINUX_UMD_NAME");
    const auto path = info->dlpi_name;
    if (name && path) {
        const auto slash = std::strrchr(path, '/');
        if (!std::strcmp(slash ? slash + 1 : path, name)) *static_cast<bool*>(data) = true;
    }
    return 0;
}
// Do not change the application's SIGPIPE disposition. Block the signal for
// this thread's write and consume only a signal that was not already pending.
struct PipeSignal {
    sigset_t previous{}, blocked{}; bool pending = false, changed = false;
    PipeSignal() {
        sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
        sigset_t before{}; sigpending(&before); pending = sigismember(&before, SIGPIPE) == 1;
        const auto result = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
        if (result) throw Error(result);
        changed = true;
    }
    ~PipeSignal() {
        if (!changed) return;
        if (!pending) { timespec immediate{}; sigtimedwait(&blocked, nullptr, &immediate); }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    }
};
class Transport {
    int input = -1, output = -1;
    pid_t child = -1;
    bool started = false, broken = false;
    void transfer(void* data, std::size_t size, bool reading,
                  std::chrono::steady_clock::time_point deadline) {
        auto bytes = static_cast<std::uint8_t*>(data);
        while (size) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) throw Error(ETIMEDOUT);
            pollfd ready{reading ? input : output, static_cast<short>(reading ? POLLIN : POLLOUT), 0};
            const auto result = poll(&ready, 1, static_cast<int>(remaining));
            if (result < 0 && errno == EINTR) continue;
            if (result == 0) throw Error(ETIMEDOUT);
            if (result < 0 || (ready.revents & (POLLERR | POLLNVAL))) throw Error(EIO);
            const auto count = reading ? read(input, bytes, size) : write(output, bytes, size);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) throw Error(EIO);
            bytes += count; size -= static_cast<std::size_t>(count);
        }
    }
    void disconnect() {
        if (input >= 0) rawClose(input);
        if (output >= 0 && output != input) rawClose(output);
        input = output = -1;
    }
public:
    ~Transport() {
        disconnect();
        if (child <= 0) return;
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            const auto result = waitpid(child, &status, WNOHANG);
            if (result == child) {
                std::fprintf(stderr, "LINUX_BRIDGE workerReaped=true exit=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                return;
            }
            if (result < 0 && errno != EINTR) {
                std::fprintf(stderr, "LINUX_BRIDGE workerReaped=false errno=%d\n", errno); return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                // WSL interop owns the Windows process. Never claim that
                // killing the Linux relay necessarily kills that process.
                std::fprintf(stderr, "LINUX_BRIDGE workerReaped=false timeout=true\n"); return;
            }
            timespec pause{0, 10000000}; nanosleep(&pause, nullptr);
        }
    }
    void start() {
        if (started) { if (broken) throw Error(EIO); return; }
        started = true;
        try {
            const auto worker = std::getenv("WDDM_BRIDGE_WINDOWS_WORKER");
            const auto port = std::getenv("WDDM_BRIDGE_PORT");
            if ((!worker || !*worker) == (!port || !*port)) throw Error(EINVAL);
            if (port && *port) {
                input = output = rawOpen(port, O_RDWR | O_CLOEXEC | O_NONBLOCK);
                if (input < 0) throw Error(errno);
                std::fprintf(stderr, "LINUX_BRIDGE transport=virtio-port\n"); return;
            }
            if (worker[0] != '/') throw Error(EINVAL);
            int toChild[2]{-1, -1}, fromChild[2]{-1, -1};
            if (pipe2(toChild, O_CLOEXEC | O_NONBLOCK)) throw Error(errno);
            if (pipe2(fromChild, O_CLOEXEC | O_NONBLOCK)) {
                const auto error = errno; rawClose(toChild[0]); rawClose(toChild[1]); throw Error(error);
            }
            // Only the parent ends are nonblocking. The native worker uses
            // synchronous binary stdio; the spawn actions close unused ends.
            int descriptorError = 0;
            for (const auto fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]})
                if (fd < 3) descriptorError = EBADF;
            for (const auto fd : {toChild[0], fromChild[1]}) {
                const auto flags = fcntl(fd, F_GETFL);
                if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)) descriptorError = errno;
            }
            if (descriptorError) {
                for (const auto fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]}) rawClose(fd);
                throw Error(descriptorError);
            }
            posix_spawn_file_actions_t actions{};
            int error = posix_spawn_file_actions_init(&actions);
            if (error) {
                for (const auto fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]}) rawClose(fd);
                throw Error(error);
            }
            if (!error) error = posix_spawn_file_actions_adddup2(&actions, toChild[0], STDIN_FILENO);
            if (!error) error = posix_spawn_file_actions_adddup2(&actions, fromChild[1], STDOUT_FILENO);
            for (const auto fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]})
                if (!error) error = posix_spawn_file_actions_addclose(&actions, fd);
            std::vector<char*> environment;
            for (auto item = environ; *item; ++item)
                if (std::strncmp(*item, "LD_PRELOAD=", 11) && std::strncmp(*item, "WDDM_BRIDGE_", 12)) environment.push_back(*item);
            environment.push_back(nullptr);
            char stdio[] = "--stdio", queries[] = "--driver-queries", contexts[] = "--driver-contexts";
            char* arguments[]{const_cast<char*>(worker), stdio, queries, contexts, nullptr};
            if (!error) error = posix_spawn(&child, worker, &actions, nullptr, arguments, environment.data());
            posix_spawn_file_actions_destroy(&actions);
            rawClose(toChild[0]); rawClose(fromChild[1]);
            if (error) { rawClose(toChild[1]); rawClose(fromChild[0]); throw Error(error); }
            output = toChild[1]; input = fromChild[0];
            std::fprintf(stderr, "LINUX_BRIDGE transport=owned-worker-stdio\n");
        } catch (...) { broken = true; disconnect(); throw; }
    }
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& packet) {
        start();
        if (packet.size() < sizeof(Header) || packet.size() > native_gpu::MaxPacket) throw Error(EINVAL);
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            PipeSignal signal;
            auto size = static_cast<std::uint32_t>(packet.size());
            transfer(&size, sizeof size, false, deadline);
            transfer(const_cast<std::uint8_t*>(packet.data()), packet.size(), false, deadline);
            transfer(&size, sizeof size, true, deadline);
            if (size < sizeof(Header) || size > native_gpu::MaxPacket) throw Error(EPROTO);
            std::vector<std::uint8_t> result(size); transfer(result.data(), size, true, deadline);
            return result;
        } catch (...) { broken = true; disconnect(); throw; }
    }
    void fail() { broken = true; disconnect(); }
};
struct Response { Header header; Reply result; std::vector<std::uint8_t> data; };
class Bridge {
    Transport transport;
    guest_paging::Fences pagingFences;
    guest_paging::Fences hwQueueFences;
    guest_paging::Fences syncFences;
    guest_allocation::Memory allocationMemory;
    std::set<std::uint32_t> cpuLocks;
    bool negotiated = false;
    native_gpu::Capabilities caps{};
    unsigned counts[256]{}, completedQueries = 0, privateQueries = 0, contexts = 0;
    std::map<int, std::pair<dev_t, ino_t>> descriptors;
    std::map<std::uint32_t, std::uint32_t> vendorOwners;
    struct VendorGpuRange { std::uint64_t address, bytes; };
    std::map<std::uint32_t, VendorGpuRange> vendorGpuRanges;
    std::map<std::uint32_t, std::uint32_t> vendorWireIds, deviceAdapters;
    std::set<std::uint32_t> ownedAdapters;
    struct GpuReservation { std::uint32_t adapter; std::uint64_t address, bytes; };
    std::map<std::uint32_t, GpuReservation> gpuReservations;
    std::uint64_t gpuReservedBytes = 0;
    GpuStateRanges gpuStates;
    std::map<std::uint32_t, std::uint32_t> pagingOwners;
    struct Context { std::uint32_t device, flags; };
    std::map<std::uint32_t, Context> contextOwners;
    std::map<std::uint32_t, std::uint32_t> hwQueueContexts;
    std::map<std::uint32_t, std::uint32_t> vendorResources; // typed resource ID -> its sole allocation alias
    struct BorrowedFence { std::uint32_t owner, device; bool hardware; };
    std::map<std::uint32_t, BorrowedFence> borrowedFences;
    struct Synchronization { std::uint32_t device, type, flags; std::uint64_t lastSignal = 0; };
    std::map<std::uint32_t, Synchronization> syncObjects;
    static constexpr std::uint32_t GuestLuidLow = 0x57475055;
    static constexpr std::int32_t GuestLuidHigh = 0;
    Response call(const std::vector<std::uint8_t>& requestPacket, std::uint32_t bytes = 0, bool preserveNativeFailure = false) {
        const auto packet = transport.exchange(requestPacket);
        Header wanted{}, header{}; std::memcpy(&wanted, requestPacket.data(), sizeof wanted);
        std::memcpy(&header, packet.data(), sizeof header);
        if (header.type != wanted.type || header.padding) { transport.fail(); throw Error(EPROTO); }
        if (header.status) {
            if (header.status > 0 || header.status < -4095 || packet.size() != sizeof(Header)) {
                transport.fail(); throw Error(EPROTO);
            }
            throw Error(-header.status);
        }
        if (packet.size() < sizeof(Header) + sizeof(Reply)) { transport.fail(); throw Error(EPROTO); }
        Reply reply{}; std::memcpy(&reply, packet.data() + sizeof(Header), sizeof reply);
        if (reply.reserved) { transport.fail(); throw Error(EPROTO); }
        if (!preserveNativeFailure) checkNt(reply.ntstatus);
        if (packet.size() != sizeof(Header) + sizeof(Reply) + bytes) { transport.fail(); throw Error(EPROTO); }
        return {header, reply, {packet.begin() + sizeof(Header) + sizeof(Reply), packet.end()}};
    }
    void negotiate() {
        if (negotiated) { transport.start(); return; }
        const auto packet = transport.exchange(request(Op::Hello, 0, Version));
        if (packet.size() != sizeof(Header) + sizeof caps) { transport.fail(); throw Error(EPROTO); }
        Header header{}; std::memcpy(&header, packet.data(), sizeof header);
        std::memcpy(&caps, packet.data() + sizeof header, sizeof caps);
        if (header.type != static_cast<unsigned>(Op::Hello) || header.handle || header.status || header.padding ||
            caps.version != Version || caps.vendor != 0x10de || (caps.flags & (QueryCapability | ContextCapability)) != (QueryCapability | ContextCapability)) {
            transport.fail(); throw Error(EPROTO);
        }
        negotiated = true;
        std::fprintf(stderr, "LINUX_BRIDGE vendor=%u device=%u realDxgForwarding=false\n", caps.vendor, caps.device);
    }
    std::uint32_t create(Op op, std::uint32_t parent = 0) {
        const auto out = call(request(op, parent));
        if (!out.header.handle) { transport.fail(); throw Error(EPROTO); }
        if (op == Op::OpenAdapter && !ownedAdapters.emplace(out.header.handle).second) { transport.fail(); throw Error(EPROTO); }
        return out.header.handle;
    }
    void destroy(Op op, std::uint32_t handle) {
        const auto out = call(request(op, handle));
        if (out.header.handle != handle) { transport.fail(); throw Error(EPROTO); }
    }
    static void adaptUmdName(std::vector<std::uint8_t>& data) {
        // DXCore does the UTF-16/WCHAR conversion and maps DriverStore paths
        // to /usr/lib/wsl/drivers. Change only the module basename selected
        // by the user, never vendor-private adapter initialization data.
        const auto name = std::getenv("WDDM_BRIDGE_LINUX_UMD_NAME");
        if (!name || !*name || std::strlen(name) > 128) throw Error(EINVAL);
        for (auto p = name; *p; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) throw Error(EINVAL);
        if (data.size() != 524) throw Error(EPROTO);
        std::uint16_t path[260]{}; std::memcpy(path, data.data() + 4, sizeof path);
        const auto end = std::find(std::begin(path), std::end(path), std::uint16_t{0});
        if (end == std::end(path)) throw Error(EPROTO);
        std::size_t prefix = 0;
        for (auto p = std::begin(path); p != end; ++p) if (*p == '\\' || *p == '/') prefix = static_cast<std::size_t>(p - path) + 1;
        if (!prefix || prefix + std::strlen(name) >= 260) throw Error(EOVERFLOW);
        std::fill(path + prefix, std::end(path), std::uint16_t{0});
        for (std::size_t n = 0; name[n]; ++n) path[prefix + n] = static_cast<unsigned char>(name[n]);
        std::memcpy(data.data() + 4, path, sizeof path);
    }
    template<class T> static T& args(unsigned long requestNumber, void* pointer) {
        if (_IOC_SIZE(requestNumber) != sizeof(T)) throw Error(EINVAL);
        return *static_cast<T*>(pointer);
    }
    std::uint32_t wireAllocation(std::uint32_t alias) const {
        const auto found = vendorWireIds.find(alias);
        if (found == vendorWireIds.end()) throw Error(EBADF);
        return found->second;
    }
    void unlockCpu(std::uint32_t allocation, std::uint32_t device, bool force = false) {
        if (!cpuLocks.count(allocation)) throw Error(EBADF);
        try { if (!allocationMemory.release(allocation, force)) return; }
        catch (...) { transport.fail(); throw; }
        const auto wire = wireAllocation(allocation);
        const auto result = call(request(Op::UnlockVendorAllocation, wire, VendorCpuDesc{device, 0}), 0, true);
        if (result.header.handle != wire || result.result.value || result.result.ntstatus > 0) {
            transport.fail(); throw Error(EPROTO);
        }
        checkNt(result.result.ntstatus); cpuLocks.erase(allocation);
        std::fprintf(stderr, "LINUX_BRIDGE nativeVendorCpuUnlocked=true\n");
    }
public:
    std::mutex guard;
    ~Bridge() {
        std::fprintf(stderr, "LINUX_BRIDGE summary completedQueries=%u privateQueries=%u nativeContexts=%u realDxgForwarding=false\n",
                     completedQueries, privateQueries, contexts);
        std::fprintf(stderr, "LINUX_BRIDGE pagingSummary queues=%u directLoads=%llu\n", pagingFences.total(),
                     static_cast<unsigned long long>(pagingFences.directLoads()));
        std::fprintf(stderr, "LINUX_BRIDGE allocationCpuSummary mapped=%u unmapped=%u directLoads=%llu\n",
                     allocationMemory.total(), allocationMemory.released(), static_cast<unsigned long long>(allocationMemory.directLoads()));
        for (unsigned n = 0; n < 256; ++n)
            if (counts[n]) std::fprintf(stderr, "LINUX_BRIDGE ioctlSummary nr=%u count=%u\n", n, counts[n]);
        for (const auto& entry : descriptors) rawClose(entry.first);
    }
    int openDevice(int flags) {
        std::lock_guard<std::mutex> lock(guard);
        if ((flags & O_CREAT) || (flags & O_TRUNC) || (flags & O_TMPFILE) == O_TMPFILE) throw Error(EINVAL);
        negotiate();
        const auto fd = static_cast<int>(syscall(SYS_memfd_create, "wddm-bridge-diagnostic", (flags & O_CLOEXEC) ? MFD_CLOEXEC : 0));
        if (fd < 0) throw Error(errno);
        struct stat information{};
        if (fstat(fd, &information)) { const auto error = errno; rawClose(fd); throw Error(error); }
        descriptors.emplace(fd, std::make_pair(information.st_dev, information.st_ino));
        return fd;
    }
    bool owns(int fd) {
        const auto found = descriptors.find(fd);
        if (found == descriptors.end()) return false;
        struct stat information{};
        if (fstat(fd, &information) || found->second != std::make_pair(information.st_dev, information.st_ino)) {
            descriptors.erase(found); return false;
        }
        return true;
    }
    void closed(int fd) { descriptors.erase(fd); }
    int dispatch(unsigned long requestNumber, void* pointer) {
        if (_IOC_TYPE(requestNumber) != 'G' || !pointer || _IOC_DIR(requestNumber) != (_IOC_READ | _IOC_WRITE)) throw Error(EINVAL);
        const auto nr = static_cast<unsigned>(_IOC_NR(requestNumber)); ++counts[nr];
        switch (nr) {
            case 62: {
                auto& a = args<D3DKMT_ENUMADAPTERS3>(requestNumber, pointer);
                if (a.Filter.Value & ~7ull) throw Error(EINVAL);
                if (!a.pAdapters) { a.NumAdapters = 1; return 0; }
                if (!a.NumAdapters) { a.NumAdapters = 1; throw Error(EOVERFLOW); }
                D3DKMT_ADAPTERINFO info{}; info.hAdapter = create(Op::OpenAdapter);
                info.AdapterLuid.LowPart = GuestLuidLow; info.AdapterLuid.HighPart = GuestLuidHigh;
                // Physical Windows outputs are not guest outputs. QEMU
                // scanout is a separate, currently unimplemented interface.
                info.NumOfSources = 0; info.bPrecisePresentRegionsPreferred = 0;
                a.pAdapters[0] = info; a.NumAdapters = 1; return 0;
            }
            case 1: {
                auto& a = args<D3DKMT_OPENADAPTERFROMLUID>(requestNumber, pointer);
                if (a.AdapterLuid.LowPart != GuestLuidLow || a.AdapterLuid.HighPart != GuestLuidHigh) throw Error(ENODEV);
                a.hAdapter = create(Op::OpenAdapter); return 0;
            }
            case 21: {
                auto& a = args<D3DKMT_CLOSEADAPTER>(requestNumber, pointer);
                if (!ownedAdapters.count(a.hAdapter)) throw Error(EBADF);
                destroy(Op::CloseAdapter, a.hAdapter); ownedAdapters.erase(a.hAdapter); return 0;
            }
            case 9: {
                auto& a = args<D3DKMT_QUERYADAPTERINFO>(requestNumber, pointer);
                const auto type = static_cast<unsigned>(a.Type);
                std::fprintf(stderr, "LINUX_BRIDGE query type=%u bytes=%u\n", type, a.PrivateDriverDataSize);
                if (!type) {
                    bool loaded = false; dl_iterate_phdr(findNvidiaModule, &loaded);
                    std::fprintf(stderr, "LINUX_BRIDGE nvidiaUmdPresentDuringPrivateQuery=%s\n", loaded ? "true" : "false");
                }
                if (!a.pPrivateDriverData || !validQuery({type, a.PrivateDriverDataSize, 0, 0})) throw Error(ENOSYS);
                const auto begin = static_cast<const std::uint8_t*>(a.pPrivateDriverData);
                const auto result = adapterQuery([&](const auto& packet) { return transport.exchange(packet); },
                                                 a.hAdapter, type, std::vector<std::uint8_t>(begin, begin + a.PrivateDriverDataSize));
                auto data = result.data;
                // Preserve native in/out bytes even when the driver fails.
                if (result.ntstatus >= 0) {
                    if (type == 1) adaptUmdName(data);
                    if (type == 15) {
                        std::uint32_t flags{}; std::memcpy(&flags, data.data(), sizeof flags);
                        flags |= 1u << 7; // Paravirtualized.
                        flags &= ~((1u << 1) | (1u << 3) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 11));
                        std::memcpy(data.data(), &flags, sizeof flags);
                    }
                }
                std::memcpy(a.pPrivateDriverData, data.data(), data.size()); checkNt(result.ntstatus);
                ++completedQueries; if (type == 0) ++privateQueries;
                std::fprintf(stderr, "LINUX_BRIDGE queryCompleted type=%u bytes=%zu\n", type, data.size()); return 0;
            }
            case 2: {
                auto& a = args<D3DKMT_CREATEDEVICE>(requestNumber, pointer);
                unsigned flags{}; static_assert(sizeof a.Flags == sizeof flags); std::memcpy(&flags, &a.Flags, sizeof flags);
                if (flags != 2) throw Error(ENOSYS);
                a.hDevice = create(Op::CreateDevice, a.hAdapter);
                if (!deviceAdapters.emplace(a.hDevice, a.hAdapter).second) { transport.fail(); throw Error(EPROTO); }
                a.pCommandBuffer = nullptr; a.CommandBufferSize = 0; a.pAllocationList = nullptr;
                a.AllocationListSize = 0; a.pPatchLocationList = nullptr; a.PatchLocationListSize = 0;
                std::fprintf(stderr, "LINUX_BRIDGE deviceCreated=true\n"); return 0;
            }
            case 25: { auto& a = args<D3DKMT_DESTROYDEVICE>(requestNumber, pointer); destroy(Op::DestroyDevice, a.hDevice); deviceAdapters.erase(a.hDevice); return 0; }
            case 7: {
                auto& a = args<D3DKMT_CREATEPAGINGQUEUE>(requestNumber, pointer);
                if (!(caps.flags & GuestPagingCapability) || a.Priority != D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL || a.PhysicalAdapterIndex) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=7 bytes=%zu\n", sizeof a);
                    throw Error(ENOSYS);
                }
                const auto result = call(request(Op::CreateGuestPagingQueue, a.hDevice), sizeof(PagingReply));
                PagingReply reply{}; std::memcpy(&reply, result.data.data(), sizeof reply);
                if (!result.header.handle || !reply.sync || reply.sync == result.header.handle || reply.reserved || result.result.value ||
                    reply.offset % 8 || reply.offset >= FenceApertureBytes) { transport.fail(); throw Error(EPROTO); }
                try { a.FenceValueCPUVirtualAddress = pagingFences.map(result.header.handle, reply.offset); }
                catch (...) {
                    // The failed mapping never escapes to the runtime. The
                    // native owner acknowledges unmap before freeing its page.
                    destroy(Op::DestroyPagingQueue, result.header.handle);
                    throw;
                }
                a.hPagingQueue = result.header.handle; a.hSyncObject = reply.sync;
                pagingOwners.emplace(a.hPagingQueue, a.hDevice);
                if (!borrowedFences.emplace(reply.sync, BorrowedFence{a.hPagingQueue, a.hDevice, false}).second) { transport.fail(); throw Error(EPROTO); }
                return 0;
            }
            case 28: {
                auto& a = args<D3DDDI_DESTROYPAGINGQUEUE>(requestNumber, pointer);
                pagingFences.unmap(a.hPagingQueue);
                destroy(Op::DestroyPagingQueue, a.hPagingQueue); pagingOwners.erase(a.hPagingQueue);
                for (auto fence = borrowedFences.begin(); fence != borrowedFences.end();) {
                    if (!fence->second.hardware && fence->second.owner == a.hPagingQueue) fence = borrowedFences.erase(fence); else ++fence;
                }
                return 0;
            }
            case 4: {
                auto& a = args<D3DKMT_CREATECONTEXTVIRTUAL>(requestNumber, pointer);
                const ContextDesc desc{a.NodeOrdinal, a.EngineAffinity, a.Flags.Value, static_cast<unsigned>(a.ClientHint), a.PrivateDriverDataSize, 0};
                if (!validContext(desc) || (desc.privateBytes && !a.pPrivateDriverData)) throw Error(ENOSYS);
                auto packet = request(Op::CreateContext, a.hDevice, desc);
                if (desc.privateBytes) {
                    const auto begin = static_cast<const std::uint8_t*>(a.pPrivateDriverData); packet.insert(packet.end(), begin, begin + desc.privateBytes);
                }
                const auto result = call(packet, desc.privateBytes);
                if (!result.header.handle) { transport.fail(); throw Error(EPROTO); }
                a.hContext = result.header.handle;
                if (!contextOwners.emplace(a.hContext, Context{a.hDevice, desc.flags}).second) { transport.fail(); throw Error(EPROTO); }
                if (desc.privateBytes) std::memcpy(a.pPrivateDriverData, result.data.data(), result.data.size());
                ++contexts; std::fprintf(stderr, "LINUX_BRIDGE nativeContextCreated=true bytes=%u flags=%u\n", desc.privateBytes, desc.flags); return 0;
            }
            case 5: {
                auto& a = args<D3DKMT_DESTROYCONTEXT>(requestNumber, pointer);
                for (const auto& queue : hwQueueContexts) if (queue.second == a.hContext) throw Error(EBUSY);
                destroy(Op::DestroyContext, a.hContext); contextOwners.erase(a.hContext); return 0;
            }
            case 6: {
                auto& a = args<D3DKMT_CREATEALLOCATION>(requestNumber, pointer);
                unsigned flags{}; static_assert(sizeof a.Flags == sizeof flags);
                std::memcpy(&flags, &a.Flags, sizeof flags);
                std::fprintf(stderr, "LINUX_BRIDGE allocationInput flags=%u count=%u runtimeBytes=%u privateBytes=%u resourceReuse=%u\n",
                             flags, a.NumAllocations, a.PrivateRuntimeDataSize, a.PrivateDriverDataSize, a.hResource ? 1u : 0u);
                if (a.NumAllocations <= 16 && a.pAllocationInfo2)
                    for (unsigned n = 0; n < a.NumAllocations; ++n) {
                        const auto& item = a.pAllocationInfo2[n];
                        std::fprintf(stderr, "LINUX_BRIDGE allocationItem index=%u flags=%u privateBytes=%u hasSystemMemory=%u priority=%u source=%u\n",
                                     n, item.Flags.Value, item.PrivateDriverDataSize, item.pSystemMem ? 1u : 0u, item.Priority, item.VidPnSourceId);
                    }
                const bool withResource = flags == 1;
                if (!(caps.flags & VendorAllocationCapability) || (flags != 0 && !withResource) ||
                    (withResource && !(caps.flags & VendorResourceCapability)) || a.hResource || a.PrivateRuntimeDataSize ||
                    a.PrivateDriverDataSize || a.NumAllocations != 1 || !a.pAllocationInfo2) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=6 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                auto& item = a.pAllocationInfo2[0];
                const VendorAllocationDesc desc{item.Flags.Value, item.Priority, item.VidPnSourceId, item.PrivateDriverDataSize, 0, 0};
                if (!validVendorAllocation(desc) || item.pSystemMem || !item.pPrivateDriverData ||
                    std::any_of(std::begin(item.Reserved), std::end(item.Reserved), [](auto value) { return value != 0; })) throw Error(EINVAL);
                if (vendorOwners.size() >= MaxVendorAllocations) throw Error(EMFILE);
                auto packet = request(withResource ? Op::CreateVendorResourceAllocation : Op::CreateVendorAllocation, a.hDevice, desc);
                const auto begin = static_cast<const std::uint8_t*>(item.pPrivateDriverData);
                packet.insert(packet.end(), begin, begin + desc.privateBytes);
                const auto result = call(packet, desc.privateBytes + (withResource ? sizeof(VendorResourceReply) : 0), true);
                VendorResourceReply resource{};
                if (withResource) std::memcpy(&resource, result.data.data(), sizeof resource);
                const bool reusedWireId = std::any_of(vendorWireIds.begin(), vendorWireIds.end(), [&](const auto& owned) {
                    return owned.second == result.header.handle;
                });
                if (result.result.ntstatus > 0 || resource.reserved ||
                    (result.result.ntstatus >= 0 && (!result.header.handle || result.result.value % 4096 || reusedWireId ||
                        (withResource && (!resource.resource || resource.resource == result.header.handle || vendorResources.count(resource.resource))))) ||
                    (result.result.ntstatus < 0 && (result.header.handle || result.result.value || resource.resource))) { transport.fail(); throw Error(EPROTO); }
                std::memcpy(item.pPrivateDriverData, result.data.data() + (withResource ? sizeof resource : 0), desc.privateBytes); checkNt(result.result.ntstatus);
                std::uint32_t alias = result.header.handle;
                if (caps.flags & VendorTranslationCapability) {
                    const auto parent = deviceAdapters.find(a.hDevice);
                    if (parent == deviceAdapters.end()) { transport.fail(); throw Error(EPROTO); }
                    const auto translated = call(request(Op::TranslateVendorAllocation, result.header.handle,
                                                        VendorTranslationDesc{a.hDevice, parent->second, 0, 0}), 0, true);
                    if (translated.header.handle != result.header.handle || translated.result.ntstatus > 0 ||
                        (translated.result.ntstatus >= 0 && (!translated.result.value || translated.result.value > UINT32_MAX)) ||
                        (translated.result.ntstatus < 0 && translated.result.value)) { transport.fail(); throw Error(EPROTO); }
                    if (translated.result.ntstatus < 0) {
                        auto cleanup = request(Op::DestroyVendorAllocations, a.hDevice, DestroyVendorDesc{1, 0});
                        const auto id = reinterpret_cast<const std::uint8_t*>(&result.header.handle);
                        cleanup.insert(cleanup.end(), id, id + 4);
                        const auto released = call(cleanup);
                        if (released.header.handle != a.hDevice || released.result.value) { transport.fail(); throw Error(EPROTO); }
                        checkNt(translated.result.ntstatus);
                    }
                    alias = static_cast<std::uint32_t>(translated.result.value);
                    std::fprintf(stderr, "LINUX_BRIDGE allocationDriverAliasCreated=true typedWireIdentityRetained=true\n");
                }
                if (!vendorOwners.emplace(alias, a.hDevice).second || !vendorWireIds.emplace(alias, result.header.handle).second) {
                    transport.fail(); throw Error(EPROTO);
                }
                if (withResource && !vendorResources.emplace(resource.resource, alias).second) { transport.fail(); throw Error(EPROTO); }
                item.hAllocation = alias; item.GpuVirtualAddress = result.result.value;
                a.hResource = resource.resource; a.hGlobalShare = 0;
                if (withResource) std::fprintf(stderr, "LINUX_BRIDGE nativeVendorResourceCreated=true allocationCount=1 shared=false systemMemory=false\n");
                if (desc.source == UninitializedDisplaySource)
                    std::fprintf(stderr, "LINUX_BRIDGE standaloneSourceUninitializedAccepted=true primary=false privateDataPreserved=true\n");
                std::fprintf(stderr, "LINUX_BRIDGE nativeVendorAllocationCreated=true privateBytes=%u\n", desc.privateBytes); return 0;
            }
            case 19: {
                auto& a = args<D3DKMT_DESTROYALLOCATION2>(requestNumber, pointer);
                if (!(caps.flags & VendorAllocationCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=19 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                if (a.Flags.Value & ~3u) throw Error(EINVAL);
                unsigned ownedQueues = 0;
                for (const auto& queue : hwQueueContexts)
                    if (contextOwners.at(queue.second).device == a.hDevice) ++ownedQueues;
                if (ownedQueues && !(caps.flags & VendorRetirementCapability)) throw Error(EBUSY);
                if (a.hResource) {
                    if (!(caps.flags & VendorResourceCapability)) throw Error(ENOSYS);
                    if (a.AllocationCount || a.phAllocationList) throw Error(EINVAL);
                    const auto resource = vendorResources.find(a.hResource);
                    if (resource == vendorResources.end() || vendorOwners.at(resource->second) != a.hDevice) throw Error(EBADF);
                    const auto alias = resource->second;
                    if (cpuLocks.count(alias)) unlockCpu(alias, a.hDevice, true);
                    const auto result = call(request(Op::DestroyVendorResource, a.hResource, a.hDevice), 0, true);
                    if (result.header.handle != a.hResource || result.result.value || result.result.ntstatus > 0) { transport.fail(); throw Error(EPROTO); }
                    checkNt(result.result.ntstatus);
                    vendorResources.erase(resource); vendorOwners.erase(alias); vendorWireIds.erase(alias); vendorGpuRanges.erase(alias);
                    std::fprintf(stderr, "LINUX_BRIDGE nativeVendorResourceDestroyed=true allocationCount=1 hardwareQueuesAlive=%u\n", ownedQueues);
                    std::fprintf(stderr, "LINUX_BRIDGE nativeVendorAllocationsDestroyed=true count=1 hardwareQueuesAlive=%u\n", ownedQueues); return 0;
                }
                if (!a.AllocationCount || a.AllocationCount > MaxVendorAllocations || !a.phAllocationList) throw Error(EINVAL);
                // Accept the public destruction hints, but always request
                // synchronous native destruction rather than trusting the
                // guest's AssumeNotInUse hint to shorten object lifetime.
                for (unsigned n = 0; n < a.AllocationCount; ++n) {
                    const auto found = vendorOwners.find(a.phAllocationList[n]);
                    if (found == vendorOwners.end() || found->second != a.hDevice) throw Error(EBADF);
                    if (a.AllocationCount != 1 && std::any_of(vendorResources.begin(), vendorResources.end(), [&](const auto& resource) {
                            return resource.second == a.phAllocationList[n]; })) throw Error(EINVAL);
                    for (unsigned previous = 0; previous < n; ++previous)
                        if (a.phAllocationList[previous] == a.phAllocationList[n]) throw Error(EINVAL);
                }
                auto packet = request(Op::DestroyVendorAllocations, a.hDevice, DestroyVendorDesc{a.AllocationCount, 0});
                for (unsigned n = 0; n < a.AllocationCount; ++n)
                    if (cpuLocks.count(a.phAllocationList[n])) unlockCpu(a.phAllocationList[n], a.hDevice, true);
                std::vector<std::uint32_t> wireIds;
                for (unsigned n = 0; n < a.AllocationCount; ++n) wireIds.push_back(wireAllocation(a.phAllocationList[n]));
                const auto begin = reinterpret_cast<const std::uint8_t*>(wireIds.data());
                packet.insert(packet.end(), begin, begin + a.AllocationCount * sizeof(wireIds[0]));
                const auto result = call(packet, 0, true);
                if (result.header.handle != a.hDevice || result.result.value || result.result.ntstatus > 0) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                for (unsigned n = 0; n < a.AllocationCount; ++n) {
                    for (auto resource = vendorResources.begin(); resource != vendorResources.end();) {
                        if (resource->second == a.phAllocationList[n]) resource = vendorResources.erase(resource); else ++resource;
                    }
                    vendorOwners.erase(a.phAllocationList[n]); vendorWireIds.erase(a.phAllocationList[n]); vendorGpuRanges.erase(a.phAllocationList[n]);
                }
                std::fprintf(stderr, "LINUX_BRIDGE nativeVendorAllocationsDestroyed=true count=%u hardwareQueuesAlive=%u\n", a.AllocationCount, ownedQueues); return 0;
            }
            case 11: {
                auto& a = args<D3DDDI_MAKERESIDENT>(requestNumber, pointer);
                std::fprintf(stderr, "LINUX_BRIDGE residentInput count=%u flags=%u hasAllocations=%u hasPriorities=%u\n",
                             a.NumAllocations, a.Flags.Value, a.AllocationList ? 1u : 0u, a.PriorityList ? 1u : 0u);
                if (!(caps.flags & VendorResidencyCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=11 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const ResidentDesc desc{a.NumAllocations, a.Flags.Value, a.PriorityList ? 1u : 0u, 0};
                if (!validResident(desc) || !a.AllocationList) throw Error(EINVAL);
                const auto queue = pagingOwners.find(a.hPagingQueue);
                if (queue == pagingOwners.end()) throw Error(EBADF);
                for (unsigned n = 0; n < desc.count; ++n) {
                    const auto allocation = vendorOwners.find(a.AllocationList[n]);
                    if (allocation == vendorOwners.end() || allocation->second != queue->second) throw Error(EBADF);
                    for (unsigned previous = 0; previous < n; ++previous)
                        if (a.AllocationList[previous] == a.AllocationList[n]) throw Error(EINVAL);
                }
                auto packet = request(Op::MakeVendorResident, a.hPagingQueue, desc);
                std::vector<std::uint32_t> wireIds;
                for (unsigned n = 0; n < desc.count; ++n) wireIds.push_back(wireAllocation(a.AllocationList[n]));
                const auto ids = reinterpret_cast<const std::uint8_t*>(wireIds.data());
                packet.insert(packet.end(), ids, ids + desc.count * sizeof(wireIds[0]));
                if (desc.priorities) {
                    const auto priorities = reinterpret_cast<const std::uint8_t*>(a.PriorityList);
                    packet.insert(packet.end(), priorities, priorities + desc.count * sizeof(a.PriorityList[0]));
                }
                const auto result = call(packet, sizeof(ResidentReply), true);
                ResidentReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.header.handle != a.hPagingQueue || output.reserved || output.count > desc.count ||
                    (result.result.ntstatus >= 0 && result.result.ntstatus != 0 && result.result.ntstatus != 0x103)) {
                    transport.fail(); throw Error(EPROTO);
                }
                // Windows returns in/out count, fence and bytes-to-trim on
                // budget failure too. Copy them before translating NTSTATUS.
                a.NumAllocations = output.count; a.PagingFenceValue = result.result.value; a.NumBytesToTrim = output.bytesToTrim;
                checkNt(result.result.ntstatus);
                pagingFences.verifyRetired(a.hPagingQueue, a.PagingFenceValue, "residency");
                std::fprintf(stderr, "LINUX_BRIDGE nativeVendorResident=true count=%u status=%d fence=%llu bytesToTrim=%llu\n",
                             a.NumAllocations, result.result.ntstatus, static_cast<unsigned long long>(a.PagingFenceValue),
                             static_cast<unsigned long long>(a.NumBytesToTrim));
                return result.result.ntstatus;
            }
            case 8: {
                auto& a = args<D3DDDI_RESERVEGPUVIRTUALADDRESS>(requestNumber, pointer);
                std::fprintf(stderr, "LINUX_BRIDGE reserveGpuVaInput ownedAdapter=%u ownedPagingQueue=%u base=%llu minimum=%llu maximum=%llu bytes=%llu reserved0=%u reserved1=%llu reserved2=%llu\n",
                    ownedAdapters.count(a.hAdapter) ? 1u : 0u,
                    pagingOwners.count(a.hPagingQueue) ? 1u : 0u,
                    static_cast<unsigned long long>(a.BaseAddress), static_cast<unsigned long long>(a.MinimumAddress),
                    static_cast<unsigned long long>(a.MaximumAddress), static_cast<unsigned long long>(a.Size), a.Reserved0,
                    static_cast<unsigned long long>(a.Reserved1), static_cast<unsigned long long>(a.Reserved2));
                if (!(caps.flags & GpuReservationCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=8 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const GpuReservationDesc desc{a.BaseAddress, a.MinimumAddress, a.MaximumAddress, a.Size};
                if (a.Reserved0 || a.Reserved1 || a.Reserved2 || !validGpuReservation(desc)) throw Error(EINVAL);
                if (!ownedAdapters.count(a.hAdapter)) throw Error(EBADF);
                if (gpuReservations.size() >= MaxGpuReservations || desc.bytes > MaxGpuReservedBytes - gpuReservedBytes) throw Error(EMFILE);
                if (desc.base) for (const auto& range : gpuReservations)
                    if (gpuRangesOverlap(desc.base, desc.bytes, range.second.address, range.second.bytes)) throw Error(EBUSY);
                const auto result = call(request(Op::ReserveGpuAddress, a.hAdapter, desc), 0, true);
                if ((result.result.ntstatus >= 0 && (result.result.ntstatus != 0 || !result.header.handle ||
                        gpuReservations.count(result.header.handle) || !validGpuReservationOutput(desc, result.result.value))) ||
                    (result.result.ntstatus < 0 && (result.header.handle || result.result.value))) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                for (const auto& range : gpuReservations)
                    if (gpuRangesOverlap(result.result.value, desc.bytes, range.second.address, range.second.bytes)) {
                        transport.fail(); throw Error(EPROTO);
                    }
                try { gpuReservations.emplace(result.header.handle, GpuReservation{a.hAdapter, result.result.value, desc.bytes}); }
                catch (...) { transport.fail(); throw; } // Host retains ownership until its VM has exited.
                gpuReservedBytes += desc.bytes; a.VirtualAddress = result.result.value;
                std::fprintf(stderr, "LINUX_BRIDGE nativeGpuReserved=true bytes=%llu\n", static_cast<unsigned long long>(desc.bytes));
                const auto eofTest = std::getenv("WDDM_BRIDGE_RESERVATION_EOF_TEST");
                if (eofTest && !std::strcmp(eofTest, "1")) {
                    std::fprintf(stderr, "LINUX_BRIDGE reservationEofTest=true exitingWithGpuReservationOwned=true\n"); _exit(1);
                }
                return 0;
            }
            case 32: {
                const auto& a = args<D3DKMT_FREEGPUVIRTUALADDRESS>(requestNumber, pointer);
                if (!(caps.flags & GpuReservationCapability)) throw Error(ENOSYS);
                if (!a.BaseAddress || a.BaseAddress % 4096 || !a.Size || a.Size % 4096 ||
                    a.Size > MaxGpuAddress || a.BaseAddress > MaxGpuAddress - a.Size) throw Error(EINVAL);
                if (!ownedAdapters.count(a.hAdapter)) throw Error(EBADF);
                const auto range = std::find_if(gpuReservations.begin(), gpuReservations.end(), [&a](const auto& item) {
                    return item.second.adapter == a.hAdapter && item.second.address == a.BaseAddress && item.second.bytes == a.Size;
                });
                if (range == gpuReservations.end()) throw Error(EBADF);
                for (const auto& mapped : vendorGpuRanges)
                    if (gpuRangesOverlap(range->second.address, range->second.bytes, mapped.second.address, mapped.second.bytes) &&
                        !gpuRangeContains(range->second.address, range->second.bytes, mapped.second.address, mapped.second.bytes)) throw Error(EBUSY);
                const auto result = call(request(Op::FreeGpuReservation, range->first, FreeGpuReservationDesc{a.hAdapter, 0}), 0, true);
                if (result.header.handle != range->first || result.result.ntstatus > 0 || result.result.value) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                for (auto mapped = vendorGpuRanges.begin(); mapped != vendorGpuRanges.end();)
                    if (gpuRangesOverlap(range->second.address, range->second.bytes, mapped->second.address, mapped->second.bytes)) mapped = vendorGpuRanges.erase(mapped);
                    else ++mapped;
                gpuStates.release(range->first); gpuReservedBytes -= range->second.bytes; gpuReservations.erase(range);
                std::fprintf(stderr, "LINUX_BRIDGE nativeGpuReservationFreed=true bytes=%llu\n", static_cast<unsigned long long>(a.Size));
                return 0;
            }
            case 12: {
                auto& a = args<D3DDDI_MAPGPUVIRTUALADDRESS>(requestNumber, pointer);
                std::fprintf(stderr, "LINUX_BRIDGE gpuVaAllocationInput allocationIsNull=%u ownedAllocation=%u\n",
                    a.hAllocation ? 0u : 1u, vendorOwners.count(a.hAllocation) ? 1u : 0u);
                std::fprintf(stderr, "LINUX_BRIDGE gpuVaInput base=%llu minimum=%llu maximum=%llu offsetPages=%llu sizePages=%llu protection=%llu driverProtection=%llu reserved0=%u reserved1=%llu\n",
                    static_cast<unsigned long long>(a.BaseAddress), static_cast<unsigned long long>(a.MinimumAddress),
                    static_cast<unsigned long long>(a.MaximumAddress), static_cast<unsigned long long>(a.OffsetInPages),
                    static_cast<unsigned long long>(a.SizeInPages), static_cast<unsigned long long>(a.Protection.Value),
                    static_cast<unsigned long long>(a.DriverProtection), a.Reserved0, static_cast<unsigned long long>(a.Reserved1));
                if (!(caps.flags & VendorGpuVaCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=12 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const GpuVaDesc desc{a.hPagingQueue, a.Reserved0, a.BaseAddress, a.MinimumAddress, a.MaximumAddress,
                                     a.OffsetInPages, a.SizeInPages, a.Protection.Value, a.DriverProtection};
                if (a.Protection.Value & 12) {
                    if (!(caps.flags & GpuStateCapability)) {
                        std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=12 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                    }
                    if (a.hAllocation || a.Reserved1 || !validGpuState(desc)) throw Error(EINVAL);
                    const auto queue = pagingOwners.find(a.hPagingQueue);
                    if (queue == pagingOwners.end() || !deviceAdapters.count(queue->second)) throw Error(EBADF);
                    const auto adapter = deviceAdapters.at(queue->second);
                    const auto range = std::find_if(gpuReservations.begin(), gpuReservations.end(), [&](const auto& item) {
                        return item.second.adapter == adapter && gpuRangeContains(item.second.address, item.second.bytes, desc.base, desc.sizePages * 4096);
                    });
                    if (range == gpuReservations.end()) throw Error(EBADF);
                    for (const auto& mapped : vendorGpuRanges)
                        if (gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.second.address, mapped.second.bytes) &&
                            (vendorOwners.at(mapped.first) != queue->second || !gpuRangeContains(desc.base, desc.sizePages * 4096, mapped.second.address, mapped.second.bytes))) throw Error(EBUSY);
                    GpuStateRanges::Plan plan;
                    if (!gpuStates.prepare(range->first, desc.base, desc.sizePages * 4096, desc.protection, plan)) throw Error(EMFILE);
                    const auto result = call(request(Op::MapGpuState, range->first, desc), sizeof(GpuVaReply), true);
                    GpuVaReply fence{}; std::memcpy(&fence, result.data.data(), sizeof fence);
                    if (result.header.handle != range->first || (result.result.ntstatus >= 0 &&
                        ((result.result.ntstatus != 0 && result.result.ntstatus != 259) || result.result.value != desc.base)) ||
                        (result.result.ntstatus < 0 && (result.result.value || fence.fence))) { transport.fail(); throw Error(EPROTO); }
                    checkNt(result.result.ntstatus); pagingFences.verifyRetired(a.hPagingQueue, fence.fence, "gpu-state");
                    for (auto mapped = vendorGpuRanges.begin(); mapped != vendorGpuRanges.end();)
                        if (gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped->second.address, mapped->second.bytes)) mapped = vendorGpuRanges.erase(mapped);
                        else ++mapped;
                    gpuStates.commit(plan); a.VirtualAddress = result.result.value; a.PagingFenceValue = fence.fence;
                    std::fprintf(stderr, "LINUX_BRIDGE nativeGpuStateMapped=true pages=%llu protection=%llu status=%d fence=%llu allocationIsNull=true\n",
                        static_cast<unsigned long long>(desc.sizePages), static_cast<unsigned long long>(desc.protection), result.result.ntstatus,
                        static_cast<unsigned long long>(fence.fence));
                    const auto eofTest = std::getenv("WDDM_BRIDGE_GPU_STATE_EOF_TEST");
                    if (eofTest && !std::strcmp(eofTest, "1")) {
                        std::fprintf(stderr, "LINUX_BRIDGE gpuStateEofTest=true exitingWithGpuStateOwned=true\n"); _exit(1);
                    }
                    return result.result.ntstatus;
                }
                if (a.Reserved1 || !validGpuVa(desc)) throw Error(EINVAL);
                const auto allocation = vendorOwners.find(a.hAllocation), queue = pagingOwners.find(a.hPagingQueue);
                if (allocation == vendorOwners.end() || queue == pagingOwners.end() || allocation->second != queue->second) throw Error(EBADF);
                if (desc.driverProtection && !std::any_of(vendorResources.begin(), vendorResources.end(), [&](const auto& resource) {
                    return resource.second == a.hAllocation; })) throw Error(EINVAL);
                const auto wire = wireAllocation(a.hAllocation);
                GpuStateRanges::Plan statePlan;
                if (desc.base && !gpuStates.prepareRemove(desc.base, desc.sizePages * 4096, statePlan)) throw Error(EMFILE);
                const auto result = call(request(Op::MapVendorAllocation, wire, desc), sizeof(GpuVaReply), true);
                GpuVaReply fence{}; std::memcpy(&fence, result.data.data(), sizeof fence);
                if (result.header.handle != wire ||
                    (result.result.ntstatus >= 0 && ((result.result.ntstatus != 0 && result.result.ntstatus != 0x103) || !validGpuVaOutput(desc, result.result.value))) ||
                    (result.result.ntstatus < 0 && (result.result.value || fence.fence))) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                pagingFences.verifyRetired(a.hPagingQueue, fence.fence, "gpuva");
                if (!desc.base && !gpuStates.prepareRemove(result.result.value, desc.sizePages * 4096, statePlan)) { transport.fail(); throw Error(EIO); }
                gpuStates.commit(statePlan);
                vendorGpuRanges.emplace(a.hAllocation, VendorGpuRange{result.result.value, desc.sizePages * 4096});
                a.VirtualAddress = result.result.value; a.PagingFenceValue = fence.fence;
                if (desc.driverProtection) std::fprintf(stderr, "LINUX_BRIDGE nativeVendorDriverProtectionMapped=true value=%llu resourceOwned=true\n",
                    static_cast<unsigned long long>(desc.driverProtection));
                std::fprintf(stderr, "LINUX_BRIDGE nativeVendorGpuVaMapped=true pages=%llu status=%d fence=%llu\n",
                             static_cast<unsigned long long>(desc.sizePages), result.result.ntstatus, static_cast<unsigned long long>(fence.fence));
                return result.result.ntstatus;
            }
            case 37: {
                auto& a = args<D3DKMT_LOCK2>(requestNumber, pointer);
                const auto allocation = vendorOwners.find(a.hAllocation);
                std::fprintf(stderr, "LINUX_BRIDGE lock2Input flags=%u ownedAllocation=%u hasInputData=%u\n",
                             a.Flags.Value, allocation != vendorOwners.end() && allocation->second == a.hDevice ? 1u : 0u,
                             a.pData ? 1u : 0u);
                if (!(caps.flags & VendorCpuCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=37 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                if (a.Flags.Value) throw Error(EINVAL);
                if (allocation == vendorOwners.end() || allocation->second != a.hDevice) throw Error(EBADF);
                if (allocationMemory.contains(a.hAllocation)) {
                    a.pData = allocationMemory.retain(a.hAllocation); return 0;
                }
                if (cpuLocks.count(a.hAllocation)) throw Error(EBUSY);
                const auto wire = wireAllocation(a.hAllocation);
                const auto result = call(request(Op::LockVendorAllocation, wire, VendorCpuDesc{a.hDevice, 0}), sizeof(VendorCpuReply), true);
                VendorCpuReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.header.handle != wire || result.result.ntstatus > 0 ||
                    (result.result.ntstatus >= 0 && !validVendorCpuReply(result.result.value, output)) ||
                    (result.result.ntstatus < 0 && (result.result.value || output.bytes || output.reserved || output.generation))) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                try {
                    cpuLocks.insert(a.hAllocation);
                    a.pData = allocationMemory.map(a.hAllocation, result.result.value, output);
                } catch (...) { transport.fail(); throw; }
                const auto referenceTest = std::getenv("WDDM_BRIDGE_CPU_REFERENCE_TEST");
                if (referenceTest && !std::strcmp(referenceTest, "1")) {
                    D3DKMT_LOCK2 repeated = a;
                    D3DKMT_UNLOCK2 release{}; release.hDevice = a.hDevice; release.hAllocation = a.hAllocation;
                    if (dispatch(_IOWR('G', 37, D3DKMT_LOCK2), &repeated) || repeated.pData != a.pData ||
                        dispatch(_IOWR('G', 55, D3DKMT_UNLOCK2), &release) || !allocationMemory.contains(a.hAllocation)) {
                        transport.fail(); throw Error(EPROTO);
                    }
                    std::fprintf(stderr, "LINUX_BRIDGE allocationCpuReferenceTest=true sameGuestPointer=true intermediateUnlockRetained=true\n");
                }
                std::fprintf(stderr, "LINUX_BRIDGE nativeVendorCpuLocked=true bytes=%u\n", output.bytes);
                const auto spanEofTest = std::getenv("WDDM_BRIDGE_CPU_SPAN_EOF_TEST");
                if (spanEofTest && !std::strcmp(spanEofTest, "1") && output.bytes > VendorCpuSlotBytes) {
                    std::fprintf(stderr, "LINUX_BRIDGE allocationCpuSpanEofTest=true exitingWithSpanOwned=true\n"); _exit(1);
                }
                const auto eofTest = std::getenv("WDDM_BRIDGE_CPU_EOF_TEST");
                if (eofTest && !std::strcmp(eofTest, "1")) {
                    std::fprintf(stderr, "LINUX_BRIDGE allocationCpuEofTest=true exitingWithLockOwned=true\n");
                    _exit(1); // Deliberately bypass C++/UMD teardown in this private diagnostic.
                }
                return 0;
            }
            case 55: {
                auto& a = args<D3DKMT_UNLOCK2>(requestNumber, pointer);
                if (!(caps.flags & VendorCpuCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=55 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const auto allocation = vendorOwners.find(a.hAllocation);
                if (allocation == vendorOwners.end() || allocation->second != a.hDevice) throw Error(EBADF);
                unlockCpu(a.hAllocation, a.hDevice); return 0;
            }
            case 13: {
                auto& a = args<D3DKMT_ESCAPE>(requestNumber, pointer);
                std::fprintf(stderr, "LINUX_BRIDGE escapeInput type=%u flags=%u privateBytes=%u hasContext=%u\n",
                             static_cast<unsigned>(a.Type), a.Flags.Value, a.PrivateDriverDataSize, a.hContext ? 1u : 0u);
                if (a.Type == D3DKMT_ESCAPE_DRIVERPRIVATE && a.Flags.Value == 64 && a.pPrivateDriverData &&
                    a.PrivateDriverDataSize == sizeof(D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE)) {
                    D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE known{}; std::memcpy(&known, a.pPrivateDriverData, sizeof known);
                    const auto allocation = vendorOwners.find(known.hAllocation);
                    std::fprintf(stderr, "LINUX_BRIDGE knownEscapeInput type=%u ownedAllocation=%u\n",
                                 static_cast<unsigned>(known.EscapeType), allocation != vendorOwners.end() && allocation->second == a.hDevice ? 1u : 0u);
                    if ((caps.flags & VendorTranslationCapability) && known.EscapeType == D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE) {
                        if (a.hContext) throw Error(EINVAL);
                        const auto parent = deviceAdapters.find(a.hDevice);
                        if (allocation == vendorOwners.end() || allocation->second != a.hDevice || parent == deviceAdapters.end() || parent->second != a.hAdapter)
                            throw Error(EBADF);
                        // Allocation aliases already carry the documented driver
                        // token. This known escape is a validated identity operation,
                        // matching WSL's host/guest token contract. No private rewrite.
                        std::fprintf(stderr, "LINUX_BRIDGE knownAllocationTranslation=true aliasAlreadyTranslated=true\n"); return 0;
                    }
                }
                std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=13 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
            }
            case 24: {
                auto& a = args<D3DKMT_CREATEHWQUEUE>(requestNumber, pointer);
                const HwQueueDesc desc{a.Flags.Value, a.PrivateDriverDataSize, 0, 0};
                std::fprintf(stderr, "LINUX_BRIDGE hwQueueDescriptor flags=%u privateBytes=%u hasPrivateData=%u\n",
                             desc.flags, desc.privateBytes, a.pPrivateDriverData ? 1u : 0u);
                if (!validHwQueue(desc) || !a.pPrivateDriverData) throw Error(EINVAL);
                unsigned allocationReferences = 0;
                if (a.pPrivateDriverData && a.PrivateDriverDataSize <= MaxContextPrivateBytes) {
                    for (std::size_t offset = 0; offset + 4 <= a.PrivateDriverDataSize; offset += 4) {
                        std::uint32_t value{}; std::memcpy(&value, static_cast<const char*>(a.pPrivateDriverData) + offset, sizeof value);
                        if (vendorOwners.count(value)) ++allocationReferences;
                    }
                }
                std::fprintf(stderr, "LINUX_BRIDGE hwQueueInput flags=%u privateBytes=%u hasPrivateData=%u guestAllocationReferenceMatches=%u\n",
                             a.Flags.Value, a.PrivateDriverDataSize, a.pPrivateDriverData ? 1u : 0u, allocationReferences);
                if (!(caps.flags & HwQueueCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=24 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                if ((desc.flags & NoBroadcastSignalHwQueueFlag) && !(caps.flags & NoBroadcastSignalHwQueueCapability)) throw Error(ENOSYS);
                if ((desc.flags & NoBroadcastWaitHwQueueFlag) && !(caps.flags & NoBroadcastWaitHwQueueCapability)) throw Error(ENOSYS);
                const auto parent = contextOwners.find(a.hHwContext);
                if (parent == contextOwners.end()) throw Error(EBADF);
                if (parent->second.flags != 16) throw Error(ENOSYS);
                if (hwQueueContexts.size() >= MaxHwQueues) throw Error(EMFILE);
                auto packet = request(Op::CreateHwQueue, a.hHwContext, desc);
                const auto begin = static_cast<const std::uint8_t*>(a.pPrivateDriverData);
                packet.insert(packet.end(), begin, begin + desc.privateBytes);
                const auto result = call(packet, sizeof(HwQueueReply) + desc.privateBytes, true);
                HwQueueReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.result.ntstatus > 0 || result.result.value || output.reserved ||
                    (result.result.ntstatus == 0 && (!result.header.handle || hwQueueContexts.count(result.header.handle) ||
                        !output.sync || output.sync == result.header.handle || output.offset % 8 || output.offset >= FenceApertureBytes ||
                        !output.gpuAddress || output.gpuAddress % 8 || output.gpuAddress >= MaxGpuAddress)) ||
                    (result.result.ntstatus < 0 && (result.header.handle || output.sync || output.offset || output.gpuAddress))) {
                    transport.fail(); throw Error(EPROTO);
                }
                std::memcpy(a.pPrivateDriverData, result.data.data() + sizeof output, desc.privateBytes);
                checkNt(result.result.ntstatus);
                void* fence = nullptr;
                try { fence = hwQueueFences.map(result.header.handle, output.offset, "hwQueue"); }
                catch (...) { destroy(Op::DestroyHwQueue, result.header.handle); throw; }
                if (!hwQueueContexts.emplace(result.header.handle, a.hHwContext).second) { transport.fail(); throw Error(EPROTO); }
                if (!borrowedFences.emplace(output.sync, BorrowedFence{result.header.handle, parent->second.device, true}).second) { transport.fail(); throw Error(EPROTO); }
                a.hHwQueue = result.header.handle; a.hHwQueueProgressFence = output.sync;
                a.HwQueueProgressFenceCPUVirtualAddress = fence; a.HwQueueProgressFenceGPUVirtualAddress = output.gpuAddress;
                std::fprintf(stderr, "LINUX_BRIDGE nativeHwQueueCreated=true privateBytes=%u progressFenceDirect=true flags=%u\n", desc.privateBytes, desc.flags);
                const auto eofTest = std::getenv("WDDM_BRIDGE_HWQUEUE_EOF_TEST");
                if (eofTest && (std::strcmp(eofTest, "1") == 0 ||
                    (std::strcmp(eofTest, "2") == 0 && desc.flags == NoBroadcastSignalHwQueueFlag))) {
                    std::fprintf(stderr, "LINUX_BRIDGE hwQueueEofTest=true exitingWithQueueOwned=true flags=%u\n", desc.flags);
                    _exit(1);
                }
                return 0;
            }
            case 16: {
                auto& a = args<D3DKMT_CREATESYNCHRONIZATIONOBJECT2>(requestNumber, pointer);
                std::fprintf(stderr, "LINUX_BRIDGE synchronizationInput type=%u flags=%u ownedDevice=%u hasSharedInput=%u\n",
                             static_cast<unsigned>(a.Info.Type), a.Info.Flags.Value, deviceAdapters.count(a.hDevice) ? 1u : 0u,
                             a.Info.SharedHandle ? 1u : 0u);
                if (a.Info.Type == D3DDDI_MONITORED_FENCE)
                    std::fprintf(stderr, "LINUX_BRIDGE monitoredFenceInput initial=%llu affinity=%u padding=%u hasCpuInput=%u hasGpuInput=%u\n",
                                 static_cast<unsigned long long>(a.Info.MonitoredFence.InitialFenceValue), a.Info.MonitoredFence.EngineAffinity,
                                 a.Info.MonitoredFence.Padding, a.Info.MonitoredFence.FenceValueCPUVirtualAddress ? 1u : 0u,
                                 a.Info.MonitoredFence.FenceValueGPUVirtualAddress ? 1u : 0u);
                if (a.Info.Type == D3DDDI_CPU_NOTIFICATION)
                    std::fprintf(stderr, "LINUX_BRIDGE cpuNotificationInput hasEvent=%u\n", a.Info.CPUNotification.Event ? 1u : 0u);
                if (!(caps.flags & SyncCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=16 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                SyncDesc desc{static_cast<unsigned>(a.Info.Type), a.Info.Flags.Value, 0, 0, 0};
                if (desc.type == 1) desc.initial = static_cast<std::uint64_t>(a.Info.SynchronizationMutex.InitialState);
                else if (desc.type == 5) {
                    desc.initial = a.Info.MonitoredFence.InitialFenceValue; desc.affinity = a.Info.MonitoredFence.EngineAffinity;
                    if (a.Info.MonitoredFence.Padding) throw Error(EINVAL);
                } else throw Error(ENOSYS);
                if (!validSync(desc) || a.Info.SharedHandle) throw Error(EINVAL);
                if (!deviceAdapters.count(a.hDevice)) throw Error(EBADF);
                if (syncObjects.size() >= MaxSyncObjects) throw Error(EMFILE);
                const auto result = call(request(Op::CreateSync, a.hDevice, desc), sizeof(SyncReply), true);
                SyncReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.result.ntstatus > 0 || result.result.value ||
                    (result.result.ntstatus == 0 && (!result.header.handle || syncObjects.count(result.header.handle) ||
                        (desc.type == 1 && (output.offset || output.gpuAddress)) ||
                        (desc.type == 5 && (output.offset % 8 || output.offset >= FenceApertureBytes ||
                            !validSyncGpuAddress(desc, output.gpuAddress))))) ||
                    (result.result.ntstatus < 0 && (result.header.handle || output.offset || output.gpuAddress))) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                void* fence = nullptr;
                if (desc.type == 5) {
                    try { fence = syncFences.map(result.header.handle, output.offset, "monitored"); }
                    catch (...) { destroy(Op::DestroySync, result.header.handle); throw; }
                }
                if (!syncObjects.emplace(result.header.handle, Synchronization{a.hDevice, desc.type, desc.flags, desc.initial}).second) {
                    transport.fail(); throw Error(EPROTO);
                }
                a.hSyncObject = result.header.handle; a.Info.SharedHandle = 0;
                if (desc.type == 5) {
                    a.Info.MonitoredFence.FenceValueCPUVirtualAddress = fence;
                    a.Info.MonitoredFence.FenceValueGPUVirtualAddress = output.gpuAddress;
                }
                std::fprintf(stderr, "LINUX_BRIDGE nativeSynchronizationCreated=true type=%u flags=%u gpuMapped=%u\n",
                             desc.type, desc.flags, output.gpuAddress ? 1u : 0u);
                const auto noMaxEofTest = std::getenv("WDDM_BRIDGE_SYNC_NO_MAX_EOF_TEST");
                if (desc.flags == NoSignalMaxValueOnTdrSyncFlag && noMaxEofTest && std::strcmp(noMaxEofTest, "1") == 0) {
                    std::fprintf(stderr, "LINUX_BRIDGE syncNoMaxEofTest=true exitingWithNoMaxFenceOwned=true\n"); _exit(1);
                }
                const auto eofTest = std::getenv("WDDM_BRIDGE_SYNC_EOF_TEST");
                if (desc.flags == NoGpuAccessSyncFlag && eofTest && std::strcmp(eofTest, "1") == 0) {
                    std::fprintf(stderr, "LINUX_BRIDGE syncEofTest=true exitingWithNoGpuAccessFenceOwned=true\n");
                    _exit(1);
                }
                return 0;
            }
            case 47: {
                const auto& a = args<D3DKMT_SETCONTEXTINPROCESSSCHEDULINGPRIORITY>(requestNumber, pointer);
                const auto owner = contextOwners.find(a.hContext);
                std::fprintf(stderr, "LINUX_BRIDGE contextPriorityInput priority=%d ownedContext=%u inProcessOnly=true\n", a.Priority, owner != contextOwners.end() ? 1u : 0u);
                if (!(caps.flags & ContextCapability)) throw Error(ENOSYS);
                if (!validContextPriority(a.Priority)) throw Error(EINVAL);
                if (owner == contextOwners.end()) throw Error(EBADF);
                if (owner->second.flags != 16) throw Error(ENOSYS);
                const auto result = call(request(Op::SetContextInProcessPriority, a.hContext, static_cast<std::int32_t>(a.Priority)), 0, true);
                if (result.header.handle != a.hContext || result.result.value || result.result.ntstatus > 0) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                std::fprintf(stderr, "LINUX_BRIDGE nativeContextPriorityChanged=true priority=%d inProcessOnly=true\n", a.Priority);
                return 0;
            }
            case 58: {
                const auto& a = args<D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU>(requestNumber, pointer);
                std::fprintf(stderr, "LINUX_BRIDGE cpuFenceWaitInput count=%u flags=%u asyncEvent=%u ownedDevice=%u hasObjects=%u hasValues=%u\n",
                    a.ObjectCount, a.Flags.Value, a.hAsyncEvent ? 1u : 0u, deviceAdapters.count(a.hDevice) ? 1u : 0u,
                    a.ObjectHandleArray ? 1u : 0u, a.FenceValueArray ? 1u : 0u);
                if (a.hAsyncEvent || a.ObjectCount != 1 || a.Flags.Value) throw Error(ENOSYS);
                if (!a.ObjectHandleArray || !a.FenceValueArray || a.FenceValueArray[0] == UINT64_MAX) throw Error(EINVAL);
                const auto id = a.ObjectHandleArray[0]; const auto target = a.FenceValueArray[0];
                const auto borrowed = borrowedFences.find(id);
                guest_paging::Fences* fences = nullptr; std::uint32_t owner = id; const char* category = "independent";
                if (borrowed != borrowedFences.end()) {
                    if (borrowed->second.device != a.hDevice) throw Error(EBADF);
                    owner = borrowed->second.owner;
                    fences = borrowed->second.hardware ? &hwQueueFences : &pagingFences;
                    category = borrowed->second.hardware ? "hardware" : "paging";
                } else {
                    const auto sync = syncObjects.find(id);
                    if (sync == syncObjects.end() || sync->second.device != a.hDevice || sync->second.type != 5) throw Error(EBADF);
                    fences = &syncFences;
                }
                if (!fences->waitRetired(owner, target, category)) throw Error(EAGAIN);
                return 0;
            }
            case 51: {
                const auto& a = args<D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2>(requestNumber, pointer);
                std::fprintf(stderr, "LINUX_BRIDGE contextSignalInput objects=%u flags=%u contexts=%u hasObjects=%u hasContexts=%u hasValues=%u\n",
                    a.ObjectCount, a.Flags.Value, a.BroadcastContextCount, a.ObjectHandleArray ? 1u : 0u,
                    a.BroadcastContextArray ? 1u : 0u, a.MonitoredFenceValueArray ? 1u : 0u);
                if (!(caps.flags & ContextSignalCapability) || a.ObjectCount != 1 || a.BroadcastContextCount != 1 || a.Flags.Value != 4)
                    throw Error(ENOSYS);
                if (!a.ObjectHandleArray || !a.BroadcastContextArray || !a.MonitoredFenceValueArray ||
                    std::any_of(std::begin(a.Reserved) + 1, std::end(a.Reserved), [](auto value) { return value != 0; })) throw Error(EINVAL);
                if (a.ObjectCount == 1 && a.ObjectHandleArray && a.MonitoredFenceValueArray) {
                    const auto sync = syncObjects.find(a.ObjectHandleArray[0]);
                    std::fprintf(stderr, "LINUX_BRIDGE contextSignalObject owned=%u type=%u flags=%u value=%llu\n", sync != syncObjects.end() ? 1u : 0u,
                        sync == syncObjects.end() ? 0u : sync->second.type, sync == syncObjects.end() ? 0u : sync->second.flags,
                        static_cast<unsigned long long>(a.MonitoredFenceValueArray[0]));
                }
                if (a.BroadcastContextCount == 1 && a.BroadcastContextArray) {
                    const auto context = contextOwners.find(a.BroadcastContextArray[0]);
                    std::fprintf(stderr, "LINUX_BRIDGE contextSignalContext owned=%u flags=%u\n", context != contextOwners.end() ? 1u : 0u,
                        context == contextOwners.end() ? 0u : context->second.flags);
                }
                const ContextSignalDesc desc{a.ObjectHandleArray[0],a.Flags.Value,a.MonitoredFenceValueArray[0]};
                if (!validContextSignal(desc)) throw Error(EINVAL);
                const auto context = contextOwners.find(a.BroadcastContextArray[0]); const auto sync = syncObjects.find(a.ObjectHandleArray[0]);
                if (context == contextOwners.end() || context->second.flags != 16 || sync == syncObjects.end() ||
                    sync->second.type != 5 || sync->second.flags != NoGpuAccessSyncFlag || sync->second.device != context->second.device) throw Error(EBADF);
                if (desc.fence <= sync->second.lastSignal) throw Error(EINVAL);
                const auto result = call(request(Op::SignalContextSync, context->first, desc), 0, true);
                if (result.header.handle != context->first || result.result.ntstatus > 0 ||
                    (result.result.ntstatus == 0 && (result.result.value < desc.fence || result.result.value == UINT64_MAX)) ||
                    (result.result.ntstatus < 0 && result.result.value)) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                syncFences.verifyRetired(sync->first, desc.fence, "context-signal"); sync->second.lastSignal = desc.fence;
                std::fprintf(stderr, "LINUX_BRIDGE nativeContextSignalSubmitted=true flags=%u target=%llu observed=%llu noGpuAccess=true\n",
                    desc.flags,static_cast<unsigned long long>(desc.fence),static_cast<unsigned long long>(result.result.value));
                return 0;
            }
            case 29: {
                auto& a = args<D3DKMT_DESTROYSYNCHRONIZATIONOBJECT>(requestNumber, pointer);
                if (!(caps.flags & SyncCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=29 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const auto owned = syncObjects.find(a.hSyncObject);
                // Paging and queue progress synchronization handles are borrowed.
                // Only independent synchronization objects reach this native destroy.
                if (owned == syncObjects.end()) throw Error(EBADF);
                if (syncFences.contains(a.hSyncObject)) syncFences.unmap(a.hSyncObject);
                const auto result = call(request(Op::DestroySync, a.hSyncObject), 0, true);
                if (result.header.handle != a.hSyncObject || result.result.value || result.result.ntstatus > 0) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                std::fprintf(stderr, "LINUX_BRIDGE nativeSynchronizationDestroyed=true type=%u\n", owned->second.type);
                syncObjects.erase(owned); return 0;
            }
            case 52: {
                auto& a = args<D3DKMT_SUBMITCOMMANDTOHWQUEUE>(requestNumber, pointer);
                unsigned commandOwners = 0, commandCpuLocked = 0;
                std::uint64_t commandOffset = 0, mappedBytes = 0;
                const auto queue = hwQueueContexts.find(a.hHwQueue);
                if (queue != hwQueueContexts.end()) {
                    const auto device = contextOwners.at(queue->second).device;
                    for (const auto& range : vendorGpuRanges) {
                        if (vendorOwners.at(range.first) != device || a.CommandBuffer < range.second.address) continue;
                        const auto offset = a.CommandBuffer - range.second.address;
                        if (offset > range.second.bytes || a.CommandLength > range.second.bytes - offset) continue;
                        ++commandOwners; commandOffset = offset; mappedBytes = range.second.bytes;
                        if (cpuLocks.count(range.first)) ++commandCpuLocked;
                    }
                }
                std::fprintf(stderr, "LINUX_BRIDGE submitInput ownedQueue=%u commandBytes=%u privateBytes=%u primaries=%u hasCommand=%u hasPrivateData=%u hasPrimariesPointer=%u fence=%llu\n",
                             hwQueueContexts.count(a.hHwQueue) ? 1u : 0u, a.CommandLength, a.PrivateDriverDataSize, a.NumPrimaries,
                             a.CommandBuffer ? 1u : 0u, a.pPrivateDriverData ? 1u : 0u, a.WrittenPrimaries ? 1u : 0u,
                             static_cast<unsigned long long>(a.HwQueueProgressFenceId));
                std::fprintf(stderr, "LINUX_BRIDGE submitOwnership commandOwners=%u commandCpuLocked=%u commandOffset=%llu mappedBytes=%llu addressAlignment=%llu lengthAlignment=%u\n",
                             commandOwners, commandCpuLocked, static_cast<unsigned long long>(commandOffset), static_cast<unsigned long long>(mappedBytes),
                             static_cast<unsigned long long>(a.CommandBuffer % 4096), a.CommandLength % 4096);
                if (!(caps.flags & HwSubmitCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=52 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const HwSubmitDesc desc{a.CommandBuffer, a.HwQueueProgressFenceId, a.CommandLength, a.PrivateDriverDataSize, a.NumPrimaries, 0};
                // With zero primaries the pointer is ignored, as in dxgkrnl;
                // it is never read or forwarded to the native process.
                if (!validHwSubmit(desc) || (desc.privateBytes && !a.pPrivateDriverData)) throw Error(EINVAL);
                if (queue == hwQueueContexts.end() || commandOwners != 1 || commandCpuLocked != 1) throw Error(EBADF);
                auto packet = request(Op::SubmitHwQueue, a.hHwQueue, desc);
                if (desc.privateBytes) {
                    const auto data = static_cast<const std::uint8_t*>(a.pPrivateDriverData);
                    packet.insert(packet.end(), data, data + desc.privateBytes);
                }
                const auto result = call(packet, 0, true);
                if (result.header.handle != a.hHwQueue || result.result.ntstatus > 0 ||
                    (result.result.ntstatus == 0 && (result.result.value < desc.fence || result.result.value == UINT64_MAX)) ||
                    (result.result.ntstatus < 0 && result.result.value)) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                hwQueueFences.verifyRetired(a.hHwQueue, desc.fence, "command");
                std::fprintf(stderr, "LINUX_BRIDGE nativeCommandSubmitted=true bytes=%u privateBytes=%u target=%llu observed=%llu\n",
                             desc.bytes, desc.privateBytes, static_cast<unsigned long long>(desc.fence), static_cast<unsigned long long>(result.result.value));
                return 0;
            }
            case 27: {
                auto& a = args<D3DKMT_DESTROYHWQUEUE>(requestNumber, pointer);
                if (!(caps.flags & HwQueueCapability)) {
                    std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=27 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                if (!hwQueueContexts.count(a.hHwQueue)) throw Error(EBADF);
                if (hwQueueFences.contains(a.hHwQueue)) hwQueueFences.unmap(a.hHwQueue);
                const auto result = call(request(Op::DestroyHwQueue, a.hHwQueue));
                if (result.header.handle != a.hHwQueue || result.result.value || result.result.ntstatus) { transport.fail(); throw Error(EPROTO); }
                hwQueueContexts.erase(a.hHwQueue);
                for (auto fence = borrowedFences.begin(); fence != borrowedFences.end();) {
                    if (fence->second.hardware && fence->second.owner == a.hHwQueue) fence = borrowedFences.erase(fence); else ++fence;
                }
                std::fprintf(stderr, "LINUX_BRIDGE nativeHwQueueDestroyed=true\n"); return 0;
            }
            default:
                std::fprintf(stderr, "LINUX_BRIDGE unsupported nr=%u bytes=%u\n", nr, static_cast<unsigned>(_IOC_SIZE(requestNumber)));
                throw Error(ENOSYS);
        }
    }
    void protocolFailed() { transport.fail(); }
};
// Some runtime DSO finalizers close descriptors after C++ exit handlers have
// run. Preserve the allocation for their libc hooks, but explicitly destroy
// owned bridge state once and bypass it in later finalizers.
std::atomic<bool> bridgeStopped{false};
Bridge* bridgeInstance = nullptr;
void stopBridge() {
    bridgeStopped = true;
    if (bridgeInstance) bridgeInstance->~Bridge();
}
Bridge& bridge() {
    static Bridge* instance = [] {
        auto result = new Bridge;
        bridgeInstance = result;
        std::atexit(stopBridge);
        return result;
    }();
    return *instance;
}
int deviceOpen(const char* path, int flags) {
    if (!path || std::strcmp(path, "/dev/dxg")) return -2;
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { return bridge().openDevice(flags); }
    catch (const Error& error) { errno = error.number; }
    catch (...) { errno = EIO; }
    std::fprintf(stderr, "LINUX_BRIDGE openFailed errno=%d\n", errno); return -1;
}
bool modeArgument(int flags) { return (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE; }
}
extern "C" int open(const char* path, int flags, ...) {
    const auto result = deviceOpen(path, flags); if (result != -2) return result;
    mode_t mode = 0; if (modeArgument(flags)) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}
extern "C" int open64(const char* path, int flags, ...) {
    const auto result = deviceOpen(path, flags); if (result != -2) return result;
    mode_t mode = 0; if (modeArgument(flags)) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags | O_LARGEFILE, mode));
}
extern "C" int openat(int directory, const char* path, int flags, ...) {
    const auto result = deviceOpen(path, flags); if (result != -2) return result;
    mode_t mode = 0; if (modeArgument(flags)) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    return static_cast<int>(syscall(SYS_openat, directory, path, flags, mode));
}
extern "C" int close(int fd) {
    if (bridgeStopped) return rawClose(fd);
    const auto saved = errno;
    { auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard); state.closed(fd); }
    errno = saved; return rawClose(fd);
}
extern "C" int ioctl(int fd, unsigned long requestNumber, ...) noexcept {
    // This diagnostic is for this runtime's three-argument ioctl calls only.
    // A general libc interposer must also handle no-argument ioctl variants.
    va_list a; va_start(a, requestNumber); void* pointer = va_arg(a, void*); va_end(a);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    const auto saved = errno;
    if (!state.owns(fd)) { errno = saved; return static_cast<int>(syscall(SYS_ioctl, fd, requestNumber, pointer)); }
    try { return state.dispatch(requestNumber, pointer); }
    catch (const QueryProtocolError& error) { state.protocolFailed(); errno = error.code < 0 ? -error.code : EPROTO; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    std::fprintf(stderr, "LINUX_BRIDGE ioctlFailed nr=%u errno=%d\n", static_cast<unsigned>(_IOC_NR(requestNumber)), errno); return -1;
}
