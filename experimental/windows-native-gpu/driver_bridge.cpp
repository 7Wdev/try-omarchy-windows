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
#include <set>
#include "driver_wire.h"
#include "qemu_runtime_host.h"
using Microsoft::WRL::ComPtr;
using namespace driver_bridge;
class KmtDriver : public Driver {
    LUID luid{};
    DXGI_ADAPTER_DESC1 description{};
    struct Paging {
        volatile std::uint64_t* fence;
        std::uint32_t sync;
        std::optional<driver_qemu::FenceLease> lease;
        std::uint32_t device;
    };
    std::map<std::uint32_t, Paging> pagingFences;
    struct Context { std::uint32_t device, flags; };
    std::map<std::uint32_t, Context> contextOwners;
    struct HwQueue {
        std::uint32_t context, device, sync;
        volatile std::uint64_t* fence;
        std::uint64_t gpuAddress;
        std::optional<driver_qemu::FenceLease> lease;
        std::uint64_t submittedFence = 0;
    };
    std::map<std::uint32_t, HwQueue> hwQueues;
    bool submitEnabled = false;
    unsigned submissionAttempts = 0, completedSubmissions = 0, failedSubmissions = 0;
    unsigned submissionTimeouts = 0;
    std::uint64_t submittedCommandBytes = 0;
    bool hwQueuesEnabled = false;
    bool retirementEnabled = false;
    unsigned vendorDestructionsWithHwQueues = 0;
    unsigned vendorCpuSlotQuotaRejections = 0;
    unsigned completedHwQueues = 0, destroyedHwQueues = 0, failedHwQueues = 0, hwQueuesReleasedAfterVmExit = 0;
    unsigned completedNoBroadcastSignalHwQueues = 0;
    unsigned completedNoBroadcastWaitHwQueues = 0;
    struct Synchronization {
        std::uint32_t device, type, flags;
        volatile std::uint64_t* fence;
        std::uint64_t gpuAddress;
        std::optional<driver_qemu::FenceLease> lease;
        std::uint64_t lastSignal = 0;
    };
    std::map<std::uint32_t, Synchronization> syncObjects;
    bool syncEnabled = false;
    unsigned completedSyncs = 0, destroyedSyncs = 0, failedSyncs = 0, monitoredSyncs = 0, mutexSyncs = 0;
    unsigned syncsReleasedAfterVmExit = 0;
    unsigned noGpuAccessSyncs = 0, noSignalMaxValueOnTdrSyncs = 0;
    unsigned contextSignalAttempts = 0, completedContextSignals = 0, failedContextSignals = 0, contextSignalTimeouts = 0;
    driver_qemu::Runtime* runtime = nullptr;
    struct Allocation {
        std::uint32_t device, resource, size;
        void* memory;
        std::uint64_t gpuAddress = 0;
        bool resident = false;
        bool pagingFailed = false;
        bool owned = true;
    };
    std::map<std::uint32_t, Allocation> allocations;
    // Vendor video-memory allocations have no host CPU backing or public
    // size field. Never mix these with the byte-counted standard allocations.
    struct VendorAllocation {
        std::uint32_t device;
        std::uint32_t resource = 0;
        std::uint64_t address = 0, pages = 0;
        std::uint32_t residencyAttempts = 0;
        std::uint64_t offsetPages = 0;
        bool locked = false;
        bool resident = false;
        void* cpuData = nullptr;
        std::uint32_t cpuBytes = 0;
        std::uint64_t initialCpuFingerprint = 0;
        std::uint64_t firstWord = 0, lastWord = 0;
        std::optional<driver_qemu::AllocationLease> cpuLease;
    };
    std::map<std::uint32_t, VendorAllocation> vendorAllocations;
    unsigned completedVendorResources = 0, destroyedVendorResources = 0;
    unsigned completedVendorDriverProtectionMaps = 0;
    ComPtr<IDXGIAdapter3> allocationBudgetAdapter;
    bool allocationsEnabled = false;
    bool gpuVaEnabled = false;
    bool residencyEnabled = false;
    bool cpuEnabled = false;
    std::uint32_t vendorCpuBytes = 0, peakVendorCpuBytes = 0;
    unsigned completedVendorLocks = 0, completedVendorUnlocks = 0, failedVendorLocks = 0, failedVendorUnlocks = 0;
    unsigned completedVendorSubrangeLocks = 0;
    unsigned vendorCpuContentsChanged = 0;
    bool cpuStoreTest = false;
    unsigned completedCpuStoreTests = 0, failedCpuStoreTests = 0;
    unsigned cpuLocksReleasedAfterVmExit = 0;
    static std::uint64_t fingerprint(void* memory, std::uint32_t bytes) {
        const auto data = static_cast<volatile const unsigned char*>(memory);
        std::uint64_t hash = 14695981039346656037ull;
        for (std::uint32_t n = 0; n < bytes; ++n) hash = (hash ^ data[n]) * 1099511628211ull;
        return hash;
    }
    std::uint32_t vendorResidencyAttempts = 0, peakVendorResidencyAttempts = 0;
    unsigned completedVendorResidency = 0, failedVendorResidency = 0, completedVendorResidencyWaits = 0;
    std::uint64_t vendorAllocationsMadeResident = 0;
    std::uint32_t vendorMappedPages = 0, peakVendorMappedPages = 0;
    unsigned completedVendorMaps = 0, failedVendorMaps = 0, completedVendorMapWaits = 0;
    unsigned completedVendorAllocations = 0, destroyedVendorAllocations = 0, failedVendorAllocations = 0, uninitializedSourceAllocations = 0;
    std::size_t peakVendorAllocationObjects = 0;
    std::uint64_t peakReportedGpuUsage = 0;
    static constexpr std::uint64_t ReportedGpuUsageLimit = 64ull * 1024 * 1024;
    std::uint32_t allocatedBytes = 0;
    unsigned activeAdapters = 0, activeDevices = 0, cleanupFailures = 0;
    std::map<std::uint32_t, std::uint32_t> deviceAdapters;
    std::set<std::uint32_t> ownedAdapters;
    struct GpuReservation { std::uint32_t adapter; std::uint64_t address, bytes; };
    std::map<std::uint32_t, GpuReservation> gpuReservations;
    std::uint32_t nextGpuReservation = 0;
    std::uint64_t gpuReservedBytes = 0, peakGpuReservedBytes = 0;
    bool reservationEnabled = false;
    bool gpuStateEnabled = false;
    GpuStateRanges gpuStates;
    unsigned completedGpuStateMaps = 0, failedGpuStateMaps = 0, completedGpuStateWaits = 0;
    unsigned gpuStateReplacedAllocationMappings = 0;
    unsigned completedReservations = 0, freedReservations = 0, failedReservations = 0;
    unsigned reservationsReleasedAfterVmExit = 0;
    bool translationEnabled = false;
    unsigned completedTranslations = 0, failedTranslations = 0;
    std::map<std::uint32_t, std::uint32_t> translatedAllocations;
    unsigned activeContexts = 0;
    unsigned completedContextPriorities = 0, failedContextPriorities = 0;
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
    bool reportedGpuUsageAcceptable() {
        // This is a reported-usage guard, not an allocation-size decoder or
        // a pre-allocation hard quota. Nonresident allocations and transient
        // growth can escape CurrentUsage; the endpoint stays diagnostic-only.
        if (!allocationBudgetAdapter) return false;
        std::uint64_t total = 0;
        for (const auto group : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (FAILED(allocationBudgetAdapter->QueryVideoMemoryInfo(0, group, &info)) ||
                info.CurrentUsage > UINT64_MAX - total) return false;
            total += info.CurrentUsage;
        }
        peakReportedGpuUsage = (std::max)(peakReportedGpuUsage, total);
        return total <= ReportedGpuUsageLimit;
    }
    Result copyFailure(HRESULT error) {
        lastCopyError = error; return {static_cast<std::int32_t>(0xc0000001u), 0, 0};
    }
    bool waitPaging(std::uint32_t queue, std::uint64_t target) const {
        const auto fence = pagingFences.find(queue);
        if (fence == pagingFences.end() || !fence->second.fence) return false;
        const auto deadline = GetTickCount64() + 5000;
        while (*fence->second.fence < target && GetTickCount64() < deadline) Sleep(1);
        MemoryBarrier(); return *fence->second.fence >= target;
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
    KmtDriver(std::string sectionName, std::uint32_t sectionBytes, bool enableContexts, bool enableQueries,
              driver_qemu::Runtime* ownedRuntime = nullptr, bool enableAllocations = false, bool enableGpuVa = false, bool enableResidency = false, bool enableCpu = false, bool testCpuStores = false, bool enableTranslation = false, bool enableHwQueues = false, bool enableSync = false, bool enableSubmit = false, bool enableRetirement = false, bool enableReservation = false, bool enableGpuState = false)
        : runtime(ownedRuntime), allocationsEnabled(enableAllocations), gpuVaEnabled(enableGpuVa), residencyEnabled(enableResidency), cpuEnabled(enableCpu), cpuStoreTest(testCpuStores), contextsEnabled(enableContexts), queriesEnabled(enableQueries),
          guestSectionName(std::move(sectionName)), guestBytes(sectionBytes) {
        translationEnabled = enableTranslation;
        hwQueuesEnabled = enableHwQueues;
        syncEnabled = enableSync;
        submitEnabled = enableSubmit;
        retirementEnabled = enableRetirement;
        reservationEnabled = enableReservation;
        gpuStateEnabled = enableGpuState;
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
                if (allocationsEnabled && FAILED(adapter.As(&allocationBudgetAdapter)))
                    throw std::runtime_error("NVIDIA video-memory accounting unavailable");
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
                (queriesEnabled ? QueryCapability : 0u) | (runtime ? GuestPagingCapability : 0u) |
                (allocationsEnabled ? VendorAllocationCapability : 0u) | (gpuVaEnabled ? VendorGpuVaCapability : 0u) |
                (residencyEnabled ? VendorResidencyCapability : 0u) | (cpuEnabled ? VendorCpuCapability : 0u) |
                (cpuEnabled && runtime && runtime->allocationSlotLimit() == 128 ? ExpandedVendorCpuCapability : 0u) |
                (gpuVaEnabled && runtime && runtime->allocationSlotLimit() == 128 ? ExpandedVendorGpuCapability : 0u) |
                (translationEnabled ? VendorTranslationCapability : 0u) | (hwQueuesEnabled ? HwQueueCapability : 0u) |
                (hwQueuesEnabled ? NoBroadcastSignalHwQueueCapability : 0u) |
                (hwQueuesEnabled ? NoBroadcastWaitHwQueueCapability : 0u) |
                (syncEnabled ? SyncCapability : 0u) | (submitEnabled ? HwSubmitCapability : 0u) |
                (allocationsEnabled ? VendorResourceCapability : 0u) |
                (submitEnabled && syncEnabled ? ContextSignalCapability : 0u) |
                (retirementEnabled ? VendorRetirementCapability : 0u) | (reservationEnabled ? GpuReservationCapability : 0u) |
                (gpuStateEnabled ? GpuStateCapability : 0u),
                description.VendorId, description.DeviceId};
    }
    Result openAdapter() override {
        D3DKMT_OPENADAPTERFROMLUID a{}; a.AdapterLuid = luid;
        const auto status = D3DKMTOpenAdapterFromLuid(&a);
        if (status >= 0) {
            if (!a.hAdapter || ownedAdapters.count(a.hAdapter)) throw std::runtime_error("Unexpected native adapter identity");
            try {
                ownedAdapters.emplace(a.hAdapter);
            } catch (...) {
                D3DKMT_CLOSEADAPTER release{}; release.hAdapter = a.hAdapter;
                if (a.hAdapter && D3DKMTCloseAdapter(&release) < 0) ++cleanupFailures;
                throw;
            }
            ++activeAdapters;
        }
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
        if (status >= 0) {
            try { deviceAdapters.emplace(a.hDevice, adapter); }
            catch (...) {
                D3DKMT_DESTROYDEVICE release{}; release.hDevice = a.hDevice;
                if (D3DKMTDestroyDevice(&release) < 0) ++cleanupFailures;
                throw;
            }
            ++activeDevices;
        }
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
        if (status >= 0) {
            try {
                if (!a.hContext || !contextOwners.emplace(a.hContext, Context{device, desc.flags}).second)
                    throw std::runtime_error("Unexpected native context identity");
            } catch (...) {
                D3DKMT_DESTROYCONTEXT cleanup{}; cleanup.hContext = a.hContext;
                if (D3DKMTDestroyContext(&cleanup) < 0) ++cleanupFailures;
                throw;
            }
            ++activeContexts;
        }
        return {status, a.hContext, 0};
    }
    Result setContextInProcessPriority(std::uint32_t context, std::int32_t priority) override {
        const auto owner = contextOwners.find(context);
        if (!contextsEnabled || !validContextPriority(priority) || owner == contextOwners.end() || owner->second.flags != 16)
            return {Invalid, 0, 0};
        D3DKMT_SETCONTEXTINPROCESSSCHEDULINGPRIORITY setting{}; setting.hContext = context; setting.Priority = priority;
        const auto status = D3DKMTSetContextInProcessSchedulingPriority(&setting);
        if (status != 0) { ++failedContextPriorities; return {status, 0, 0}; }
        D3DKMT_GETCONTEXTINPROCESSSCHEDULINGPRIORITY observed{}; observed.hContext = context;
        const auto readStatus = D3DKMTGetContextInProcessSchedulingPriority(&observed);
        if (readStatus != 0 || observed.Priority != priority) {
            ++failedContextPriorities;
            return {readStatus < 0 ? readStatus : Invalid, 0, 0};
        }
        ++completedContextPriorities;
        std::cerr << "{\"nativeContextPriorityVerified\":true,\"priority\":" << priority << ",\"inProcessOnly\":true}\n";
        return {0, 0, 0};
    }
    HwQueueResult createHwQueue(std::uint32_t context, HwQueueDesc desc, std::vector<std::uint8_t>& data) override {
        const auto parent = contextOwners.find(context);
        if (!hwQueuesEnabled || !runtime || !validHwQueue(desc) || data.size() != desc.privateBytes ||
            parent == contextOwners.end() || parent->second.flags != 16 || hwQueues.size() >= MaxHwQueues)
            return {{Invalid, 0, 0}, 0, 0, 0};
        if (!reportedGpuUsageAcceptable()) return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, 0, 0, 0};
        D3DKMT_CREATEHWQUEUE a{}; a.hHwContext = context; a.Flags.Value = desc.flags;
        D3DDDI_CREATEHWQUEUEFLAGS layout{}; layout.NoBroadcastSignal = 1;
        if (layout.Value != NoBroadcastSignalHwQueueFlag) throw std::runtime_error("Native NoBroadcastSignal bit layout mismatch");
        layout.Value = 0; layout.NoBroadcastWait = 1;
        if (layout.Value != NoBroadcastWaitHwQueueFlag) throw std::runtime_error("Native NoBroadcastWait bit layout mismatch");
        a.pPrivateDriverData = data.data(); a.PrivateDriverDataSize = desc.privateBytes;
        // The live UMD constructs this private payload using its translated
        // allocation alias. No captured payload or vendor offset patch is used.
        const auto status = D3DKMTCreateHwQueue(&a);
        if (status < 0) { ++failedHwQueues; return {{status, 0, 0}, 0, 0, 0}; }
        try {
            if (!a.hHwQueue || !hwQueues.emplace(a.hHwQueue, HwQueue{context, parent->second.device, a.hHwQueueProgressFence,
                    static_cast<volatile std::uint64_t*>(a.HwQueueProgressFenceCPUVirtualAddress),
                    a.HwQueueProgressFenceGPUVirtualAddress, std::nullopt}).second)
                throw std::runtime_error("Unexpected native hardware queue identity");
        } catch (...) {
            runtime->stop(0);
            D3DKMT_DESTROYHWQUEUE cleanup{}; cleanup.hHwQueue = a.hHwQueue;
            if (D3DKMTDestroyHwQueue(&cleanup) < 0) ++cleanupFailures;
            throw;
        }
        auto& owned = hwQueues.at(a.hHwQueue);
        try {
            if (status != 0 || a.Flags.Value != desc.flags || a.PrivateDriverDataSize != desc.privateBytes || !owned.sync || owned.sync == a.hHwQueue ||
                !owned.fence || !owned.gpuAddress || owned.gpuAddress % 8 || owned.gpuAddress >= MaxGpuAddress)
                throw std::runtime_error("Unexpected native hardware queue output");
            owned.lease = runtime->map(owned.fence);
            ++completedHwQueues;
            if (desc.flags & NoBroadcastSignalHwQueueFlag) ++completedNoBroadcastSignalHwQueues;
            if (desc.flags & NoBroadcastWaitHwQueueFlag) ++completedNoBroadcastWaitHwQueues;
            std::cerr << "{\"nativeHardwareQueueFlagsVerified\":true,\"flags\":" << desc.flags
                      << ",\"unchanged\":true,\"ntstatus\":" << status << "}\n";
            return {{status, a.hHwQueue, 0}, owned.sync, owned.lease->offset, owned.gpuAddress};
        } catch (...) {
            ++failedHwQueues;
            runtime->stop(0); destroy(Kind::HwQueue, a.hHwQueue);
            throw;
        }
    }
    Result submitHwQueue(std::uint32_t handle, HwSubmitDesc desc, const std::vector<std::uint8_t>& data) override {
        const auto queue = hwQueues.find(handle);
        const auto reject = [](unsigned reason) {
            std::cerr << "{\"nativeSubmissionRejected\":true,\"preflightReason\":" << reason << "}\n";
            return Result{Invalid, 0, 0};
        };
        if (!submitEnabled || !runtime || runtime->hasStopped() || !validHwSubmit(desc) || data.size() != desc.privateBytes ||
            queue == hwQueues.end() || !queue->second.lease || desc.fence <= queue->second.submittedFence ||
            submissionAttempts >= MaxHwSubmissions) return reject(1);
        unsigned owners = 0;
        for (const auto& item : vendorAllocations) {
            const auto& allocation = item.second;
            if (allocation.device != queue->second.device) continue;
            if (!allocation.resident) return reject(2);
            if (!allocation.pages || desc.address < allocation.address) continue;
            const auto offset = desc.address - allocation.address;
            if (offset <= allocation.pages * 4096 && desc.bytes <= allocation.pages * 4096 - offset) {
                if (!allocation.locked || !allocation.cpuLease || offset > allocation.cpuBytes || desc.bytes > allocation.cpuBytes - offset)
                    return reject(3);
                ++owners;
            }
        }
        if (owners != 1) return reject(4);
        MemoryBarrier();
        const auto initial = *queue->second.fence;
        std::cerr << "{\"nativeSubmissionPreflight\":true,\"initialFence\":" << initial << ",\"targetFence\":" << desc.fence << "}\n";
        if (initial >= desc.fence) return reject(5);
        D3DKMT_SUBMITCOMMANDTOHWQUEUE a{}; a.hHwQueue = handle; a.HwQueueProgressFenceId = desc.fence;
        a.CommandBuffer = desc.address; a.CommandLength = desc.bytes; a.PrivateDriverDataSize = desc.privateBytes;
        a.pPrivateDriverData = data.empty() ? nullptr : const_cast<std::uint8_t*>(data.data());
        ++submissionAttempts;
        const auto status = D3DKMTSubmitCommandToHwQueue(&a);
        if (status < 0) { ++failedSubmissions; return {status, 0, 0}; }
        queue->second.submittedFence = desc.fence;
        if (status != 0) { ++failedSubmissions; throw std::runtime_error("Unexpected native submission status"); }
        const auto deadline = GetTickCount64() + 5000;
        std::uint64_t observed = 0;
        do {
            MemoryBarrier(); observed = *queue->second.fence;
            if (observed >= desc.fence && observed != UINT64_MAX) {
                ++completedSubmissions; submittedCommandBytes += desc.bytes;
                return {status, 0, observed};
            }
            if (observed == UINT64_MAX) break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        ++failedSubmissions; ++submissionTimeouts;
        // Keep the queue, all allocations and sync objects owned. The outer
        // server reaps its VM before attempting native queue destruction.
        throw std::runtime_error("Native command progress fence deadline exceeded");
    }
    Result reserveGpuAddress(std::uint32_t adapter, GpuReservationDesc desc) override {
        if (!reservationEnabled || !runtime || !ownedAdapters.count(adapter) || !validGpuReservation(desc) ||
            gpuReservations.size() >= MaxGpuReservations || nextGpuReservation == UINT32_MAX ||
            desc.bytes > MaxGpuReservedBytes - gpuReservedBytes) return {Invalid, 0, 0};
        D3DDDI_RESERVEGPUVIRTUALADDRESS a{}; a.hAdapter = adapter;
        a.BaseAddress = desc.base; a.MinimumAddress = desc.minimum; a.MaximumAddress = desc.maximum; a.Size = desc.bytes;
        const auto status = D3DKMTReserveGpuVirtualAddress(&a);
        if (status < 0) { ++failedReservations; return {status, 0, 0}; }
        const auto id = ++nextGpuReservation;
        try { gpuReservations.emplace(id, GpuReservation{adapter, a.VirtualAddress, desc.bytes}); }
        catch (...) {
            D3DKMT_FREEGPUVIRTUALADDRESS release{}; release.hAdapter = adapter; release.BaseAddress = a.VirtualAddress; release.Size = desc.bytes;
            if (!a.VirtualAddress || D3DKMTFreeGpuVirtualAddress(&release) != 0) ++cleanupFailures;
            throw;
        }
        gpuReservedBytes += desc.bytes; peakGpuReservedBytes = (std::max)(peakGpuReservedBytes, gpuReservedBytes);
        if (status != 0 || !validGpuReservationOutput(desc, a.VirtualAddress) || a.Reserved0 || a.Reserved1 || a.Reserved2) {
            ++failedReservations; runtime->stop(0); destroy(Kind::GpuReservation, id);
            throw std::runtime_error("Unexpected native GPU reservation output");
        }
        ++completedReservations; return {status, id, a.VirtualAddress};
    }
    GpuVaResult mapGpuState(std::uint32_t reservation, std::uint32_t queue, GpuVaDesc desc) override {
        const auto range = gpuReservations.find(reservation);
        const auto paging = pagingFences.find(queue);
        if (!gpuStateEnabled || !runtime || runtime->hasStopped() || !validGpuState(desc) || range == gpuReservations.end() ||
            paging == pagingFences.end() || !deviceAdapters.count(paging->second.device) ||
            deviceAdapters.at(paging->second.device) != range->second.adapter ||
            !gpuRangeContains(range->second.address, range->second.bytes, desc.base, desc.sizePages * 4096)) return {{Invalid, 0, 0}, 0};
        for (const auto& mapped : vendorAllocations)
            if (gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.second.address, mapped.second.pages * 4096) &&
                (mapped.second.device != paging->second.device || !gpuRangeContains(desc.base, desc.sizePages * 4096, mapped.second.address, mapped.second.pages * 4096))) return {{Invalid, 0, 0}, 0};
        for (const auto& mapped : allocations)
            if (gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.second.gpuAddress, mapped.second.size)) return {{Invalid, 0, 0}, 0};
        GpuStateRanges::Plan plan;
        if (!gpuStates.prepare(reservation, desc.base, desc.sizePages * 4096, desc.protection, plan)) return {{Invalid, 0, 0}, 0};
        // The diagnostic submits synchronously. Before replacing a mapping,
        // require every tracked hardware command to have retired.
        for (const auto& item : hwQueues) {
            const auto& hardware = item.second;
            if (!hardware.fence) return {{Invalid, 0, 0}, 0};
            MemoryBarrier(); const auto retired = *hardware.fence;
            if (retired == UINT64_MAX || retired < hardware.submittedFence) return {{Invalid, 0, 0}, 0};
        }
        D3DDDI_MAPGPUVIRTUALADDRESS a{}; a.hPagingQueue = queue; // NULL allocation is required for Zero/NoAccess.
        a.BaseAddress = desc.base; a.MinimumAddress = desc.minimum; a.MaximumAddress = desc.maximum;
        a.SizeInPages = desc.sizePages; a.Protection.Value = desc.protection;
        const auto status = D3DKMTMapGpuVirtualAddress(&a);
        if (status < 0) { ++failedGpuStateMaps; return {{status, 0, 0}, 0}; }
        for (auto& item : vendorAllocations) {
            auto& mapped = item.second;
            if (gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.address, mapped.pages * 4096)) {
                vendorMappedPages -= static_cast<std::uint32_t>(mapped.pages); mapped.pages = 0; mapped.address = 0; mapped.offsetPages = 0;
                ++gpuStateReplacedAllocationMappings;
            }
        }
        gpuStates.commit(plan); // Retain ownership if validation or paging retirement fails.
        if ((status != 0 && status != 259) || a.VirtualAddress != desc.base || a.Reserved0 || a.Reserved1 ||
            !waitPaging(queue, a.PagingFenceValue)) {
            ++failedGpuStateMaps; throw std::runtime_error("Native GPU address-state mapping or paging retirement failed");
        }
        ++completedGpuStateMaps; ++completedGpuStateWaits;
        return {{status, 0, a.VirtualAddress}, a.PagingFenceValue};
    }
    SyncResult createSync(std::uint32_t device, SyncDesc desc) override {
        if (!syncEnabled || !runtime || !validSync(desc) || !deviceAdapters.count(device) || syncObjects.size() >= MaxSyncObjects)
            return {{Invalid, 0, 0}, {0, 0}};
        if (desc.type == 5 && !runtime->canMapFence())
            return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, {0, 0}};
        D3DKMT_CREATESYNCHRONIZATIONOBJECT2 a{}; a.hDevice = device;
        D3DDDI_SYNCHRONIZATIONOBJECT_FLAGS noMaxFlag{}; noMaxFlag.NoSignalMaxValueOnTdr = 1;
        if (noMaxFlag.Value != NoSignalMaxValueOnTdrSyncFlag) throw std::runtime_error("Native synchronization flag layout changed");
        a.Info.Type = static_cast<D3DDDI_SYNCHRONIZATIONOBJECT_TYPE>(desc.type);
        a.Info.Flags.Value = desc.flags;
        if (desc.type == 1) a.Info.SynchronizationMutex.InitialState = static_cast<BOOL>(desc.initial);
        else { a.Info.MonitoredFence.InitialFenceValue = desc.initial; a.Info.MonitoredFence.EngineAffinity = desc.affinity; }
        const auto status = D3DKMTCreateSynchronizationObject2(&a);
        if (status < 0) { ++failedSyncs; return {{status, 0, 0}, {0, 0}}; }
        Synchronization owned{device, desc.type, desc.flags, nullptr, 0, std::nullopt};
        owned.lastSignal = desc.initial;
        if (desc.type == 5) {
            owned.fence = static_cast<volatile std::uint64_t*>(a.Info.MonitoredFence.FenceValueCPUVirtualAddress);
            owned.gpuAddress = a.Info.MonitoredFence.FenceValueGPUVirtualAddress;
        }
        try {
            if (!a.hSyncObject || !syncObjects.emplace(a.hSyncObject, owned).second)
                throw std::runtime_error("Unexpected native synchronization identity");
        } catch (...) {
            D3DKMT_DESTROYSYNCHRONIZATIONOBJECT cleanup{}; cleanup.hSyncObject = a.hSyncObject;
            if (D3DKMTDestroySynchronizationObject(&cleanup) < 0) ++cleanupFailures;
            throw;
        }
        auto& retained = syncObjects.at(a.hSyncObject);
        try {
            if (status != 0 || a.Info.Type != static_cast<D3DDDI_SYNCHRONIZATIONOBJECT_TYPE>(desc.type) ||
                a.Info.Flags.Value != retained.flags || a.Info.SharedHandle ||
                (desc.type == 5 && (!retained.fence || !validSyncGpuAddress(desc, retained.gpuAddress))))
                throw std::runtime_error("Unexpected native synchronization output");
            if (desc.type == 5) retained.lease = runtime->map(retained.fence);
            ++completedSyncs;
            if (desc.flags == NoGpuAccessSyncFlag) ++noGpuAccessSyncs;
            if (desc.flags == NoSignalMaxValueOnTdrSyncFlag) ++noSignalMaxValueOnTdrSyncs;
            if (desc.type == 5) ++monitoredSyncs; else ++mutexSyncs;
            return {{status, a.hSyncObject, 0}, {retained.lease ? retained.lease->offset : 0, retained.gpuAddress}};
        } catch (...) {
            ++failedSyncs; runtime->stop(0); destroy(Kind::Sync, a.hSyncObject); throw;
        }
    }
    Result signalContextSync(std::uint32_t context, std::uint32_t sync, ContextSignalDesc desc) override {
        const auto owner = contextOwners.find(context); const auto object = syncObjects.find(sync);
        if (!submitEnabled || !syncEnabled || !runtime || runtime->hasStopped() || !validContextSignal(desc) ||
            owner == contextOwners.end() || owner->second.flags != 16 || object == syncObjects.end() ||
            object->second.device != owner->second.device || object->second.type != 5 || object->second.flags != NoGpuAccessSyncFlag ||
            !object->second.lease || !object->second.fence || desc.fence <= object->second.lastSignal || contextSignalAttempts >= MaxContextSignals)
            return {Invalid,0,0};
        D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 a{};
        a.ObjectCount = 1; a.ObjectHandleArray = &sync; a.Flags.Value = desc.flags;
        a.BroadcastContextCount = 1; a.BroadcastContextArray = &context; a.MonitoredFenceValueArray = &desc.fence;
        ++contextSignalAttempts;
        const auto status = D3DKMTSignalSynchronizationObjectFromGpu2(&a);
        std::cerr << "{\"nativeContextSignalStatus\":true,\"flags\":" << desc.flags << ",\"target\":" << desc.fence << ",\"ntstatus\":" << status << "}\n";
        if (status < 0) { ++failedContextSignals; return {status,0,0}; }
        object->second.lastSignal = desc.fence;
        if (status != 0) { ++failedContextSignals; throw std::runtime_error("Unexpected native context signal status"); }
        const auto deadline = GetTickCount64() + 5000;
        std::uint64_t observed = 0;
        do {
            MemoryBarrier(); observed = *object->second.fence;
            if (observed >= desc.fence && observed != UINT64_MAX) {
                ++completedContextSignals;
                std::cerr << "{\"nativeContextSignalRetired\":true,\"target\":" << desc.fence << ",\"observed\":" << observed
                          << ",\"noGpuAccess\":true,\"cpuValueWrittenByBridge\":false}\n";
                return {0,0,observed};
            }
            if (observed == UINT64_MAX) break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        ++failedContextSignals; ++contextSignalTimeouts;
        throw std::runtime_error("Native context signal retirement deadline exceeded");
    }
    Result createVendorAllocation(std::uint32_t device, VendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        return createVendorAllocationImpl(device, desc, data, false);
    }
    VendorResourceResult createVendorResourceAllocation(std::uint32_t device, VendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        const auto result = createVendorAllocationImpl(device, desc, data, true);
        return {result, result.ntstatus == 0 ? vendorAllocations.at(result.nativeHandle).resource : 0};
    }
    Result createVendorAllocationImpl(std::uint32_t device, VendorAllocationDesc desc, std::vector<std::uint8_t>& data, bool withResource) {
        if (!allocationsEnabled || !validVendorAllocation(desc) || data.size() != desc.privateBytes) return {Invalid, 0, 0};
        if (vendorAllocations.size() >= MaxVendorAllocations || !reportedGpuUsageAcceptable())
            return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
        D3DDDI_ALLOCATIONINFO2 info{}; info.Flags.Value = desc.flags; info.Priority = desc.priority;
        static_assert(UninitializedDisplaySource == D3DDDI_ID_UNINITIALIZED && D3DDDI_ID_NOTAPPLICABLE == 0,
                      "Display source sentinel contract changed");
        // Primary is forbidden by validVendorAllocation. VidPnSourceId only
        // identifies a source for primary surfaces; an unset guest value is
        // normalized to the documented not-applicable value here.
        info.VidPnSourceId = D3DDDI_ID_NOTAPPLICABLE;
        info.pPrivateDriverData = data.data(); info.PrivateDriverDataSize = desc.privateBytes;
        D3DKMT_CREATEALLOCATION a{}; a.hDevice = device; a.NumAllocations = 1; a.pAllocationInfo2 = &info;
        a.Flags.CreateResource = withResource ? 1u : 0u;
        auto status = D3DKMTCreateAllocation2(&a);
        if (desc.source == UninitializedDisplaySource)
            std::cerr << "{\"vendorAllocationSourceNormalization\":true,\"inputSource\":" << desc.source
                      << ",\"nativeSource\":" << info.VidPnSourceId << ",\"primary\":false,\"ntstatus\":" << status << "}\n";
        if (status < 0) { ++failedVendorAllocations; return {status, 0, 0}; }
        if (!info.hAllocation || status != 0 || (a.hResource != 0) != withResource || a.hResource == info.hAllocation ||
            a.hGlobalShare || info.GpuVirtualAddress % 4096 || info.PrivateDriverDataSize != desc.privateBytes) {
            D3DKMT_DESTROYALLOCATION2 release{}; release.hDevice = device;
            release.hResource = a.hResource; release.Flags.SynchronousDestroy = 1;
            if (!a.hResource) { release.phAllocationList = &info.hAllocation; release.AllocationCount = 1; }
            if (D3DKMTDestroyAllocation2(&release) < 0) ++cleanupFailures;
            throw std::runtime_error("Unexpected native vendor-allocation result");
        }
        try {
            VendorAllocation owned{}; owned.device = device; owned.address = info.GpuVirtualAddress; owned.resource = a.hResource;
            vendorAllocations.emplace(info.hAllocation, owned);
        }
        catch (...) {
            D3DKMT_DESTROYALLOCATION2 release{}; release.hDevice = device; release.phAllocationList = &info.hAllocation;
            release.AllocationCount = 1; release.Flags.SynchronousDestroy = 1;
            if (a.hResource) { release.hResource = a.hResource; release.phAllocationList = nullptr; release.AllocationCount = 0; }
            if (D3DKMTDestroyAllocation2(&release) < 0) ++cleanupFailures;
            throw;
        }
        if (!reportedGpuUsageAcceptable()) {
            const auto released = destroyVendorAllocations(device, {info.hAllocation});
            if (released.ntstatus < 0) throw std::runtime_error("Cannot release vendor allocation after reported-usage limit");
            ++failedVendorAllocations; return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
        }
        ++completedVendorAllocations;
        if (withResource) {
            ++completedVendorResources;
            std::cerr << "{\"nativeVendorResourceCreated\":true,\"allocationCount\":1,\"shared\":false,\"systemMemory\":false,\"ntstatus\":" << status << "}\n";
        }
        if (desc.source == UninitializedDisplaySource) ++uninitializedSourceAllocations;
        peakVendorAllocationObjects = (std::max)(peakVendorAllocationObjects, vendorAllocations.size());
        return {status, info.hAllocation, info.GpuVirtualAddress};
    }
    Result destroyVendorAllocations(std::uint32_t device, const std::vector<std::uint32_t>& handles) override {
        if (!allocationsEnabled || handles.empty() || handles.size() > MaxVendorAllocations) return {Invalid, 0, 0};
        unsigned ownedQueues = 0;
        for (const auto& item : hwQueues) {
            const auto& queue = item.second;
            if (queue.device != device) continue;
            // EOF cleanup must retain allocations if queue destruction failed.
            if (!retirementEnabled || !runtime || runtime->hasStopped() || !queue.fence) return {Invalid, 0, 0};
            MemoryBarrier();
            const auto observed = *queue.fence;
            if (observed == UINT64_MAX || observed < queue.submittedFence) return {Invalid, 0, 0};
            ++ownedQueues;
        }
        for (std::size_t n = 0; n < handles.size(); ++n) {
            const auto found = vendorAllocations.find(handles[n]);
            if (found == vendorAllocations.end() || found->second.device != device ||
                std::find(handles.begin(), handles.begin() + n, handles[n]) != handles.begin() + n) return {Invalid, 0, 0};
            if (found->second.resource && handles.size() != 1) return {Invalid, 0, 0};
        }
        // Explicit protocol destruction rejects locked objects. EOF cleanup
        // reaches this path only after the server has reaped its owned VM.
        for (const auto handle : handles) {
            if (vendorAllocations.at(handle).locked) {
                const auto released = unlockVendorAllocation(handle, device);
                if (released.ntstatus < 0) return released;
            }
        }
        D3DKMT_DESTROYALLOCATION2 a{}; a.hDevice = device; a.phAllocationList = handles.data();
        // AssumeNotInUse remains zero: VidMm retains memory needed by earlier
        // commands. SynchronousDestroy confirms reclamation before reuse.
        a.AllocationCount = static_cast<UINT>(handles.size()); a.Flags.SynchronousDestroy = 1;
        const auto resource = vendorAllocations.at(handles.front()).resource;
        if (resource) { a.hResource = resource; a.phAllocationList = nullptr; a.AllocationCount = 0; }
        const auto status = D3DKMTDestroyAllocation2(&a);
        if (status > 0) throw std::runtime_error("Unexpected native allocation-destruction status");
        if (status == 0) {
            if (resource) ++destroyedVendorResources;
            for (const auto handle : handles) {
                vendorMappedPages -= static_cast<std::uint32_t>(vendorAllocations.at(handle).pages);
                vendorResidencyAttempts -= vendorAllocations.at(handle).residencyAttempts;
                translatedAllocations.erase(handle);
                vendorAllocations.erase(handle);
            }
            destroyedVendorAllocations += static_cast<unsigned>(handles.size());
            if (ownedQueues) vendorDestructionsWithHwQueues += static_cast<unsigned>(handles.size());
        } else ++cleanupFailures;
        return {status, 0, 0};
    }
    ResidentResult makeVendorResident(std::uint32_t queue, ResidentDesc desc, const std::vector<std::uint32_t>& handles,
                                     const std::vector<std::uint32_t>& priorities) override {
        const auto paging = pagingFences.find(queue);
        if (!residencyEnabled || !validResident(desc) || paging == pagingFences.end() || handles.size() != desc.count ||
            priorities.size() != (desc.priorities ? desc.count : 0)) return {{Invalid, 0, 0}, {0, 0, 0}};
        for (std::size_t n = 0; n < handles.size(); ++n) {
            const auto allocation = vendorAllocations.find(handles[n]);
            if (allocation == vendorAllocations.end() || allocation->second.device != paging->second.device ||
                std::find(handles.begin(), handles.begin() + n, handles[n]) != handles.begin() + n ||
                allocation->second.residencyAttempts >= MaxVendorResidencyAttempts) return {{Invalid, 0, 0}, {0, 0, 0}};
        }
        if (!reportedGpuUsageAcceptable()) {
            ++failedVendorResidency;
            return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, {0, 0, 0}};
        }
        D3DDDI_MAKERESIDENT a{}; a.hPagingQueue = queue; a.NumAllocations = desc.count;
        a.AllocationList = handles.data(); a.PriorityList = priorities.empty() ? nullptr : priorities.data();
        a.Flags.Value = desc.flags;
        // Even a failing list operation may affect a subset. Bound every
        // attempt until synchronous allocation destruction releases ownership.
        for (const auto handle : handles) ++vendorAllocations.at(handle).residencyAttempts;
        vendorResidencyAttempts += desc.count;
        peakVendorResidencyAttempts = (std::max)(peakVendorResidencyAttempts, vendorResidencyAttempts);
        const auto status = D3DKMTMakeResident(&a);
        if (a.NumAllocations > desc.count || (status >= 0 && status != 0 && status != 0x103))
            throw std::runtime_error("Unexpected native residency output");
        if (status >= 0 && !waitPaging(queue, a.PagingFenceValue)) {
            ++failedVendorResidency;
            throw std::runtime_error("Native residency paging deadline exceeded");
        }
        if (!reportedGpuUsageAcceptable()) {
            ++failedVendorResidency;
            // Keep all allocations owned. The server stops its VM before
            // session destruction, including after partial native failure.
            throw std::runtime_error("Reported GPU usage limit after residency");
        }
        if (status >= 0) {
            ++completedVendorResidency; ++completedVendorResidencyWaits;
            vendorAllocationsMadeResident += a.NumAllocations;
            if (a.NumAllocations == desc.count)
                for (const auto handle : handles) vendorAllocations.at(handle).resident = true;
        } else ++failedVendorResidency;
        return {{status, 0, a.PagingFenceValue}, {a.NumAllocations, 0, a.NumBytesToTrim}};
    }
    GpuVaResult mapVendorAllocation(std::uint32_t allocation, std::uint32_t queue, GpuVaDesc desc) override {
        const auto entry = vendorAllocations.find(allocation);
        if (!gpuVaEnabled || !validGpuVa(desc) || entry == vendorAllocations.end() || !pagingFences.count(queue) ||
            entry->second.device != pagingFences.at(queue).device ||
            (desc.driverProtection && !entry->second.resource) ||
            entry->second.pages || entry->second.address || !vendorGpuBudgetFits(vendorMappedPages, desc.sizePages, capabilities().flags))
            return {{Invalid, 0, 0}, 0};
        if (desc.base) {
            const auto overlaps = [&](std::uint64_t address, std::uint64_t bytes) {
                return address && bytes && desc.base < address + bytes && address < desc.base + desc.sizePages * 4096;
            };
            for (const auto& mapped : vendorAllocations)
                if (overlaps(mapped.second.address, mapped.second.pages * 4096)) return {{Invalid, 0, 0}, 0};
            for (const auto& mapped : allocations)
                if (overlaps(mapped.second.gpuAddress, mapped.second.size)) return {{Invalid, 0, 0}, 0};
        }
        GpuStateRanges::Plan statePlan;
        if (desc.base && !gpuStates.prepareRemove(desc.base, desc.sizePages * 4096, statePlan)) return {{Invalid, 0, 0}, 0};
        D3DDDI_MAPGPUVIRTUALADDRESS a{}; a.hPagingQueue = queue; a.hAllocation = allocation;
        a.BaseAddress = desc.base; a.MinimumAddress = desc.minimum; a.MaximumAddress = desc.maximum;
        a.OffsetInPages = desc.offsetPages; a.SizeInPages = desc.sizePages;
        a.Protection.Value = desc.protection; a.DriverProtection = desc.driverProtection;
        const auto status = D3DKMTMapGpuVirtualAddress(&a);
        if (status < 0) { ++failedVendorMaps; return {{status, 0, 0}, 0}; }
        // Record ownership before any validation/wait can throw. Destruction
        // releases this VA even if paging fails; retries cannot create a leak.
        entry->second.address = a.VirtualAddress; entry->second.pages = desc.sizePages;
        entry->second.offsetPages = desc.offsetPages;
        vendorMappedPages += static_cast<std::uint32_t>(desc.sizePages);
        peakVendorMappedPages = (std::max)(peakVendorMappedPages, vendorMappedPages);
        if ((status != 0 && status != 0x103) || !validGpuVaOutput(desc, a.VirtualAddress))
            throw std::runtime_error("Unexpected native GPU-address mapping output");
        if (!desc.base && !gpuStates.prepareRemove(a.VirtualAddress, desc.sizePages * 4096, statePlan))
            throw std::runtime_error("Native GPU state tracking quota after allocation mapping");
        gpuStates.commit(statePlan);
        if (!waitPaging(queue, a.PagingFenceValue)) {
            ++failedVendorMaps;
            // Do not expose an incomplete mapping. The server first reaps its
            // owned VM, then destroys the allocation and its paging queue.
            throw std::runtime_error("Native GPU-address paging deadline exceeded");
        }
        if (!reportedGpuUsageAcceptable()) {
            ++failedVendorMaps;
            throw std::runtime_error("Reported GPU usage limit after address mapping");
        }
        ++completedVendorMapWaits; ++completedVendorMaps;
        if (desc.driverProtection) {
            ++completedVendorDriverProtectionMaps;
            std::cerr << "{\"nativeVendorDriverProtectionMap\":true,\"driverProtection\":" << desc.driverProtection
                      << ",\"resourceOwned\":true,\"unchanged\":true,\"ntstatus\":" << status << "}\n";
        }
        // Preserve STATUS_PENDING even though our native wait already retired
        // the fence. The Linux KMT thunk preserves positive ioctl statuses.
        return {{status, 0, a.VirtualAddress}, a.PagingFenceValue};
    }
    VendorCpuResult lockVendorAllocation(std::uint32_t allocation, std::uint32_t device) override {
        const auto entry = vendorAllocations.find(allocation);
        if (!cpuEnabled || !runtime || entry == vendorAllocations.end() || entry->second.device != device ||
            entry->second.locked || !entry->second.pages || entry->second.offsetPages)
            return {{Invalid, 0, 0}, {0, 0, 0}};
        auto& owned = entry->second;
        const auto bytes = static_cast<std::uint32_t>(owned.pages * 4096);
        if (!bytes || bytes > MaxVendorCpuMappingBytes || !vendorCpuBudgetFits(vendorCpuBytes, bytes, capabilities().flags) || !reportedGpuUsageAcceptable())
            return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, {0, 0, 0}};
        // Exhaustion is a native allocation failure that the live UMD can
        // recover from, not a QMP mapping exception after obtaining a lock.
        if (!runtime->canMapAllocation(bytes)) {
            ++vendorCpuSlotQuotaRejections;
            return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, {0, 0, 0}};
        }
        D3DKMT_LOCK2 a{}; a.hDevice = device; a.hAllocation = allocation;
        const auto status = D3DKMTLock2(&a);
        if (status < 0) {
            std::cerr << "{\"nativeCpuLockRejected\":true,\"ntstatus\":" << status << ",\"mappedBytes\":" << bytes << "}\n";
            ++failedVendorLocks; return {{status, 0, 0}, {0, 0, 0}};
        }
        // Retain the native lock before validation/control can fail. No host
        // CPU address leaves this process; the guest receives a BAR lease.
        owned.locked = true; owned.cpuData = a.pData;
        MEMORY_BASIC_INFORMATION region{}, next{};
        const auto source = reinterpret_cast<std::uintptr_t>(a.pData);
        const bool committedRegion = source && VirtualQuery(a.pData, &region, sizeof region) == sizeof region &&
            region.AllocationBase && driver_cpu::validNativeAllocationView(source, bytes,
                reinterpret_cast<std::uintptr_t>(region.BaseAddress), region.RegionSize) &&
            region.Type == MEM_PRIVATE && region.State == MEM_COMMIT &&
            (region.Protect == PAGE_READWRITE || region.Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (committedRegion) VirtualQuery(reinterpret_cast<void*>(source + bytes), &next, sizeof next);
        std::cerr << "{\"vendorCpuRegionCandidate\":true,\"status\":" << status << ",\"expectedBytes\":" << bytes
                  << ",\"regionBytes\":" << region.RegionSize << ",\"type\":" << region.Type << ",\"state\":" << region.State
                  << ",\"protection\":" << region.Protect << ",\"baseMatches\":" << (region.BaseAddress == a.pData ? "true" : "false")
                  << ",\"allocationBaseMatches\":" << (region.AllocationBase == a.pData ? "true" : "false")
                  << ",\"regionBaseOffsetBytes\":" << (source >= reinterpret_cast<std::uintptr_t>(region.BaseAddress) ? source - reinterpret_cast<std::uintptr_t>(region.BaseAddress) : 0)
                  << ",\"exportedRangeContained\":" << (committedRegion ? "true" : "false")
                  << ",\"nextState\":" << next.State << ",\"nextAllocationBaseMatches\":" << (next.AllocationBase == a.pData ? "true" : "false") << "}\n";
        // One Windows reservation may contain several driver allocations.
        // Export only the native allocation's mapped byte range at offset zero,
        // never the full region or reservation. The native lock owns its lifetime;
        // the QEMU hub also rejects overlapping active leases.
        // VirtualQuery checks only this committed region's eligibility,
        // not a public decoder for the size of arbitrary vendor allocations.
        if (status != 0 || !committedRegion || !reportedGpuUsageAcceptable()) {
            ++failedVendorLocks;
            const auto released = unlockVendorAllocation(allocation, device);
            if (released.ntstatus < 0) throw std::runtime_error("Cannot release ineligible native CPU lock");
            return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, {0, 0, 0}};
        }
        owned.cpuBytes = bytes; vendorCpuBytes += bytes;
        peakVendorCpuBytes = (std::max)(peakVendorCpuBytes, vendorCpuBytes);
        owned.initialCpuFingerprint = fingerprint(owned.cpuData, bytes);
        owned.firstWord = *static_cast<volatile std::uint64_t*>(owned.cpuData);
        owned.lastWord = *reinterpret_cast<volatile std::uint64_t*>(static_cast<char*>(owned.cpuData) + bytes - 8);
        owned.cpuLease = runtime->mapAllocation(a.pData, bytes);
        if (region.RegionSize != bytes || region.BaseAddress != a.pData) ++completedVendorSubrangeLocks;
        ++completedVendorLocks;
        return {{status, 0, owned.cpuLease->offset}, {bytes, 0, owned.cpuLease->generation}};
    }
    Result unlockVendorAllocation(std::uint32_t allocation, std::uint32_t device) override {
        const auto entry = vendorAllocations.find(allocation);
        if (!cpuEnabled || !runtime || entry == vendorAllocations.end() || entry->second.device != device || !entry->second.locked)
            return {Invalid, 0, 0};
        auto& owned = entry->second;
        const bool afterVmExit = owned.cpuLease.has_value() && runtime->hasStopped();
        if (owned.cpuLease) {
            // A control failure reaps QEMU before throwing. Retain native
            // ownership until that boundary is proved; never free live pages.
            runtime->unmapAllocation(*owned.cpuLease);
            owned.cpuLease.reset();
        }
        MemoryBarrier();
        const bool changed = owned.cpuBytes && fingerprint(owned.cpuData, owned.cpuBytes) != owned.initialCpuFingerprint;
        if (cpuStoreTest && owned.cpuBytes) {
            auto first = static_cast<volatile std::uint64_t*>(owned.cpuData);
            auto last = reinterpret_cast<volatile std::uint64_t*>(static_cast<char*>(owned.cpuData) + owned.cpuBytes - 8);
            const bool matched = *first == CpuStoreFirstMarker && *last == CpuStoreLastMarker;
            *first = owned.firstWord; *last = owned.lastWord; MemoryBarrier();
            const bool restored = *first == owned.firstWord && *last == owned.lastWord;
            if (matched && restored) ++completedCpuStoreTests; else ++failedCpuStoreTests;
        }
        D3DKMT_UNLOCK2 a{}; a.hDevice = device; a.hAllocation = allocation;
        const auto status = D3DKMTUnlock2(&a);
        if (status < 0) { ++failedVendorUnlocks; return {status, 0, 0}; }
        if (status != 0) throw std::runtime_error("Unexpected native CPU unlock status");
        if (afterVmExit) ++cpuLocksReleasedAfterVmExit;
        if (changed) ++vendorCpuContentsChanged;
        vendorCpuBytes -= owned.cpuBytes; owned.cpuBytes = 0;
        owned.cpuData = nullptr; owned.locked = false; ++completedVendorUnlocks;
        return {status, 0, 0};
    }
    Result translateVendorAllocation(std::uint32_t allocation, std::uint32_t device, std::uint32_t adapter) override {
        const auto owned = vendorAllocations.find(allocation);
        const auto parent = deviceAdapters.find(device);
        if (!translationEnabled || owned == vendorAllocations.end() || owned->second.device != device ||
            parent == deviceAdapters.end() || parent->second != adapter) return {Invalid, 0, 0};
        const auto cached = translatedAllocations.find(allocation);
        if (cached != translatedAllocations.end()) return {0, 0, cached->second};
        D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE data{};
        data.EscapeType = D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE; data.hAllocation = allocation;
        D3DKMT_ESCAPE escape{}; escape.hAdapter = adapter; escape.hDevice = device;
        escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE; escape.Flags.DriverKnownEscape = 1;
        escape.pPrivateDriverData = &data; escape.PrivateDriverDataSize = sizeof data;
        const auto status = D3DKMTEscape(&escape);
        if (status < 0) { ++failedTranslations; return {status, 0, 0}; }
        if (status != 0 || data.EscapeType != D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE || !data.hAllocation)
            throw std::runtime_error("Unexpected native allocation translation output");
        for (const auto& token : translatedAllocations)
            if (token.second == data.hAllocation) throw std::runtime_error("Duplicate native allocation driver token");
        translatedAllocations.emplace(allocation, data.hAllocation); ++completedTranslations;
        return {status, 0, data.hAllocation};
    }
    Result createPagingQueue(std::uint32_t device) override {
        D3DKMT_CREATEPAGINGQUEUE a{}; a.hDevice = device; a.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        const auto status = D3DKMTCreatePagingQueue(&a);
        if (status >= 0) {
            try { pagingFences.emplace(a.hPagingQueue, Paging{
                static_cast<volatile std::uint64_t*>(a.FenceValueCPUVirtualAddress), a.hSyncObject, std::nullopt, device}); }
            catch (...) {
                D3DDDI_DESTROYPAGINGQUEUE cleanup{}; cleanup.hPagingQueue = a.hPagingQueue;
                if (D3DKMTDestroyPagingQueue(&cleanup) < 0) ++cleanupFailures;
                throw;
            }
        }
        return {status, a.hPagingQueue, 0};
    }
    GuestPagingResult createGuestPagingQueue(std::uint32_t device) override {
        if (!runtime) return Driver::createGuestPagingQueue(device);
        const auto result = createPagingQueue(device);
        if (result.ntstatus < 0) return {result, 0, 0};
        auto& queue = pagingFences.at(result.nativeHandle);
        try {
            if (!queue.sync) throw std::runtime_error("Native paging synchronization object absent");
            queue.lease = runtime->map(queue.fence);
            return {result, queue.sync, queue.lease->offset};
        } catch (...) {
            // map() stops its owned VM before returning a control failure.
            // Also stop on a malformed native result before releasing pages.
            runtime->stop(0);
            destroy(Kind::PagingQueue, result.nativeHandle);
            throw;
        }
    }
    Result readPagingFence(std::uint32_t queue) override {
        const auto entry = pagingFences.find(queue);
        if (entry == pagingFences.end() || !entry->second.fence) return {static_cast<std::int32_t>(0xc0000008u), 0, 0};
        // x64 aligned read from the KMT-owned read-only mapping, never exported.
        const auto value = *entry->second.fence; MemoryBarrier(); return {0, 0, value};
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
        if (kind == Kind::VendorAllocation) {
            const auto found = vendorAllocations.find(handle);
            if (found == vendorAllocations.end()) return {Invalid, 0, 0};
            return destroyVendorAllocations(found->second.device, {handle});
        } else if (kind == Kind::VendorResource) {
            return {0, 0, 0}; // Borrowed resource identity; its single allocation owns native destruction.
        } else if (kind == Kind::Allocation) {
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
            const auto queue = pagingFences.find(handle);
            if (queue == pagingFences.end()) return {Invalid, 0, 0};
            if (queue->second.lease) {
                try { runtime->unmap(*queue->second.lease); }
                catch (...) {
                    // No unmap acknowledgement: retain pages until the owned
                    // VM has exited. Its control failure is reported separately.
                    runtime->stop(0);
                }
                queue->second.lease.reset();
            }
            D3DDDI_DESTROYPAGINGQUEUE a{}; a.hPagingQueue = handle; status = D3DKMTDestroyPagingQueue(&a);
            if (status >= 0) pagingFences.erase(handle);
        } else if (kind == Kind::PagingSync) {
            // Borrowed queue-owned KMT sync object; the queue releases it.
            return {0, 0, 0};
        } else if (kind == Kind::HwQueue) {
            const auto queue = hwQueues.find(handle);
            if (queue == hwQueues.end() || !runtime) return {Invalid, 0, 0};
            const bool afterVmExit = runtime->hasStopped();
            if (queue->second.lease) {
                try { runtime->unmap(*queue->second.lease); }
                catch (...) { runtime->stop(0); }
                queue->second.lease.reset();
            }
            D3DKMT_DESTROYHWQUEUE a{}; a.hHwQueue = handle; status = D3DKMTDestroyHwQueue(&a);
            if (status == 0) {
                hwQueues.erase(queue); ++destroyedHwQueues;
                if (afterVmExit) ++hwQueuesReleasedAfterVmExit;
            }
        } else if (kind == Kind::HwQueueSync) {
            return {0, 0, 0}; // Borrowed progress sync, owned by the hardware queue.
        } else if (kind == Kind::GpuReservation) {
            const auto entry = gpuReservations.find(handle);
            if (entry == gpuReservations.end() || !runtime) return {Invalid, 0, 0};
            const auto& range = entry->second;
            if (!validGpuReservationOutput({range.address, 0, 0, range.bytes}, range.address)) return {Invalid, 0, 0};
            for (const auto& allocation : vendorAllocations)
                if (gpuRangesOverlap(range.address, range.bytes, allocation.second.address, allocation.second.pages * 4096) &&
                    !gpuRangeContains(range.address, range.bytes, allocation.second.address, allocation.second.pages * 4096)) return {Invalid, 0, 0};
            for (const auto& allocation : allocations)
                if (gpuRangesOverlap(range.address, range.bytes, allocation.second.gpuAddress, allocation.second.size) &&
                    !gpuRangeContains(range.address, range.bytes, allocation.second.gpuAddress, allocation.second.size)) return {Invalid, 0, 0};
            for (const auto& item : hwQueues) {
                const auto& queue = item.second;
                // Failure to destroy a queue at EOF retains address ownership.
                if (runtime->hasStopped() || !queue.fence) return {Invalid, 0, 0};
                MemoryBarrier(); const auto value = *queue.fence;
                if (value == UINT64_MAX || value < queue.submittedFence) return {Invalid, 0, 0};
            }
            D3DKMT_FREEGPUVIRTUALADDRESS a{}; a.hAdapter = range.adapter; a.BaseAddress = range.address; a.Size = range.bytes;
            status = D3DKMTFreeGpuVirtualAddress(&a);
            if (status > 0) throw std::runtime_error("Unexpected native GPU reservation release status");
            if (status == 0) {
                for (auto& allocation : vendorAllocations) {
                    auto& mapped = allocation.second;
                    if (gpuRangesOverlap(range.address, range.bytes, mapped.address, mapped.pages * 4096)) {
                        vendorMappedPages -= static_cast<std::uint32_t>(mapped.pages); mapped.pages = 0; mapped.address = 0; mapped.offsetPages = 0;
                    }
                }
                for (auto& allocation : allocations)
                    if (gpuRangesOverlap(range.address, range.bytes, allocation.second.gpuAddress, allocation.second.size)) allocation.second.gpuAddress = 0;
                gpuStates.release(handle); gpuReservedBytes -= range.bytes; gpuReservations.erase(entry); ++freedReservations;
                if (runtime->hasStopped()) ++reservationsReleasedAfterVmExit;
            }
        } else if (kind == Kind::Sync) {
            const auto object = syncObjects.find(handle);
            if (object == syncObjects.end() || !runtime) return {Invalid, 0, 0};
            const bool afterVmExit = runtime->hasStopped();
            if (object->second.lease) {
                try { runtime->unmap(*object->second.lease); }
                catch (...) { runtime->stop(0); }
                object->second.lease.reset();
            }
            D3DKMT_DESTROYSYNCHRONIZATIONOBJECT a{}; a.hSyncObject = handle;
            status = D3DKMTDestroySynchronizationObject(&a);
            if (status == 0) {
                syncObjects.erase(object); ++destroyedSyncs;
                if (afterVmExit) ++syncsReleasedAfterVmExit;
            }
        } else if (kind == Kind::Context) {
            if (!contextOwners.count(handle)) return {Invalid, 0, 0};
            for (const auto& queue : hwQueues) if (queue.second.context == handle) return {Invalid, 0, 0};
            D3DKMT_DESTROYCONTEXT a{}; a.hContext = handle; status = D3DKMTDestroyContext(&a);
            if (status >= 0) { --activeContexts; contextOwners.erase(handle); }
        } else if (kind == Kind::Device) {
            D3DKMT_DESTROYDEVICE a{}; a.hDevice = handle; status = D3DKMTDestroyDevice(&a);
            if (status >= 0) { --activeDevices; deviceAdapters.erase(handle); }
        } else {
            if (kind != Kind::Adapter || !ownedAdapters.count(handle)) return {Invalid, 0, 0};
            for (const auto& range : gpuReservations) if (range.second.adapter == handle) return {Invalid, 0, 0};
            D3DKMT_CLOSEADAPTER a{}; a.hAdapter = handle; status = D3DKMTCloseAdapter(&a);
            if (status >= 0) { --activeAdapters; ownedAdapters.erase(handle); }
        }
        if (status < 0) ++cleanupFailures;
        return {status, 0, 0};
    }
    void releaseGuestMemory() {
        if (!allocations.empty() || !vendorAllocations.empty()) return;
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
        return !activeAdapters && !activeDevices && !activeContexts && pagingFences.empty() && allocations.empty() && vendorAllocations.empty() &&
               !vendorMappedPages && !vendorResidencyAttempts && !vendorCpuBytes && deviceAdapters.empty() && translatedAllocations.empty() &&
               contextOwners.empty() && hwQueues.empty() && syncObjects.empty() && gpuReservations.empty() && !gpuReservedBytes && gpuStates.empty() && ownedAdapters.empty() &&
               !guestMemory && !guestSection && !copyDevice && !copyHeap && !copyQueue && !copyFence && !copyEvent && !cleanupFailures;
    }
    void reportCleanup() const {
        std::cerr << "{\"driverCleanupVerified\":" << (clean() ? "true" : "false")
                  << ",\"liveAdapters\":" << activeAdapters << ",\"liveDevices\":" << activeDevices
                  << ",\"liveContexts\":" << activeContexts
                  << ",\"completedContextPriorityChanges\":" << completedContextPriorities
                  << ",\"failedContextPriorityChanges\":" << failedContextPriorities
                  << ",\"liveHwQueues\":" << hwQueues.size() << ",\"completedHwQueues\":" << completedHwQueues
                  << ",\"completedNoBroadcastSignalHwQueues\":" << completedNoBroadcastSignalHwQueues
                  << ",\"completedNoBroadcastWaitHwQueues\":" << completedNoBroadcastWaitHwQueues
                  << ",\"destroyedHwQueues\":" << destroyedHwQueues << ",\"failedHwQueues\":" << failedHwQueues
                  << ",\"hwQueuesReleasedAfterVmExit\":" << hwQueuesReleasedAfterVmExit
                  << ",\"liveSyncObjects\":" << syncObjects.size() << ",\"completedSyncObjects\":" << completedSyncs
                  << ",\"destroyedSyncObjects\":" << destroyedSyncs << ",\"failedSyncObjects\":" << failedSyncs
                  << ",\"completedMonitoredFences\":" << monitoredSyncs << ",\"completedSynchronizationMutexes\":" << mutexSyncs
                  << ",\"completedNoGpuAccessFences\":" << noGpuAccessSyncs << ",\"syncObjectLimit\":" << MaxSyncObjects
                  << ",\"completedNoSignalMaxValueOnTdrFences\":" << noSignalMaxValueOnTdrSyncs
                  << ",\"nativeContextSignalAttempts\":" << contextSignalAttempts
                  << ",\"completedNativeContextSignals\":" << completedContextSignals
                  << ",\"failedNativeContextSignals\":" << failedContextSignals
                  << ",\"nativeContextSignalTimeouts\":" << contextSignalTimeouts
                  << ",\"syncObjectsReleasedAfterVmExit\":" << syncsReleasedAfterVmExit
                  << ",\"liveGpuReservations\":" << gpuReservations.size() << ",\"liveGpuReservedBytes\":" << gpuReservedBytes
                  << ",\"peakGpuReservedBytes\":" << peakGpuReservedBytes << ",\"completedGpuReservations\":" << completedReservations
                  << ",\"freedGpuReservations\":" << freedReservations << ",\"failedGpuReservations\":" << failedReservations
                  << ",\"gpuReservationsReleasedAfterVmExit\":" << reservationsReleasedAfterVmExit
                  << ",\"gpuStateMappingOptIn\":" << (gpuStateEnabled ? "true" : "false")
                  << ",\"completedGpuStateMaps\":" << completedGpuStateMaps << ",\"failedGpuStateMaps\":" << failedGpuStateMaps
                  << ",\"completedGpuStateWaits\":" << completedGpuStateWaits
                  << ",\"gpuStateReplacedAllocationMappings\":" << gpuStateReplacedAllocationMappings
                  << ",\"liveGpuStateRanges\":" << gpuStates.size() << ",\"liveGpuStateBytes\":" << gpuStates.bytes()
                  << ",\"commandSubmissionOptIn\":" << (submitEnabled ? "true" : "false")
                  << ",\"nativeSubmissionAttempts\":" << submissionAttempts << ",\"completedNativeSubmissions\":" << completedSubmissions
                  << ",\"failedNativeSubmissions\":" << failedSubmissions << ",\"nativeSubmissionTimeouts\":" << submissionTimeouts
                  << ",\"submittedCommandBytes\":" << submittedCommandBytes
                  << ",\"livePagingQueues\":" << pagingFences.size() << ",\"liveAllocations\":" << allocations.size()
                  << ",\"allocatedBytes\":" << allocatedBytes << ",\"cleanupFailures\":" << cleanupFailures
                  << ",\"liveGuestMappings\":" << (guestMemory ? 1 : 0) << ",\"liveGuestSections\":" << (guestSection ? 1 : 0)
                  << ",\"liveGpuCopyObjects\":" << (!!copyDevice + !!copyHeap + !!copyQueue + !!copyFence + !!copyEvent)
                  << ",\"completedGpuCopies\":" << completedCopies << ",\"gpuCopiedBytes\":" << gpuCopiedBytes
                  << ",\"lastGpuCopyHresult\":" << static_cast<std::uint32_t>(lastCopyError)
                  << ",\"completedAdapterQueries\":" << completedQueries
                  << ",\"failedAdapterQueries\":" << failedQueries
                  << ",\"liveVendorAllocations\":" << vendorAllocations.size()
                  << ",\"peakVendorAllocationObjects\":" << peakVendorAllocationObjects
                  << ",\"vendorAllocationObjectLimit\":" << MaxVendorAllocations
                  << ",\"vendorCpuSlotLimit\":" << (runtime ? runtime->allocationSlotLimit() : DefaultVendorCpuSlots)
                  << ",\"vendorCpuMappedByteLimit\":" << vendorCpuByteLimit(capabilities().flags)
                  << ",\"vendorCpuSlotQuotaRejections\":" << vendorCpuSlotQuotaRejections
                  << ",\"completedVendorAllocations\":" << completedVendorAllocations
                  << ",\"completedVendorResources\":" << completedVendorResources
                  << ",\"destroyedVendorResources\":" << destroyedVendorResources
                  << ",\"liveVendorResources\":" << std::count_if(vendorAllocations.begin(), vendorAllocations.end(), [](const auto& item) { return item.second.resource != 0; })
                  << ",\"completedVendorDriverProtectionMaps\":" << completedVendorDriverProtectionMaps
                  << ",\"completedVendorUninitializedSourceAllocations\":" << uninitializedSourceAllocations
                  << ",\"destroyedVendorAllocations\":" << destroyedVendorAllocations
                  << ",\"allocationRetirementOptIn\":" << (retirementEnabled ? "true" : "false")
                  << ",\"vendorDestructionsWithHwQueues\":" << vendorDestructionsWithHwQueues
                  << ",\"failedVendorAllocations\":" << failedVendorAllocations
                  << ",\"completedVendorGpuVaMaps\":" << completedVendorMaps
                  << ",\"failedVendorGpuVaMaps\":" << failedVendorMaps
                  << ",\"completedVendorGpuVaWaits\":" << completedVendorMapWaits
                  << ",\"liveVendorMappedPages\":" << vendorMappedPages
                  << ",\"peakVendorMappedPages\":" << peakVendorMappedPages
                  << ",\"vendorGpuMappedByteLimit\":" << vendorGpuPageLimit(capabilities().flags) * 4096
                  << ",\"completedVendorResidencyRequests\":" << completedVendorResidency
                  << ",\"failedVendorResidencyRequests\":" << failedVendorResidency
                  << ",\"completedVendorResidencyWaits\":" << completedVendorResidencyWaits
                  << ",\"vendorAllocationsMadeResident\":" << vendorAllocationsMadeResident
                  << ",\"liveVendorResidencyAttempts\":" << vendorResidencyAttempts
                  << ",\"peakVendorResidencyAttempts\":" << peakVendorResidencyAttempts
                  << ",\"completedVendorCpuLocks\":" << completedVendorLocks
                  << ",\"completedVendorCpuUnlocks\":" << completedVendorUnlocks
                  << ",\"completedVendorCpuSubrangeLocks\":" << completedVendorSubrangeLocks
                  << ",\"failedVendorCpuLocks\":" << failedVendorLocks
                  << ",\"failedVendorCpuUnlocks\":" << failedVendorUnlocks
                  << ",\"liveVendorCpuBytes\":" << vendorCpuBytes
                  << ",\"peakVendorCpuBytes\":" << peakVendorCpuBytes
                  << ",\"vendorCpuContentsChanged\":" << vendorCpuContentsChanged
                  << ",\"cpuStoreTestRequested\":" << (cpuStoreTest ? "true" : "false")
                  << ",\"completedCpuStoreTests\":" << completedCpuStoreTests << ",\"failedCpuStoreTests\":" << failedCpuStoreTests
                  << ",\"cpuLocksReleasedAfterVmExit\":" << cpuLocksReleasedAfterVmExit
                  << ",\"completedAllocationTranslations\":" << completedTranslations << ",\"failedAllocationTranslations\":" << failedTranslations
                  << ",\"liveAllocationTranslations\":" << translatedAllocations.size()
                  << ",\"peakReportedGpuUsageBytes\":" << peakReportedGpuUsage
                  << ",\"reportedGpuUsageLimitBytes\":" << ReportedGpuUsageLimit
                  << ",\"hardVendorAllocationByteQuotaImplemented\":false}\n";
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
                  bool contexts = false, bool queries = false, driver_qemu::Runtime* runtime = nullptr, bool allocations = false, bool gpuVa = false, bool residency = false, bool cpu = false, bool cpuStoreTest = false, bool translation = false, bool hwQueues = false, bool sync = false, bool submit = false, bool retirement = false, bool reservation = false, bool gpuState = false) {
    KmtDriver driver(sectionName, sectionBytes, contexts, queries, runtime, allocations, gpuVa, residency, cpu, cpuStoreTest, translation, hwQueues, sync, submit, retirement, reservation, gpuState);
    std::exception_ptr failure;
    {
      Session session(driver);
      std::size_t peakWireObjects = 0;
      try {
      unsigned count = 0;
      const auto deadline = GetTickCount64() + 60000;
      for (; count < 10000; ++count) {
        if (runtime && GetTickCount64() >= deadline) throw std::runtime_error("Owned runtime diagnostic deadline exceeded");
        // A byte stream needs a length prefix outside the borrowed 16-byte
        // descriptor header. Both are little endian; target host/guest are x64.
        std::uint32_t size{};
        if (!stream.read(&size, sizeof size, true)) break;
        if (size < sizeof(Header) || size > native_gpu::MaxPacket) throw std::runtime_error("Invalid stream frame length");
        std::vector<std::uint8_t> packet(size); stream.read(packet.data(), packet.size());
        const auto reply = session.dispatch(packet);
        peakWireObjects = (std::max)(peakWireObjects, session.objectCount());
        if (reply.size() >= sizeof(Header)) {
            Header response{}; std::memcpy(&response, reply.data(), sizeof response);
            if (response.status == -24)
                std::cerr << "{\"wireQuotaRejection\":true,\"op\":" << response.type
                          << ",\"liveObjects\":" << session.objectCount()
                          << ",\"objectLimit\":" << MaxObjects << "}\n";
        }
        size = static_cast<std::uint32_t>(reply.size()); stream.write(&size, sizeof size);
        stream.write(reply.data(), reply.size());
      }
      if (count == 10000) throw std::runtime_error("Session request quota exceeded");
      } catch (...) { failure = std::current_exception(); }
      std::cerr << "{\"wireObjectSummary\":true,\"peakLiveObjects\":" << peakWireObjects
                << ",\"liveObjectsBeforeDisconnect\":" << session.objectCount()
                << ",\"objectLimit\":" << MaxObjects << "}\n";
      // Session destruction releases queue pages. A transport failure must
      // first stop the owned VM; graceful EOF waits for its normal poweroff.
      if (runtime) runtime->stop(failure ? 0 : 5000);
    }
    driver.releaseGuestMemory(); driver.reportCleanup();
    if (!driver.clean()) throw std::runtime_error("Driver object cleanup failed");
    if (failure) std::rethrow_exception(failure);
}
int main(int argc, char** argv) {
    try {
        // Explicit experimental opt-in; keep the existing allocation endpoint
        // closed to vendor-private context data unless requested by its owner.
        bool contexts = false, queries = false, allocations = false, gpuVa = false, residency = false, cpu = false, cpuStoreTest = false, cpuEofTest = false, translation = false, hwQueues = false, hwQueueEofTest = false, sync = false, syncEofTest = false, submit = false, retirement = false, reservation = false, reservationEofTest = false, gpuState = false, gpuStateEofTest = false, cpuSpanEofTest = false, syncNoMaxEofTest = false;
        std::size_t cpuSlots = DefaultVendorCpuSlots;
        bool cpuSlotsConfigured = false;
        bool hwQueueNoBroadcastEofTest = false;
        bool hwQueueNoBroadcastWaitEofTest = false;
        while (argc > 1) {
            if (argc > 2 && std::string(argv[argc - 2]) == "--driver-cpu-slots") {
                const auto value = std::string(argv[argc - 1]);
                if (cpuSlotsConfigured || (value != "16" && value != "32" && value != "64" && value != "128"))
                    throw std::runtime_error("CPU aperture slots must be specified once as 16, 32, 64 or 128");
                cpuSlots = static_cast<std::size_t>(std::stoul(value)); cpuSlotsConfigured = true;
                argc -= 2; continue;
            }
            const auto option = std::string(argv[argc - 1]);
            if (option == "--driver-contexts" && !contexts) contexts = true;
            else if (option == "--driver-queries" && !queries) queries = true;
            else if (option == "--driver-allocations" && !allocations) allocations = true;
            else if (option == "--driver-gpuva" && !gpuVa) gpuVa = true;
            else if (option == "--driver-residency" && !residency) residency = true;
            else if (option == "--driver-cpu" && !cpu) cpu = true;
            else if (option == "--cpu-store-test" && !cpuStoreTest) cpuStoreTest = true;
            else if (option == "--cpu-eof-test" && !cpuEofTest) cpuEofTest = true;
            else if (option == "--cpu-span-eof-test" && !cpuSpanEofTest) cpuSpanEofTest = true;
            else if (option == "--driver-translation" && !translation) translation = true;
            else if (option == "--driver-hwqueues" && !hwQueues) hwQueues = true;
            else if (option == "--hwqueue-eof-test" && !hwQueueEofTest) hwQueueEofTest = true;
            else if (option == "--hwqueue-no-broadcast-eof-test" && !hwQueueNoBroadcastEofTest) hwQueueNoBroadcastEofTest = true;
            else if (option == "--hwqueue-no-broadcast-wait-eof-test" && !hwQueueNoBroadcastWaitEofTest) hwQueueNoBroadcastWaitEofTest = true;
            else if (option == "--sync-eof-test" && !syncEofTest) syncEofTest = true;
            else if (option == "--sync-no-max-eof-test" && !syncNoMaxEofTest) syncNoMaxEofTest = true;
            else if (option == "--driver-syncs" && !sync) sync = true;
            else if (option == "--driver-submit" && !submit) submit = true;
            else if (option == "--driver-retirement" && !retirement) retirement = true;
            else if (option == "--driver-reservation" && !reservation) reservation = true;
            else if (option == "--reservation-eof-test" && !reservationEofTest) reservationEofTest = true;
            else if (option == "--driver-gpu-state" && !gpuState) gpuState = true;
            else if (option == "--gpu-state-eof-test" && !gpuStateEofTest) gpuStateEofTest = true;
            else break;
            --argc;
        }
        if (allocations && (!contexts || !queries)) throw std::runtime_error("Vendor allocations require explicit query/context opt-ins");
        if (gpuVa && !allocations) throw std::runtime_error("Vendor GPU-address mappings require explicit allocation opt-in");
        if (residency && !allocations) throw std::runtime_error("Vendor residency requires explicit allocation opt-in");
        if (translation && !allocations) throw std::runtime_error("Allocation translation requires explicit allocation opt-in");
        if (hwQueues && (!contexts || !translation || argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("Hardware queues require context and allocation translation opt-ins in an owned QEMU runtime");
        if (sync && (argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("Synchronization objects require an owned QEMU runtime");
        if (retirement && !hwQueues)
            throw std::runtime_error("Allocation retirement requires owned hardware queues");
        if (reservation && (!gpuVa || argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("GPU reservation requires GPU-address mappings in an owned QEMU runtime");
        if (syncNoMaxEofTest && (!sync || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest || reservationEofTest || gpuStateEofTest || cpuSpanEofTest))
            throw std::runtime_error("NoSignalMaxValueOnTdr EOF control requires syncs and a separate diagnostic run");
        if (reservationEofTest && (!reservation || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest))
            throw std::runtime_error("Reservation EOF control requires reservation opt-in and a separate diagnostic run");
        if (gpuState && !reservation) throw std::runtime_error("GPU state mappings require the owned reservation opt-in");
        if (cpuSpanEofTest && (!cpu || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest || reservationEofTest || gpuStateEofTest))
            throw std::runtime_error("CPU span EOF control requires CPU locks and a separate diagnostic run");
        if (gpuStateEofTest && (!gpuState || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest || reservationEofTest))
            throw std::runtime_error("GPU state EOF control requires state mappings and a separate diagnostic run");
        if (submit && (!hwQueues || !sync || !cpu || !residency || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest || reservationEofTest || gpuStateEofTest || cpuSpanEofTest || syncNoMaxEofTest))
            throw std::runtime_error("Submission requires owned queues, syncs, CPU mappings and residency in a separate diagnostic run");
        if (cpu && (!gpuVa || argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("Vendor CPU locks require GPU-address mappings and an owned QEMU runtime");
        if (cpuStoreTest && !cpu) throw std::runtime_error("CPU store control requires explicit CPU-lock opt-in");
        if (cpuSlotsConfigured && !cpu) throw std::runtime_error("CPU aperture capacity requires the explicit CPU-lock opt-in");
        if (syncEofTest && (!sync || cpuStoreTest || cpuEofTest || hwQueueEofTest))
            throw std::runtime_error("Synchronization EOF control requires synchronization opt-in and a separate diagnostic run");
        if (cpuEofTest && (!cpu || cpuStoreTest)) throw std::runtime_error("CPU EOF control requires CPU locks and a separate run from store control");
        if (hwQueueEofTest && (!hwQueues || cpuEofTest || cpuStoreTest))
            throw std::runtime_error("Hardware queue EOF control requires hardware queues and a separate diagnostic run");
        if (hwQueueNoBroadcastEofTest && (!hwQueues || !submit || !retirement || cpuStoreTest || cpuEofTest || hwQueueEofTest ||
            syncEofTest || reservationEofTest || gpuStateEofTest || cpuSpanEofTest || syncNoMaxEofTest))
            throw std::runtime_error("NoBroadcastSignal queue EOF control requires submission, retirement and a separate diagnostic run");
        if (hwQueueNoBroadcastWaitEofTest && (!hwQueues || !submit || !retirement || cpuStoreTest || cpuEofTest || hwQueueEofTest ||
            syncEofTest || reservationEofTest || gpuStateEofTest || cpuSpanEofTest || syncNoMaxEofTest || hwQueueNoBroadcastEofTest))
            throw std::runtime_error("NoBroadcastWait queue EOF control requires submission, retirement and a separate diagnostic run");
        if (argc == 2 && std::string(argv[1]) == "--stdio") {
            if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
                throw std::runtime_error("Cannot set binary stdio mode");
            Stream stream; serve(stream, {}, 0, contexts, queries, nullptr, allocations, gpuVa, residency, false, false, translation); return 0;
        }
        const bool ownedRuntime = argc == 7 && std::string(argv[1]) == "--run-qemu";
        if (!ownedRuntime && ((argc != 3 && argc != 7) || std::string(argv[1]) != "--listen")) {
            std::cerr << "Usage: driver-bridge.exe --stdio | --listen port [--guest-section name --guest-ram-bytes count] "
                "[--driver-contexts] [--driver-queries] [--driver-allocations] [--driver-gpuva] [--driver-residency] [--driver-cpu] [--driver-cpu-slots 16|32|64|128] [--driver-translation] [--driver-hwqueues] [--driver-syncs] [--driver-submit] [--driver-retirement] [--driver-reservation] [--driver-gpu-state] [--cpu-store-test | --cpu-eof-test | --cpu-span-eof-test | --hwqueue-eof-test | --hwqueue-no-broadcast-eof-test | --hwqueue-no-broadcast-wait-eof-test | --sync-eof-test | --sync-no-max-eof-test | --reservation-eof-test | --gpu-state-eof-test]\n"
                "       driver-bridge.exe --run-qemu qemu firmware kernel initramfs fresh-log --driver-contexts --driver-queries\n"; return 2;
        }
        if (ownedRuntime && (!contexts || !queries)) throw std::runtime_error("Owned QEMU runtime requires explicit query/context opt-ins");
        std::size_t end{}; const auto port = ownedRuntime ? 0ul : std::stoul(argv[2], &end);
        if (!ownedRuntime && (end != std::string(argv[2]).size() || port > 65535)) throw std::runtime_error("Invalid port");
        std::string sectionName; std::uint32_t sectionBytes = 0;
        if (argc == 7 && !ownedRuntime) {
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
        std::unique_ptr<driver_qemu::Runtime> runtime;
        if (ownedRuntime) runtime = std::make_unique<driver_qemu::Runtime>(ntohs(address.sin_port),
            qemu_fence::Paths{argv[2], argv[3], argv[4], argv[5], argv[6]}, cpu, cpuStoreTest, cpuEofTest, hwQueueEofTest, cpuSlots, syncEofTest, reservationEofTest, gpuStateEofTest, cpuSpanEofTest, syncNoMaxEofTest, hwQueueNoBroadcastEofTest, hwQueueNoBroadcastWaitEofTest);
        fd_set reads; FD_ZERO(&reads); FD_SET(listener.value, &reads); timeval timeout{60, 0};
        if (select(0, &reads, nullptr, nullptr, &timeout) != 1) throw std::runtime_error("Connection timed out");
        Socket client; client.value = accept(listener.value, nullptr, nullptr);
        if (client.value == INVALID_SOCKET) throw std::runtime_error("accept failed");
        const DWORD milliseconds = 30000;
        if (setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds) ||
            setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds))
            throw std::runtime_error("Cannot set socket timeouts");
        Stream stream(client.value); serve(stream, sectionName, sectionBytes, contexts, queries, runtime.get(), allocations, gpuVa, residency, cpu, cpuStoreTest, translation, hwQueues, sync, submit, retirement, reservation, gpuState);
        if (runtime) {
            runtime->report();
            if (!runtime->cleanExit()) throw std::runtime_error("Owned QEMU runtime did not exit cleanly");
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
