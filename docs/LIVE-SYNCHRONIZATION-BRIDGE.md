# Live Windows synchronization objects in QEMU

The live NVIDIA Linux runtime now creates and destroys Windows monitored
fences and synchronization mutexes through the owned QEMU/WHPX bridge. The
RTX 5090 Laptop test created eight of each, alongside three hardware queues
and 16 allocations. All objects and mappings were released cleanly. The
[metadata-only evidence](evidence/QEMU-LIVE-SYNCHRONIZATION-2026-10-09.json)
pins the tested sources, binaries and three physical cases.

This implements synchronization object lifetime and direct fence reads.
Signal/wait operations, general GPU command submission, eviction, general allocation
lifetimes and desktop integration remain unfinished. D3D12 device creation
still returns `80004005`, and the diagnostic still reaches its 16-allocation
limit. No accelerated Omarchy desktop or near-native performance is claimed.

## Native contract and ownership

The separate `--driver-syncs` opt-in is accepted only with an owned
`--run-qemu` runtime. Capability `16384` enables create/destroy operations
`0x2070`/`0x2071`. A fixed 24-byte descriptor carries type, flags, affinity,
reserved zero and the initial value. It contains no CPU pointer or event
handle. Creation requires a typed, owned device. At most 16 independent
synchronization objects and 64 total wire objects can exist at once.

The host reconstructs the documented
[D3DDDI_SYNCHRONIZATIONOBJECTINFO2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_synchronizationobjectinfo2)
and calls
[D3DKMTCreateSynchronizationObject2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtcreatesynchronizationobject2).
The supported forms are a mutex (type 1, Boolean initial state) and a monitored
fence (type 5, 64-bit initial value and affinity 0 or 1). Flags must be zero.
Shared objects, CPU notification events, other types and unsupported affinities
are rejected. Native failure NTSTATUS is preserved, with zero object/mapping
outputs; malformed replies terminate the guest transport.

Monitored fences use the existing read-only PCI fence hub. The native process
retains the synchronization object and its mapping lease; the guest receives a
local object ID, aperture offset and validated GPU fence address. Each guest
mapping verifies 10,000 direct loads. Native CPU addresses never cross the wire.
Mutexes have local identities but no mapped page.

Explicit destruction first unmaps the guest view, then requires QMP unmap
acknowledgement before native destruction. Failed native destruction retains
the object and device ownership for retry. Borrowed paging and hardware-queue
progress synchronization objects cannot use this independent destroy operation.
Device destruction remains blocked while an independent child is owned.

On disconnect the host stops and reaps its QEMU before releasing native objects.
The abrupt-exit case retains four monitored fences, eight mutexes, one hardware
queue and one CPU allocation lock until confirmed VM exit. It reports zero
QMP unmap acknowledgements and complete native cleanup afterward.

## Verification

| Case | Result |
| --- | --- |
| Live runtime | Eight monitored fences, eight mutexes, three hardware queues, 16 allocations; 13 fence mappings and 13 acknowledged unmaps; all 16 independent sync objects explicitly destroyed |
| Guest exits with queue owned | Twelve independent sync objects retained until VM exit, then destroyed; six fence mappings retired by VM exit; no false unmap acknowledgement |
| Synchronization disabled | Zero independent native sync objects; previous hardware-queue route still passes; unsupported ioctls 16 and 29 remain visible |

MSVC and GCC protocol checks pass with warnings treated as errors. All 90 Linux
fault cases pass, including successful mutex creation, destruction retry,
stale/wrong-type handles, unsupported inputs, malformed native replies and
cleanup when no guest fence hub exists. The normal physical case observes
450,000 direct fence loads across paging, hardware-queue and monitored fences
and paging retirement checks. These counts are diagnostic checks, not a
performance benchmark.

Use the existing [owned runtime instructions](LIVE-HARDWARE-QUEUE-BRIDGE.md),
adding `--driver-syncs --minimum-monitored-fences 8 --minimum-sync-mutexes 8`
to `test_owned_runtime_qemu.py`. For its separate `--hwqueue-eof-test` control,
require four monitored fences and eight mutexes, with one allocation/queue.
Use a fresh report path for every run. The locally packed NVIDIA runtime image
and private driver bytes must remain local.

The subsequent [bounded command route](LIVE-COMMAND-SUBMISSION.md) executes the
initialization command and verifies its retirement. The synchronization-only
cases recorded here reject submission. Their observed request is an owned hardware queue, a 4 KiB GPU
command buffer, 1,880 private bytes, no primaries and progress fence value 5.
Those recorded cases log it as metadata and reject it as unsupported ioctl 52.
GPU signal/wait behavior, fence advancement under
guest submission, timeout/reset recovery and sustained desktop rendering are
not verified by these object-lifetime tests.
