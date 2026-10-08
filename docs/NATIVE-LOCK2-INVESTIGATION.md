# Native NVIDIA Lock2 CPU sharing

`D3DKMTLock2` succeeded on the RTX 5090 Laptop GPU for the same local captured
586-byte allocation input used by the existing native allocation controls.
After native creation, a 16-page GPU mapping and residency, it returned a
dedicated, aligned 64 KiB `MEM_PRIVATE` CPU region with
`PAGE_READWRITE | PAGE_WRITECOMBINE` protection. An isolated child WHP partition
read the actual driver memory. A read-only mapping denied a guest store.
A separate writable mapping covered the 64 KiB region; guest writes and
readback at its first and last pages matched Windows' view. The parent restored
both original words after the child exited, then unlocked and synchronously
destroyed the allocation. Paging, device and adapter cleanup also succeeded.
The [metadata-only hardware report](evidence/WHP-NATIVE-LOCK2-2026-10-08.json)
pins the tested source, executable and private input hashes.

This is a native dependency experiment with one captured private input, not
a live UMD or QEMU acceptance run. It submits no GPU command stream. It does
not prove that arbitrary NVIDIA allocations are safely shareable, determine
their opaque allocation sizes, establish a hard GPU-memory quota, measure
desktop performance or accelerate Hyprland. The live QEMU runtime currently
stops at `Lock2` (ioctl 37) after successful allocation, GPU VA and residency.
Its [live request observation](evidence/QEMU-LIVE-LOCK2-INPUT-2026-10-08.json)
confirmed flags zero, an owned allocation from the matching device and a null
input data pointer. That disk-free QEMU run preserved native residency and
30,000 direct paging-fence loads, then cleaned up successfully. The writable
allocation hub was not attached in this input-only observation.

## Supported Windows contract

[Lock2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtlock2)
returns CPU access through
[D3DKMT_LOCK2.pData](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_lock2).
[Lock2 flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddicb_lock2flags)
are reserved and must be zero. The Microsoft WSL kernel's `dxgkio_lock2`
and `dxgvmb_send_lock2` map CPU-visible allocation memory into Linux and retain
per-allocation CPU references until the final `Unlock2`. That code is a
lifetime reference; this bridge does not forward calls through WSL's dxg device.

[WHvMapGpaRange2](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/funcs/whvmapgparange2)
accepts a source-process handle with VM read/write/operation rights. A child
partition maps the parent's actual driver-owned pages with read/write flags
and no execute flag. The native probe inherits only that handle. Five-second
VP execution and bounded child waits govern the test. The parent keeps the
locked allocation alive until the child is reaped, including stop failure.
Guest stores use SFENCE before readback; host visibility and restoration are
checked separately after child teardown.

## Reproduce the native control

Build with `experimental/windows-native-gpu/build.ps1`, then run
`vendor-lock-probe.exe C:/local/wsl-allocation-input-1.bin` using the existing
private local capture. The capture must have the documented control-file
header; it is not part of the repository or CI. The probe requires the observed
64 KiB dedicated CPU layout and tests only that layout. `VirtualQuery` describes
accessible virtual memory; it is not a general vendor allocation-size decoder.
The utility outputs metadata and result codes, without host addresses,
private bytes or memory contents. Hosted CI builds it without vendor binaries.

## QEMU route under implementation

The new `wddm-allocation-hub` supplies a separate, bounded writable PCI aperture
and WHPX foreign mapping path. It retains the existing read-only fence path.
Host-only commands validate slots, ranges and generation ownership; the native
owner must still validate the locked CPU extent and device/allocation identity.
Portable tests cover malformed, stale, occupied, unaligned, oversize and
overflowing commands. The pinned QEMU recipe applies in an isolated checkout.

Required next acceptance is a live guest `Lock2`/`Unlock2` route that returns a
guest CPU pointer into these actual pages. It must preserve native failures,
balance repeated lock references, remove guest and WHPX mappings before native
unlock/destruction, and stop/reap owned QEMU before freeing pages after transport
or control failure. Capability-disabled controls and physical read/write,
disconnect and memory regressions remain required. Kernel/DRM integration,
command submission, scanout, Hyprland, recovery, stability and performance
testing still remain beyond this initialization work.
