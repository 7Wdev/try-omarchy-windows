# Live NVIDIA allocation from QEMU to Windows

This document records the allocation milestone. The
[later GPU-address mapping result](LIVE-GPUVA-BRIDGE.md) now reaches live
`MapGpuVirtualAddress`; its next unsupported call is `MakeResident`, ioctl 11.
The earlier ioctl 12 boundary below describes the allocation-only configuration.

The NVIDIA Linux runtime now creates a standalone Windows video-memory
allocation through the QEMU diagnostic bridge. Physical testing on the RTX
5090 Laptop GPU with Windows driver `32.0.16.1742` used the runtime's actual
allocation request and its 586-byte private input/output buffer, with no
captured private fixture. Native synchronous destruction succeeded and all
tracked objects were released. The
[hardware report](evidence/QEMU-LIVE-ALLOCATION-2026-10-08.json) records the
exact runtime image, native worker, QEMU and kernel hashes.

**D3D12 initialization still fails with `0x80004001`.** Its next unsupported
allocation-related call is `MapGpuVirtualAddress`, ioctl 12. Synchronization
creation, allocation-token translation, hardware queues, rendering, DRM/Mesa
integration and the Hyprland desktop remain unfinished. These initialization
tests do not establish desktop acceleration, stability or near-native speed.
The app launcher does not enable this backend.

## Explicit allocation contract

Capability bit 256 requires the host's `--driver-allocations` option, in addition
to `--driver-contexts --driver-queries`. It stays closed by default. The live
Linux ioctl subset accepts one allocation, create flags zero, no existing
resource, no runtime/resource private bytes, no system-memory or section
pointer, and zero reserved fields. Public per-allocation flags are zero with
priority zero, or `OverridePriority` with priority in the range
`0x28000000..0x78ffffff`. Primary, stereo, shared resources and higher priorities
are outside this diagnostic. The observed request uses flags 4, priority
`0x78100000` and source zero.

`CreateVendorAllocation` (`0x2050`) has a typed parent device ID, a 24-byte
descriptor and 1..4000 private bytes. Native code calls the documented
[D3DKMTCreateAllocation2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtcreateallocation2)
with one zero-initialized
[D3DDDI_ALLOCATIONINFO2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_allocationinfo2).
The reply preserves NTSTATUS, the entire private input/output buffer even on
native failure, and the documented GPU-address output. Success returns a
connection-local allocation ID; no native KMT handle or host CPU pointer is
returned. These objects have a distinct type from the existing CPU-backed
standard allocations and cannot use their read/write or copy operations.

`DestroyVendorAllocations` (`0x2051`) contains a typed device ID, an 8-byte
count/reserved descriptor and 1..16 local allocation IDs. All IDs, parents,
types and duplicates are validated before calling
[D3DKMTDestroyAllocation2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtdestroyallocation2).
The host always requests synchronous destruction, including when the guest
supplies the public `AssumeNotInUse` hint. A failed native destruction retains
the IDs. Disconnect releases child allocations before their devices.

## Memory accounting limitation

The connection admits at most 16 vendor allocations and 64 total objects.
Each transaction bounds private staging to 4000 bytes; private-data length is
not the GPU allocation size. The native worker checks its own local and
nonlocal reported usage through the documented
[IDXGIAdapter3 video-memory query](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)
before and after creation. Failed accounting or more than 64 MiB rejects the
operation and synchronously releases a newly created allocation.

This is **not a hard allocation-byte quota**: nonresident allocations and
transient growth can escape reported `CurrentUsage`. The tested allocation
reported zero usage before residency. No code decodes NVIDIA-private size
fields or equates 586 bytes with GPU memory. A substantiated allocation-size
and residency budget is still required before exposing this to a general
guest or enabling it in the product.

## Reproduce and verify

Build the native worker with `experimental/windows-native-gpu/build.ps1`.
Build the Linux diagnostic and a fresh private runtime image following
[the live runtime instructions](LIVE-NVIDIA-RUNTIME.md). Keep locally installed
NVIDIA binaries and all private images out of uploads and commits. Use the
QEMU build with the existing dynamic paging-fence hub; this change does not
require a new QEMU device build.

```powershell
python experimental/windows-native-gpu/test_owned_runtime_qemu.py `
  --qemu C:/local/qemu/bin/qemu-system-x86_64.exe `
  --firmware C:/local/qemu/share/qemu `
  --kernel C:/local/vmlinuz-linux `
  --initramfs C:/local/private-runtime.cpio.gz `
  --bridge C:/local/driver-bridge.exe `
  --report C:/local/fresh-allocation-report.json `
  --driver-allocations --minimum-vendor-allocations 1 `
  --expected-unimplemented-ioctl 12
```

Acceptance requires at least one live native allocation, matching native
create/destroy counts, zero live objects, direct paging-fence reads, acknowledged
unmap and normal owned VM exit. It separately records the failed D3D12 result
and next unsupported call. A same-image negative control without
`--driver-allocations` still fails at ioctl 6 and creates no vendor allocations.
The disk-free tests attach no guest disk, NIC or host share.

A separate native EOF control sends Hello, OpenAdapter, CreateDevice and
CreateVendorAllocation, then closes input without any destroy request. Native
session cleanup released the allocation and both parents. That narrower control
uses one existing local captured input and no QEMU or live UMD; its provenance
is recorded separately in the hardware report.

GCC and MSVC test the typed ownership, invalid layouts, quota, cross-device
and wrong-type IDs, duplicates, native failures, private input/output
preservation and disconnect. The Linux transport suite now has 22 cases,
including malformed allocation replies and failed destruction followed by
retry. The [physical regression](evidence/QEMU-ALLOCATION-MEMORY-REGRESSION-2026-10-08.json)
preserves six standard allocation cycles, six synchronization contexts, five
captured NVIDIA contexts, 32 adapter queries and six fenced GPU buffer copies
covering 262,443 bytes. Those copy operations have a narrower scope than a
live guest command stream.
