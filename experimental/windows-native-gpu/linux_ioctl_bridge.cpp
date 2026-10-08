// SPDX-License-Identifier: MIT
// Local runtime diagnostic, not a kernel driver or a usable graphics device.
// Interposes /dev/dxg only in the explicitly preloaded process. Unsupported
// calls fail here; they never fall through to the real WSL GPU device.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
#include "adapter_query_client.h"
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
    bool negotiated = false;
    native_gpu::Capabilities caps{};
    unsigned counts[256]{}, completedQueries = 0, privateQueries = 0, contexts = 0;
    std::map<int, std::pair<dev_t, ino_t>> descriptors;
    static constexpr std::uint32_t GuestLuidLow = 0x57475055;
    static constexpr std::int32_t GuestLuidHigh = 0;
    Response call(const std::vector<std::uint8_t>& requestPacket, std::uint32_t bytes = 0) {
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
        checkNt(reply.ntstatus);
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
public:
    std::mutex guard;
    ~Bridge() {
        std::fprintf(stderr, "LINUX_BRIDGE summary completedQueries=%u privateQueries=%u nativeContexts=%u realDxgForwarding=false\n",
                     completedQueries, privateQueries, contexts);
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
            case 21: { auto& a = args<D3DKMT_CLOSEADAPTER>(requestNumber, pointer); destroy(Op::CloseAdapter, a.hAdapter); return 0; }
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
                a.pCommandBuffer = nullptr; a.CommandBufferSize = 0; a.pAllocationList = nullptr;
                a.AllocationListSize = 0; a.pPatchLocationList = nullptr; a.PatchLocationListSize = 0;
                std::fprintf(stderr, "LINUX_BRIDGE deviceCreated=true\n"); return 0;
            }
            case 25: { auto& a = args<D3DKMT_DESTROYDEVICE>(requestNumber, pointer); destroy(Op::DestroyDevice, a.hDevice); return 0; }
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
                if (desc.privateBytes) std::memcpy(a.pPrivateDriverData, result.data.data(), result.data.size());
                ++contexts; std::fprintf(stderr, "LINUX_BRIDGE nativeContextCreated=true bytes=%u\n", desc.privateBytes); return 0;
            }
            case 5: { auto& a = args<D3DKMT_DESTROYCONTEXT>(requestNumber, pointer); destroy(Op::DestroyContext, a.hContext); return 0; }
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
