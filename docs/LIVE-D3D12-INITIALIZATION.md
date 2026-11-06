# Full NVIDIA D3D12 initialization in QEMU

On 2026-10-09, the genuine NVIDIA Linux user-mode driver in an owned QEMU/WHPX
guest created a D3D12 device and copy command queue through the Windows driver
bridge. Both returned `00000000`. Two normal runs passed the complete resource
and cleanup checks, with normal probe and VM exit. This achieves the full D3D12
initialization milestone. It does not establish accelerated Omarchy rendering,
CUDA, OpenGL, Vulkan, video support, sustained stability or the required 93% of
native performance. The launcher still does not select this experiment.

The [hardware evidence](evidence/QEMU-LIVE-D3D12-INITIALIZATION-2026-10-09.json)
contains tested source and binary hashes, dependency pins and metadata for six
accepted physical diagnostics. Vendor libraries, the private runtime images and
private driver data remain local. The Linux interposer uses the bridge's virtio
port; it does not forward to a real WSL `/dev/dxg` or replay captured requests.

## Compatibility changes

The live UMD requests standalone allocations with `VidPnSourceId = UINT_MAX`.
The pinned public header calls this `D3DDDI_ID_UNINITIALIZED`; zero is
`D3DDDI_ID_NOTAPPLICABLE`. The bridge accepts either for non-primary allocations
and supplies NOTAPPLICABLE to native KMT. The guest source field remains
unchanged; private buffers receive normal native in/out handling. Primary surfaces, ANY, other source identifiers,
sharing, system-memory input and unknown flags remain rejected. Microsoft's
[allocation-info contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_allocationinfo2)
associates that source identifier with primary allocations; this route neither
selects a Windows output nor implements guest scanout.

Monitored fences now preserve `NoSignalMaxValueOnTdr` (flag 64) through the native
call. A GPU-accessible fence must return an aligned GPU address; NoGPUAccess
(flag 128) must return zero. Other bits and combinations remain rejected.
The native owner verifies the SDK bit layout and unchanged output flags.
[Microsoft documents](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_synchronizationobject_flags)
the flag's scheduler behavior during a timeout. No physical GPU reset was
injected, and these results do not prove reset recovery.

Windows sometimes coalesces several driver backing stores into one committed
MEM_PRIVATE region. Requiring its RegionSize to equal one allocation's size
rejected valid locks. The CPU view now uses the successful native GPU mapping's
page count at allocation offset zero and the pointer returned by native
[Lock2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_lock2).
The [mapping contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_mapgpuvirtualaddress)
defines that allocation range. The owner proves the complete bounded view fits
inside one committed, read/write private region, checks alignment and overflow,
and exports only those bytes. It never exports the rest of the region or
reservation. The retained native lock and acknowledged QEMU leases own the
lifetime; active source-range overlap remains rejected. This is not a public
decoder of arbitrary vendor allocation layouts or a hard allocation-byte quota.

Copy queue startup also requests an in-process context priority. The typed
route supports documented priorities zero and one only, on an owned virtual
hardware queue context. Native
[SetContextInProcessSchedulingPriority](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsetcontextinprocessschedulingpriority)
is followed by native GetContextInProcessSchedulingPriority readback. Guest and
host records must agree before acceptance. No global scheduler setting or
foreign process/context is exposed. Hardware verified priority zero; priority
one forwarding was tested portably, not physically.

## Bounded capacity

Measurements identified independent startup limits: a paging queue needed two
handles with 127 of 128 wire objects already owned; a NoGPUAccess fence exceeded
64 independent synchronization objects; another 4 MiB GPU map exceeded the
16 MiB mapping budget; and another allocation exceeded 64 live allocations.
Each rejection preceded its native operation. The updated limits are:

| Resource | Limit |
| --- | --- |
| Aggregate wire objects | 256 |
| Live vendor allocations | 96 |
| Independent synchronization objects, including mutexes | 96 |
| Native fence BAR slots | 64 read-only pages, unchanged |
| Live vendor GPU mapping bytes | 32 MiB |
| Live CPU mapping bytes | 16 MiB, unchanged |
| One GPU mapping or CPU view | 4 MiB, unchanged |
| CPU aperture | 16/32/64 slots with 1 MiB stride, unchanged |
| Reported GPU usage guard | 64 MiB, unchanged; not a hard physical byte quota |

Mutexes consume no fence BAR slot. A monitored fence checks native BAR capacity
before native creation. Existing queue, reservation, submission and interval
budgets remain in force. Portable tests cover aggregate exhaustion, a two-handle
queue with only one slot remaining, per-kind quotas, CPU range containment,
overflow, stale identities and malformed replies. No QEMU rebuild was needed.

## Physical acceptance

Each normal 64-slot run creates 67 allocations and GPU maps, 46 direct CPU
views, four hardware queues, 50 monitored fences and 16 mutexes. Thirty-six
fences use NoGPUAccess and two use NoSignalMaxValueOnTdr. Three source sentinel
allocations and one contained CPU subrange are accepted. Three CPU views span
multiple slots: 1,114,112 bytes and two 4 MiB views. Two live UMD commands of
4 KiB each retire at progress fences 5 and 17. Priority zero is set and read
back. Peak ownership is 164 wire objects, 66 allocations, 6,106 GPU pages and
13,795,328 CPU bytes, all below their limits.

Normal cleanup acknowledges 59 fence unmaps and 53 CPU slot unmaps for 46 CPU
views. All five reservations, four Zero state maps, native queues, contexts,
allocations and synchronization objects are reclaimed. Every native failure
counter and live-resource counter is zero; no forced VM stop occurs.

The abrupt guest exit control stops immediately after the first native
NoSignalMaxValueOnTdr fence is owned. Native cleanup retains the CPU locks,
queues, synchronization objects and reservations until confirmed VM exit,
without inventing guest unmap acknowledgements. The 32-slot control rejects the
4 MiB CPU view before Lock2 and preserves the expected device memory failure.
The older guest image retains its earlier source-sentinel rejection, confirming
that native bridge expansion does not silently reinterpret that guest's ABI.
The 64-slot store control verifies and restores first/last words in all 46 CPU
views and checks repeated guest references; command submission is disabled in
that control. Its successful device/queue results do not prove GPU execution.

MSVC/GCC warning-as-error builds, 140 Linux ioctl fault cases, 42 native CLI
rejection cases and the initialization-result guard tests pass. The guard
requires unique device and copy queue successes, a genuine guest UMD, the
bridge transport, normal probe/VM exit and native cleanup. The report marks
`runtimeInitializationComplete` only after every enabled resource check passes.
Mocks and CI do not establish the hardware milestone.

## Work remaining

The next functional gate is actual guest rendering and verified output through
the initialized queue. General submission, asynchronous signal/wait, eviction,
guest kernel/DRM integration, resource sharing and host presentation remain
unfinished. Omarchy/Hyprland and every requested API need separate integration
and acceptance. Reset recovery, sustained workloads and controlled native-versus-
guest performance measurements remain required by
[PRODUCT-ACCEPTANCE.md](PRODUCT-ACCEPTANCE.md).
