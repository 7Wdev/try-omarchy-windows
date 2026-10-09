// SPDX-License-Identifier: MIT
// Independent, deliberately small WDDM experiment, not the /dev/dxg or RM ABI.
#pragma once
#include "wire.h"
#include "gpu_state_ranges.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace driver_bridge {
using native_gpu::Header;
constexpr std::uint32_t Version = 1;
// The live NVIDIA D3D12 runtime needs a fifth paging queue with 127
// objects already owned. Keep the aggregate cap separate from the tighter
// per-kind and byte budgets, including two handles per native queue.
constexpr std::size_t MaxObjects = 256;
constexpr std::uint32_t MaxAllocation = 1024 * 1024;
constexpr std::uint32_t MaxAllocatedBytes = 16 * 1024 * 1024;
constexpr std::uint32_t MaxChunk = 4064;
constexpr std::uint32_t ContextCapability = 32;
constexpr std::uint32_t MaxContextPrivateBytes = 4000;
constexpr std::uint32_t QueryCapability = 64;
constexpr std::uint32_t GuestPagingCapability = 128;
constexpr std::uint32_t VendorAllocationCapability = 256;
constexpr std::uint32_t VendorResourceCapability = 524288;
constexpr std::uint32_t MaxVendorPrivateBytes = 4000;
constexpr std::size_t MaxVendorAllocations = 96;
constexpr std::size_t DefaultVendorCpuSlots = 16;
constexpr std::size_t MaxVendorCpuSlots = 64;
inline bool validVendorCpuSlots(std::size_t slots) {
    return slots >= DefaultVendorCpuSlots && slots <= MaxVendorCpuSlots && !(slots & (slots - 1));
}
constexpr std::uint32_t VendorGpuVaCapability = 512;
constexpr std::uint32_t VendorResidencyCapability = 1024;
constexpr std::uint32_t VendorCpuCapability = 2048;
constexpr std::uint32_t VendorTranslationCapability = 4096;
constexpr std::uint32_t HwQueueCapability = 8192;
constexpr std::uint32_t SyncCapability = 16384;
// Synchronization mutexes consume no fence BAR slots. Live queue startup
// exceeds 64 independent objects while staying inside the 64-page BAR.
constexpr std::size_t MaxSyncObjects = 96;
constexpr std::uint32_t NoSignalMaxValueOnTdrSyncFlag = 64;
constexpr std::uint32_t NoGpuAccessSyncFlag = 128;
constexpr std::uint32_t ContextSignalCapability = 1048576;
constexpr std::uint32_t MaxContextSignals = 16;
struct ContextSignalDesc { std::uint32_t sync, flags; std::uint64_t fence; };
static_assert(sizeof(ContextSignalDesc) == 16, "fixed context signal layout");
inline bool validContextSignal(ContextSignalDesc d) { return d.sync && d.flags == 4 && d.fence && d.fence != UINT64_MAX; }
struct SyncDesc { std::uint32_t type, flags, affinity, reserved; std::uint64_t initial; };
struct SyncReply { std::uint64_t offset, gpuAddress; };
static_assert(sizeof(SyncDesc) == 24 && sizeof(SyncReply) == 16, "fixed synchronization layouts");
inline bool validSync(SyncDesc d) {
    return !d.reserved && ((d.type == 1 && !d.flags && !d.affinity && d.initial <= 1) ||
           (d.type == 5 && (d.flags == 0 || d.flags == NoSignalMaxValueOnTdrSyncFlag || d.flags == NoGpuAccessSyncFlag) && d.affinity <= 1));
}
constexpr std::size_t MaxHwQueues = 8;
constexpr std::uint32_t NoBroadcastSignalHwQueueCapability = 2097152;
constexpr std::uint32_t NoBroadcastSignalHwQueueFlag = 2;
struct HwQueueDesc { std::uint32_t flags, privateBytes, reserved, reserved2; };
struct HwQueueReply { std::uint32_t sync, reserved; std::uint64_t offset, gpuAddress; };
static_assert(sizeof(HwQueueDesc) == 16 && sizeof(HwQueueReply) == 24, "fixed hardware queue layouts");
inline bool validHwQueue(HwQueueDesc d) {
    return (d.flags == 0 || d.flags == NoBroadcastSignalHwQueueFlag) && d.privateBytes && d.privateBytes <= 4000 && !d.reserved && !d.reserved2;
}
struct VendorTranslationDesc { std::uint32_t device, adapter, reserved, reserved2; };
static_assert(sizeof(VendorTranslationDesc) == 16, "fixed allocation translation layout");
constexpr std::uint64_t VendorCpuSlotBytes = MaxAllocation;
constexpr std::uint32_t MaxVendorCpuMappingBytes = 4 * MaxAllocation;
constexpr std::uint64_t VendorCpuApertureBytes = MaxVendorCpuSlots * VendorCpuSlotBytes;
constexpr std::uint64_t CpuStoreFirstMarker = 0x4350554649525354ull, CpuStoreLastMarker = 0x4350554c41535421ull;
struct VendorCpuDesc { std::uint32_t device, flags; };
struct VendorCpuReply { std::uint32_t bytes, reserved; std::uint64_t generation; };
static_assert(sizeof(VendorCpuDesc) == 8 && sizeof(VendorCpuReply) == 16, "fixed CPU lock layouts");
inline bool validVendorCpuReply(std::uint64_t offset, VendorCpuReply r) {
    return r.bytes && r.bytes <= MaxVendorCpuMappingBytes && !(r.bytes % 4096) && !r.reserved && r.generation &&
           !(offset % VendorCpuSlotBytes) && offset < VendorCpuApertureBytes && r.bytes <= VendorCpuApertureBytes - offset;
}
constexpr std::uint32_t MaxVendorResidencyAttempts = 64;
struct ResidentDesc { std::uint32_t count, flags, priorities, reserved; };
static_assert(sizeof(ResidentDesc) == 16, "fixed residency descriptor");
inline bool validResident(ResidentDesc d) {
    // CantTrimFurther is used by the live runtime. MustSucceed can force
    // device loss and is deliberately absent from this diagnostic.
    return d.count && d.count <= MaxVendorAllocations && d.flags <= 1 && d.priorities <= 1 && !d.reserved;
}
struct ResidentReply { std::uint32_t count, reserved; std::uint64_t bytesToTrim; };
static_assert(sizeof(ResidentReply) == 16, "fixed residency output");
constexpr std::uint64_t MaxGpuAddress = 1ull << 48;
inline bool validSyncGpuAddress(SyncDesc desc, std::uint64_t address) {
    // NoGPUAccess fences have CPU storage but no GPU virtual address.
    return desc.flags == NoGpuAccessSyncFlag ? address == 0 : address && !(address % 8) && address < MaxGpuAddress;
}
constexpr std::uint32_t HwSubmitCapability = 32768;
constexpr std::uint32_t VendorRetirementCapability = 65536;
constexpr std::uint32_t GpuReservationCapability = 131072;
constexpr std::uint32_t GpuStateCapability = 262144;
constexpr std::size_t MaxGpuReservations = 8;
constexpr std::uint64_t MaxGpuReservationBytes = 4ull * 1024 * 1024 * 1024;
constexpr std::uint64_t MaxGpuReservedBytes = 4 * MaxGpuReservationBytes;
struct GpuReservationDesc { std::uint64_t base, minimum, maximum, bytes; };
struct FreeGpuReservationDesc { std::uint32_t adapter, reserved; };
static_assert(sizeof(GpuReservationDesc) == 32 && sizeof(FreeGpuReservationDesc) == 8, "fixed GPU reservation layouts");
inline bool validGpuReservation(GpuReservationDesc d) {
    constexpr auto alignment = 65536ull;
    if (!d.bytes || d.bytes % alignment || d.bytes > MaxGpuReservationBytes ||
        d.base % alignment || d.minimum % alignment || d.maximum % alignment ||
        d.base >= MaxGpuAddress || d.minimum >= MaxGpuAddress || d.maximum > MaxGpuAddress) return false;
    if (d.base) return d.base <= MaxGpuAddress - d.bytes;
    const auto end = d.maximum ? d.maximum : MaxGpuAddress;
    return d.minimum <= end && d.bytes <= end - d.minimum;
}
inline bool validGpuReservationOutput(GpuReservationDesc d, std::uint64_t address) {
    if (!validGpuReservation(d) || !address || address % 65536 || address > MaxGpuAddress - d.bytes) return false;
    return d.base ? address == d.base : address >= d.minimum && (!d.maximum || address + d.bytes <= d.maximum);
}
inline bool gpuRangesOverlap(std::uint64_t a, std::uint64_t bytes, std::uint64_t b, std::uint64_t otherBytes) {
    return a && bytes && b && otherBytes && a < b + otherBytes && b < a + bytes;
}
inline bool gpuRangeContains(std::uint64_t a, std::uint64_t bytes, std::uint64_t b, std::uint64_t otherBytes) {
    return b >= a && otherBytes <= bytes && b - a <= bytes - otherBytes;
}
constexpr std::uint32_t MaxHwSubmissions = 16;
struct HwSubmitDesc {
    std::uint64_t address, fence;
    std::uint32_t bytes, privateBytes, primaries, reserved;
};
static_assert(sizeof(HwSubmitDesc) == 32, "fixed command submission layout");
inline bool validHwSubmit(HwSubmitDesc d) {
    return d.address && !(d.address % 4096) && d.address < MaxGpuAddress &&
           d.bytes && !(d.bytes % 4096) && d.bytes <= MaxAllocation && d.address <= MaxGpuAddress - d.bytes &&
           d.fence && d.fence != UINT64_MAX && d.privateBytes <= 4000 && !d.primaries && !d.reserved;
}
// GPU-only mappings do not consume the separate 1 MiB CPU aperture slots.
constexpr std::uint32_t MaxVendorMapPages = 4 * MaxAllocation / 4096;
// A successful device occupies almost all of the old 16 MiB GPU map budget;
// queue startup then needs another 4 MiB allocation. CPU views keep their
// independent 16 MiB cap and native reported usage keeps its 64 MiB guard.
constexpr std::uint32_t MaxVendorGpuMappedBytes = 32 * 1024 * 1024;
constexpr std::uint32_t MaxVendorMappedPages = MaxVendorGpuMappedBytes / 4096;
struct GpuVaDesc {
    std::uint32_t queue, reserved;
    std::uint64_t base, minimum, maximum, offsetPages, sizePages, protection, driverProtection;
};
// Opaque NVIDIA mapping value observed for a nonshared D3D12 DEFAULT resource.
constexpr std::uint64_t VendorDefaultDriverProtection = 0x10000001;
static_assert(sizeof(GpuVaDesc) == 64, "fixed GPU-address descriptor");
inline bool validGpuVa(GpuVaDesc d) {
    if (!d.queue || d.reserved || !d.sizePages || d.sizePages > MaxVendorMapPages ||
        d.offsetPages > MaxVendorMapPages - d.sizePages || d.protection > 3 ||
        (d.driverProtection && (d.driverProtection != VendorDefaultDriverProtection || d.protection != 1)) ||
        d.base % 4096 || d.minimum % 4096 || d.maximum % 4096 ||
        d.base >= MaxGpuAddress || d.minimum >= MaxGpuAddress || d.maximum > MaxGpuAddress) return false;
    const auto bytes = d.sizePages * 4096;
    if (d.base) return d.base <= MaxGpuAddress - bytes;
    const auto end = d.maximum ? d.maximum : MaxGpuAddress;
    return d.minimum <= end && bytes <= end - d.minimum;
}
inline bool validGpuVaOutput(GpuVaDesc d, std::uint64_t address) {
    if (!validGpuVa(d) || !address || address % 4096 || address > MaxGpuAddress - d.sizePages * 4096) return false;
    if (d.base) return address == d.base;
    return address >= d.minimum && (!d.maximum || address + d.sizePages * 4096 <= d.maximum);
}
inline bool validGpuState(GpuVaDesc d) {
    const auto state = d.protection & 12;
    return d.queue && !d.reserved && d.base && !d.offsetPages && !d.driverProtection &&
           !(d.protection & ~15ull) && (state == 4 || state == 8) &&
           d.sizePages && d.sizePages <= MaxGpuReservationBytes / 4096 &&
           !(d.base % 4096) && !(d.minimum % 4096) && !(d.maximum % 4096) &&
           d.base < MaxGpuAddress && d.minimum < MaxGpuAddress && d.maximum <= MaxGpuAddress &&
           d.base <= MaxGpuAddress - d.sizePages * 4096;
}
struct GpuVaReply { std::uint64_t fence; };
static_assert(sizeof(GpuVaReply) == 8, "fixed GPU-address fence reply");
struct VendorAllocationDesc {
    std::uint32_t flags, priority, source, privateBytes, reserved, reserved2;
};
static_assert(sizeof(VendorAllocationDesc) == 24, "fixed vendor allocation layout");
constexpr std::uint32_t UninitializedDisplaySource = UINT32_MAX;
inline bool validVendorAllocation(VendorAllocationDesc d) {
    // Standalone video memory only. No primary, stereo, resource sharing,
    // system-memory pointer or host CPU address crosses this interface.
    // An unset source is allowed for a non-primary allocation. It does not
    // select a display source and is normalized to NOTAPPLICABLE on Windows.
    return (d.source == 0 || d.source == UninitializedDisplaySource) &&
           !d.reserved && !d.reserved2 && d.privateBytes && d.privateBytes <= MaxVendorPrivateBytes &&
           ((d.flags == 0 && d.priority == 0) ||
            (d.flags == 4 && d.priority >= 0x28000000u && d.priority <= 0xc8000000u));
}
struct DestroyVendorDesc { std::uint32_t count, reserved; };
static_assert(sizeof(DestroyVendorDesc) == 8, "fixed vendor destruction layout");
struct VendorResourceReply { std::uint32_t resource, reserved; };
static_assert(sizeof(VendorResourceReply) == 8, "fixed resource ownership reply");
constexpr std::uint64_t FenceApertureBytes = 64 * 4096;
constexpr std::uint32_t MaxQueryBytes = 65536;
constexpr std::size_t MaxActiveQueries = 2;
struct QueryDesc { std::uint32_t type, bytes, reserved, reserved2; };
static_assert(sizeof(QueryDesc) == 16, "fixed query layout");
inline bool validQuery(QueryDesc d) {
    if (d.reserved || d.reserved2 || !d.bytes || d.bytes > MaxQueryBytes) return false;
    // Inline WDDM x64/UTF-16 layouts observed during Linux D3D12 startup.
    // Pointer-bearing PnP queries, registry queries and SetWorkingSet are
    // deliberately absent. Opaque UMD data requires an explicit host opt-in.
    switch (d.type) {
        case 0: return true;
        case 1: return d.bytes == 524;
        case 3: return d.bytes == 24;
        case 13: case 15: case 24: case 27: case 30: case 55: case 56: return d.bytes == 4;
        case 17: case 34: return d.bytes == 12;
        case 18: return d.bytes == 8;
        case 31: return d.bytes == 28;
        case 60: return d.bytes == 80;
        case 61: return d.bytes == 56;
        case 62: return d.bytes == 64;
        case 66: return d.bytes == 8192;
        default: return false;
    }
}
enum class Op : std::uint32_t {
    Hello = 0x2000, OpenAdapter, QueryDriverVersion, CloseAdapter,
    CreateDevice, DestroyDevice, CreatePagingQueue, ReadPagingFence, DestroyPagingQueue,
    CreateAllocation = 0x2010, WriteAllocation, ReadAllocation, MakeResident,
    MapAllocation, QueryResidency, DestroyAllocation, CreateSharedAllocation, CopySharedAllocation,
    CreateContext = 0x2020, DestroyContext, SetContextInProcessPriority,
    BeginAdapterQuery = 0x2030, WriteAdapterQuery, RunAdapterQuery, ReadAdapterQuery, EndAdapterQuery,
    CreateGuestPagingQueue = 0x2040,
    CreateVendorAllocation = 0x2050, DestroyVendorAllocations, MapVendorAllocation, MakeVendorResident,
    LockVendorAllocation, UnlockVendorAllocation, TranslateVendorAllocation,
    CreateVendorResourceAllocation, DestroyVendorResource,
    CreateHwQueue = 0x2060, DestroyHwQueue, SubmitHwQueue,
    CreateSync = 0x2070, DestroySync, SignalContextSync,
    ReserveGpuAddress = 0x2080, FreeGpuReservation, MapGpuState
};
enum class Kind { Adapter, Device, PagingQueue, Allocation, Context, PagingSync, VendorAllocation, HwQueue, HwQueueSync, Sync, GpuReservation, VendorResource };
struct ContextDesc {
    std::uint32_t node, engine, flags, clientHint, privateBytes, reserved;
};
static_assert(sizeof(ContextDesc) == 24, "fixed context layout");
inline bool validContext(ContextDesc d) {
    if (d.reserved || d.privateBytes > MaxContextPrivateBytes) return false;
    // Synchronization-only contexts do not require vendor initialization data.
    if (d.flags == 8) return d.node == 0 && d.engine == 0 && d.clientHint == 0 && !d.privateBytes;
    // Initial graphics bridge scope: D3D12 virtual contexts. In particular,
    // never forward DisableGpuTimeout, TestContext or unknown flag bits.
    return d.node < 64 && d.engine == 1 && (d.flags == 0 || d.flags == 16) &&
           d.clientHint == 12 && d.privateBytes;
}
inline bool validContextPriority(std::int32_t priority) { return priority == 0 || priority == 1; }
struct Range { std::uint32_t offset; std::uint32_t size; };
static_assert(sizeof(Range) == 8, "fixed transfer layout");
struct GuestRange { std::uint64_t offset; std::uint32_t size; std::uint32_t reserved; };
static_assert(sizeof(GuestRange) == 16, "fixed guest range layout");
struct CopyRange { std::uint32_t source; std::uint32_t sourceOffset; std::uint32_t destinationOffset; std::uint32_t size; };
static_assert(sizeof(CopyRange) == 16, "fixed GPU copy layout");
struct Result { std::int32_t ntstatus; std::uint32_t nativeHandle; std::uint64_t value; };
struct GpuVaResult { Result map; std::uint64_t fence; };
struct ResidentResult { Result residency; ResidentReply output; };
struct VendorCpuResult { Result lock; VendorCpuReply output; };
struct VendorResourceResult { Result allocation; std::uint32_t resource; };
// The synchronization object is borrowed from the paging queue. Destruction
// of the queue owns its lifetime; no native handle is exposed to the guest.
struct GuestPagingResult { Result queue; std::uint32_t sync; std::uint64_t offset; };
struct HwQueueResult { Result queue; std::uint32_t sync; std::uint64_t offset, gpuAddress; };
struct SyncResult { Result object; SyncReply output; };
struct PagingReply { std::uint32_t sync, reserved; std::uint64_t offset; };
static_assert(sizeof(PagingReply) == 16, "fixed guest paging reply");
// Native KMT object fields use local IDs, never raw handles/pointers. Opt-in
// context private data is opaque and requires a compatible PV-aware UMD.
// NTSTATUS is preserved in
// this fixed payload; Header.status is a negative errno for protocol errors.
struct Reply { std::int32_t ntstatus; std::uint32_t reserved; std::uint64_t value; };
static_assert(sizeof(Reply) == 16, "fixed reply layout");
class Driver {
public:
    virtual ~Driver() = default;
    virtual native_gpu::Capabilities capabilities() const = 0;
    virtual Result openAdapter() = 0;
    virtual Result queryVersion(std::uint32_t adapter) = 0;
    virtual Result queryAdapter(std::uint32_t adapter, QueryDesc desc, std::vector<std::uint8_t>& data) = 0;
    virtual Result createDevice(std::uint32_t adapter) = 0;
    virtual Result createContext(std::uint32_t device, ContextDesc desc, std::vector<std::uint8_t>& data) = 0;
    virtual Result setContextInProcessPriority(std::uint32_t, std::int32_t) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result createPagingQueue(std::uint32_t device) = 0;
    virtual Result createVendorAllocation(std::uint32_t, VendorAllocationDesc, std::vector<std::uint8_t>&) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual VendorResourceResult createVendorResourceAllocation(std::uint32_t, VendorAllocationDesc, std::vector<std::uint8_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0};
    }
    virtual Result destroyVendorAllocations(std::uint32_t, const std::vector<std::uint32_t>&) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual GpuVaResult mapVendorAllocation(std::uint32_t, std::uint32_t, GpuVaDesc) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0};
    }
    virtual ResidentResult makeVendorResident(std::uint32_t, ResidentDesc, const std::vector<std::uint32_t>&,
                                            const std::vector<std::uint32_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, {0, 0, 0}};
    }
    virtual GuestPagingResult createGuestPagingQueue(std::uint32_t) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0, 0};
    }
    virtual VendorCpuResult lockVendorAllocation(std::uint32_t, std::uint32_t) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, {0, 0, 0}};
    }
    virtual Result unlockVendorAllocation(std::uint32_t, std::uint32_t) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result translateVendorAllocation(std::uint32_t, std::uint32_t, std::uint32_t) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual HwQueueResult createHwQueue(std::uint32_t, HwQueueDesc, std::vector<std::uint8_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0, 0, 0};
    }
    virtual SyncResult createSync(std::uint32_t, SyncDesc) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, {0, 0}};
    }
    virtual Result signalContextSync(std::uint32_t, std::uint32_t, ContextSignalDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result submitHwQueue(std::uint32_t, HwSubmitDesc, const std::vector<std::uint8_t>&) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result reserveGpuAddress(std::uint32_t, GpuReservationDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual GpuVaResult mapGpuState(std::uint32_t, std::uint32_t, GpuVaDesc) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0};
    }
    virtual Result readPagingFence(std::uint32_t queue) = 0;
    virtual Result createAllocation(std::uint32_t device, std::uint32_t size) = 0;
    virtual Result createSharedAllocation(std::uint32_t device, GuestRange range) = 0;
    virtual Result copySharedAllocation(std::uint32_t destination, std::uint32_t source, CopyRange range) = 0;
    virtual Result writeAllocation(std::uint32_t allocation, Range range, const std::uint8_t* data) = 0;
    virtual Result readAllocation(std::uint32_t allocation, Range range, std::vector<std::uint8_t>& data) = 0;
    virtual Result makeResident(std::uint32_t allocation, std::uint32_t queue) = 0;
    virtual Result mapAllocation(std::uint32_t allocation, std::uint32_t queue) = 0;
    virtual Result queryResidency(std::uint32_t allocation) = 0;
    virtual Result destroy(Kind kind, std::uint32_t handle) = 0;
};
class Session {
    struct Object {
        Kind kind; std::uint32_t parent; std::uint32_t nativeHandle; std::uint32_t size;
        std::uint64_t guestOffset; bool shared;
        std::uint32_t gpuPages = 0;
        std::uint64_t gpuAddress = 0;
        std::uint32_t residencyAttempts = 0;
        std::uint64_t gpuOffsetPages = 0;
        std::uint32_t cpuBytes = 0;
        std::uint64_t cpuOffset = 0;
        std::uint32_t driverToken = 0;
        std::uint32_t contextFlags = 0;
        std::uint32_t syncType = 0;
        std::uint64_t reservedGpuBytes = 0;
        bool resident = false;
        std::uint64_t submittedFence = 0;
        std::uint32_t vendorResource = 0;
        std::uint32_t syncFlags = 0;
    };
    Driver& driver;
    std::map<std::uint32_t, Object> objects;
    struct Query {
        QueryDesc desc; std::vector<std::uint8_t> data;
        std::uint32_t written = 0; bool executed = false; Result result{};
    };
    std::map<std::uint32_t, Query> queries;
    std::uint32_t nextId = 1;
    std::uint32_t allocatedBytes = 0;
    std::uint32_t vendorMappedPages = 0;
    std::uint32_t vendorCpuBytes = 0;
    std::uint32_t submissionAttempts = 0;
    std::uint32_t contextSignalAttempts = 0;
    std::uint64_t gpuReservedBytes = 0;
    GpuStateRanges gpuStates;
    bool negotiated = false;
    static std::vector<std::uint8_t> reply(Header h, std::int32_t error,
                                         std::uint32_t id = 0, const Result* result = nullptr) {
        h.handle = id; h.status = error; h.padding = 0;
        std::vector<std::uint8_t> out(sizeof h + (result ? sizeof(Reply) : 0));
        std::memcpy(out.data(), &h, sizeof h);
        if (result) {
            const Reply body{result->ntstatus, 0, result->value};
            std::memcpy(out.data() + sizeof h, &body, sizeof body);
        }
        return out;
    }
    std::vector<std::uint8_t> insert(Header h, Kind kind, std::uint32_t parent, Result result,
                                   std::uint32_t size = 0, std::uint64_t guestOffset = 0, bool shared = false, std::uint32_t contextFlags = 0) {
        if (result.ntstatus < 0) return reply(h, 0, 0, &result);
        if (!result.nativeHandle) return reply(h, -5);
        const auto id = nextId++;
        Object object{kind, parent, result.nativeHandle, size, guestOffset, shared}; object.contextFlags = contextFlags;
        objects.emplace(id, object);
        allocatedBytes += size;
        return reply(h, 0, id, &result);
    }
public:
    explicit Session(Driver& d) : driver(d) {}
    std::size_t objectCount() const { return objects.size(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // A disconnect releases child objects before their parents. The Windows
    // worker is also one process per connection, so OS teardown is a backstop.
    ~Session() {
        queries.clear();
        // Queue-private data may reference any allocation on its device.
        // Destroy queues before allocations even when allocations were created
        // later. Borrowed progress syncs are released by their owning queue.
        for (auto i = objects.rbegin(); i != objects.rend(); ++i)
            if (i->second.kind == Kind::HwQueue || i->second.kind == Kind::HwQueueSync)
                driver.destroy(i->second.kind, i->second.nativeHandle);
        for (auto i = objects.rbegin(); i != objects.rend(); ++i)
            if (i->second.kind != Kind::HwQueue && i->second.kind != Kind::HwQueueSync &&
                i->second.kind != Kind::GpuReservation && i->second.kind != Kind::Adapter)
                driver.destroy(i->second.kind, i->second.nativeHandle);
        // Reserved process address ranges outlive their allocation mappings,
        // but must be released before their adapter is closed.
        for (auto i = objects.rbegin(); i != objects.rend(); ++i)
            if (i->second.kind == Kind::GpuReservation) driver.destroy(i->second.kind, i->second.nativeHandle);
        for (auto i = objects.rbegin(); i != objects.rend(); ++i)
            if (i->second.kind == Kind::Adapter) driver.destroy(i->second.kind, i->second.nativeHandle);
    }
    std::vector<std::uint8_t> dispatch(const std::vector<std::uint8_t>& packet) {
        if (packet.size() < sizeof(Header)) return {};
        Header h{}; std::memcpy(&h, packet.data(), sizeof h);
        if (packet.size() > native_gpu::MaxPacket || h.status || h.padding) return reply(h, -22);
        const auto op = static_cast<Op>(h.type);
        if (op == Op::Hello) {
            if (h.handle || packet.size() != sizeof h + 4) return reply(h, -22);
            if (!objects.empty()) return reply(h, -16);
            std::uint32_t version{}; std::memcpy(&version, packet.data() + sizeof h, 4);
            negotiated = false;
            if (version != Version) return reply(h, -93);
            const auto caps = driver.capabilities();
            if (caps.version != Version) return reply(h, -93);
            negotiated = true;
            auto out = reply(h, 0); out.resize(sizeof h + sizeof caps);
            std::memcpy(out.data() + sizeof h, &caps, sizeof caps);
            return out;
        }
        if (op >= Op::BeginAdapterQuery && op <= Op::EndAdapterQuery) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & QueryCapability)) return reply(h, -95);
            const auto adapter = objects.find(h.handle);
            if (adapter == objects.end() || adapter->second.kind != Kind::Adapter) return reply(h, -9);
            if (op == Op::BeginAdapterQuery) {
                if (packet.size() != sizeof h + sizeof(QueryDesc)) return reply(h, -22);
                QueryDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
                if (!validQuery(desc)) return reply(h, -22);
                if (queries.count(h.handle)) return reply(h, -16);
                if (queries.size() >= MaxActiveQueries) return reply(h, -24);
                queries.emplace(h.handle, Query{desc, std::vector<std::uint8_t>(desc.bytes)});
                const Result result{0, 0, desc.bytes}; return reply(h, 0, h.handle, &result);
            }
            const auto entry = queries.find(h.handle);
            if (entry == queries.end()) return reply(h, -2);
            auto& query = entry->second;
            if (op == Op::WriteAdapterQuery || op == Op::ReadAdapterQuery) {
                if (packet.size() < sizeof h + sizeof(Range)) return reply(h, -22);
                Range range{}; std::memcpy(&range, packet.data() + sizeof h, sizeof range);
                const bool write = op == Op::WriteAdapterQuery;
                if (!range.size || range.size > MaxChunk || range.size > query.desc.bytes ||
                    range.offset > query.desc.bytes - range.size ||
                    packet.size() != sizeof h + sizeof range + (write ? range.size : 0)) return reply(h, -22);
                if (write) {
                    // A complete, ordered initializer is mandatory. Reject
                    // holes, overlap, retries and writes after execution.
                    if (query.executed || range.offset != query.written) return reply(h, -71);
                    std::memcpy(query.data.data() + range.offset, packet.data() + sizeof h + sizeof range, range.size);
                    query.written += range.size;
                    const Result result{0, 0, query.written}; return reply(h, 0, h.handle, &result);
                }
                if (!query.executed) return reply(h, -71);
                auto out = reply(h, 0, h.handle, &query.result);
                out.insert(out.end(), query.data.begin() + range.offset, query.data.begin() + range.offset + range.size);
                return out;
            }
            if (packet.size() != sizeof h) return reply(h, -22);
            if (op == Op::EndAdapterQuery) {
                queries.erase(entry);
                const Result result{}; return reply(h, 0, h.handle, &result);
            }
            if (query.executed || query.written != query.desc.bytes) return reply(h, -71);
            query.result = driver.queryAdapter(adapter->second.nativeHandle, query.desc, query.data);
            query.executed = true;
            if (query.data.size() != query.desc.bytes) { queries.erase(entry); return reply(h, -5); }
            return reply(h, 0, h.handle, &query.result);
        }
        if (op == Op::SetContextInProcessPriority) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & ContextCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(std::int32_t)) return reply(h, -22);
            std::int32_t priority{}; std::memcpy(&priority, packet.data() + sizeof h, sizeof priority);
            if (!validContextPriority(priority)) return reply(h, -22);
            const auto entry = objects.find(h.handle);
            if (entry == objects.end() || entry->second.kind != Kind::Context) return reply(h, -9);
            if (entry->second.contextFlags != 16) return reply(h, -95);
            const auto result = driver.setContextInProcessPriority(entry->second.nativeHandle, priority);
            if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::CreateContext || op == Op::DestroyContext) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & ContextCapability)) return reply(h, -95);
            const auto entry = objects.find(h.handle);
            const auto required = op == Op::CreateContext ? Kind::Device : Kind::Context;
            if (entry == objects.end() || entry->second.kind != required) return reply(h, -9);
            if (op == Op::DestroyContext) {
                if (packet.size() != sizeof h) return reply(h, -22);
                for (const auto& child : objects) if (child.second.parent == h.handle) return reply(h, -16);
                const auto result = driver.destroy(Kind::Context, entry->second.nativeHandle);
                if (result.ntstatus >= 0) objects.erase(entry);
                return reply(h, 0, h.handle, &result);
            }
            if (packet.size() < sizeof h + sizeof(ContextDesc)) return reply(h, -22);
            ContextDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validContext(desc) || packet.size() != sizeof h + sizeof desc + desc.privateBytes) return reply(h, -22);
            if (objects.size() >= MaxObjects || nextId == UINT32_MAX) return reply(h, -24);
            std::vector<std::uint8_t> data(packet.begin() + sizeof h + sizeof desc, packet.end());
            const auto result = driver.createContext(entry->second.nativeHandle, desc, data);
            if (data.size() != desc.privateBytes) {
                if (result.ntstatus >= 0 && result.nativeHandle) driver.destroy(Kind::Context, result.nativeHandle);
                return reply(h, -5);
            }
            auto out = insert(h, Kind::Context, h.handle, result, 0, 0, false, desc.flags);
            if (result.ntstatus >= 0 && result.nativeHandle) out.insert(out.end(), data.begin(), data.end());
            return out;
        }
        if (op == Op::CreateGuestPagingQueue) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & GuestPagingCapability)) return reply(h, -95);
            if (packet.size() != sizeof h) return reply(h, -22);
            const auto device = objects.find(h.handle);
            if (device == objects.end() || device->second.kind != Kind::Device) return reply(h, -9);
            if (objects.size() > MaxObjects - 2 || nextId >= UINT32_MAX - 1) return reply(h, -24);
            const auto native = driver.createGuestPagingQueue(device->second.nativeHandle);
            if (native.queue.ntstatus < 0) return reply(h, 0, 0, &native.queue);
            if (!native.queue.nativeHandle || !native.sync || native.queue.value ||
                native.offset % 8 || native.offset >= FenceApertureBytes) {
                if (native.queue.nativeHandle) driver.destroy(Kind::PagingQueue, native.queue.nativeHandle);
                return reply(h, -5);
            }
            const auto queue = nextId++, sync = nextId++;
            try {
                auto out = reply(h, 0, queue, &native.queue);
                const PagingReply body{sync, 0, native.offset};
                const auto start = out.size(); out.resize(start + sizeof body);
                std::memcpy(out.data() + start, &body, sizeof body);
                objects.emplace(queue, Object{Kind::PagingQueue, h.handle, native.queue.nativeHandle, 0, 0, false});
                objects.emplace(sync, Object{Kind::PagingSync, queue, native.sync, 0, 0, false});
                return out;
            } catch (...) {
                objects.erase(sync); objects.erase(queue);
                driver.destroy(Kind::PagingQueue, native.queue.nativeHandle);
                throw;
            }
        }
        if (op == Op::CreateHwQueue || op == Op::DestroyHwQueue) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & HwQueueCapability)) return reply(h, -95);
            const auto entry = objects.find(h.handle);
            if (entry == objects.end() || entry->second.kind != (op == Op::CreateHwQueue ? Kind::Context : Kind::HwQueue)) return reply(h, -9);
            if (op == Op::DestroyHwQueue) {
                if (packet.size() != sizeof h) return reply(h, -22);
                const auto result = driver.destroy(Kind::HwQueue, entry->second.nativeHandle);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (result.ntstatus == 0) {
                    for (auto child = objects.begin(); child != objects.end(); )
                        if (child->second.parent == h.handle && child->second.kind == Kind::HwQueueSync) child = objects.erase(child);
                        else ++child;
                    objects.erase(entry);
                }
                return reply(h, 0, h.handle, &result);
            }
            if (entry->second.contextFlags != 16) return reply(h, -95);
            if (packet.size() < sizeof h + sizeof(HwQueueDesc)) return reply(h, -22);
            HwQueueDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validHwQueue(desc) || packet.size() != sizeof h + sizeof desc + desc.privateBytes) return reply(h, -22);
            if (desc.flags == NoBroadcastSignalHwQueueFlag && !(driver.capabilities().flags & NoBroadcastSignalHwQueueCapability)) return reply(h, -95);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::HwQueue; });
            if (objects.size() > MaxObjects - 2 || nextId >= UINT32_MAX - 1 || static_cast<std::size_t>(count) >= MaxHwQueues) return reply(h, -24);
            std::vector<std::uint8_t> data(packet.begin() + sizeof h + sizeof desc, packet.end());
            const auto native = driver.createHwQueue(entry->second.nativeHandle, desc, data);
            if (data.size() != desc.privateBytes || native.queue.value ||
                (native.queue.ntstatus >= 0 && (native.queue.ntstatus != 0 || !native.queue.nativeHandle || !native.sync ||
                    native.sync == native.queue.nativeHandle || native.offset % 8 || native.offset >= FenceApertureBytes ||
                    !native.gpuAddress || native.gpuAddress % 8 || native.gpuAddress >= MaxGpuAddress)) ||
                (native.queue.ntstatus < 0 && (native.queue.nativeHandle || native.sync || native.offset || native.gpuAddress))) {
                if (native.queue.nativeHandle) driver.destroy(Kind::HwQueue, native.queue.nativeHandle);
                return reply(h, -5);
            }
            const auto queue = native.queue.ntstatus == 0 ? nextId++ : 0;
            const auto sync = queue ? nextId++ : 0;
            try {
                auto out = reply(h, 0, queue, &native.queue);
                const HwQueueReply body{sync, 0, native.offset, native.gpuAddress};
                const auto start = out.size(); out.resize(start + sizeof body); std::memcpy(out.data() + start, &body, sizeof body);
                out.insert(out.end(), data.begin(), data.end()); // Private in/out remains opaque, including native failure.
                if (queue) {
                    objects.emplace(queue, Object{Kind::HwQueue, h.handle, native.queue.nativeHandle, 0, 0, false});
                    objects.emplace(sync, Object{Kind::HwQueueSync, queue, native.sync, 0, 0, false});
                }
                return out;
            } catch (...) {
                objects.erase(sync); objects.erase(queue);
                if (native.queue.nativeHandle) driver.destroy(Kind::HwQueue, native.queue.nativeHandle);
                throw;
            }
        }
        if (op == Op::SubmitHwQueue) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & HwSubmitCapability)) return reply(h, -95);
            const auto queue = objects.find(h.handle);
            if (queue == objects.end() || queue->second.kind != Kind::HwQueue) return reply(h, -9);
            if (packet.size() < sizeof h + sizeof(HwSubmitDesc)) return reply(h, -22);
            HwSubmitDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validHwSubmit(desc) || packet.size() != sizeof h + sizeof desc + desc.privateBytes ||
                desc.fence <= queue->second.submittedFence) return reply(h, -22);
            const auto device = objects.at(queue->second.parent).parent;
            unsigned commandOwners = 0;
            for (const auto& item : objects) {
                const auto& allocation = item.second;
                if (allocation.kind != Kind::VendorAllocation || allocation.parent != device) continue;
                if (!allocation.resident) return reply(h, -16);
                if (!allocation.gpuPages || desc.address < allocation.gpuAddress) continue;
                const auto offset = desc.address - allocation.gpuAddress;
                if (offset <= allocation.gpuPages * 4096ull && desc.bytes <= allocation.gpuPages * 4096ull - offset) {
                    if (!allocation.cpuBytes || offset > allocation.cpuBytes || desc.bytes > allocation.cpuBytes - offset) return reply(h, -16);
                    ++commandOwners;
                }
            }
            if (commandOwners != 1) return reply(h, -9);
            if (submissionAttempts >= MaxHwSubmissions) return reply(h, -24);
            ++submissionAttempts;
            const std::vector<std::uint8_t> data(packet.begin() + sizeof h + sizeof desc, packet.end());
            const auto native = driver.submitHwQueue(queue->second.nativeHandle, desc, data);
            if (native.nativeHandle || native.ntstatus > 0 ||
                (native.ntstatus == 0 && (native.value < desc.fence || native.value == UINT64_MAX)) ||
                (native.ntstatus < 0 && native.value)) return reply(h, -5);
            if (native.ntstatus == 0) queue->second.submittedFence = desc.fence;
            return reply(h, 0, h.handle, &native);
        }
        if (op == Op::CreateSync || op == Op::DestroySync) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & SyncCapability)) return reply(h, -95);
            const auto entry = objects.find(h.handle);
            if (entry == objects.end() || entry->second.kind != (op == Op::CreateSync ? Kind::Device : Kind::Sync)) return reply(h, -9);
            if (op == Op::DestroySync) {
                if (packet.size() != sizeof h) return reply(h, -22);
                const auto result = driver.destroy(Kind::Sync, entry->second.nativeHandle);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (result.ntstatus == 0) objects.erase(entry);
                return reply(h, 0, h.handle, &result);
            }
            if (packet.size() != sizeof h + sizeof(SyncDesc)) return reply(h, -22);
            SyncDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validSync(desc)) return reply(h, -22);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::Sync; });
            if (objects.size() >= MaxObjects || nextId == UINT32_MAX || static_cast<std::size_t>(count) >= MaxSyncObjects) return reply(h, -24);
            const auto native = driver.createSync(entry->second.nativeHandle, desc);
            if (native.object.value || (native.object.ntstatus >= 0 && (native.object.ntstatus != 0 || !native.object.nativeHandle ||
                    (desc.type == 1 && (native.output.offset || native.output.gpuAddress)) ||
                    (desc.type == 5 && (native.output.offset % 8 || native.output.offset >= FenceApertureBytes ||
                        !validSyncGpuAddress(desc, native.output.gpuAddress))))) ||
                (native.object.ntstatus < 0 && (native.object.nativeHandle || native.output.offset || native.output.gpuAddress))) {
                if (native.object.nativeHandle) driver.destroy(Kind::Sync, native.object.nativeHandle);
                return reply(h, -5);
            }
            const auto id = native.object.ntstatus == 0 ? nextId++ : 0;
            try {
                auto out = reply(h, 0, id, &native.object);
                const auto start = out.size(); out.resize(start + sizeof native.output);
                std::memcpy(out.data() + start, &native.output, sizeof native.output);
                if (id) {
                    Object owned{Kind::Sync, h.handle, native.object.nativeHandle, 0, 0, false}; owned.syncType = desc.type;
                    owned.syncFlags = desc.flags; owned.submittedFence = desc.initial;
                    objects.emplace(id, owned);
                }
                return out;
            } catch (...) {
                if (native.object.nativeHandle) driver.destroy(Kind::Sync, native.object.nativeHandle);
                throw;
            }
        }
        if (op == Op::SignalContextSync) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & ContextSignalCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(ContextSignalDesc)) return reply(h, -22);
            ContextSignalDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validContextSignal(desc)) return reply(h, -22);
            const auto context = objects.find(h.handle), sync = objects.find(desc.sync);
            if (context == objects.end() || context->second.kind != Kind::Context || context->second.contextFlags != 16 ||
                sync == objects.end() || sync->second.kind != Kind::Sync || sync->second.syncType != 5 ||
                sync->second.syncFlags != NoGpuAccessSyncFlag || sync->second.parent != context->second.parent) return reply(h, -9);
            if (desc.fence <= sync->second.submittedFence) return reply(h, -22);
            if (contextSignalAttempts >= MaxContextSignals) return reply(h, -24);
            ++contextSignalAttempts;
            const auto result = driver.signalContextSync(context->second.nativeHandle, sync->second.nativeHandle, desc);
            if (result.ntstatus > 0 || result.nativeHandle ||
                (result.ntstatus == 0 && (result.value < desc.fence || result.value == UINT64_MAX)) ||
                (result.ntstatus < 0 && result.value)) return reply(h, -5);
            if (result.ntstatus == 0) sync->second.submittedFence = desc.fence;
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::DestroyVendorResource) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorResourceCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + 4) return reply(h, -22);
            std::uint32_t deviceId{}; std::memcpy(&deviceId, packet.data() + sizeof h, 4);
            const auto resource = objects.find(h.handle);
            if (resource == objects.end() || resource->second.kind != Kind::VendorResource) return reply(h, -9);
            const auto allocation = objects.find(resource->second.parent);
            if (allocation == objects.end() || allocation->second.parent != deviceId ||
                allocation->second.kind != Kind::VendorAllocation || allocation->second.vendorResource != h.handle) return reply(h, -9);
            const Header destroyHeader{static_cast<std::uint32_t>(Op::DestroyVendorAllocations), deviceId, 0, 0};
            const DestroyVendorDesc desc{1, 0}; const auto id = allocation->first;
            std::vector<std::uint8_t> destroyPacket(sizeof destroyHeader + sizeof desc + sizeof id);
            std::memcpy(destroyPacket.data(), &destroyHeader, sizeof destroyHeader);
            std::memcpy(destroyPacket.data() + sizeof destroyHeader, &desc, sizeof desc);
            std::memcpy(destroyPacket.data() + sizeof destroyHeader + sizeof desc, &id, sizeof id);
            auto out = dispatch(destroyPacket);
            Header response{}; std::memcpy(&response, out.data(), sizeof response);
            response.type = h.type; response.handle = response.status ? 0 : h.handle;
            std::memcpy(out.data(), &response, sizeof response); return out;
        }
        if (op == Op::CreateVendorAllocation || op == Op::CreateVendorResourceAllocation || op == Op::DestroyVendorAllocations) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorAllocationCapability)) return reply(h, -95);
            const bool withResource = op == Op::CreateVendorResourceAllocation;
            if (withResource && !(driver.capabilities().flags & VendorResourceCapability)) return reply(h, -95);
            const auto device = objects.find(h.handle);
            if (device == objects.end() || device->second.kind != Kind::Device) return reply(h, -9);
            if (op == Op::DestroyVendorAllocations) {
                // Opt-in native retirement uses VidMm's documented deferred
                // destruction contract rather than decoding private queue data.
                if (!(driver.capabilities().flags & VendorRetirementCapability))
                    for (const auto& queue : objects)
                        if (queue.second.kind == Kind::HwQueue && objects.at(queue.second.parent).parent == h.handle) return reply(h, -16);
                if (packet.size() < sizeof h + sizeof(DestroyVendorDesc)) return reply(h, -22);
                DestroyVendorDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
                if (desc.reserved || !desc.count || desc.count > MaxVendorAllocations ||
                    packet.size() != sizeof h + sizeof desc + desc.count * sizeof(std::uint32_t)) return reply(h, -22);
                std::vector<std::uint32_t> ids(desc.count), native(desc.count);
                std::memcpy(ids.data(), packet.data() + sizeof h + sizeof desc, ids.size() * sizeof(ids[0]));
                for (std::size_t n = 0; n < ids.size(); ++n) {
                    const auto entry = objects.find(ids[n]);
                    if (entry == objects.end() || entry->second.kind != Kind::VendorAllocation || entry->second.parent != h.handle)
                        return reply(h, -9);
                    if (std::find(ids.begin(), ids.begin() + n, ids[n]) != ids.begin() + n) return reply(h, -22);
                    if (entry->second.cpuBytes) return reply(h, -16);
                    if (entry->second.vendorResource && ids.size() != 1) return reply(h, -22);
                    native[n] = entry->second.nativeHandle;
                }
                const auto result = driver.destroyVendorAllocations(device->second.nativeHandle, native);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (result.ntstatus == 0) for (const auto id : ids) {
                    if (objects.at(id).vendorResource) objects.erase(objects.at(id).vendorResource);
                    vendorMappedPages -= objects.at(id).gpuPages; objects.erase(id);
                }
                return reply(h, 0, h.handle, &result);
            }
            if (packet.size() < sizeof h + sizeof(VendorAllocationDesc)) return reply(h, -22);
            VendorAllocationDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validVendorAllocation(desc) || packet.size() != sizeof h + sizeof desc + desc.privateBytes) return reply(h, -22);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::VendorAllocation; });
            if (objects.size() > MaxObjects - (withResource ? 2 : 1) || nextId >= UINT32_MAX - (withResource ? 1 : 0) || static_cast<std::size_t>(count) >= MaxVendorAllocations) return reply(h, -24);
            std::vector<std::uint8_t> data(packet.begin() + sizeof h + sizeof desc, packet.end());
            const auto created = withResource ? driver.createVendorResourceAllocation(device->second.nativeHandle, desc, data) :
                                               VendorResourceResult{driver.createVendorAllocation(device->second.nativeHandle, desc, data), 0};
            const auto result = created.allocation;
            if (data.size() != desc.privateBytes || (result.ntstatus >= 0 && (!result.nativeHandle || result.value % 4096)) ||
                (result.ntstatus >= 0 && withResource && (!created.resource || created.resource == result.nativeHandle)) ||
                (result.ntstatus < 0 && (result.nativeHandle || result.value || created.resource))) {
                if (result.nativeHandle) driver.destroy(Kind::VendorAllocation, result.nativeHandle);
                return reply(h, -5);
            }
            const auto id = result.ntstatus >= 0 ? nextId++ : 0;
            const auto resourceId = id && withResource ? nextId++ : 0;
            try {
                auto out = reply(h, 0, id, &result);
                if (withResource) {
                    const VendorResourceReply resource{resourceId, 0};
                    const auto bytes = reinterpret_cast<const std::uint8_t*>(&resource);
                    out.insert(out.end(), bytes, bytes + sizeof resource);
                }
                out.insert(out.end(), data.begin(), data.end()); // Preserve in/out on native failure too.
                if (id) {
                    Object allocation{Kind::VendorAllocation, h.handle, result.nativeHandle, 0, 0, false}; allocation.vendorResource = resourceId;
                    objects.emplace(id, allocation);
                    if (resourceId) objects.emplace(resourceId, Object{Kind::VendorResource, id, created.resource, 0, 0, false});
                }
                return out;
            } catch (...) {
                objects.erase(id); objects.erase(resourceId);
                if (result.nativeHandle) driver.destroy(Kind::VendorAllocation, result.nativeHandle);
                throw;
            }
        }
        if (op == Op::TranslateVendorAllocation) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorTranslationCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(VendorTranslationDesc)) return reply(h, -22);
            VendorTranslationDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!desc.device || !desc.adapter || desc.reserved || desc.reserved2) return reply(h, -22);
            const auto allocation = objects.find(h.handle), device = objects.find(desc.device), adapter = objects.find(desc.adapter);
            if (allocation == objects.end() || allocation->second.kind != Kind::VendorAllocation ||
                device == objects.end() || device->second.kind != Kind::Device ||
                adapter == objects.end() || adapter->second.kind != Kind::Adapter ||
                allocation->second.parent != desc.device || device->second.parent != desc.adapter) return reply(h, -9);
            const auto result = driver.translateVendorAllocation(allocation->second.nativeHandle, device->second.nativeHandle, adapter->second.nativeHandle);
            if (result.nativeHandle || (result.ntstatus >= 0 && (result.ntstatus != 0 || !result.value || result.value > UINT32_MAX)) ||
                (result.ntstatus < 0 && result.value)) return reply(h, -5);
            if (result.ntstatus >= 0) {
                if (allocation->second.driverToken && allocation->second.driverToken != result.value) return reply(h, -5);
                for (const auto& item : objects)
                    if (item.first != h.handle && item.second.kind == Kind::VendorAllocation && item.second.driverToken == result.value)
                        return reply(h, -5);
                allocation->second.driverToken = static_cast<std::uint32_t>(result.value);
            }
            // This opaque KMD token is an explicit driver-private contract,
            // never a substitute for a typed ID in public bridge operations.
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::LockVendorAllocation || op == Op::UnlockVendorAllocation) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorCpuCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(VendorCpuDesc)) return reply(h, -22);
            VendorCpuDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!desc.device || desc.flags) return reply(h, -22);
            const auto allocation = objects.find(h.handle), device = objects.find(desc.device);
            if (allocation == objects.end() || allocation->second.kind != Kind::VendorAllocation ||
                device == objects.end() || device->second.kind != Kind::Device || allocation->second.parent != desc.device)
                return reply(h, -9);
            auto& object = allocation->second;
            if (op == Op::UnlockVendorAllocation) {
                if (!object.cpuBytes) return reply(h, -9);
                const auto result = driver.unlockVendorAllocation(object.nativeHandle, device->second.nativeHandle);
                if (result.nativeHandle || result.value || (result.ntstatus >= 0 && result.ntstatus != 0)) return reply(h, -5);
                if (result.ntstatus >= 0) { vendorCpuBytes -= object.cpuBytes; object.cpuBytes = 0; object.cpuOffset = 0; }
                return reply(h, 0, h.handle, &result);
            }
            if (object.cpuBytes) return reply(h, -16);
            if (!object.gpuPages || object.gpuOffsetPages) return reply(h, -95);
            const auto bytes = object.gpuPages * 4096u;
            if (bytes > MaxAllocatedBytes - vendorCpuBytes) return reply(h, -24);
            const auto native = driver.lockVendorAllocation(object.nativeHandle, device->second.nativeHandle);
            if (native.lock.nativeHandle || (native.lock.ntstatus >= 0 && (native.lock.ntstatus != 0 ||
                !validVendorCpuReply(native.lock.value, native.output) || native.output.bytes != bytes)) ||
                (native.lock.ntstatus < 0 && (native.lock.value || native.output.bytes || native.output.reserved || native.output.generation)))
                return reply(h, -5);
            if (native.lock.ntstatus >= 0) {
                for (const auto& item : objects) if (item.second.cpuBytes &&
                    native.lock.value < item.second.cpuOffset + item.second.cpuBytes && item.second.cpuOffset < native.lock.value + native.output.bytes)
                    return reply(h, -5);
                object.cpuBytes = native.output.bytes; object.cpuOffset = native.lock.value; vendorCpuBytes += object.cpuBytes;
            }
            auto out = reply(h, 0, h.handle, &native.lock);
            const auto start = out.size(); out.resize(start + sizeof(VendorCpuReply));
            std::memcpy(out.data() + start, &native.output, sizeof native.output); return out;
        }
        if (op == Op::MapGpuState) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & GpuStateCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(GpuVaDesc)) return reply(h, -22);
            GpuVaDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validGpuState(desc)) return reply(h, -22);
            const auto reservation = objects.find(h.handle), queue = objects.find(desc.queue);
            if (reservation == objects.end() || reservation->second.kind != Kind::GpuReservation ||
                queue == objects.end() || queue->second.kind != Kind::PagingQueue ||
                objects.at(queue->second.parent).parent != reservation->second.parent) return reply(h, -9);
            if (!gpuRangeContains(reservation->second.gpuAddress, reservation->second.reservedGpuBytes, desc.base, desc.sizePages * 4096)) return reply(h, -9);
            for (const auto& item : objects) {
                const auto& mapped = item.second;
                if (mapped.kind == Kind::Allocation && gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.gpuAddress, mapped.size)) return reply(h, -16);
                if (mapped.kind == Kind::VendorAllocation && gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.gpuAddress, mapped.gpuPages * 4096ull) &&
                    (mapped.parent != queue->second.parent || !gpuRangeContains(desc.base, desc.sizePages * 4096, mapped.gpuAddress, mapped.gpuPages * 4096ull))) return reply(h, -16);
            }
            GpuStateRanges::Plan plan;
            if (!gpuStates.prepare(h.handle, desc.base, desc.sizePages * 4096, desc.protection, plan)) return reply(h, -24);
            const auto result = driver.mapGpuState(reservation->second.nativeHandle, queue->second.nativeHandle, desc);
            if (result.map.nativeHandle || (result.map.ntstatus >= 0 &&
                ((result.map.ntstatus != 0 && result.map.ntstatus != 259) || result.map.value != desc.base)) ||
                (result.map.ntstatus < 0 && (result.map.value || result.fence))) return reply(h, -5);
            if (result.map.ntstatus >= 0) {
                for (auto& item : objects) {
                    auto& mapped = item.second;
                    if (mapped.kind == Kind::VendorAllocation && gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.gpuAddress, mapped.gpuPages * 4096ull)) {
                        vendorMappedPages -= mapped.gpuPages; mapped.gpuPages = 0; mapped.gpuAddress = 0; mapped.gpuOffsetPages = 0;
                    }
                }
                gpuStates.commit(plan);
            }
            auto out = reply(h, 0, h.handle, &result.map);
            const auto start = out.size(); out.resize(start + sizeof(GpuVaReply));
            const GpuVaReply fence{result.fence}; std::memcpy(out.data() + start, &fence, sizeof fence); return out;
        }
        if (op == Op::ReserveGpuAddress || op == Op::FreeGpuReservation) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & GpuReservationCapability)) return reply(h, -95);
            const auto entry = objects.find(h.handle);
            if (entry == objects.end() || entry->second.kind != (op == Op::ReserveGpuAddress ? Kind::Adapter : Kind::GpuReservation))
                return reply(h, -9);
            if (op == Op::FreeGpuReservation) {
                if (packet.size() != sizeof h + sizeof(FreeGpuReservationDesc)) return reply(h, -22);
                FreeGpuReservationDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
                if (desc.reserved) return reply(h, -22);
                if (desc.adapter != entry->second.parent) return reply(h, -9);
                for (const auto& item : objects) {
                    const auto& mapped = item.second;
                    const auto bytes = mapped.kind == Kind::Allocation ? mapped.size : mapped.gpuPages * 4096ull;
                    if ((mapped.kind == Kind::VendorAllocation || mapped.kind == Kind::Allocation) &&
                        gpuRangesOverlap(entry->second.gpuAddress, entry->second.reservedGpuBytes, mapped.gpuAddress, bytes) &&
                        !gpuRangeContains(entry->second.gpuAddress, entry->second.reservedGpuBytes, mapped.gpuAddress, bytes)) return reply(h, -16);
                }
                const auto result = driver.destroy(Kind::GpuReservation, entry->second.nativeHandle);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (result.ntstatus == 0) {
                    for (auto& item : objects) {
                        auto& mapped = item.second;
                        const auto bytes = mapped.kind == Kind::Allocation ? mapped.size : mapped.gpuPages * 4096ull;
                        if ((mapped.kind == Kind::VendorAllocation || mapped.kind == Kind::Allocation) &&
                            gpuRangesOverlap(entry->second.gpuAddress, entry->second.reservedGpuBytes, mapped.gpuAddress, bytes)) {
                            vendorMappedPages -= mapped.gpuPages; mapped.gpuPages = 0; mapped.gpuAddress = 0; mapped.gpuOffsetPages = 0;
                        }
                    }
                    gpuStates.release(h.handle); gpuReservedBytes -= entry->second.reservedGpuBytes; objects.erase(entry);
                }
                return reply(h, 0, h.handle, &result);
            }
            if (packet.size() != sizeof h + sizeof(GpuReservationDesc)) return reply(h, -22);
            GpuReservationDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validGpuReservation(desc)) return reply(h, -22);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::GpuReservation; });
            if (objects.size() >= MaxObjects || nextId == UINT32_MAX || static_cast<std::size_t>(count) >= MaxGpuReservations ||
                desc.bytes > MaxGpuReservedBytes - gpuReservedBytes) return reply(h, -24);
            if (desc.base) for (const auto& item : objects)
                if (item.second.kind == Kind::GpuReservation && gpuRangesOverlap(desc.base, desc.bytes, item.second.gpuAddress, item.second.reservedGpuBytes))
                    return reply(h, -16);
            const auto result = driver.reserveGpuAddress(entry->second.nativeHandle, desc);
            if ((result.ntstatus >= 0 && (result.ntstatus != 0 || !result.nativeHandle || !validGpuReservationOutput(desc, result.value))) ||
                (result.ntstatus < 0 && (result.nativeHandle || result.value))) {
                if (result.nativeHandle) driver.destroy(Kind::GpuReservation, result.nativeHandle);
                return reply(h, -5);
            }
            if (result.ntstatus < 0) return reply(h, 0, 0, &result);
            for (const auto& item : objects)
                if (item.second.kind == Kind::GpuReservation && gpuRangesOverlap(result.value, desc.bytes, item.second.gpuAddress, item.second.reservedGpuBytes)) {
                    driver.destroy(Kind::GpuReservation, result.nativeHandle); return reply(h, -5);
                }
            const auto id = nextId++;
            try {
                auto out = reply(h, 0, id, &result);
                Object owned{Kind::GpuReservation, h.handle, result.nativeHandle, 0, 0, false};
                owned.gpuAddress = result.value; owned.reservedGpuBytes = desc.bytes;
                objects.emplace(id, owned); gpuReservedBytes += desc.bytes; return out;
            } catch (...) { driver.destroy(Kind::GpuReservation, result.nativeHandle); throw; }
        }
        if (op == Op::MakeVendorResident) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorResidencyCapability)) return reply(h, -95);
            if (packet.size() < sizeof h + sizeof(ResidentDesc)) return reply(h, -22);
            ResidentDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validResident(desc) || packet.size() != sizeof h + sizeof desc + desc.count * 4 * (1 + desc.priorities))
                return reply(h, -22);
            const auto queue = objects.find(h.handle);
            if (queue == objects.end() || queue->second.kind != Kind::PagingQueue) return reply(h, -9);
            std::vector<std::uint32_t> ids(desc.count), native(desc.count), priorities(desc.priorities ? desc.count : 0);
            std::memcpy(ids.data(), packet.data() + sizeof h + sizeof desc, ids.size() * 4);
            if (!priorities.empty()) std::memcpy(priorities.data(), packet.data() + sizeof h + sizeof desc + ids.size() * 4, priorities.size() * 4);
            for (std::size_t n = 0; n < ids.size(); ++n) {
                const auto entry = objects.find(ids[n]);
                if (entry == objects.end() || entry->second.kind != Kind::VendorAllocation || entry->second.parent != queue->second.parent)
                    return reply(h, -9);
                if (std::find(ids.begin(), ids.begin() + n, ids[n]) != ids.begin() + n) return reply(h, -22);
                if (entry->second.residencyAttempts >= MaxVendorResidencyAttempts) return reply(h, -24);
                native[n] = entry->second.nativeHandle;
            }
            // Conservatively bound all native attempts, including failure
            // that may have made only part of the list resident.
            for (const auto id : ids) ++objects.at(id).residencyAttempts;
            const auto result = driver.makeVendorResident(queue->second.nativeHandle, desc, native, priorities);
            if (result.residency.nativeHandle || result.output.reserved || result.output.count > desc.count ||
                (result.residency.ntstatus >= 0 && result.residency.ntstatus != 0 && result.residency.ntstatus != 0x103))
                return reply(h, -5);
            if (result.residency.ntstatus >= 0 && result.output.count == desc.count)
                for (const auto id : ids) objects.at(id).resident = true;
            auto out = reply(h, 0, h.handle, &result.residency);
            const auto start = out.size(); out.resize(start + sizeof(ResidentReply));
            std::memcpy(out.data() + start, &result.output, sizeof result.output);
            return out;
        }
        if (op == Op::MapVendorAllocation) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorGpuVaCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(GpuVaDesc)) return reply(h, -22);
            GpuVaDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validGpuVa(desc)) return reply(h, -22);
            const auto allocation = objects.find(h.handle), queue = objects.find(desc.queue);
            if (allocation == objects.end() || allocation->second.kind != Kind::VendorAllocation ||
                queue == objects.end() || queue->second.kind != Kind::PagingQueue || allocation->second.parent != queue->second.parent)
                return reply(h, -9);
            auto& object = allocation->second;
            if (desc.driverProtection && !object.vendorResource) return reply(h, -22);
            if (object.gpuPages) return reply(h, -16);
            if (desc.sizePages > MaxVendorMappedPages - vendorMappedPages) return reply(h, -24);
            if (desc.base) for (const auto& item : objects) {
                const auto& mapped = item.second;
                if (mapped.kind == Kind::VendorAllocation && mapped.gpuPages &&
                    desc.base < mapped.gpuAddress + mapped.gpuPages * 4096ull &&
                    mapped.gpuAddress < desc.base + desc.sizePages * 4096) return reply(h, -16);
            }
            GpuStateRanges::Plan plan;
            if (desc.base && !gpuStates.prepareRemove(desc.base, desc.sizePages * 4096, plan)) return reply(h, -24);
            const auto result = driver.mapVendorAllocation(object.nativeHandle, queue->second.nativeHandle, desc);
            if (result.map.nativeHandle || (result.map.ntstatus >= 0 &&
                (result.map.ntstatus != 0 && result.map.ntstatus != 0x103)) ||
                (result.map.ntstatus >= 0 && !validGpuVaOutput(desc, result.map.value)) ||
                (result.map.ntstatus < 0 && (result.map.value || result.fence))) return reply(h, -5);
            if (result.map.ntstatus >= 0) {
                object.gpuPages = static_cast<std::uint32_t>(desc.sizePages); object.gpuAddress = result.map.value;
                object.gpuOffsetPages = desc.offsetPages;
                vendorMappedPages += object.gpuPages;
                if (!desc.base && !gpuStates.prepareRemove(result.map.value, desc.sizePages * 4096, plan))
                    throw std::runtime_error("GPU state tracking quota after allocation mapping");
                gpuStates.commit(plan);
            }
            auto out = reply(h, 0, h.handle, &result.map);
            const auto start = out.size(); out.resize(start + sizeof(GpuVaReply));
            const GpuVaReply fence{result.fence}; std::memcpy(out.data() + start, &fence, sizeof fence);
            return out;
        }
        const bool memoryOp = op >= Op::CreateAllocation && op <= Op::CopySharedAllocation;
        if (!memoryOp && (op < Op::OpenAdapter || op > Op::DestroyPagingQueue)) return reply(h, -95);
        const bool scalar = op == Op::CreateAllocation || op == Op::MakeResident || op == Op::MapAllocation;
        const bool transfer = op == Op::WriteAllocation || op == Op::ReadAllocation;
        const bool shared = op == Op::CreateSharedAllocation;
        const bool copy = op == Op::CopySharedAllocation;
        if ((!scalar && !transfer && !shared && !copy && packet.size() != sizeof h) ||
            (scalar && packet.size() != sizeof h + 4) ||
            (shared && packet.size() != sizeof h + sizeof(GuestRange)) ||
            (copy && packet.size() != sizeof h + sizeof(CopyRange)) ||
            (transfer && packet.size() < sizeof h + sizeof(Range))) return reply(h, -22);
        if (!negotiated) return reply(h, -71);
        if (op == Op::OpenAdapter) {
            if (h.handle) return reply(h, -22);
            if (objects.size() >= MaxObjects || nextId == UINT32_MAX) return reply(h, -24);
            return insert(h, Kind::Adapter, 0, driver.openAdapter());
        }
        const auto entry = objects.find(h.handle);
        if (entry == objects.end()) return reply(h, -9);
        const auto& object = entry->second;
        const bool adapterOp = op == Op::QueryDriverVersion || op == Op::CloseAdapter || op == Op::CreateDevice;
        const bool deviceOp = op == Op::DestroyDevice || op == Op::CreatePagingQueue || op == Op::CreateAllocation || shared;
        const auto required = adapterOp ? Kind::Adapter : deviceOp ? Kind::Device : memoryOp ? Kind::Allocation : Kind::PagingQueue;
        if (object.kind != required) return reply(h, -9);
        if (copy) {
            CopyRange range{}; std::memcpy(&range, packet.data() + sizeof h, sizeof range);
            const auto source = objects.find(range.source);
            if (source == objects.end() || source->second.kind != Kind::Allocation ||
                source->second.parent != object.parent || !source->second.shared || !object.shared) return reply(h, -9);
            const auto& from = source->second;
            if (range.source == h.handle || !range.size || range.size > object.size || range.size > from.size ||
                range.sourceOffset > from.size - range.size || range.destinationOffset > object.size - range.size ||
                object.guestOffset % 65536 || from.guestOffset % 65536 || object.size % 65536 || from.size % 65536)
                return reply(h, -22);
            const auto result = driver.copySharedAllocation(object.nativeHandle, from.nativeHandle, range);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::CreateAllocation || shared) {
            std::uint32_t size{}; std::memcpy(&size, packet.data() + sizeof h, 4);
            GuestRange range{};
            if (shared) {
                std::memcpy(&range, packet.data() + sizeof h, sizeof range); size = range.size;
                if (range.reserved || range.offset % 4096 || range.offset > UINT64_MAX - size) return reply(h, -22);
                if (!size || size % 4096 || size > MaxAllocation) return reply(h, -22);
                for (const auto& live : objects)
                    if (live.second.shared && range.offset < live.second.guestOffset + live.second.size &&
                        live.second.guestOffset < range.offset + size) return reply(h, -16);
            }
            if (!size || size % 4096 || size > MaxAllocation) return reply(h, -22);
            if (objects.size() >= MaxObjects || nextId == UINT32_MAX || size > MaxAllocatedBytes - allocatedBytes)
                return reply(h, -24);
            const auto result = shared ? driver.createSharedAllocation(object.nativeHandle, range)
                                       : driver.createAllocation(object.nativeHandle, size);
            return insert(h, Kind::Allocation, h.handle, result, size, range.offset, shared);
        }
        if (transfer) {
            Range range{}; std::memcpy(&range, packet.data() + sizeof h, sizeof range);
            const auto expected = sizeof h + sizeof range + (op == Op::WriteAllocation ? range.size : 0);
            if (!range.size || range.size > MaxChunk || packet.size() != expected ||
                range.size > object.size || range.offset > object.size - range.size) return reply(h, -22);
            std::vector<std::uint8_t> data;
            const auto result = op == Op::WriteAllocation
                ? driver.writeAllocation(object.nativeHandle, range, packet.data() + sizeof h + sizeof range)
                : driver.readAllocation(object.nativeHandle, range, data);
            auto out = reply(h, 0, h.handle, &result);
            if (result.ntstatus >= 0 && op == Op::ReadAllocation) {
                if (data.size() != range.size) return reply(h, -5);
                out.insert(out.end(), data.begin(), data.end());
            }
            return out;
        }
        if (op == Op::MakeResident || op == Op::MapAllocation) {
            std::uint32_t queueId{}; std::memcpy(&queueId, packet.data() + sizeof h, 4);
            const auto queue = objects.find(queueId);
            if (queue == objects.end() || queue->second.kind != Kind::PagingQueue || queue->second.parent != object.parent)
                return reply(h, -9);
            const auto result = op == Op::MakeResident ? driver.makeResident(object.nativeHandle, queue->second.nativeHandle)
                                                     : driver.mapAllocation(object.nativeHandle, queue->second.nativeHandle);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::QueryResidency) {
            const auto result = driver.queryResidency(object.nativeHandle); return reply(h, 0, h.handle, &result);
        }
        if (op == Op::QueryDriverVersion || op == Op::ReadPagingFence) {
            const auto result = op == Op::QueryDriverVersion ? driver.queryVersion(object.nativeHandle)
                                                            : driver.readPagingFence(object.nativeHandle);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::CreateDevice || op == Op::CreatePagingQueue) {
            if (objects.size() >= MaxObjects || nextId == UINT32_MAX) return reply(h, -24);
            const auto result = op == Op::CreateDevice ? driver.createDevice(object.nativeHandle)
                                                       : driver.createPagingQueue(object.nativeHandle);
            return insert(h, op == Op::CreateDevice ? Kind::Device : Kind::PagingQueue, h.handle, result);
        }
        if (queries.count(h.handle)) return reply(h, -16);
        for (const auto& child : objects)
            if (child.second.parent == h.handle &&
                !(object.kind == Kind::PagingQueue && child.second.kind == Kind::PagingSync)) return reply(h, -16);
        const auto result = driver.destroy(object.kind, object.nativeHandle);
        if (result.ntstatus >= 0) {
            if (object.kind == Kind::PagingQueue) {
                for (auto child = objects.begin(); child != objects.end();) {
                    if (child->second.parent == h.handle && child->second.kind == Kind::PagingSync) child = objects.erase(child);
                    else ++child;
                }
            }
            allocatedBytes -= object.size; objects.erase(entry);
        }
        return reply(h, 0, h.handle, &result);
    }
};
inline std::vector<std::uint8_t> request(Op operation, std::uint32_t handle = 0) {
    const Header h{static_cast<std::uint32_t>(operation), handle, 0, 0};
    std::vector<std::uint8_t> out(sizeof h); std::memcpy(out.data(), &h, sizeof h); return out;
}
inline std::vector<std::uint8_t> hello(std::uint32_t version = Version) {
    auto out = request(Op::Hello); out.resize(sizeof(Header) + 4);
    std::memcpy(out.data() + sizeof(Header), &version, 4); return out;
}
template<class T> inline std::vector<std::uint8_t> request(Op operation, std::uint32_t handle, const T& body) {
    auto out = request(operation, handle); out.resize(sizeof(Header) + sizeof body);
    std::memcpy(out.data() + sizeof(Header), &body, sizeof body); return out;
}
} // namespace driver_bridge
