# Live NVIDIA GPU-address mapping from QEMU to Windows

This document records the GPU-address milestone. The
[later residency result](LIVE-RESIDENCY-BRIDGE.md) now reaches live
`MakeResident`; the next unsupported call is `Lock2`, ioctl 37. The ioctl 11
boundary below describes the mapping-only configuration.

The NVIDIA Linux runtime running in QEMU now maps its live Windows allocation
into GPU virtual address space through the Windows native driver bridge. The
RTX 5090 Laptop GPU with Windows driver `32.0.16.1742` accepted the actual
16-page (64 KiB) request, returned `STATUS_PENDING` (`259`), and completed its
paging fence at `7001`. The guest directly observed that value through the
read-only QEMU PCI fence hub. Together with the initial fence check, 20,000
loads bypassed MMIO emulation. This run used no captured private input fixture
and no real WSL `/dev/dxg` forwarding. The
[hardware report](evidence/QEMU-LIVE-GPUVA-2026-10-08.json) records the binaries,
private runtime image, kernel and source hashes.

**D3D12 initialization still fails with `0x80004001`.** The first unsupported
call after successful mapping is `MakeResident`, ioctl 11. Synchronization,
allocation-token translation, hardware queues, guest command submission,
DRM/Mesa integration and Hyprland remain unfinished. These tests do not
establish desktop acceleration, stability or near-native performance. The
application launcher does not enable this backend.

## Explicit mapping contract

Capability bit 512 requires `--driver-gpuva`, together with
`--driver-allocations --driver-contexts --driver-queries`. It stays closed by
default. `MapVendorAllocation` (`0x2052`) contains a typed allocation ID and a
64-byte descriptor with the typed paging-queue ID, reserved field, base,
minimum and maximum addresses, offset and size in pages, protection and driver
protection. Both objects must belong to the same connection and device. It
cannot map a standard CPU-backed allocation through this interface.

The admitted subset uses 4 KiB alignment, 48-bit addresses, at most 256 pages
per mapping, offset plus size at most 256 pages, and at most 4096 mapped pages
per connection. Only read, write and execute protection bits are accepted;
driver-specific protection and reserved fields must be zero. Each vendor
allocation can be mapped once. An explicit base cannot replace another tracked
mapping; automatic placement remains the video-memory manager's choice.
The observed live request uses base zero, minimum 64 MiB, maximum 1 TiB,
offset zero, 16 pages and write permission. The Windows call receives native
handles resolved locally; no host CPU address or KMT handle is exported.

The native worker calls the documented
[D3DKMTMapGpuVirtualAddress](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtmapgpuvirtualaddress).
It validates the returned GPU address, retains mapping ownership before any
later check can fail, and waits up to five seconds for paging completion.
The reply preserves the original NTSTATUS, GPU address and paging fence value,
including positive `STATUS_PENDING`. The Linux thunk preserves that positive
status too. Before giving the result to the runtime, the guest verifies that
its directly mapped queue fence has reached the returned value without QEMU
MMIO callbacks. This implements the documented
[paging completion requirement](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_mapgpuvirtualaddress).

Synchronous allocation destruction releases its GPU VA. Failed destruction
retains the typed allocation and mapped-page charge. Disconnect releases
allocations before their devices. If native paging or fence control fails,
the owner stops and reaps its QEMU process before releasing native pages. The
existing fence hub and ownership lifecycle are reused; this change requires
no new QEMU device build.

The mapped-page limit bounds GPU VA, **not opaque vendor allocation bytes**.
The [reported-memory guard and its limitation](LIVE-ALLOCATION-BRIDGE.md)
still apply. A substantiated allocation and residency budget is required
before this interface is exposed to a general guest.

## Reproduce and verify

Build the native worker using `experimental/windows-native-gpu/build.ps1` and
create a fresh private image using the
[live runtime instructions](LIVE-NVIDIA-RUNTIME.md). Keep locally installed
NVIDIA binaries and private images out of commits and uploads.

```powershell
python experimental/windows-native-gpu/test_owned_runtime_qemu.py `
  --qemu C:/local/qemu/bin/qemu-system-x86_64.exe `
  --firmware C:/local/qemu/share/qemu `
  --kernel C:/local/vmlinuz-linux `
  --initramfs C:/local/private-runtime.cpio.gz `
  --bridge C:/local/driver-bridge.exe `
  --report C:/local/fresh-gpuva-report.json `
  --driver-allocations --minimum-vendor-allocations 1 `
  --driver-gpuva --minimum-vendor-gpuva-maps 1 `
  --expected-unimplemented-ioctl 11
```

Acceptance requires live allocation creation/destruction, a native GPU-address
map and completed paging wait, the matching direct guest fence observation,
zero mapped pages and native objects after cleanup, acknowledged fence unmap,
and normal owned VM exit. The harness checks the first unsupported call after
the accepted stage, rather than accepting any unsupported call elsewhere in
the log. The failed D3D12 result is recorded separately from this diagnostic's
acceptance. No guest disk, NIC or host share is attached.

A same-worker, same-image control without `--driver-gpuva` stops at ioctl 12,
creates no native GPU-address mappings and performs no retirement check.
A separate native EOF control creates a paging queue and allocation, maps
16 pages, then closes input without any explicit destroy. Native cleanup
released the allocation, mapping and parents. That narrower control uses one
existing local captured private input, with no QEMU or live UMD; its provenance
is recorded separately in the report.

GCC and MSVC tests cover the capability gate, layout and address bounds,
typed ownership, wrong/cross-device IDs, positive status preservation,
malformed native outputs, explicit overlap, duplicate mappings, failed
destruction and disconnect. The Linux transport suite passes 24 cases. The
[physical regression](evidence/QEMU-GPUVA-MEMORY-REGRESSION-2026-10-08.json)
preserves six standard allocation cycles, six synchronization contexts,
five captured NVIDIA contexts, 32 adapter queries and six fenced GPU copies
covering 262,443 bytes. Those copies do not represent a live guest command
stream or desktop rendering.
