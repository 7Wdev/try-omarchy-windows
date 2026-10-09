// SPDX-License-Identifier: MIT
// Fault tests for the diagnostic interposer. No real GPU calls are made.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
#include "driver_wire.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
static int failed(const char* message) { std::fprintf(stderr, "FAIL: %s errno=%d\n", message, errno); return 1; }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto mode = argv[1];
    const auto fd = open("/dev/dxg", O_RDONLY | O_CLOEXEC);
    if (!std::strcmp(mode, "no-worker") || !std::strcmp(mode, "wrong-version") || !std::strcmp(mode, "priority-disabled")) {
        if (fd != -1 || errno != (!std::strcmp(mode, "no-worker") ? EINVAL : EPROTO)) return failed("unexpected open result");
        return 0;
    }
    if (fd < 0) return failed("open");
    if (!std::strncmp(mode, "priority-", 9)) {
        D3DKMT_SETCONTEXTINPROCESSSCHEDULINGPRIORITY priority{}; priority.hContext = 99;
        if (std::strcmp(mode, "priority-disabled") && std::strcmp(mode, "priority-unowned")) {
            D3DKMT_CREATEDEVICE device{}; device.hAdapter = 1; device.Flags.RequestVSync = 1;
            if (ioctl(fd, _IOWR('G', 2, D3DKMT_CREATEDEVICE), &device)) return failed("priority device");
            unsigned char data[]{37, 38, 39, 40};
            D3DKMT_CREATECONTEXTVIRTUAL context{}; context.hDevice = device.hDevice;
            context.NodeOrdinal = 0; context.EngineAffinity = 1; context.Flags.Value = 16;
            context.ClientHint = static_cast<D3DKMT_CLIENTHINT>(12); context.pPrivateDriverData = data; context.PrivateDriverDataSize = sizeof data;
            if (ioctl(fd, _IOWR('G', 4, D3DKMT_CREATECONTEXTVIRTUAL), &context)) return failed("priority context");
            priority.hContext = context.hContext;
        }
        const bool normal = !std::strcmp(mode, "priority-normal");
        priority.Priority = !std::strcmp(mode, "priority-invalid") ? 2 : 1;
        const auto result = ioctl(fd, _IOWR('G', 47, D3DKMT_SETCONTEXTINPROCESSSCHEDULINGPRIORITY), &priority);
        const auto expected = !std::strcmp(mode, "priority-disabled") ? ENOSYS : !std::strcmp(mode, "priority-unowned") ? EBADF :
            !std::strcmp(mode, "priority-invalid") ? EINVAL : !std::strcmp(mode, "priority-nt-failure") ? EINVAL : EPROTO;
        if (normal ? result != 0 : result != -1 || errno != expected) return failed("priority forwarding and reply validation");
        if (!normal && expected == EPROTO && (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO)) return failed("priority broken transport reused");
        return 0;
    }
    if (!std::strncmp(mode, "gpu-state-", 10)) {
        D3DDDI_MAPGPUVIRTUALADDRESS mapping{}; mapping.hPagingQueue = 3; mapping.BaseAddress = 67108864;
        mapping.SizeInPages = 16; mapping.Protection.Value = 5;
        if (!std::strcmp(mode, "gpu-state-invalid")) {
            for (unsigned n = 0; n < 13; ++n) {
                auto bad = mapping;
                switch (n) {
                    case 0: bad.hAllocation = 1; break;
                    case 1: bad.BaseAddress = 0; break;
                    case 2: bad.BaseAddress++; break;
                    case 3: bad.OffsetInPages = 1; break;
                    case 4: bad.Protection.Value = 12; break;
                    case 5: bad.Protection.Value = 20; break;
                    case 6: bad.SizeInPages = 0; break;
                    case 7: bad.SizeInPages = driver_bridge::MaxGpuReservationBytes / 4096 + 1; break;
                    case 8: bad.BaseAddress = driver_bridge::MaxGpuAddress - 4096; break;
                    case 9: bad.Reserved0 = 1; break;
                    case 10: bad.Reserved1 = 1; break;
                    case 11: bad.DriverProtection = 1; break;
                    case 12: bad.hPagingQueue = 0; break;
                }
                if (ioctl(fd, _IOWR('G', 12, D3DDDI_MAPGPUVIRTUALADDRESS), &bad) != -1 || errno != EINVAL)
                    return failed("invalid GPU state mapping forwarded");
            }
        } else {
            const auto expected = !std::strcmp(mode, "gpu-state-disabled") ? ENOSYS : EBADF;
            for (const auto protection : {4ull, 5ull, 8ull, 11ull}) {
                mapping.Protection.Value = protection;
                if (ioctl(fd, _IOWR('G', 12, D3DDDI_MAPGPUVIRTUALADDRESS), &mapping) != -1 || errno != expected)
                    return failed("GPU state opt-in or ownership rejection");
            }
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "reservation-", 12)) {
        D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart = 0x57475055;
        if (ioctl(fd, _IOWR('G', 1, D3DKMT_OPENADAPTERFROMLUID), &adapter)) return failed("reservation adapter");
        D3DDDI_RESERVEGPUVIRTUALADDRESS reserve{}; reserve.hAdapter = adapter.hAdapter;
        reserve.MinimumAddress = 67108864; reserve.MaximumAddress = 1ull << 40; reserve.Size = 65536;
        if (!std::strcmp(mode, "reservation-invalid")) {
            for (unsigned n = 0; n < 16; ++n) {
                auto invalid = reserve;
                switch (n) {
                    case 0: invalid.Size = 0; break;
                    case 1: ++invalid.Size; break;
                    case 2: invalid.Size = (4ull << 30) + 65536; break;
                    case 3: invalid.BaseAddress = 1; break;
                    case 4: ++invalid.MinimumAddress; break;
                    case 5: ++invalid.MaximumAddress; break;
                    case 6: invalid.BaseAddress = 1ull << 48; break;
                    case 7: invalid.MinimumAddress = 1ull << 48; break;
                    case 8: invalid.MaximumAddress = (1ull << 48) + 65536; break;
                    case 9: invalid.MaximumAddress = invalid.MinimumAddress; break;
                    case 10: invalid.BaseAddress = (1ull << 48) - 65536; invalid.Size = 131072; break;
                    case 11: invalid.Reserved0 = 1; break;
                    case 12: invalid.Reserved1 = 1; break;
                    case 13: invalid.Reserved2 = 1; break;
                    case 14: invalid.hAdapter = 0; break;
                    default: invalid.hAdapter = 999; break;
                }
                if (ioctl(fd, _IOWR('G', 8, D3DDDI_RESERVEGPUVIRTUALADDRESS), &invalid) != -1 || errno != (n < 14 ? EINVAL : EBADF))
                    return failed("reservation invalid input");
            }
        } else if (!std::strcmp(mode, "reservation-quota") || !std::strcmp(mode, "reservation-byte-quota")) {
            const bool byteQuota = !std::strcmp(mode, "reservation-byte-quota");
            if (byteQuota) reserve.Size = 4ull << 30;
            for (unsigned n = 0; n < (byteQuota ? 4u : 8u); ++n)
                if (ioctl(fd, _IOWR('G', 8, D3DDDI_RESERVEGPUVIRTUALADDRESS), &reserve) || !reserve.VirtualAddress) return failed("reservation quota setup");
            reserve.Size = 65536;
            if (ioctl(fd, _IOWR('G', 8, D3DDDI_RESERVEGPUVIRTUALADDRESS), &reserve) != -1 || errno != EMFILE) return failed("reservation quota exceeded");
        } else if (!std::strcmp(mode, "reservation-duplicate") || !std::strcmp(mode, "reservation-reply-overlap")) {
            if (ioctl(fd, _IOWR('G', 8, D3DDDI_RESERVEGPUVIRTUALADDRESS), &reserve)) return failed("reservation duplicate setup");
            if (ioctl(fd, _IOWR('G', 8, D3DDDI_RESERVEGPUVIRTUALADDRESS), &reserve) != -1 || errno != EPROTO) return failed("reservation duplicate output");
            if (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO) return failed("reservation broken transport reused");
        } else if (!std::strcmp(mode, "reservation-normal") || !std::strcmp(mode, "reservation-failed-free") ||
                   !std::strcmp(mode, "reservation-overlap") || !std::strncmp(mode, "reservation-free-", 17)) {
            if (ioctl(fd, _IOWR('G', 8, D3DDDI_RESERVEGPUVIRTUALADDRESS), &reserve) || !reserve.VirtualAddress || reserve.VirtualAddress % 65536)
                return failed("reservation setup");
            if (!std::strcmp(mode, "reservation-overlap")) {
                auto overlap = reserve; overlap.BaseAddress = reserve.VirtualAddress; overlap.MinimumAddress = overlap.MaximumAddress = 0;
                if (ioctl(fd, _IOWR('G', 8, D3DDDI_RESERVEGPUVIRTUALADDRESS), &overlap) != -1 || errno != EBUSY) return failed("reservation overlapping request");
            }
            D3DKMT_FREEGPUVIRTUALADDRESS release{}; release.hAdapter = adapter.hAdapter; release.BaseAddress = reserve.VirtualAddress; release.Size = reserve.Size;
            auto partial = release; partial.Size = 4096;
            if (ioctl(fd, _IOWR('G', 32, D3DKMT_FREEGPUVIRTUALADDRESS), &partial) != -1 || errno != EBADF) return failed("reservation partial release");
            auto foreign = release; foreign.hAdapter = 999;
            if (ioctl(fd, _IOWR('G', 32, D3DKMT_FREEGPUVIRTUALADDRESS), &foreign) != -1 || errno != EBADF) return failed("reservation foreign release");
            if (!std::strncmp(mode, "reservation-free-", 17)) {
                if (ioctl(fd, _IOWR('G', 32, D3DKMT_FREEGPUVIRTUALADDRESS), &release) != -1 || errno != EPROTO) return failed("reservation malformed free reply");
                if (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO) return failed("reservation broken free reused");
            } else {
                if (!std::strcmp(mode, "reservation-failed-free") &&
                    (ioctl(fd, _IOWR('G', 32, D3DKMT_FREEGPUVIRTUALADDRESS), &release) != -1 || errno != EINVAL)) return failed("reservation failed release");
                if (ioctl(fd, _IOWR('G', 32, D3DKMT_FREEGPUVIRTUALADDRESS), &release)) return failed("reservation release retry");
                if (ioctl(fd, _IOWR('G', 32, D3DKMT_FREEGPUVIRTUALADDRESS), &release) != -1 || errno != EBADF) return failed("reservation stale range");
            }
        } else {
            const auto expected = !std::strcmp(mode, "reservation-disabled") ? ENOSYS : !std::strcmp(mode, "reservation-nt-failure") ? EINVAL : EPROTO;
            if (ioctl(fd, _IOWR('G', 8, D3DDDI_RESERVEGPUVIRTUALADDRESS), &reserve) != -1 || errno != expected || reserve.VirtualAddress)
                return failed("reservation invalid reply");
            if (expected == EPROTO && (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO)) return failed("reservation broken reply reused");
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "submit-", 7)) {
        unsigned char data[]{37, 38, 39, 40};
        D3DKMT_SUBMITCOMMANDTOHWQUEUE submit{}; submit.hHwQueue = 999; submit.CommandBuffer = 65536;
        submit.CommandLength = 4096; submit.PrivateDriverDataSize = sizeof data; submit.pPrivateDriverData = data;
        submit.HwQueueProgressFenceId = 5;
        if (!std::strcmp(mode, "submit-ignored-pointer"))
            submit.WrittenPrimaries = reinterpret_cast<const D3DKMT_HANDLE*>(1);
        if (!std::strcmp(mode, "submit-invalid")) {
            for (unsigned n = 0; n < 12; ++n) {
                auto invalid = submit;
                switch (n) {
                    case 0: invalid.CommandBuffer = 0; break;
                    case 1: ++invalid.CommandBuffer; break;
                    case 2: invalid.CommandBuffer = (1ull << 48) - 4096; invalid.CommandLength = 8192; break;
                    case 3: invalid.CommandLength = 0; break;
                    case 4: ++invalid.CommandLength; break;
                    case 5: invalid.CommandLength = 1048576 + 4096; break;
                    case 6: invalid.HwQueueProgressFenceId = 0; break;
                    case 7: invalid.HwQueueProgressFenceId = UINT64_MAX; break;
                    case 8: invalid.PrivateDriverDataSize = 4001; break;
                    case 9: invalid.NumPrimaries = 1; break;
                    case 10: invalid.NumPrimaries = 1; invalid.WrittenPrimaries = &submit.hHwQueue; break;
                    default: invalid.pPrivateDriverData = nullptr; break;
                }
                if (ioctl(fd, _IOWR('G', 52, D3DKMT_SUBMITCOMMANDTOHWQUEUE), &invalid) != -1 || errno != EINVAL)
                    return failed("submit invalid input");
            }
        } else {
            const auto expected = !std::strcmp(mode, "submit-disabled") ? ENOSYS : EBADF;
            if (ioctl(fd, _IOWR('G', 52, D3DKMT_SUBMITCOMMANDTOHWQUEUE), &submit) != -1 || errno != expected)
                return failed("submit unowned/disabled input");
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "sync-", 5)) {
        D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart = 0x57475055;
        if (ioctl(fd, _IOWR('G', 1, D3DKMT_OPENADAPTERFROMLUID), &adapter)) return failed("sync adapter");
        D3DKMT_CREATEDEVICE device{}; device.hAdapter = adapter.hAdapter; device.Flags.RequestVSync = 1;
        if (ioctl(fd, _IOWR('G', 2, D3DKMT_CREATEDEVICE), &device)) return failed("sync device");
        const bool mutex = !std::strcmp(mode, "sync-mutex") || !std::strcmp(mode, "sync-failed-destroy") ||
                           !std::strcmp(mode, "sync-mutex-bad-map") || !std::strncmp(mode, "sync-destroy-", 13);
        D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create{}; create.hDevice = device.hDevice;
        create.Info.Type = mutex ? D3DDDI_SYNCHRONIZATION_MUTEX : D3DDDI_MONITORED_FENCE;
        if (mutex) create.Info.SynchronizationMutex.InitialState = 1;
        else { create.Info.MonitoredFence.InitialFenceValue = 42; create.Info.MonitoredFence.EngineAffinity = 1; }
        if (!std::strncmp(mode, "sync-nogpu-", 11)) create.Info.Flags.NoGPUAccess = 1;
        if (!std::strncmp(mode, "sync-nomax-", 11)) create.Info.Flags.NoSignalMaxValueOnTdr = 1;
        if (!std::strcmp(mode, "sync-invalid")) {
            for (unsigned n = 0; n < 15; ++n) {
                auto invalid = create;
                switch (n) {
                    case 0: invalid.Info.Flags.Value = 1; break;
                    case 1: invalid.Info.MonitoredFence.EngineAffinity = 2; break;
                    case 2: invalid.Info.MonitoredFence.Padding = 1; break;
                    case 3: invalid.Info.Type = D3DDDI_SYNCHRONIZATION_MUTEX; invalid.Info.SynchronizationMutex.InitialState = 2; break;
                    case 4: invalid.Info.Type = D3DDDI_CPU_NOTIFICATION; break;
                    case 5: invalid.Info.Type = D3DDDI_SEMAPHORE; break;
                    case 6: invalid.hDevice = 999; break;
                    case 7: invalid.hDevice = adapter.hAdapter; break;
                    case 8: invalid.Info.Type = D3DDDI_SYNCHRONIZATION_MUTEX; invalid.Info.Flags.NoGPUAccess = 1; break;
                    case 9: invalid.Info.Flags.Value = 129; break;
                    case 10: invalid.Info.Flags.Value = 256; break;
                    case 11: invalid.Info.Type = D3DDDI_SYNCHRONIZATION_MUTEX; invalid.Info.Flags.NoSignalMaxValueOnTdr = 1; break;
                    case 12: invalid.Info.Flags.Value = 192; break;
                    case 13: invalid.Info.Flags.Value = 65; break;
                    default: invalid.Info.SharedHandle = 123; break;
                }
                const auto expected = n < 4 || n >= 8 ? EINVAL : n < 6 ? ENOSYS : EBADF;
                if (ioctl(fd, _IOWR('G', 16, D3DKMT_CREATESYNCHRONIZATIONOBJECT2), &invalid) != -1 || errno != expected) return failed("sync invalid input");
            }
            D3DKMT_DESTROYSYNCHRONIZATIONOBJECT release{}; release.hSyncObject = device.hDevice;
            if (ioctl(fd, _IOWR('G', 29, D3DKMT_DESTROYSYNCHRONIZATIONOBJECT), &release) != -1 || errno != EBADF) return failed("sync wrong-kind destruction");
        } else if (mutex && std::strcmp(mode, "sync-mutex-bad-map")) {
            if (ioctl(fd, _IOWR('G', 16, D3DKMT_CREATESYNCHRONIZATIONOBJECT2), &create) || create.hSyncObject != 3 || create.Info.SharedHandle)
                return failed("sync mutex creation");
            D3DKMT_DESTROYSYNCHRONIZATIONOBJECT release{}; release.hSyncObject = create.hSyncObject;
            if (!std::strncmp(mode, "sync-destroy-", 13)) {
                if (ioctl(fd, _IOWR('G', 29, D3DKMT_DESTROYSYNCHRONIZATIONOBJECT), &release) != -1 || errno != EPROTO)
                    return failed("sync malformed destruction");
                if (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO) return failed("sync broken destruction reused");
            } else {
                if (!std::strcmp(mode, "sync-failed-destroy") &&
                    (ioctl(fd, _IOWR('G', 29, D3DKMT_DESTROYSYNCHRONIZATIONOBJECT), &release) != -1 || errno != EINVAL))
                    return failed("sync failed destruction");
                if (ioctl(fd, _IOWR('G', 29, D3DKMT_DESTROYSYNCHRONIZATIONOBJECT), &release)) return failed("sync destruction retry");
                if (ioctl(fd, _IOWR('G', 29, D3DKMT_DESTROYSYNCHRONIZATIONOBJECT), &release) != -1 || errno != EBADF) return failed("sync stale handle");
            }
        } else {
            const auto expected = !std::strcmp(mode, "sync-disabled") ? ENOSYS : !std::strcmp(mode, "sync-nt-failure") ? EINVAL :
                                  (!std::strcmp(mode, "sync-no-hub") || !std::strcmp(mode, "sync-nogpu-no-hub") || !std::strcmp(mode, "sync-nomax-no-hub")) ? EIO : EPROTO;
            if (ioctl(fd, _IOWR('G', 16, D3DKMT_CREATESYNCHRONIZATIONOBJECT2), &create) != -1 || errno != expected || create.hSyncObject || create.Info.SharedHandle ||
                (!mutex && (create.Info.MonitoredFence.FenceValueCPUVirtualAddress || create.Info.MonitoredFence.FenceValueGPUVirtualAddress)))
                return failed("sync malformed response");
            if ((expected == EPROTO || expected == EIO) && (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO)) return failed("sync broken transport reused");
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "hwqueue-", 8)) {
        D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart = 0x57475055;
        if (ioctl(fd, _IOWR('G', 1, D3DKMT_OPENADAPTERFROMLUID), &adapter)) return failed("queue adapter");
        D3DKMT_CREATEDEVICE device{}; device.hAdapter = adapter.hAdapter; device.Flags.RequestVSync = 1;
        if (ioctl(fd, _IOWR('G', 2, D3DKMT_CREATEDEVICE), &device)) return failed("queue device");
        unsigned char contextData[]{37, 38, 39, 40};
        D3DKMT_CREATECONTEXTVIRTUAL context{}; context.hDevice = device.hDevice;
        const bool sync = !std::strcmp(mode, "hwqueue-sync-context");
        context.EngineAffinity = sync ? 0 : 1; context.Flags.Value = sync ? 8 : 16;
        context.ClientHint = sync ? D3DKMT_CLIENTHINT_UNKNOWN : D3DKMT_CLIENTHINT_DX12;
        context.PrivateDriverDataSize = sync ? 0 : sizeof contextData; context.pPrivateDriverData = sync ? nullptr : contextData;
        if (ioctl(fd, _IOWR('G', 4, D3DKMT_CREATECONTEXTVIRTUAL), &context) || context.hContext != 3) return failed("queue context");
        unsigned char data[]{37, 38, 39, 40};
        D3DKMT_CREATEHWQUEUE queue{}; queue.hHwContext = context.hContext; queue.pPrivateDriverData = data; queue.PrivateDriverDataSize = sizeof data;
        if (!std::strcmp(mode, "hwqueue-invalid")) {
            for (unsigned n = 0; n < 8; ++n) {
                auto invalid = queue;
                switch (n) {
                    case 0: invalid.Flags.Value = 1; break;
                    case 1: invalid.Flags.Value = 2; break;
                    case 2: invalid.Flags.Value = 4; break;
                    case 3: invalid.PrivateDriverDataSize = 0; break;
                    case 4: invalid.PrivateDriverDataSize = 4001; break;
                    case 5: invalid.pPrivateDriverData = nullptr; break;
                    case 6: invalid.hHwContext = 999; break;
                    default: invalid.hHwContext = device.hDevice; break;
                }
                if (ioctl(fd, _IOWR('G', 24, D3DKMT_CREATEHWQUEUE), &invalid) != -1 || errno != (n < 6 ? EINVAL : EBADF)) return failed("queue invalid input");
            }
            D3DKMT_DESTROYHWQUEUE release{}; release.hHwQueue = 999;
            if (ioctl(fd, _IOWR('G', 27, D3DKMT_DESTROYHWQUEUE), &release) != -1 || errno != EBADF) return failed("queue unowned destruction");
        } else {
            const auto expected = !std::strcmp(mode, "hwqueue-disabled") || sync ? ENOSYS :
                                  !std::strcmp(mode, "hwqueue-nt-failure") ? EINVAL : !std::strcmp(mode, "hwqueue-no-hub") ? EIO : EPROTO;
            if (ioctl(fd, _IOWR('G', 24, D3DKMT_CREATEHWQUEUE), &queue) != -1 || errno != expected || queue.hHwQueue ||
                queue.hHwQueueProgressFence || queue.HwQueueProgressFenceCPUVirtualAddress || queue.HwQueueProgressFenceGPUVirtualAddress)
                return failed("queue malformed response");
            if (!std::strcmp(mode, "hwqueue-nt-failure") && data[0] != (37 ^ 255)) return failed("queue failure private in/out");
            if ((expected == EPROTO || expected == EIO) && (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO)) return failed("queue broken transport reused");
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "translation-", 12)) {
        D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart = 0x57475055;
        if (ioctl(fd, _IOWR('G', 1, D3DKMT_OPENADAPTERFROMLUID), &adapter)) return failed("translation adapter");
        D3DKMT_CREATEDEVICE device{}; device.hAdapter = adapter.hAdapter; device.Flags.RequestVSync = 1;
        if (ioctl(fd, _IOWR('G', 2, D3DKMT_CREATEDEVICE), &device)) return failed("translation device");
        unsigned char data[]{37, 38, 39, 40};
        D3DDDI_ALLOCATIONINFO2 info{}; info.Flags.Value = 4; info.Priority = 0x78100000;
        info.pPrivateDriverData = data; info.PrivateDriverDataSize = sizeof data;
        D3DKMT_CREATEALLOCATION allocation{}; allocation.hDevice = device.hDevice; allocation.NumAllocations = 1;
        allocation.pAllocationInfo2 = &info;
        const bool success = !std::strcmp(mode, "translation-normal") || !std::strcmp(mode, "translation-invalid") || !std::strcmp(mode, "translation-disabled");
        const auto created = ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation);
        if (success) {
            const D3DKMT_HANDLE alias = !std::strcmp(mode, "translation-disabled") ? 3 : 0x10000003;
            if (created || info.hAllocation != alias || data[0] != (37 ^ 255)) return failed("translation allocation alias");
            D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE known{};
            known.EscapeType = D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE; known.hAllocation = alias;
            D3DKMT_ESCAPE escape{}; escape.hAdapter = adapter.hAdapter; escape.hDevice = device.hDevice;
            escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE; escape.Flags.DriverKnownEscape = 1;
            escape.pPrivateDriverData = &known; escape.PrivateDriverDataSize = sizeof known;
            if (!std::strcmp(mode, "translation-disabled")) {
                if (ioctl(fd, _IOWR('G', 13, D3DKMT_ESCAPE), &escape) != -1 || errno != ENOSYS) return failed("translation disabled escape");
            } else {
                if (!std::strcmp(mode, "translation-invalid")) {
                    for (unsigned n = 0; n < 4; ++n) {
                        auto invalid = escape; auto invalidKnown = known; invalid.pPrivateDriverData = &invalidKnown;
                        switch (n) {
                            case 0: invalid.hContext = 999; break;
                            case 1: invalid.hDevice = 999; break;
                            case 2: invalid.hAdapter = 999; break;
                            default: invalidKnown.hAllocation = 3; break;
                        }
                        if (ioctl(fd, _IOWR('G', 13, D3DKMT_ESCAPE), &invalid) != -1 || errno != (n == 0 ? EINVAL : EBADF)) return failed("translation wrong identity");
                    }
                }
                if (ioctl(fd, _IOWR('G', 13, D3DKMT_ESCAPE), &escape) || known.hAllocation != alias) return failed("translation repeated known escape");
            }
            D3DKMT_HANDLE handle = alias;
            D3DKMT_DESTROYALLOCATION2 release{}; release.hDevice = device.hDevice; release.phAllocationList = &handle; release.AllocationCount = 1;
            if (alias != 3) {
                handle = 3;
                if (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release) != -1 || errno != EBADF) return failed("raw wire ID accepted as guest alias");
                handle = alias;
            }
            if (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release)) return failed("translation alias destruction");
        } else {
            const auto expected = !std::strcmp(mode, "translation-nt-failure") || !std::strcmp(mode, "translation-failed-cleanup") ? EINVAL : EPROTO;
            if (created != -1 || errno != expected || info.hAllocation || data[0] != (37 ^ 255)) return failed("translation malformed response");
            if (expected == EPROTO && (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO)) return failed("translation broken connection reused");
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "cpu-", 4)) {
        D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart = 0x57475055;
        if (ioctl(fd, _IOWR('G', 1, D3DKMT_OPENADAPTERFROMLUID), &adapter)) return failed("CPU adapter");
        D3DKMT_CREATEDEVICE device{}; device.hAdapter = adapter.hAdapter; device.Flags.RequestVSync = 1;
        if (ioctl(fd, _IOWR('G', 2, D3DKMT_CREATEDEVICE), &device)) return failed("CPU device");
        unsigned char data[]{37, 38, 39, 40};
        D3DDDI_ALLOCATIONINFO2 info{}; info.Flags.Value = 4; info.Priority = 0x78100000;
        info.pPrivateDriverData = data; info.PrivateDriverDataSize = sizeof data;
        D3DKMT_CREATEALLOCATION allocation{}; allocation.hDevice = device.hDevice; allocation.NumAllocations = 1;
        allocation.pAllocationInfo2 = &info;
        if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) || info.hAllocation != 3) return failed("CPU allocation");
        D3DKMT_LOCK2 lock{}; lock.hDevice = device.hDevice; lock.hAllocation = info.hAllocation;
        D3DKMT_UNLOCK2 unlock{}; unlock.hDevice = device.hDevice; unlock.hAllocation = info.hAllocation;
        if (!std::strcmp(mode, "cpu-invalid")) {
            if (ioctl(fd, _IOWR('G', 55, D3DKMT_UNLOCK2), &unlock) != -1 || errno != EBADF) return failed("CPU unlock before lock");
            lock.Flags.Value = 1;
            if (ioctl(fd, _IOWR('G', 37, D3DKMT_LOCK2), &lock) != -1 || errno != EINVAL) return failed("CPU reserved flags");
            lock.Flags.Value = 0; lock.hDevice = 999;
            if (ioctl(fd, _IOWR('G', 37, D3DKMT_LOCK2), &lock) != -1 || errno != EBADF) return failed("CPU wrong device");
            lock.hDevice = device.hDevice; lock.hAllocation = 999;
            if (ioctl(fd, _IOWR('G', 37, D3DKMT_LOCK2), &lock) != -1 || errno != EBADF) return failed("CPU wrong allocation");
            if (ioctl(fd, _IOWR('G', 37, unsigned), &lock) != -1 || errno != EINVAL) return failed("CPU wrong ABI size");
        } else {
            const auto expected = !std::strcmp(mode, "cpu-disabled") ? ENOSYS : !std::strcmp(mode, "cpu-nt-failure") ? EINVAL :
                                  !std::strcmp(mode, "cpu-no-hub") ? EIO : EPROTO;
            if (ioctl(fd, _IOWR('G', 37, D3DKMT_LOCK2), &lock) != -1 || errno != expected || lock.pData) return failed("CPU malformed response");
            if ((expected == EPROTO || expected == EIO) && (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO)) return failed("CPU broken connection reused");
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "resident-", 9)) {
        D3DKMT_HANDLE allocation = 10;
        UINT priority = 0x78100000;
        D3DDDI_MAKERESIDENT resident{}; resident.hPagingQueue = 9; resident.NumAllocations = 1;
        resident.AllocationList = &allocation; resident.PriorityList = &priority; resident.Flags.Value = 1;
        if (!std::strcmp(mode, "resident-disabled")) {
            if (ioctl(fd, _IOWR('G', 11, D3DDDI_MAKERESIDENT), &resident) != -1 || errno != ENOSYS) return failed("disabled residency");
        } else {
            for (unsigned n = 0; n < 5; ++n) {
                auto invalid = resident;
                switch (n) {
                    case 0: invalid.NumAllocations = 0; break;
                    case 1: invalid.NumAllocations = driver_bridge::MaxVendorAllocations + 1; break;
                    case 2: invalid.Flags.Value = 2; break;
                    case 3: invalid.Flags.Value = 4; break;
                    default: invalid.AllocationList = nullptr; break;
                }
                if (ioctl(fd, _IOWR('G', 11, D3DDDI_MAKERESIDENT), &invalid) != -1 || errno != EINVAL) return failed("invalid residency descriptor");
            }
            if (ioctl(fd, _IOWR('G', 11, D3DDDI_MAKERESIDENT), &resident) != -1 || errno != EBADF) return failed("residency unowned queue");
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "gpuva-", 6)) {
        D3DDDI_MAPGPUVIRTUALADDRESS map{}; map.hPagingQueue = 9; map.hAllocation = 10;
        map.MinimumAddress = 67108864; map.MaximumAddress = 1099511627776ull;
        map.SizeInPages = 16; map.Protection.Write = 1;
        if (!std::strcmp(mode, "gpuva-disabled")) {
            if (ioctl(fd, _IOWR('G', 12, D3DDDI_MAPGPUVIRTUALADDRESS), &map) != -1 || errno != ENOSYS) return failed("disabled GPU-address mapping");
        } else {
            for (unsigned n = 0; n < 9; ++n) {
                auto invalid = map;
                switch (n) {
                    case 0: invalid.Reserved0 = 1; break;
                    case 1: invalid.Reserved1 = 1; break;
                    case 2: invalid.SizeInPages = 0; break;
                    case 3: invalid.SizeInPages = driver_bridge::MaxVendorMapPages + 1; break;
                    case 4: invalid.OffsetInPages = ~0ull; break;
                    case 5: invalid.Protection.Value = 16; break;
                    case 6: invalid.DriverProtection = 1; break;
                    case 7: invalid.BaseAddress = 1; break;
                    default: invalid.MaximumAddress = invalid.MinimumAddress; break;
                }
                if (ioctl(fd, _IOWR('G', 12, D3DDDI_MAPGPUVIRTUALADDRESS), &invalid) != -1 || errno != EINVAL) return failed("invalid GPU-address descriptor");
            }
            if (ioctl(fd, _IOWR('G', 12, D3DDDI_MAPGPUVIRTUALADDRESS), &map) != -1 || errno != EBADF) return failed("GPU-address unowned object");
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "allocation-", 11)) {
        D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart = 0x57475055;
        if (ioctl(fd, _IOWR('G', 1, D3DKMT_OPENADAPTERFROMLUID), &adapter)) return failed("allocation adapter");
        D3DKMT_CREATEDEVICE device{}; device.hAdapter = adapter.hAdapter; device.Flags.RequestVSync = 1;
        if (ioctl(fd, _IOWR('G', 2, D3DKMT_CREATEDEVICE), &device)) return failed("allocation device");
        unsigned char data[]{37, 38, 39, 40};
        D3DDDI_ALLOCATIONINFO2 info{}; info.Flags.Value = 4; info.Priority = 0x78100000;
        info.pPrivateDriverData = data; info.PrivateDriverDataSize = sizeof data;
        D3DKMT_CREATEALLOCATION allocation{}; allocation.hDevice = device.hDevice; allocation.NumAllocations = 1;
        allocation.pAllocationInfo2 = &info;
        if (!std::strcmp(mode, "allocation-maximum")) info.Priority = D3DDDI_ALLOCATIONPRIORITY_MAXIMUM;
        if (!std::strcmp(mode, "allocation-uninitialized-source")) info.VidPnSourceId = D3DDDI_ID_UNINITIALIZED;
        if (!std::strcmp(mode, "allocation-invalid")) {
            info.pSystemMem = data;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL) return failed("guest CPU pointer accepted");
            info.pSystemMem = nullptr; info.Reserved[0] = 1;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL) return failed("reserved input accepted");
            info.Reserved[0] = 0; info.Priority = 0xc8000001;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL) return failed("out-of-range priority accepted");
            info.Priority = 0x78100000; info.VidPnSourceId = 1;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL) return failed("display source accepted");
            info.VidPnSourceId = D3DDDI_ID_ANY;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL) return failed("any display source accepted");
            info.VidPnSourceId = D3DDDI_ID_UNINITIALIZED; info.Flags.Value = 5;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL) return failed("primary unset source accepted");
            info.VidPnSourceId = 0; info.Flags.Value = 4; allocation.hResource = 999;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != ENOSYS) return failed("resource accepted");
        } else if (!std::strcmp(mode, "allocation-disabled")) {
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != ENOSYS) return failed("disabled allocation accepted");
        } else if (!std::strcmp(mode, "allocation-nt-failure")) {
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL ||
                data[0] != (37 ^ 255) || info.hAllocation) return failed("native failure lost in/out");
        } else if (!std::strcmp(mode, "allocation-normal") || !std::strcmp(mode, "allocation-maximum") || !std::strcmp(mode, "allocation-uninitialized-source") || !std::strcmp(mode, "allocation-failed-destroy") || !std::strncmp(mode, "allocation-destroy-", 19)) {
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) || info.hAllocation != 3 || info.GpuVirtualAddress ||
                data[0] != (37 ^ 255) || allocation.hResource || allocation.hGlobalShare) return failed("allocation response");
            if (!std::strcmp(mode, "allocation-uninitialized-source") && info.VidPnSourceId != D3DDDI_ID_UNINITIALIZED)
                return failed("guest display source mutated");
            D3DKMT_HANDLE duplicates[]{3, 3};
            D3DKMT_DESTROYALLOCATION2 release{}; release.hDevice = device.hDevice; release.phAllocationList = duplicates;
            release.AllocationCount = 2;
            if (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release) != -1 || errno != EINVAL) return failed("duplicate destruction");
            release.AllocationCount = 1; release.hDevice = 999;
            if (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release) != -1 || errno != EBADF) return failed("wrong device destruction");
            release.hDevice = device.hDevice; release.Flags.Value = 3;
            if (!std::strncmp(mode, "allocation-destroy-", 19)) {
                if (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release) != -1 || errno != EPROTO) return failed("malformed destruction accepted");
                if (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO) return failed("destruction broken transport reused");
                close(fd); return 0;
            }
            if (!std::strcmp(mode, "allocation-failed-destroy") &&
                (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release) != -1 || errno != EINVAL)) return failed("native destroy failure");
            if (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release)) return failed("allocation destruction");
            if (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release) != -1 || errno != EBADF) return failed("destroyed ID reused");
        } else {
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EPROTO) return failed("malformed allocation accepted");
            if (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO) return failed("allocation broken transport reused");
        }
        close(fd); return 0;
    }
    if (!std::strncmp(mode, "paging-", 7)) {
        D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart = 0x57475055;
        if (ioctl(fd, _IOWR('G', 1, D3DKMT_OPENADAPTERFROMLUID), &adapter)) return failed("paging adapter");
        D3DKMT_CREATEDEVICE device{}; device.hAdapter = adapter.hAdapter; device.Flags.RequestVSync = 1;
        if (ioctl(fd, _IOWR('G', 2, D3DKMT_CREATEDEVICE), &device)) return failed("paging device");
        D3DKMT_CREATEPAGINGQUEUE paging{}; paging.hDevice = device.hDevice;
        paging.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        if (!std::strcmp(mode, "paging-invalid")) {
            paging.PhysicalAdapterIndex = 1;
            if (ioctl(fd, _IOWR('G', 7, D3DKMT_CREATEPAGINGQUEUE), &paging) != -1 || errno != ENOSYS) return failed("paging invalid adapter index");
            paging.PhysicalAdapterIndex = 0; paging.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_ABOVE_NORMAL;
            if (ioctl(fd, _IOWR('G', 7, D3DKMT_CREATEPAGINGQUEUE), &paging) != -1 || errno != ENOSYS) return failed("paging invalid priority");
        } else {
            const auto expected = !std::strcmp(mode, "paging-disabled") ? ENOSYS : !std::strcmp(mode, "paging-no-hub") ? EIO : EPROTO;
            if (ioctl(fd, _IOWR('G', 7, D3DKMT_CREATEPAGINGQUEUE), &paging) != -1 || errno != expected) return failed("paging failure boundary");
            if (expected != ENOSYS && (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO)) return failed("paging broken transport reused");
        }
        close(fd); return 0;
    }
    if (!std::strcmp(mode, "reuse")) {
        if (close(fd)) return failed("close");
        const auto other = static_cast<int>(syscall(SYS_memfd_create, "ordinary-file", MFD_CLOEXEC));
        if (other < 0 || (other != fd && dup2(other, fd) != fd)) return failed("reuse descriptor");
        if (other != fd) close(other);
        D3DKMT_ENUMADAPTERS3 a{};
        if (ioctl(fd, _IOWR('G', 62, D3DKMT_ENUMADAPTERS3), &a) != -1 || errno != ENOTTY) return failed("reused fd routed to bridge");
        close(fd); return 0;
    }
    if (!std::strcmp(mode, "bad-reply") || !std::strcmp(mode, "closed-worker")) {
        D3DKMT_OPENADAPTERFROMLUID a{}; a.AdapterLuid.LowPart = 0x57475055;
        if (ioctl(fd, _IOWR('G', 1, D3DKMT_OPENADAPTERFROMLUID), &a) != -1 ||
            errno != (!std::strcmp(mode, "bad-reply") ? EPROTO : EIO)) return failed("broken response");
        if (open("/dev/dxg", O_RDONLY) != -1 || errno != EIO) return failed("broken connection reused");
        close(fd); return 0;
    }
    D3DKMT_ENUMADAPTERS3 a{};
    if (ioctl(fd, _IOWR('G', 62, D3DKMT_ENUMADAPTERS3), &a) || a.NumAdapters != 1) return failed("enum count");
    D3DKMT_ADAPTERINFO info{}; a.pAdapters = &info;
    if (ioctl(fd, _IOWR('G', 62, D3DKMT_ENUMADAPTERS3), &a) || !info.hAdapter || info.NumOfSources) return failed("enum adapter");
    if (ioctl(fd, _IOWR('G', 250, unsigned), &a) != -1 || errno != ENOSYS) return failed("unsupported ioctl forwarded");
    unsigned value = 0;
    D3DKMT_QUERYADAPTERINFO query{}; query.hAdapter = info.hAdapter; query.Type = KMTQAITYPE_ADAPTERTYPE;
    query.pPrivateDriverData = &value; query.PrivateDriverDataSize = sizeof value;
    if (ioctl(fd, _IOWR('G', 9, D3DKMT_QUERYADAPTERINFO), &query) || value != 0x2091) return failed("PV flags");
    query.PrivateDriverDataSize = 5;
    if (ioctl(fd, _IOWR('G', 9, D3DKMT_QUERYADAPTERINFO), &query) != -1 || errno != ENOSYS) return failed("invalid layout accepted");
    D3DKMT_CLOSEADAPTER closeAdapter{}; closeAdapter.hAdapter = info.hAdapter;
    if (ioctl(fd, _IOWR('G', 21, D3DKMT_CLOSEADAPTER), &closeAdapter)) return failed("close adapter");
    close(fd); return 0;
}
