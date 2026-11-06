# NVIDIA GPU buffer copies through QEMU

On 2026-10-09, the genuine NVIDIA Linux user-mode driver in an owned QEMU/WHPX
guest executed D3D12 buffer copies through the Windows driver bridge. Two normal
physical runs each copied two distinct 65,536-byte patterns from UPLOAD memory
to a DEFAULT resource and then to READBACK memory. The guest compared every
readback byte with its expected pattern. Both rounds in both runs matched,
native fences retired, and the probe, VM and native owner exited normally.

This advances the [full initialization milestone](LIVE-D3D12-INITIALIZATION.md)
to actual guest GPU work with checked output. It does not establish rendering,
presentation, accelerated Hyprland, any requested API integration, sustained
stability or 93% of native performance. The launcher does not select this
experiment. The transport remains the QEMU virtio port and owned native memory
apertures; requests do not reach a real WSL `/dev/dxg` or captured fixtures.

The [hardware evidence](evidence/QEMU-LIVE-GPU-BUFFER-COPY-2026-10-09.json)
records source and binary hashes, pinned dependencies, two successful copy runs,
an initialization-only control and an older-bridge negative control. Proprietary
libraries, private images and vendor-private request/reply bytes remain local.

## Workload and acceptance

The explicit `--runtime-workload copy` image option selects the guest workload;
the owned-QEMU verifier must request the same option. Initialization remains the
default, including for older private images without a workload selector.

The probe creates three committed buffer resources, a COPY command allocator,
command list and fence. In each round it writes the deterministic pattern
`(offset * 37 + (offset >> 8) * 11 + round * 73) & 255` into UPLOAD memory,
records both GPU copies and the DEFAULT resource state transitions, executes the
list, signals the queue fence and waits for actual completion. It maps READBACK
memory and compares all bytes. The second round resets and reuses the retired
allocator/list with a different pattern. A ten-second guest deadline and
`UINT64_MAX` device-loss rejection bound the wait.

| Round | Verified bytes | Expected and observed FNV-1a hash | Queue fence |
| --- | ---: | --- | --- |
| 1 | 65,536 | `28e3f2dd58641325` | target 1, observed 1 |
| 2 | 65,536 | `3ce54625a5a41325` | target 2, observed 2 |

Hashes supplement the full byte comparison. The verifier independently
recomputes each expected hash and requires unique workload markers, exactly two
ordered round results, all resource/map/reset/close/signal HRESULTs equal to
`00000000`, native submissions after workload start, matching native resource
and signal records, normal exit and complete cleanup. Initialization-only
success cannot set `guestGpuBufferCopyVerified`. Missing, duplicate, partial,
incorrect-hash or device-loss results are rejected. Mock tests establish these
guards, while the physical reports establish this bounded hardware result.

## Native bridge additions

The UMD's committed buffers require native resource ownership. A separate
negotiated capability under `--driver-allocations` creates one nonshared,
video-memory allocation with `CreateResource` and returns typed resource and
allocation identities. Two wire slots are checked before native creation.
Each resource belongs to its sole allocation and device. Resource destruction
uses native `D3DKMTDestroyAllocation2` with `hResource`, no allocation list and
synchronous destruction; native failure preserves ownership. Allocation-list
destruction also releases its sole resource identity. Sharing, existing-resource
reuse, multi-allocation resources and system-memory inputs remain unsupported.
Microsoft documents these contracts in
[CreateAllocation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_createallocation)
and [DestroyAllocation2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_destroyallocation2).

The DEFAULT buffer requests opaque DriverProtection `0x10000001`. This exact
observed value is forwarded unchanged only for an owned resource allocation
with public protection value 1. Other nonzero values are rejected at the Linux,
wire and native boundaries. This is an observed compatibility case, not a
decoder of NVIDIA's private page-table encoding. Microsoft describes
[DriverProtection](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/driver-protection)
as driver-specific mapping information. The native map preserves
`STATUS_PENDING` and verifies its paging fence as before.

The synchronous CPU-wait ioctl accepts one owned, directly mapped paging,
hardware-queue or independent monitored fence, flags zero and no asynchronous
event. It loads the read-only native fence page with a ten-second deadline and
rejects device loss. It never polls through transport packets or writes a
completion value. Physical resource creation exercised three paging waits at
7001, 7002 and 7003. General object lists and asynchronous event waits remain
unsupported. The public input shape is
[WaitForSynchronizationObjectFromCpu](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_waitforsynchronizationobjectfromcpu).

Queue Signal uses the Linux GPU2 context-signal ioctl and a separate negotiated
wire operation under `--driver-submit` plus `--driver-syncs`. This diagnostic
supports one owned virtual hardware-queue context, one independent NoGPUAccess
monitored fence on the same device, the observed flags value 4
(`AllowFenceRewind`), and monotonically increasing nonzero targets. It accepts
at most 16 native attempts per session. Native
[SignalSynchronizationObjectFromGpu2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsignalsynchronizationobjectfromgpu2)
is followed by a bounded native fence check and a direct guest fence check.
The bridge never writes CPU completion values. Supporting the observed flag
does not establish fence rewind, arbitrary synchronization types or general
asynchronous queue behavior. Native signaling currently blocks for retirement,
as does the existing command diagnostic.

## Physical results and controls

Both normal runs create and destroy three native resources and 72 allocations,
map 72 allocations into GPU VA, issue 74 residency requests and create 50 direct
CPU views across 57 aperture slots. They own four hardware queues, 51 monitored
fences and 16 mutexes; 37 fences use NoGPUAccess and two use the TDR flag. Three
native 4 KiB command packets retire at hardware fences 6, 11 and 17, including
the two workload rounds. Two native GPU2 signals retire at targets 1 and 2.
The separate host copy helper is unused (`completedGpuCopies = 0`).

Peak ownership is 176 wire objects, 71 allocations, 6,666 mapped GPU pages
(27,303,936 bytes) and 16,023,552 CPU bytes. Existing 256-object, 96-allocation,
32 MiB GPU-map and 16 MiB CPU-view caps remain in force. The fence BAR remains
64 read-only pages, the CPU aperture remains 64 selected slots, and no QEMU
rebuild or driver installation was required.

Normal cleanup acknowledges all 60 fence mappings, all 50 CPU views and all 57
CPU slot mappings. Every native live-resource and failure counter is zero;
there is no forced VM stop. The initialization-only control retains device and
COPY queue success with the earlier 67-allocation workload and cannot claim a
copy. The older bridge reaches device and COPY queue creation but rejects the
new resource route; the copy probe fails, no native command is submitted, the
report rejects the workload, and all native resources are reclaimed after VM
exit. Raw initialization HRESULTs alone cannot turn that failed run into an
accepted milestone.

MSVC/GCC warning-as-error builds, 163 Linux ioctl fault cases, 42 native CLI
rejection cases and four milestone guard test groups pass. New cases cover
resource capacity/ownership, malformed native replies, destruction failure,
resource-only protection, CPU-wait input validation and context-signal
capabilities, types, devices, stale identities, monotonic targets and attempt
quota. CI compiles the guest workload without proprietary libraries and runs
portable guards; hosted CI is not hardware evidence.

## Next gate

The next functional gate is guest graphics work with verified pixels, followed
by host presentation and interactive Omarchy integration. General resource
layouts, sharing, guest kernel/DRM integration, eviction, asynchronous
submission/synchronization and reset recovery remain unfinished. Every
requested API needs its own acceptance and controlled native-versus-guest
measurements under [PRODUCT-ACCEPTANCE.md](PRODUCT-ACCEPTANCE.md).

The [graphics queue control](LIVE-GRAPHICS-QUEUE-CONTROL.md) records the subsequent
NoBroadcastSignal queue lifecycle and the rejected graphics-startup attempt.
It preserves this GPU-copy milestone but does not prove rendered pixels.
