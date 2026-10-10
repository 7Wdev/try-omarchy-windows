# QEMU Windows WDDM bridge

The first control path is implemented and tested on 2026-10-07. A Linux guest
in the installed QEMU 11.0/WHPX runtime successfully calls documented Windows
WDDM adapter, device, paging queue and allocation operations on an NVIDIA RTX 5090 Laptop
GPU. The current code additionally implements shared guest RAM registration and
a bounded NVIDIA GPU buffer copy through a Windows section-backed D3D12 heap.
The integrated QEMU shared-memory GPU copy passed six hardware cycles on
2026-10-08, with all objects released after disconnect. The later genuine Linux
runtime route now passes [guest shader drawing with verified pixels](LIVE-GPU-SHADER-TRIANGLE.md).
This remains a **partial driver bridge**. Opt-in virtual context lifecycle
support also passed QEMU acceptance on 2026-10-08; see the
[Linux UMD compatibility experiment](UMD-CONTEXT-COMPATIBILITY.md).
The Omarchy launcher does not select this backend yet.
The [live NVIDIA runtime diagnostic](LIVE-NVIDIA-RUNTIME.md) loads the local
Linux UMD inside QEMU through device initialization, GPU buffer copies and
bounded render-target clears and vertex/pixel shader drawing. The
[dynamic paging bridge](LIVE-PAGING-BRIDGE.md) directly maps queue fences and
acknowledges unmapping before releasing native pages. Historical paging-only
controls predate the separate [full D3D12 initialization milestone](LIVE-D3D12-INITIALIZATION.md).

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
uses a private experimental protocol. The new explicitly preloaded diagnostic
adapts a subset of the `/dev/dxg` ioctl ABI within its own process. There is no
guest kernel `/dev/dxg` device, `/dev/nvidia*`, DRM render node or full NVIDIA
Linux user-mode driver compatibility.

The worker selects hardware NVIDIA using DXGI. Software adapters are refused.
Each process serves one connection, with at most 64 live objects and 10,000
requests. Allocations are page-aligned, at most 1 MiB each and 16 MiB per
connection; data copies are bounded to 4064 bytes per request. The TCP listener binds only 127.0.0.1 and has connect/read/write
timeouts. This development endpoint has no authentication and should run only
for the test's lifetime. Stdio is available for process-owned transport.

Requests never contain raw KMT object handles, host CPU pointer fields,
pointer-bearing KMT structures, arbitrary ioctl codes, or vendor-private escape
data. The opt-in context operation carries up to 4000 opaque driver-private
bytes supplied by a compatible PV-aware UMD. This payload is not interpreted or
sanitized by the bridge; the endpoint is not validated for untrusted guests.
The worker
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
| 0x2004 | adapter ID | D3DKMTCreateDevice, zero flags; RequestVSync when context mode is enabled. |
| 0x2005 | device ID | D3DKMTDestroyDevice; live children cause EBUSY. |
| 0x2006 | device ID | D3DKMTCreatePagingQueue, normal priority, physical index zero. |
| 0x2007 | queue ID | Read the host-only paging fence mapping; return value, never its address. |
| 0x2008 | queue ID | D3DKMTDestroyPagingQueue. |
| 0x2010 | device ID + u32 size | D3DKMTCreateAllocation2, standard existing heap with worker-owned CPU backing. |
| 0x2011 | allocation ID + u32 offset, u32 count, bytes | Bounded CPU copy into allocation backing. |
| 0x2012 | allocation ID + u32 offset, u32 count | Bounded CPU copy from allocation backing; bytes follow the reply body. |
| 0x2013 | allocation ID + queue ID u32 | D3DKMTMakeResident; wait for the host paging fence, at most five seconds. |
| 0x2014 | allocation ID + queue ID u32 | D3DKMTMapGpuVirtualAddress after residency; wait for paging completion, return GPU VA. |
| 0x2015 | allocation ID | D3DKMTQueryAllocationResidency. |
| 0x2016 | allocation ID | D3DKMTDestroyAllocation2; then release CPU backing. |
| 0x2017 | device ID + offset u64, size u32, reserved zero u32 | Register pages from the configured QEMU Windows section as a standard existing heap. |
| 0x2018 | destination allocation ID + source ID u32, source offset u32, destination offset u32, count u32 | Synchronous D3D12 GPU buffer copy between two nonoverlapping shared allocations of the same device. |
| 0x2020 | device ID + six u32 fields: node, engine affinity, flags, client hint, private-data byte count, reserved zero; then private bytes | Opt-in D3DKMTCreateContextVirtual; return private bytes after the normal reply on success. |
| 0x2021 | context ID, no payload | D3DKMTDestroyContext. |

Hello capability bit 0 means adapter/device lifecycle; bit 1 means paging queue
and fence query; bit 2 means bounded host-backed allocation operations. When a
RAM section is configured, bit 3 means shared RAM imports and bit 4 means GPU
buffer copy. GPU copy requires NVIDIA D3D12 Device3 and 64 KiB-aligned allocation
offsets and sizes. It does not accept shader code, arbitrary driver commands,
vendor escapes or scanout requests.
Bit 5 advertises virtual contexts only when the owner starts the worker with
`--driver-contexts` as the final argument. Synchronization-only contexts require
node/engine/client hint zero, flags 8, and empty private data. Graphics contexts
require node <64, engine affinity 1, D3D12 client hint 12, flags 0 or 16, and
1..4000 private bytes. Timeout-disabling, test and unknown flags are rejected.
This mode sets the device's documented RequestVSync flag. The ordinary mode
keeps zero device flags and rejects both context operations.
The allocation's queue must belong to the same device. Bounds and byte budget
are checked before driver calls and again before native memory access.
Other successful dispatches return a 16-byte body containing
signed NTSTATUS, reserved zero u32, value u64. Native failure remains a native
NTSTATUS, distinct from negative errno in the header for protocol validation.
Creation returns a local object ID only when the driver succeeds. A GPU virtual
address belongs to the Windows GPU address space; it is not a guest CPU mapping.
Requests are serialized. GPU copies return only after a checked completion
fence, before any further host CPU copy. A GPU timeout terminates the worker
while retaining its in-flight backing for Windows process teardown. This
synchronous fixture does not provide asynchronous guest buffer ownership.
Allocation destruction uses
[`SynchronousDestroy`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddicb_destroyallocation2flags)
so Windows has released its secured backing before `VirtualFree`. A timed-out
paging operation invalidates further allocation access until destruction.

The shared heap uses the documented
[`OpenExistingHeapFromFileMapping`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-openexistingheapfromfilemapping)
API. The Windows driver, through D3D12, produces the copy command buffers. This
is a diagnostic heap in system memory, with cross-adapter buffer restrictions.
It does not prove that general textures, arbitrary Linux UMD commands or
near-native graphics can use this path.

## Physical acceptance

The guest performed five open/query/create-device/create-queue/read-fence/
destroy-queue/destroy-device/close cycles. Every native operation succeeded;
driver version query returned WDDM enum 3200. The guest also checked rejection
of stale handles and destruction of a device with a live paging queue. QEMU
and the Windows worker both exited successfully.
The guest additionally created six 64 KiB standard allocations, transferred
393,216 bytes in bounded chunks and compared every byte after CPU readback.
Every allocation became resident (status 1) and received a nonzero GPU virtual
address. Repeated mapping returned the same address. Out-of-bounds copies were
rejected. These copies do not prove that GPU commands consumed the data.
The probe then left a live adapter/device/queue/allocation hierarchy and disconnected.
The worker verified all four object counts and allocated bytes returned to zero with no failed
driver cleanup calls.

See [machine-readable evidence](evidence/QEMU-WDDM-BRIDGE-2026-10-07.json).
Input kernel, generated initramfs and bridge executable hashes are recorded.
The test used no disk, network device, host share or existing VM process. It
used the installed guest kernel and a separate 3.3 MB initramfs containing only
a static init and probe. This test is intentionally a transport test; a
headless control probe does not meet the interactive Omarchy acceptance goal.

The separate [Windows section copy test](evidence/HOST-WDDM-SHARED-COPY-2026-10-08.json)
registered two 64 KiB views in one temporary section with KMT, made both resident
and mapped their GPU addresses. It then imported the section with D3D12 and
copied 262,443 bytes through the NVIDIA GPU in six cycles, including partial
copies. Every destination byte and guard matched; source bytes remained intact.
The worker's counters confirmed six completed copies and zero remaining KMT,
D3D12 or section objects. This is host-only evidence, not QEMU guest evidence.

The [integrated QEMU/WHPX shared copy test](evidence/QEMU-WDDM-SHARED-COPY-2026-10-08.json)
then passed the same six GPU copies in actual guest RAM: 262,443 bytes, all
destination guards and source bytes verified by the Linux guest after fence
completion. The guest also verified both directions of CPU access to one
registered 64 KiB range, residency and GPU address mapping. QEMU and the worker
exited zero, with all driver, GPU and section counts zero. The report pins the
QEMU executable as well as the kernel, initramfs and worker.

The QEMU build's [memory acceptance workflow](https://github.com/7Wdev/try-omarchy-windows/actions/runs/37688925268)
also passed physical-memory alias, size/share/name/collision, migration blocker
and lifetime checks. The worker handles a TCP reset at a frame boundary as a
disconnect, while rejecting a reset in a truncated frame; both paths passed
native object cleanup checks. These checks are evidence for a small fixture,
not a desktop stability or performance benchmark.

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

To reproduce the host-only shared GPU copy acceptance:

```powershell
python experimental/windows-native-gpu/test_host_shared.py `
  --bridge experimental/windows-native-gpu/build/driver-bridge.exe `
  --report host-shared-report.json
```

The shared guest fixture additionally needs the runtime built by
[`experimental/qemu`](../experimental/qemu). Add `--shared-memory` and
`--firmware qemu-lab/share/qemu` to the QEMU command above. It reserves a 2 MiB
huge page, checks actual guest PFNs with `/proc/self/pagemap`, verifies Windows
can read and write that RAM, then asks the GPU to copy between two registered
64 KiB ranges. The guest compares every byte after the GPU completion reply.
This root-only fixture retains its mappings until the VM disconnects. A
production guest driver must pin pages and manage their lifetime explicitly;
the fixture is not a general page-registration API.

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
| Allocation backing | Worker-owned KMT allocations, the QEMU Windows section backend and shared guest GPU copies passed physical acceptance. Production pinning and scatter/gather page registration remain absent. |
| GPU virtual addresses and residency | Typed make-resident/map and bounded paging waits are tested. Full reserve/update/eviction and guest monitored fences remain absent. |
| Actual command submission | Bounded synchronous D3D12 GPU copies and opt-in virtual context lifecycle are implemented. A native captured-input diagnostic creates hardware queues. Live guest UMD allocations, queue creation, command buffers, submit and completion remain absent. |
| Guest synchronization | A QEMU/WHPX diagnostic passed 80,000 direct reads of Windows driver fence pages through read-only PCI apertures. Live monitored-fence integration, host events and Linux sync files/dma-fences remain absent. |
| Linux graphics userspace | WDDM-aware runtime/UMD, matching driver files and ABI; ordinary Linux NVIDIA RM userspace cannot use these messages. |
| Interactive desktop | DRM buffer sharing and compositor allocation, plus fenced scanout into the existing QEMU SDL window, resizing/input and crash recovery; not implemented. |

The installed Windows QEMU rejects `memory-backend-file` as an unknown object
type and does not list `ivshmem-plain`. The experimental runtime adds a Windows
section memory backend without replacing the installed runtime. Its object
schema and RAM API patches are separate from the app so upstream fixes can
remain mergeable. A host file mapping alone does not prove a guest mapping.

The shared-memory GPU copy gate is passed. Next, the guest WDDM interface and
Linux graphics stack must be exercised,
including real DRM allocation and fenced scanout in the QEMU SDL window.
These memory and copy tests do not establish guest rendering, desktop stability
or near-native performance.
