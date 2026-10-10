# Live NVIDIA allocation residency from QEMU to Windows

The NVIDIA Linux runtime in QEMU now makes its Windows allocation resident
through the native WDDM bridge. The RTX 5090 Laptop GPU with Windows driver
`32.0.16.1742` accepted the actual one-allocation request, including its
priority array and `CantTrimFurther` flag. Windows returned `STATUS_PENDING`
(`259`), count one, paging fence `7002` and zero bytes to trim. The guest
directly observed that fence retire through the read-only PCI hub. The native
worker reported a peak 65,536 bytes of GPU-memory usage. Allocation creation,
GPU VA mapping, residency and synchronous destruction used live UMD requests,
with no private input fixture or real WSL `/dev/dxg` forwarding.
The [hardware report](evidence/QEMU-LIVE-RESIDENCY-2026-10-08.json) pins the
tested executable, image, kernel and source hashes.

**D3D12 initialization remains incomplete with `0x80004001`.** Its first
unsupported call after residency is `Lock2`, ioctl 37, which requests CPU
access to the allocation. Synchronization, allocation-token translation,
hardware queues, guest command submission, DRM/Mesa integration and Hyprland
remain unfinished. This proves residency and paging compatibility, not
desktop acceleration, stability or near-native performance. The launcher
does not enable this backend.

## Explicit residency contract

Capability bit 1024 requires `--driver-residency`, together with
`--driver-allocations --driver-contexts --driver-queries`. It stays closed by
default. `MakeVendorResident` (`0x2053`) uses a typed paging-queue ID, a 16-byte
descriptor containing count, flags, priority-array presence and reserved zero,
then 1..16 typed allocation IDs and optional matching 32-bit priorities.
The entire list must consist of distinct, owned vendor allocations from the
queue's device. Standard CPU-backed allocations use their existing separate
diagnostic interface. No guest pointer or native handle crosses the wire.

The native worker builds a zero-initialized documented
[D3DDDI_MAKERESIDENT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_makeresident)
and calls
[D3DKMTMakeResident](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtmakeresident).
Bounded priorities are copied unchanged when supplied; Windows currently
ignores that array. Flags zero and `CantTrimFurther` are accepted. The latter
is required by the observed live request and has its documented WDDM budget
semantics; it does not bypass the bridge's independent reported-usage check.
`MustSucceed` and reserved flags are rejected because the
[documented flag behavior](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_makeresident_flags)
can put the device in error when residency cannot be satisfied.

The reply preserves native NTSTATUS, output count, paging fence and bytes to
trim, including native memory-budget failure. Linux copies the in/out fields
before translating a failing NTSTATUS into errno. Successful pending work is
waited on natively for up to five seconds, then independently checked through
the guest's directly mapped fence before returning to the runtime. Positive
`STATUS_PENDING` remains positive at the ioctl boundary. Initial, GPU VA and
residency checks together performed 30,000 direct loads without MMIO callbacks
in the live acceptance run.

Every attempted native residency call charges each allocation conservatively,
including failure that may have affected only part of a list. At most 64
attempts per allocation are admitted until synchronous destruction. This
bounds reference growth; it is not a resident-byte counter. Failed destruction
retains ownership and charges. Paging or accounting failure stops and reaps
owned QEMU before native session cleanup. All tracked allocations, mapped
pages and residency attempts returned to zero in physical testing.

## Memory accounting limitation

The existing 64 MiB local-plus-nonlocal DXGI reported-usage guard is checked
before and after native residency, including partial failure. The observed
allocation increased reported usage from zero to 64 KiB. The 16 MiB mapped-VA
limit and the reference bound **are not hard vendor allocation-byte quotas**.
Opaque allocation size and transient growth remain unbounded by reported
usage alone. A substantiated memory budget, eviction accounting and recovery
are required before exposing the interface to a general guest. The current
endpoint remains a local initialization diagnostic.

## Reproduce and verify

Build the native worker and a fresh private runtime image using the
[live runtime instructions](LIVE-NVIDIA-RUNTIME.md). Keep installed NVIDIA
binaries and all private captures/images out of commits and uploads. The
existing dynamic-hub QEMU build is reused.

```powershell
python experimental/windows-native-gpu/test_owned_runtime_qemu.py `
  --qemu C:/local/qemu/bin/qemu-system-x86_64.exe `
  --firmware C:/local/qemu/share/qemu `
  --kernel C:/local/vmlinuz-linux `
  --initramfs C:/local/private-runtime.cpio.gz `
  --bridge C:/local/driver-bridge.exe `
  --report C:/local/fresh-residency-report.json `
  --driver-allocations --minimum-vendor-allocations 1 `
  --driver-gpuva --minimum-vendor-gpuva-maps 1 `
  --driver-residency --minimum-vendor-residency-requests 1 `
  --expected-unimplemented-ioctl 37
```

Acceptance requires matching native creation, mapping, residency and cleanup
counts; a direct retirement observation for each paging operation; acknowledged
unmap; normal owned VM exit; and the first unsupported Lock2 call after
residency. The failed D3D12 result is recorded separately from diagnostic
acceptance. No guest disk, NIC or host share is attached. A same-worker/image
control with residency disabled stops at ioctl 11 and makes no residency call.

A native EOF control repeated residency twice, receiving pending fence `7002`
then immediate success with fence zero. Closing input without explicit destroy
released the allocation, mapping, references and parents. This narrower control
uses one existing local captured private input and no QEMU or live UMD; its
provenance is recorded separately. GCC/MSVC tests cover typed lists, counts,
duplicates, cross-device IDs, unknown flags, malformed native replies,
memory-failure trim/count/fence preservation, repetition bounds and disconnect.
All 26 Linux transport/fault cases pass. The
[physical regression](evidence/QEMU-RESIDENCY-MEMORY-REGRESSION-2026-10-08.json)
preserves six standard allocation cycles, six synchronization contexts, five
captured NVIDIA contexts, 32 queries and six fenced GPU copies totaling
262,443 bytes. These copies do not represent a live guest command stream.
