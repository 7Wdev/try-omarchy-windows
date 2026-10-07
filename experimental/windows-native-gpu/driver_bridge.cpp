// SPDX-License-Identifier: MIT
// Documented WDDM control operations, transported without native pointer blobs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include "driver_wire.h"
using Microsoft::WRL::ComPtr;
using namespace driver_bridge;
class KmtDriver : public Driver {
    LUID luid{};
    DXGI_ADAPTER_DESC1 description{};
    std::map<std::uint32_t, volatile std::uint64_t*> pagingFences;
    struct Allocation {
        std::uint32_t device, resource, size;
        void* memory;
        std::uint64_t gpuAddress = 0;
        bool resident = false;
        bool pagingFailed = false;
    };
    std::map<std::uint32_t, Allocation> allocations;
    std::uint32_t allocatedBytes = 0;
    unsigned activeAdapters = 0, activeDevices = 0, cleanupFailures = 0;
    static constexpr auto Invalid = static_cast<std::int32_t>(0xc000000du);
    static constexpr auto PagingTimeout = static_cast<std::int32_t>(0xc00000b5u);
    bool waitPaging(std::uint32_t queue, std::uint64_t target) const {
        const auto fence = pagingFences.find(queue);
        if (fence == pagingFences.end() || !fence->second) return false;
        const auto deadline = GetTickCount64() + 5000;
        while (*fence->second < target && GetTickCount64() < deadline) Sleep(1);
        MemoryBarrier(); return *fence->second >= target;
    }
    static bool rangeValid(const Allocation& a, Range range) {
        return range.size && range.size <= MaxChunk && range.size <= a.size && range.offset <= a.size - range.size;
    }
public:
    KmtDriver() {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw std::runtime_error("CreateDXGIFactory1 failed");
        bool found = false;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto result = factory->EnumAdapters1(i, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(result) || FAILED(adapter->GetDesc1(&description))) throw std::runtime_error("Adapter enumeration failed");
            if (description.VendorId == 0x10de && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                luid = description.AdapterLuid; found = true; break;
            }
        }
        if (!found) throw std::runtime_error("No hardware NVIDIA adapter; software fallback refused");
    }
    native_gpu::Capabilities capabilities() const override {
        // Bit 0: adapter/device lifecycle. Bit 1: paging queue/fence query.
        // Bit 2: bounded host-backed allocation operations. No shared guest
        // mapping, GPU submission, event or scanout capability is advertised.
        return {Version, 7, description.VendorId, description.DeviceId};
    }
    Result openAdapter() override {
        D3DKMT_OPENADAPTERFROMLUID a{}; a.AdapterLuid = luid;
        const auto status = D3DKMTOpenAdapterFromLuid(&a);
        if (status >= 0) ++activeAdapters;
        return {status, a.hAdapter, 0};
    }
    Result queryVersion(std::uint32_t adapter) override {
        D3DKMT_DRIVERVERSION version{};
        D3DKMT_QUERYADAPTERINFO a{}; a.hAdapter = adapter; a.Type = KMTQAITYPE_DRIVERVERSION;
        a.pPrivateDriverData = &version; a.PrivateDriverDataSize = sizeof version;
        const auto status = D3DKMTQueryAdapterInfo(&a); return {status, 0, static_cast<std::uint64_t>(version)};
    }
    Result createDevice(std::uint32_t adapter) override {
        D3DKMT_CREATEDEVICE a{}; a.hAdapter = adapter;
        // No guest-controlled flags, DisableGpuTimeout, or private driver data.
        const auto status = D3DKMTCreateDevice(&a);
        if (status >= 0) ++activeDevices;
        return {status, a.hDevice, 0};
    }
    Result createPagingQueue(std::uint32_t device) override {
        D3DKMT_CREATEPAGINGQUEUE a{}; a.hDevice = device; a.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        const auto status = D3DKMTCreatePagingQueue(&a);
        if (status >= 0) pagingFences.emplace(a.hPagingQueue, static_cast<volatile std::uint64_t*>(a.FenceValueCPUVirtualAddress));
        return {status, a.hPagingQueue, 0};
    }
    Result readPagingFence(std::uint32_t queue) override {
        const auto entry = pagingFences.find(queue);
        if (entry == pagingFences.end() || !entry->second) return {static_cast<std::int32_t>(0xc0000008u), 0, 0};
        // x64 aligned read from the KMT-owned read-only mapping, never exported.
        const auto value = *entry->second; MemoryBarrier(); return {0, 0, value};
    }
    Result createAllocation(std::uint32_t device, std::uint32_t size) override {
        if (!size || size % 4096 || size > MaxAllocation || size > MaxAllocatedBytes - allocatedBytes)
            return {Invalid, 0, 0};
        auto memory = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!memory) return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
        D3DKMT_CREATESTANDARDALLOCATION standard{};
        standard.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP; standard.ExistingHeapData.Size = size;
        D3DDDI_ALLOCATIONINFO2 info{}; info.pSystemMem = memory;
        D3DKMT_CREATEALLOCATION a{}; a.hDevice = device; a.pStandardAllocation = &standard;
        a.NumAllocations = 1; a.pAllocationInfo2 = &info;
        a.Flags.StandardAllocation = 1; a.Flags.ExistingSysMem = 1;
        a.Flags.CreateShared = 1; a.Flags.CreateResource = 1; a.Flags.CrossAdapter = 1; a.Flags.NtSecuritySharing = 1;
        const auto status = D3DKMTCreateAllocation2(&a);
        if (status < 0) { VirtualFree(memory, 0, MEM_RELEASE); return {status, 0, 0}; }
        allocations.emplace(info.hAllocation, Allocation{device, a.hResource, size, memory});
        allocatedBytes += size; return {status, info.hAllocation, size};
    }
    Result writeAllocation(std::uint32_t handle, Range range, const std::uint8_t* data) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end() || entry->second.pagingFailed || !rangeValid(entry->second, range)) return {Invalid, 0, 0};
        // No guest GPU submission exists yet. Future submission support must
        // enforce CPU/GPU access synchronization before allowing these copies.
        std::memcpy(static_cast<std::uint8_t*>(entry->second.memory) + range.offset, data, range.size);
        MemoryBarrier(); return {0, 0, range.size};
    }
    Result readAllocation(std::uint32_t handle, Range range, std::vector<std::uint8_t>& data) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end() || entry->second.pagingFailed || !rangeValid(entry->second, range)) return {Invalid, 0, 0};
        data.resize(range.size); MemoryBarrier();
        std::memcpy(data.data(), static_cast<const std::uint8_t*>(entry->second.memory) + range.offset, range.size);
        return {0, 0, range.size};
    }
    Result makeResident(std::uint32_t handle, std::uint32_t queue) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end()) return {Invalid, 0, 0};
        if (entry->second.pagingFailed) return {PagingTimeout, 0, 0};
        if (entry->second.resident) return {0, 0, 0};
        D3DDDI_MAKERESIDENT a{}; a.hPagingQueue = queue; a.NumAllocations = 1; a.AllocationList = &handle;
        auto status = D3DKMTMakeResident(&a);
        if (status >= 0 && !waitPaging(queue, a.PagingFenceValue)) {
            entry->second.pagingFailed = true; status = PagingTimeout;
        }
        if (status >= 0) entry->second.resident = true;
        return {status, 0, a.PagingFenceValue};
    }
    Result mapAllocation(std::uint32_t handle, std::uint32_t queue) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end() || !entry->second.resident) return {Invalid, 0, 0};
        if (entry->second.pagingFailed) return {PagingTimeout, 0, 0};
        if (entry->second.gpuAddress) return {0, 0, entry->second.gpuAddress};
        D3DDDI_MAPGPUVIRTUALADDRESS a{}; a.hPagingQueue = queue; a.hAllocation = handle;
        a.SizeInPages = entry->second.size / 4096; a.Protection.Write = 1;
        auto status = D3DKMTMapGpuVirtualAddress(&a);
        if (status >= 0) {
            // Even a timed-out pending map is owned by the allocation. Prevent
            // another map attempt until it is destroyed rather than leaking VA.
            entry->second.gpuAddress = a.VirtualAddress;
            if (!waitPaging(queue, a.PagingFenceValue)) { entry->second.pagingFailed = true; status = PagingTimeout; }
        }
        return {status, 0, status >= 0 ? a.VirtualAddress : 0};
    }
    Result queryResidency(std::uint32_t handle) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end()) return {Invalid, 0, 0};
        D3DKMT_ALLOCATIONRESIDENCYSTATUS residency{};
        D3DKMT_QUERYALLOCATIONRESIDENCY a{}; a.hDevice = entry->second.device;
        a.phAllocationList = &handle; a.AllocationCount = 1; a.pResidencyStatus = &residency;
        const auto status = D3DKMTQueryAllocationResidency(&a); return {status, 0, static_cast<std::uint64_t>(residency)};
    }
    Result destroy(Kind kind, std::uint32_t handle) override {
        NTSTATUS status{};
        if (kind == Kind::Allocation) {
            const auto entry = allocations.find(handle);
            if (entry == allocations.end()) return {Invalid, 0, 0};
            D3DKMT_DESTROYALLOCATION2 a{}; a.hDevice = entry->second.device; a.hResource = entry->second.resource;
            // ExistingSysMem remains secured until destruction completes.
            // Wait for completion before releasing the worker's CPU backing.
            a.Flags.SynchronousDestroy = 1;
            status = D3DKMTDestroyAllocation2(&a);
            if (status >= 0) {
                // The allocation's GPU VA is released by the documented
                // allocation destruction. Free CPU backing only after success.
                if (!VirtualFree(entry->second.memory, 0, MEM_RELEASE)) ++cleanupFailures;
                allocatedBytes -= entry->second.size; allocations.erase(entry);
            }
        } else if (kind == Kind::PagingQueue) {
            D3DDDI_DESTROYPAGINGQUEUE a{}; a.hPagingQueue = handle; status = D3DKMTDestroyPagingQueue(&a);
            if (status >= 0) pagingFences.erase(handle);
        } else if (kind == Kind::Device) {
            D3DKMT_DESTROYDEVICE a{}; a.hDevice = handle; status = D3DKMTDestroyDevice(&a);
            if (status >= 0) --activeDevices;
        } else {
            D3DKMT_CLOSEADAPTER a{}; a.hAdapter = handle; status = D3DKMTCloseAdapter(&a);
            if (status >= 0) --activeAdapters;
        }
        if (status < 0) ++cleanupFailures;
        return {status, 0, 0};
    }
    bool clean() const { return !activeAdapters && !activeDevices && pagingFences.empty() && allocations.empty() && !cleanupFailures; }
    void reportCleanup() const {
        std::cerr << "{\"driverCleanupVerified\":" << (clean() ? "true" : "false")
                  << ",\"liveAdapters\":" << activeAdapters << ",\"liveDevices\":" << activeDevices
                  << ",\"livePagingQueues\":" << pagingFences.size() << ",\"liveAllocations\":" << allocations.size()
                  << ",\"allocatedBytes\":" << allocatedBytes << ",\"cleanupFailures\":" << cleanupFailures << "}\n";
    }
};
struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
struct Winsock {
    Winsock() { WSADATA w{}; if (WSAStartup(MAKEWORD(2, 2), &w)) throw std::runtime_error("WSAStartup failed"); }
    ~Winsock() { WSACleanup(); }
};
class Stream {
    SOCKET socket;
public:
    explicit Stream(SOCKET s = INVALID_SOCKET) : socket(s) {}
    bool read(void* buffer, std::size_t length, bool cleanEof = false) {
        auto p = static_cast<char*>(buffer); std::size_t done = 0;
        while (done != length) {
            int n = socket == INVALID_SOCKET ? static_cast<int>(std::fread(p + done, 1, length - done, stdin))
                                             : recv(socket, p + done, static_cast<int>(length - done), 0);
            if (!n && !done && cleanEof) return false;
            if (n <= 0) throw std::runtime_error("Truncated frame, disconnect, or receive timeout");
            done += static_cast<std::size_t>(n);
        }
        return true;
    }
    void write(const void* buffer, std::size_t length) {
        auto p = static_cast<const char*>(buffer); std::size_t done = 0;
        while (done != length) {
            int n = socket == INVALID_SOCKET ? static_cast<int>(std::fwrite(p + done, 1, length - done, stdout))
                                             : send(socket, p + done, static_cast<int>(length - done), 0);
            if (n <= 0) throw std::runtime_error("Write failed"); done += static_cast<std::size_t>(n);
        }
        if (socket == INVALID_SOCKET && std::fflush(stdout)) throw std::runtime_error("Flush failed");
    }
};
static void serve(Stream& stream) {
    KmtDriver driver;
    {
      Session session(driver);
      unsigned count = 0;
      for (; count < 10000; ++count) {
        // A byte stream needs a length prefix outside the borrowed 16-byte
        // descriptor header. Both are little endian; target host/guest are x64.
        std::uint32_t size{};
        if (!stream.read(&size, sizeof size, true)) break;
        if (size < sizeof(Header) || size > native_gpu::MaxPacket) throw std::runtime_error("Invalid stream frame length");
        std::vector<std::uint8_t> packet(size); stream.read(packet.data(), packet.size());
        const auto reply = session.dispatch(packet);
        size = static_cast<std::uint32_t>(reply.size()); stream.write(&size, sizeof size);
        stream.write(reply.data(), reply.size());
      }
      if (count == 10000) throw std::runtime_error("Session request quota exceeded");
    }
    driver.reportCleanup();
    if (!driver.clean()) throw std::runtime_error("Driver object cleanup failed");
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--stdio") {
            if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
                throw std::runtime_error("Cannot set binary stdio mode");
            Stream stream; serve(stream); return 0;
        }
        if (argc != 3 || std::string(argv[1]) != "--listen") {
            std::cerr << "Usage: driver-bridge.exe --stdio | --listen port (0 chooses a port)\n"; return 2;
        }
        std::size_t end{}; const auto port = std::stoul(argv[2], &end);
        if (end != std::string(argv[2]).size() || port > 65535) throw std::runtime_error("Invalid port");
        Winsock winsock; Socket listener;
        listener.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener.value == INVALID_SOCKET) throw std::runtime_error("socket failed");
        const BOOL exclusive = TRUE;
        if (setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive))
            throw std::runtime_error("Cannot reserve listener exclusively");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(port));
        if (bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof address) || listen(listener.value, 1))
            throw std::runtime_error("Loopback listen failed");
        int length = sizeof address;
        if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length)) throw std::runtime_error("getsockname failed");
        std::cout << "{\"port\":" << ntohs(address.sin_port) << ",\"transport\":\"tcp-loopback\"}\n" << std::flush;
        fd_set reads; FD_ZERO(&reads); FD_SET(listener.value, &reads); timeval timeout{60, 0};
        if (select(0, &reads, nullptr, nullptr, &timeout) != 1) throw std::runtime_error("Connection timed out");
        Socket client; client.value = accept(listener.value, nullptr, nullptr);
        if (client.value == INVALID_SOCKET) throw std::runtime_error("accept failed");
        const DWORD milliseconds = 30000;
        if (setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds) ||
            setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds))
            throw std::runtime_error("Cannot set socket timeouts");
        Stream stream(client.value); serve(stream); return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
