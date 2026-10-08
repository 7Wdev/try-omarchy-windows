// SPDX-License-Identifier: MIT
// Documented WDDM operations with explicit wire structures and owned handles.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <exception>
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
        bool owned = true;
    };
    std::map<std::uint32_t, Allocation> allocations;
    std::uint32_t allocatedBytes = 0;
    unsigned activeAdapters = 0, activeDevices = 0, cleanupFailures = 0;
    unsigned activeContexts = 0;
    bool contextsEnabled = false;
    bool queriesEnabled = false;
    unsigned completedQueries = 0, failedQueries = 0;
    std::string guestSectionName;
    std::uint32_t guestBytes = 0;
    HANDLE guestSection = nullptr;
    void* guestMemory = nullptr;
    ComPtr<ID3D12Device3> copyDevice;
    ComPtr<ID3D12Heap> copyHeap;
    ComPtr<ID3D12CommandQueue> copyQueue;
    ComPtr<ID3D12Fence> copyFence;
    HANDLE copyEvent = nullptr;
    std::uint64_t copyFenceValue = 0, completedCopies = 0, gpuCopiedBytes = 0;
    HRESULT lastCopyError = S_OK;
    static constexpr auto Invalid = static_cast<std::int32_t>(0xc000000du);
    static constexpr auto PagingTimeout = static_cast<std::int32_t>(0xc00000b5u);
    Result copyFailure(HRESULT error) {
        lastCopyError = error; return {static_cast<std::int32_t>(0xc0000001u), 0, 0};
    }
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
    Result createBackedAllocation(std::uint32_t device, std::uint32_t size, void* memory, bool owned) {
        D3DKMT_CREATESTANDARDALLOCATION standard{};
        standard.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP; standard.ExistingHeapData.Size = size;
        D3DDDI_ALLOCATIONINFO2 info{}; info.pSystemMem = memory;
        D3DKMT_CREATEALLOCATION a{}; a.hDevice = device; a.pStandardAllocation = &standard;
        a.NumAllocations = 1; a.pAllocationInfo2 = &info;
        a.Flags.StandardAllocation = 1; a.Flags.ExistingSysMem = 1;
        a.Flags.CreateShared = 1; a.Flags.CreateResource = 1; a.Flags.CrossAdapter = 1; a.Flags.NtSecuritySharing = 1;
        const auto status = D3DKMTCreateAllocation2(&a);
        if (status < 0) {
            if (owned && !VirtualFree(memory, 0, MEM_RELEASE)) ++cleanupFailures;
            return {status, 0, 0};
        }
        allocations.emplace(info.hAllocation, Allocation{device, a.hResource, size, memory, 0, false, false, owned});
        allocatedBytes += size; return {status, info.hAllocation, size};
    }
public:
    KmtDriver(std::string sectionName, std::uint32_t sectionBytes, bool enableContexts, bool enableQueries)
        : contextsEnabled(enableContexts), queriesEnabled(enableQueries),
          guestSectionName(std::move(sectionName)), guestBytes(sectionBytes) {
        const std::string prefix = "Local\\7Wdev-WDDM-";
        if (!guestSectionName.empty()) {
            if (guestSectionName.size() != prefix.size() + 32 || guestSectionName.compare(0, prefix.size(), prefix) ||
                guestSectionName.find_first_not_of("0123456789abcdef", prefix.size()) != std::string::npos ||
                !guestBytes || guestBytes > 1024u * 1024u * 1024u || guestBytes % 4096)
                throw std::runtime_error("Invalid configured guest RAM section");
        } else if (guestBytes) throw std::runtime_error("Missing guest RAM section name");
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw std::runtime_error("CreateDXGIFactory1 failed");
        bool found = false;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto result = factory->EnumAdapters1(i, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(result) || FAILED(adapter->GetDesc1(&description))) throw std::runtime_error("Adapter enumeration failed");
            if (description.VendorId == 0x10de && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                luid = description.AdapterLuid; found = true;
                if (!guestSectionName.empty()) {
                    // Device3 provides the documented section-backed heap
                    // import. Refuse to advertise GPU copy without this API.
                    const auto hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&copyDevice));
                    if (FAILED(hr)) throw std::runtime_error("NVIDIA D3D12 Device3 unavailable for section import");
                }
                break;
            }
        }
        if (!found) throw std::runtime_error("No hardware NVIDIA adapter; software fallback refused");
    }
    ~KmtDriver() override {
        // Retain the view for process teardown if native destruction failed;
        // do not unmap backing that is still secured by an allocation.
        releaseGuestMemory();
    }
    native_gpu::Capabilities capabilities() const override {
        // Bit 0: adapter/device lifecycle. Bit 1: paging queue/fence query.
        // Bit 2: bounded host-backed allocations. Bit 3: configured section
        // imports. Bit 4: bounded, synchronous GPU buffer copy between shared
        // allocations. No arbitrary guest command stream or scanout support.
        return {Version, (guestSectionName.empty() ? 7u : 31u) | (contextsEnabled ? ContextCapability : 0u) |
                (queriesEnabled ? QueryCapability : 0u),
                description.VendorId, description.DeviceId};
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
        // Match the Linux D3D12 runtime's documented RequestVSync flag when
        // context support is enabled. GPU timeout protection stays enabled.
        a.Flags.RequestVSync = contextsEnabled ? 1u : 0u;
        const auto status = D3DKMTCreateDevice(&a);
        if (status >= 0) ++activeDevices;
        return {status, a.hDevice, 0};
    }
    Result queryAdapter(std::uint32_t adapter, QueryDesc desc, std::vector<std::uint8_t>& data) override {
        if (!queriesEnabled || !validQuery(desc) || data.size() != desc.bytes) return {Invalid, 0, 0};
        D3DKMT_QUERYADAPTERINFO query{}; query.hAdapter = adapter;
        query.Type = static_cast<KMTQUERYADAPTERINFOTYPE>(desc.type);
        query.pPrivateDriverData = data.data(); query.PrivateDriverDataSize = desc.bytes;
        const auto status = D3DKMTQueryAdapterInfo(&query);
        if (status >= 0) ++completedQueries; else ++failedQueries;
        // Native data is returned unchanged. Linux UMD filename selection,
        // UTF-16 conversion and virtual adapter flags belong to the guest API.
        return {status, 0, desc.bytes};
    }
    Result createContext(std::uint32_t device, ContextDesc desc, std::vector<std::uint8_t>& data) override {
        if (!contextsEnabled || !validContext(desc) || data.size() != desc.privateBytes) return {Invalid, 0, 0};
        D3DKMT_CREATECONTEXTVIRTUAL a{}; a.hDevice = device;
        a.NodeOrdinal = desc.node; a.EngineAffinity = desc.engine; a.Flags.Value = desc.flags;
        a.ClientHint = static_cast<D3DKMT_CLIENTHINT>(desc.clientHint);
        a.PrivateDriverDataSize = desc.privateBytes; a.pPrivateDriverData = data.empty() ? nullptr : data.data();
        const auto status = D3DKMTCreateContextVirtual(&a);
        if (status >= 0) ++activeContexts;
        return {status, a.hContext, 0};
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
        return createBackedAllocation(device, size, memory, true);
    }
    Result createSharedAllocation(std::uint32_t device, GuestRange range) override {
        if (guestSectionName.empty()) return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
        if (range.reserved || !range.size || range.size % 4096 || range.offset % 4096 || range.size > MaxAllocation ||
            range.size > guestBytes || range.offset > guestBytes - range.size || range.size > MaxAllocatedBytes - allocatedBytes)
            return {Invalid, 0, 0};
        if (!guestSection) {
            const std::wstring name(guestSectionName.begin(), guestSectionName.end());
            // D3D12's existing-heap import also queries the section object.
            guestSection = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
            if (!guestSection) return {static_cast<std::int32_t>(0xc0000008u), 0, 0};
        }
        if (!guestMemory) {
            guestMemory = MapViewOfFile(guestSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, guestBytes);
            if (!guestMemory) return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
        }
        // This fixture's RAM is one section, with guest physical offsets equal
        // to section offsets below 512 MiB. No arbitrary host address is used.
        auto memory = static_cast<std::uint8_t*>(guestMemory) + static_cast<std::size_t>(range.offset);
        return createBackedAllocation(device, range.size, memory, false);
    }
    Result copySharedAllocation(std::uint32_t destination, std::uint32_t source, CopyRange range) override {
        const auto to = allocations.find(destination), from = allocations.find(source);
        if (!copyDevice || !guestMemory || !guestSection || to == allocations.end() || from == allocations.end() ||
            destination == source || to->second.owned || from->second.owned ||
            to->second.device != from->second.device || to->second.pagingFailed || from->second.pagingFailed ||
            !range.size || range.size > to->second.size || range.size > from->second.size ||
            range.sourceOffset > from->second.size - range.size || range.destinationOffset > to->second.size - range.size)
            return {Invalid, 0, 0};
        const auto toOffset = static_cast<std::uint8_t*>(to->second.memory) - static_cast<std::uint8_t*>(guestMemory);
        const auto fromOffset = static_cast<std::uint8_t*>(from->second.memory) - static_cast<std::uint8_t*>(guestMemory);
        if (toOffset % 65536 || fromOffset % 65536 || to->second.size % 65536 || from->second.size % 65536)
            return {Invalid, 0, 0};
        HRESULT hr;
        if (!copyHeap) {
            hr = copyDevice->OpenExistingHeapFromFileMapping(guestSection, IID_PPV_ARGS(&copyHeap));
            if (FAILED(hr)) return copyFailure(hr);
            if (copyHeap->GetDesc().SizeInBytes != guestBytes) return copyFailure(E_INVALIDARG);
        }
        if (!copyQueue) {
            D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_COPY;
            hr = copyDevice->CreateCommandQueue(&q, IID_PPV_ARGS(&copyQueue));
            if (FAILED(hr)) return copyFailure(hr);
        }
        if (!copyFence) {
            hr = copyDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence));
            if (FAILED(hr)) return copyFailure(hr);
        }
        if (!copyEvent) {
            copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!copyEvent) return copyFailure(HRESULT_FROM_WIN32(GetLastError()));
        }
        const auto createBuffer = [&](std::uint32_t size, std::uint64_t offset, D3D12_RESOURCE_STATES state,
                                      ComPtr<ID3D12Resource>& resource) {
            D3D12_RESOURCE_DESC b{}; b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = size; b.Height = 1;
            b.DepthOrArraySize = b.MipLevels = 1; b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            b.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
            return copyDevice->CreatePlacedResource(copyHeap.Get(), offset, &b, state, nullptr, IID_PPV_ARGS(&resource));
        };
        ComPtr<ID3D12Resource> toBuffer, fromBuffer;
        hr = createBuffer(to->second.size, toOffset, D3D12_RESOURCE_STATE_COPY_DEST, toBuffer);
        if (FAILED(hr)) return copyFailure(hr);
        hr = createBuffer(from->second.size, fromOffset, D3D12_RESOURCE_STATE_COPY_SOURCE, fromBuffer);
        if (FAILED(hr)) return copyFailure(hr);
        ComPtr<ID3D12CommandAllocator> allocator;
        hr = copyDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator));
        if (FAILED(hr)) return copyFailure(hr);
        ComPtr<ID3D12GraphicsCommandList> list;
        hr = copyDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        if (FAILED(hr)) return copyFailure(hr);
        MemoryBarrier(); // guest CPU uploads precede this serialized request
        list->CopyBufferRegion(toBuffer.Get(), range.destinationOffset, fromBuffer.Get(), range.sourceOffset, range.size);
        hr = list->Close(); if (FAILED(hr)) return copyFailure(hr);
        ID3D12CommandList* lists[] = {list.Get()}; copyQueue->ExecuteCommandLists(1, lists);
        const auto target = ++copyFenceValue;
        hr = copyQueue->Signal(copyFence.Get(), target);
        if (SUCCEEDED(hr)) hr = copyFence->SetEventOnCompletion(target, copyEvent);
        if (FAILED(hr) || WaitForSingleObject(copyEvent, 5000) != WAIT_OBJECT_0 ||
            copyFence->GetCompletedValue() == UINT64_MAX || copyFence->GetCompletedValue() < target) {
            // GPU may still own the section and resources. Keep them alive for
            // OS process teardown; never unwind and unmap in-flight backing.
            std::cerr << "FAIL: GPU copy did not complete; worker terminated with backing retained\n" << std::flush;
            ExitProcess(3);
        }
        MemoryBarrier(); ++completedCopies; gpuCopiedBytes += range.size;
        return {0, 0, range.size};
    }
    Result writeAllocation(std::uint32_t handle, Range range, const std::uint8_t* data) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end() || entry->second.pagingFailed || !rangeValid(entry->second, range)) return {Invalid, 0, 0};
        // Requests are serialized. GPU copy returns only after its fence;
        // future asynchronous submissions need explicit CPU/GPU ownership.
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
                if (entry->second.owned && !VirtualFree(entry->second.memory, 0, MEM_RELEASE)) ++cleanupFailures;
                allocatedBytes -= entry->second.size; allocations.erase(entry);
            }
        } else if (kind == Kind::PagingQueue) {
            D3DDDI_DESTROYPAGINGQUEUE a{}; a.hPagingQueue = handle; status = D3DKMTDestroyPagingQueue(&a);
            if (status >= 0) pagingFences.erase(handle);
        } else if (kind == Kind::Context) {
            D3DKMT_DESTROYCONTEXT a{}; a.hContext = handle; status = D3DKMTDestroyContext(&a);
            if (status >= 0) --activeContexts;
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
    void releaseGuestMemory() {
        if (!allocations.empty()) return;
        copyQueue.Reset(); copyFence.Reset(); copyHeap.Reset(); copyDevice.Reset();
        if (copyEvent) {
            if (CloseHandle(copyEvent)) copyEvent = nullptr;
            else ++cleanupFailures;
        }
        if (guestMemory) {
            if (UnmapViewOfFile(guestMemory)) guestMemory = nullptr;
            else ++cleanupFailures;
        }
        if (guestSection) {
            if (CloseHandle(guestSection)) guestSection = nullptr;
            else ++cleanupFailures;
        }
    }
    bool clean() const {
        return !activeAdapters && !activeDevices && !activeContexts && pagingFences.empty() && allocations.empty() &&
               !guestMemory && !guestSection && !copyDevice && !copyHeap && !copyQueue && !copyFence && !copyEvent && !cleanupFailures;
    }
    void reportCleanup() const {
        std::cerr << "{\"driverCleanupVerified\":" << (clean() ? "true" : "false")
                  << ",\"liveAdapters\":" << activeAdapters << ",\"liveDevices\":" << activeDevices
                  << ",\"liveContexts\":" << activeContexts
                  << ",\"livePagingQueues\":" << pagingFences.size() << ",\"liveAllocations\":" << allocations.size()
                  << ",\"allocatedBytes\":" << allocatedBytes << ",\"cleanupFailures\":" << cleanupFailures
                  << ",\"liveGuestMappings\":" << (guestMemory ? 1 : 0) << ",\"liveGuestSections\":" << (guestSection ? 1 : 0)
                  << ",\"liveGpuCopyObjects\":" << (!!copyDevice + !!copyHeap + !!copyQueue + !!copyFence + !!copyEvent)
                  << ",\"completedGpuCopies\":" << completedCopies << ",\"gpuCopiedBytes\":" << gpuCopiedBytes
                  << ",\"lastGpuCopyHresult\":" << static_cast<std::uint32_t>(lastCopyError)
                  << ",\"completedAdapterQueries\":" << completedQueries
                  << ",\"failedAdapterQueries\":" << failedQueries << "}\n";
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
            // QEMU can reset its chardev socket during VM poweroff. At a
            // frame boundary this is a disconnect, with the same teardown as
            // EOF. A reset in the middle of a frame remains a protocol failure.
            if (socket != INVALID_SOCKET && n == SOCKET_ERROR && !done && cleanEof &&
                WSAGetLastError() == WSAECONNRESET) return false;
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
static void serve(Stream& stream, const std::string& sectionName = {}, std::uint32_t sectionBytes = 0,
                  bool contexts = false, bool queries = false) {
    KmtDriver driver(sectionName, sectionBytes, contexts, queries);
    std::exception_ptr failure;
    try {
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
    } catch (...) { failure = std::current_exception(); }
    driver.releaseGuestMemory(); driver.reportCleanup();
    if (!driver.clean()) throw std::runtime_error("Driver object cleanup failed");
    if (failure) std::rethrow_exception(failure);
}
int main(int argc, char** argv) {
    try {
        // Explicit experimental opt-in; keep the existing allocation endpoint
        // closed to vendor-private context data unless requested by its owner.
        bool contexts = false, queries = false;
        while (argc > 1) {
            const auto option = std::string(argv[argc - 1]);
            if (option == "--driver-contexts" && !contexts) contexts = true;
            else if (option == "--driver-queries" && !queries) queries = true;
            else break;
            --argc;
        }
        if (argc == 2 && std::string(argv[1]) == "--stdio") {
            if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
                throw std::runtime_error("Cannot set binary stdio mode");
            Stream stream; serve(stream, {}, 0, contexts, queries); return 0;
        }
        if ((argc != 3 && argc != 7) || std::string(argv[1]) != "--listen") {
            std::cerr << "Usage: driver-bridge.exe --stdio | --listen port [--guest-section name --guest-ram-bytes count] "
                "[--driver-contexts] [--driver-queries]\n"; return 2;
        }
        std::size_t end{}; const auto port = std::stoul(argv[2], &end);
        if (end != std::string(argv[2]).size() || port > 65535) throw std::runtime_error("Invalid port");
        std::string sectionName; std::uint32_t sectionBytes = 0;
        if (argc == 7) {
            if (std::string(argv[3]) != "--guest-section" || std::string(argv[5]) != "--guest-ram-bytes")
                throw std::runtime_error("Invalid guest section arguments");
            sectionName = argv[4]; const auto bytes = std::stoul(argv[6], &end);
            if (end != std::string(argv[6]).size() || bytes > 1024u * 1024u * 1024u)
                throw std::runtime_error("Invalid guest section size");
            sectionBytes = static_cast<std::uint32_t>(bytes);
        }
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
        Stream stream(client.value); serve(stream, sectionName, sectionBytes, contexts, queries); return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
