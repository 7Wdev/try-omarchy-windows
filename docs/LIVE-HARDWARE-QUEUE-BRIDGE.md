# Live NVIDIA hardware queues in QEMU

The live NVIDIA Linux runtime now creates Windows hardware queues through an
owned QEMU/WHPX bridge. Physical testing on the RTX 5090 Laptop GPU created
three queues and 16 allocations, mapped GPU addresses, made all allocations
resident and shared their CPU mappings with the guest. The runtime changed
eight allocation views without diagnostic stores. Queue progress fences passed
10,000 direct guest loads each, with no emulated MMIO reads.

This remains an initialization experiment. The live run reached the diagnostic
16-allocation object limit and unsupported GPU command submission (ioctl 52).
Private escapes, synchronization operations and eviction are also incomplete.
The subsequent [synchronization bridge](LIVE-SYNCHRONIZATION-BRIDGE.md) adds
monitored-fence/mutex creation and destruction with direct fence mappings.
D3D12 device creation returned `80004005`. Omarchy/Hyprland acceleration,
stability and near-native performance have not been established.

The [metadata-only evidence](evidence/QEMU-LIVE-HARDWARE-QUEUES-2026-10-08.json)
records six accepted hardware cases and the exact tested source/binary hashes.
Live cases use locally installed vendor runtime binaries, with no captured
private request replay or WSL `/dev/dxg` forwarding. Vendor binaries, private
driver data and the private runtime image are not published.

## Allocation identities

The UMD places its returned allocation handle into its private queue data.
Windows requires a driver-consumable allocation token for that reference.
Capability `4096`, operation `0x2056` and the separate `--driver-translation`
opt-in add the documented
[allocation-handle translation escape](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_driverescape_translateallocationehandle).
The host validates the allocation/device/adapter ownership chain before making
the native call. It preserves failure NTSTATUS, rejects malformed output and
caches one nonzero 32-bit token per allocation. Changing or duplicate tokens
are rejected. Destruction removes the translation cache entry.

The Linux interposer returns this driver token as a guest allocation alias.
Public allocation operations resolve the alias to the session's typed local
allocation ID before crossing the wire. The token cannot substitute for a typed
allocation, device or adapter ID in the public host protocol. Matching known
translation escapes from the UMD become validated identity operations, as in
the Microsoft WSL kernel's
[driver-known escape handling](https://github.com/microsoft/WSL2-Linux-Kernel/blob/d504d40ab83839cb61942a812a99d183c42227b9/drivers/hv/dxgkrnl/ioctl.c).

The UMD builds the queue payload itself from its alias and GPU mapping. The
bridge forwards the opaque in/out bytes without replacing private fields,
patching offsets or replaying captured requests. The live payload was 180 bytes;
the protocol permits bounded private data up to 4,000 bytes.

## Queue ownership and fences

Capability `8192` and operations `0x2060`/`0x2061` add queue creation/destruction,
enabled separately by `--driver-hwqueues`. The host accepts this only with
context/translation opt-ins and an owned `--run-qemu` instance. A queue belongs
to a typed virtual context with hardware-queue support (context flags `16`).
Queue flags must be zero; timeout-disabling and other flags are rejected. The
session permits eight hardware queues and 64 total objects.

[D3DKMTCreateHwQueue](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtcreatehwqueue)
returns a queue, a borrowed progress synchronization object and CPU/GPU fence
mappings. The bridge exposes local queue/sync identities, a validated GPU fence
address and a read-only PCI BAR lease for the CPU fence. It never sends a native
CPU pointer. The existing private fence hub maps the driver-owned pages into
WHPX with read permission only. The guest verifies direct reads before exposing
its pointer to the UMD. Queue destruction owns the borrowed sync object's
lifetime; it is not independently destroyed.

An explicit guest queue destroy first unmaps the guest view. Windows then
requires QMP unmap acknowledgement before calling `D3DKMTDestroyHwQueue`.
Failed native destruction retains the queue and parent ownership for retry.
Context/device destruction is blocked while a child queue remains. Since the
bridge does not decode vendor queue formats, it conservatively retains every
allocation on a device while any hardware queue on that device exists.

On connection EOF, the server stops and reaps its owned QEMU before session
cleanup releases queues or allocation locks. Queues are destroyed before
allocations, including allocations created after a queue. The explicit
`--hwqueue-eof-test` exits the guest probe immediately after its first queue is
mapped, bypassing normal UMD teardown. Physical acceptance proved VM exit before
native queue destruction and CPU unlock. All object and mapping counts reached
zero; no QMP unmap acknowledgement was claimed for this exit path.

## Bounded CPU subregions

The next live allocation used an exact committed CPU region inside a larger
Windows address reservation. Requiring its `AllocationBase` to equal `pData`
incorrectly rejected that layout. The bridge now accepts the exact aligned
`MEM_COMMIT`/`MEM_PRIVATE` region at `pData`, with read/write protection optionally
including write combining, when its byte extent equals the bounded zero-offset
GPU mapping. Its reservation and neighboring regions are never exported.
[VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery)
checks accessibility; the native allocation lock retains ownership. The hub
also rejects overlapping active leases. Limits remain 1 MiB per view, 16 active
CPU slots and 16 MiB total. This does not decode arbitrary vendor allocation
sizes or establish a hard GPU-memory quota.

The normal live case covered 4, 16, 64 and 128 KiB CPU views, peaking at 1 MiB
of mapped CPU allocation memory. Fourteen views used acknowledged unmap and
native unlock; two stayed owned until confirmed VM exit. Acceptance accounts
for both paths separately. Eight native fingerprints changed with diagnostic
stores disabled; no contents or fingerprints are published.

## Verification

| Case | Accepted observations |
| --- | --- |
| Live queues | Three native queues, 16 translated allocations, 16 GPU maps/residency requests/CPU locks, direct fences, authentic runtime writes, explicit allocation-count rejection and complete cleanup |
| Guest exits owning queue | One queue and CPU lock retained until VM exit, then native destruction/unlock; zero false unmap acknowledgements |
| Queue capability disabled | Translation remains enabled; stops at ioctl 24, with zero native queue calls |
| Translation capability disabled | Original local allocation identities work; zero native translations or queues |
| Translated CPU store/reference control | Repeated alias locks reuse a guest pointer; first/last stores reach Windows and original words are restored |
| Existing memory regression | Six allocation cycles and six fenced GPU copies totaling 262,443 bytes, five captured vendor contexts, 22 captured vendor queries and full cleanup |

MSVC and GCC protocol checks pass with warnings treated as errors. Hosted CI
runs 69 Linux interposer cases, including alias parent validation, malformed
translation/queue replies, native failure in/out data, wrong context flags and
missing fence-hub cleanup. Wire tests also cover queue limits, borrowed sync
ownership, failed destroy retention and queue-before-allocation EOF ordering.
The existing memory regression uses local captured context/query fixtures;
the live queue cases do not.

Forced-stop/control-failure hardware injection and GPU reset recovery remain
untested. No arbitrary guest command stream is submitted. Kernel/DRM integration,
synchronization and submission, scanout, Hyprland and product-level stability and
performance testing are still required.

## Reproduce locally

Build the worker with `experimental/windows-native-gpu/build.ps1`, build the
Linux shim/probe/init with `build_runtime_diagnostic.sh`, and package a fresh
private image with `make_runtime_initramfs.py`. The evidence pins the successful
[QEMU artifact build](https://github.com/7Wdev/try-omarchy-windows/actions/runs/37822698638)
and its retained corresponding GPL source archive. Do not publish the private
image or vendor libraries.

```powershell
python experimental/windows-native-gpu/test_owned_runtime_qemu.py `
  --qemu C:/local/qemu/bin/qemu-system-x86_64.exe `
  --firmware C:/local/qemu/share/qemu --kernel C:/local/vmlinuz-linux `
  --initramfs C:/local/private-runtime.cpio.gz --bridge C:/local/driver-bridge.exe `
  --report C:/local/fresh-queue-report.json --expected-unimplemented-ioctl 13 `
  --driver-allocations --minimum-vendor-allocations 16 --expected-allocation-limit 16 `
  --driver-gpuva --minimum-vendor-gpuva-maps 16 `
  --driver-residency --minimum-vendor-residency-requests 16 `
  --driver-cpu --minimum-vendor-cpu-locks 16 `
  --driver-translation --minimum-allocation-translations 16 `
  --driver-hwqueues --minimum-hwqueues 3
```

For the EOF control, use minimum counts of one, omit `--expected-allocation-limit`
and add `--hwqueue-eof-test`. Each case requires fresh report/log paths. Hardware
tests run sequentially with disk-free guests, no NIC or shared folders and no
changes to the installed Omarchy app or guest disk.
