// SPDX-License-Identifier: MIT
// Fault tests for the diagnostic interposer. No real GPU calls are made.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
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
    if (!std::strcmp(mode, "no-worker") || !std::strcmp(mode, "wrong-version")) {
        if (fd != -1 || errno != (!std::strcmp(mode, "no-worker") ? EINVAL : EPROTO)) return failed("unexpected open result");
        return 0;
    }
    if (fd < 0) return failed("open");
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
        if (!std::strcmp(mode, "allocation-invalid")) {
            info.pSystemMem = data;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL) return failed("guest CPU pointer accepted");
            info.pSystemMem = nullptr; info.Reserved[0] = 1;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL) return failed("reserved input accepted");
            info.Reserved[0] = 0; allocation.hResource = 999;
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != ENOSYS) return failed("resource accepted");
        } else if (!std::strcmp(mode, "allocation-disabled")) {
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != ENOSYS) return failed("disabled allocation accepted");
        } else if (!std::strcmp(mode, "allocation-nt-failure")) {
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) != -1 || errno != EINVAL ||
                data[0] != (37 ^ 255) || info.hAllocation) return failed("native failure lost in/out");
        } else if (!std::strcmp(mode, "allocation-normal") || !std::strcmp(mode, "allocation-failed-destroy")) {
            if (ioctl(fd, _IOWR('G', 6, D3DKMT_CREATEALLOCATION), &allocation) || info.hAllocation != 3 || info.GpuVirtualAddress ||
                data[0] != (37 ^ 255) || allocation.hResource || allocation.hGlobalShare) return failed("allocation response");
            D3DKMT_HANDLE duplicates[]{3, 3};
            D3DKMT_DESTROYALLOCATION2 release{}; release.hDevice = device.hDevice; release.phAllocationList = duplicates;
            release.AllocationCount = 2;
            if (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release) != -1 || errno != EINVAL) return failed("duplicate destruction");
            release.AllocationCount = 1; release.hDevice = 999;
            if (ioctl(fd, _IOWR('G', 19, D3DKMT_DESTROYALLOCATION2), &release) != -1 || errno != EBADF) return failed("wrong device destruction");
            release.hDevice = device.hDevice; release.Flags.Value = 3;
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
