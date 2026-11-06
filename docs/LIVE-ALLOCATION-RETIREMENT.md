# Allocation retirement with live hardware queues

The live NVIDIA Linux runtime now destroys owned Windows allocations while
hardware queues still exist. On the RTX 5090 Laptop, it reclaimed one
allocation with two queues alive, then completed a GPU command at fence 5.
It created 18 allocations with a peak of 17 live objects. The
[physical evidence](evidence/QEMU-LIVE-RETIREMENT-2026-10-09.json) records three
accepted cases with exact source, binary and private image hashes.

The complete D3D12 device still fails initialization with `8007000e` after
exhausting the separate 16-slot CPU aperture. CUDA, OpenGL, Vulkan, video
acceleration, the interactive desktop and the 93% performance requirement
remain unverified. See [product acceptance](PRODUCT-ACCEPTANCE.md).

## Destruction contract

Capability `65536` and `--driver-retirement` enable the existing typed
`DestroyVendorAllocations` operation with live queues. The option requires an
owned QEMU runtime with hardware queues. The disabled path retains allocations
until their device's queues are destroyed.

Windows documents that VidMm defers destruction until earlier queued commands
finish. `SynchronousDestroy` requests reclamation before return.
`AssumeNotInUse` can bypass pending work and remains zero. The bridge uses this
contract without decoding private NVIDIA queue data. See Microsoft's
[allocation usage tracking](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/allocation-usage-tracking)
and [destruction flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddicb_destroyallocation2flags).

Allocations must belong to the requesting device. Duplicate, foreign, stale
and still CPU-mapped wire identities are rejected. Guest CPU views are removed
before destruction; their native QMP leases require acknowledged unmap before
unlock. The current synchronous submission diagnostic also requires every
owned queue's submitted progress to have retired. Failed destruction preserves
ownership for retry. Positive unexpected status, raw handles and malformed
completion values cannot cause allocation identities to be forgotten.

EOF cleanup reaps QEMU before destroying queues and allocations. If queue
destruction fails after VM exit, its allocations remain retained. Physical
queue-destruction failure and GPU reset recovery are not verified.

## Bounded capacities and verification

The object quota is 32 vendor allocations within the 64 total wire-object
limit. Mapped GPU addresses still have a 16 MiB budget. The CPU aperture keeps
16 slots of at most 1 MiB each, matching the tested QEMU device. CPU-slot
exhaustion returns allocation failure before native `Lock2` or QMP mapping.
The 64 MiB reported GPU-usage guard is not a hard allocation-byte quota.

MSVC/GCC protocol checks pass with warnings treated as errors. All 98 Linux
fault cases pass, including failed-destruction retry and malformed replies.
Protocol checks cover disabled retention, retirement with live queues, stale
identity rejection, native failure/output validation and the independent
mapped-byte quota.

Both physical enabled and disabled cases created 18 allocations, three queues,
eight monitored fences and eight mutexes, completed one 4 KiB command and
released all objects. Only the enabled case reclaimed an allocation with
queues alive; peak objects were 17 versus 18 in the control. Each observed two
CPU-slot quota rejections with no native lock/unlock or control-channel error.
The guest-exit control retained one queue and CPU lock until normal VM exit,
then released them without false unmap acknowledgements.

Use the [owned runtime procedure](LIVE-HARDWARE-QUEUE-BRIDGE.md) and
[command submission options](LIVE-COMMAND-SUBMISSION.md). For current sources,
omit the historical `--expected-allocation-limit 16`, set
`--expected-unimplemented-ioctl 13`, and add `--driver-retirement
--minimum-retirements-with-queues 1`. Use fresh reports and keep vendor
libraries, private images and driver bytes local.

Dynamic aperture capacity, asynchronous queues, synchronization signal/wait,
remaining driver operations and guest presentation are still required.
