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
