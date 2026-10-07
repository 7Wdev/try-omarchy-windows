# QEMU Windows WDDM bridge

The first control path is implemented and tested on 2026-10-07. A Linux guest
in the installed QEMU 11.0/WHPX runtime successfully calls documented Windows
WDDM adapter, device and paging queue operations on an NVIDIA RTX 5090 Laptop
GPU. This is a **partial driver control bridge**, not guest rendering support.
The Omarchy launcher does not select this backend yet.

## Implemented path

```text
Linux guest-probe
    -> Linux virtio_console kernel driver
    -> QEMU virtio-serial PCI device and virtserialport
    -> QEMU loopback socket chardev
    -> separate Windows driver-bridge worker
    -> gdi32 D3DKMT thunks -> Windows dxgkrnl / NVIDIA WDDM driver
```

The transport reuses QEMU's existing virtio serial device. It does not advertise
virtio-nvgpu device ID 45 or implement a custom virtio GPU. The guest program
uses a private experimental protocol; it does not expose `/dev/nvidia*`,
`/dev/dxg`, a DRM render node, or NVIDIA Linux user-mode driver compatibility.

The worker selects hardware NVIDIA using DXGI. Software adapters are refused.
Each process serves one connection, with at most 64 live objects and 10,000
requests. The TCP listener binds only 127.0.0.1 and has connect/read/write
timeouts. This development endpoint has no authentication and should run only
for the test's lifetime. Stdio is available for process-owned transport.

Requests never contain Windows handles, addresses, pointer-bearing KMT
structures, arbitrary ioctl codes, or vendor-private escape data. The worker
constructs fixed, documented KMT structures. Local object IDs are scoped to
the connection, never reused, checked for type and parent ownership, and
released in child-before-parent order when the connection ends. Windows process
teardown provides a further resource backstop. These controls do not make an
untrusted general-purpose driver proxy ready for production.

## Protocol v1

Each stream frame starts with a little-endian 32-bit packet length (16..4096).
The packet starts with the independently implemented virtio-nvgpu-style
16-byte header: type u32, object ID u32, status i32, zero padding u32.
Request status and padding must be zero. Original NVIDIA operations 1..8 and
all unrelated operations are rejected without a driver call.

| ID | Request | Windows operation |
| --- | --- | --- |
| 0x2000 | zero object ID + u32 version 1 | Negotiate; return version, flags, vendor ID, device ID. |
| 0x2001 | zero object ID, no payload | D3DKMTOpenAdapterFromLuid, selected host NVIDIA adapter. |
| 0x2002 | adapter ID | D3DKMTQueryAdapterInfo, only KMTQAITYPE_DRIVERVERSION. |
| 0x2003 | adapter ID | D3DKMTCloseAdapter; live children cause EBUSY. |
| 0x2004 | adapter ID | D3DKMTCreateDevice, zero flags. |
| 0x2005 | device ID | D3DKMTDestroyDevice; live children cause EBUSY. |
| 0x2006 | device ID | D3DKMTCreatePagingQueue, normal priority, physical index zero. |
| 0x2007 | queue ID | Read the host-only paging fence mapping; return value, never its address. |
| 0x2008 | queue ID | D3DKMTDestroyPagingQueue. |

Hello capability bit 0 means adapter/device lifecycle; bit 1 means paging queue
and fence query. No rendering, allocation, guest mapping or event capability
is advertised. Other successful dispatches return a 16-byte body containing
signed NTSTATUS, reserved zero u32, value u64. Native failure remains a native
NTSTATUS, distinct from negative errno in the header for protocol validation.
Creation returns a local object ID only when the driver succeeds.

## Physical acceptance

The guest performed five open/query/create-device/create-queue/read-fence/
destroy-queue/destroy-device/close cycles. Every native operation succeeded;
driver version query returned WDDM enum 3200. The guest also checked rejection
of stale handles and destruction of a device with a live paging queue. QEMU
and the Windows worker both exited successfully.
The probe then left a live adapter/device/queue hierarchy and disconnected.
The worker verified all three object counts returned to zero with no failed
driver cleanup calls.

See [machine-readable evidence](evidence/QEMU-WDDM-BRIDGE-2026-10-07.json).
Input kernel, generated initramfs and bridge executable hashes are recorded.
The test used no disk, network device, host share or existing VM process. It
used the installed guest kernel and a separate 3.3 MB initramfs containing only
a static init and probe. This test is intentionally a transport test; a
headless control probe does not meet the interactive Omarchy acceptance goal.

## Reproduce

Build the Windows worker using `experimental/windows-native-gpu/build.ps1`.
On x64 Linux/WSL with GCC, static C/C++ libraries and Python 3, run:

```sh
sh experimental/windows-native-gpu/build_guest.sh
```

On the Windows NVIDIA host, provide paths to a trusted QEMU/WHPX executable
and matching Linux kernel with built-in virtio_pci and virtio_console:

```powershell
python experimental/windows-native-gpu/test_qemu.py `
  --qemu C:\path\to\qemu-system-x86_64.exe `
  --kernel C:\path\to\vmlinuz-linux `
  --initramfs experimental/windows-native-gpu/build/bridge-test.cpio `
  --bridge experimental/windows-native-gpu/build/driver-bridge.exe `
  --report bridge-report.json
```

The test emits a JSON report and sibling log, and fails without the guest pass
marker, guest exit zero, QEMU exit zero and worker exit zero. It kills only its
own helper/VM processes on timeout. Hosted CI compiles this code and tests
the packet contract; it does not claim physical GPU acceptance.

## Reuse and next acceptance gate

[Microsoft libdxg](https://github.com/microsoft/libdxg/tree/5c28ebb4ead460c23ec5e87decb7d1af7285bb59)
provides MIT-licensed WDDM headers and Linux thunks to `/dev/dxg`. It is useful
for an adapted guest-facing WDDM library, but its current calls still require
the WSL kernel device. A bridge library implementing a small subset must fail
unsupported calls explicitly; substituting it into NVIDIA's WSL user-mode
driver stack has not been validated.

[WSL dxgkrnl source](https://github.com/microsoft/WSL2-Linux-Kernel/tree/d504d40ab83839cb61942a812a99d183c42227b9/drivers/hv/dxgkrnl)
shows the remaining requirements:

| Requirement | Source path / current gap |
| --- | --- |
| Guest process/device/context ownership | ioctl.c and dxgvmbus.c; current worker covers only its own process and typed device lifecycle. |
| Allocation backing | dxgvmb_send_create_allocation and existing system-memory store; Hyper-V GPADL pins/maps guest pages. A serial payload cannot replace that mapping. |
| GPU virtual addresses and residency | Map/reserve/update/make-resident plus paging fence synchronization; not implemented. |
| Actual command submission | Context/hardware queue creation, UMD-generated command buffers, submit and completion; not implemented. |
| Guest synchronization | Host events, mapped monitored fences, sync files/dma-fences; current fence query does not implement these. |
| Linux graphics userspace | WDDM-aware runtime/UMD, matching driver files and ABI; ordinary Linux NVIDIA RM userspace cannot use these messages. |
| Interactive desktop | DRM buffer sharing and compositor allocation, plus fenced scanout into the existing QEMU SDL window, resizing/input and crash recovery; not implemented. |

The next gate is a guest-owned allocation whose backing and residency are
verified on Windows, followed by a real guest command submission and completion.
Only then can a guest driver/userspace and scanout integration be accepted.
The current control success and separate native D3D12 host rendering success
must not be combined into a claim that the guest renders on the GPU.
