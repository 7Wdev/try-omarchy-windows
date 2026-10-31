# Guest D3D12 fence timelines and GPU queue ordering

On 2026-10-12, two fresh QEMU/WHPX guests passed the genuine Linux D3D12
runtime's shared-fence and future queue-wait diagnostic on an NVIDIA GeForce
RTX 5090 Laptop GPU. The guest used the Windows-native bridge, without WSL
`/dev/dxg` forwarding or captured vendor initialization fixtures.

The original fence and exported guest FD were released before four GPU queue
signals completed through the imported alias. The copy queue then waited for
the future value 5 and enqueued a dependent signal. After a 50 ms observation
interval, the shared fence still read 4 and the dependent fence read 0. A second,
direct queue signaled 5; the dependent signal then completed at 1. Both runs
subsequently verified two 65,536-byte GPU copies against an independently
calculated byte pattern.

Each run retired eight GPU signals and seven command submissions, balanced the
native export/import/close references, released all kernel memory and object
leases, unloaded the module, and exited both QEMU and the native worker with
status 0 and no forced stop. A dependency-satisfied receipt alone is not treated
as proof that the wait packet retired: the subsequently completed dependent
signal supplies the ordering evidence.

Two separate fresh Mesa OpenGL runs also passed the existing pixel oracle and
kernel FD verifier with this implementation: two 130×73 frames per run, maximum
channel error 1, 18 retired native commands, and complete cleanup. These remain
bounded rendering tests, not an accelerated Omarchy session.

## Implementation

- Preserve the real empty Linux object-attribute request, including length 0,
  while rejecting names, inheritance and unsupported security fields. C ABI
  padding is not interpreted as an attribute.
- Preserve the Windows resource-query result `STATUS_OBJECT_TYPE_MISMATCH`
  through the Linux error mapping. This lets D3D12 select its fence-import
  path. The native resource query is actually executed; the bridge does not
  fabricate a successful resource descriptor.
- Import the exact runtime fence flags, including shared NT security,
  `NoGPUAccess` and `UnwaitCpuWaitersOnlyOnDestroy`; ignore output-only input
  storage and inactive union bytes. Rebuild native structures from owned fields.
- Negotiate synchronization profile version 2, with a 20-byte fixed layout,
  import flags 1155, 16 shared references, 96 sync objects and 16 pending waits.
- Forward context-stream waits to
  [D3DKMTWaitForSynchronizationObjectFromGpu](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtwaitforsynchronizationobjectfromgpu).
  The separate hardware-queue wait endpoint forwards to
  [D3DKMTSubmitWaitForSyncObjectsToHwQueue](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsubmitwaitforsyncobjectstohwqueue).
  The genuine runtime exercised the context-stream endpoint in these captures.
- After profile negotiation, use the nonblocking context-signal endpoint with
  [D3DKMTSignalSynchronizationObjectFromGpu2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsignalsynchronizationobjectfromgpu2).
  Acceptance and observed completion are tracked separately. The earlier
  synchronous diagnostic endpoint remains available.
- Check owner/device/type, counts, duplicate objects, reserved fields and native
  replies. Retain sync objects and queues/contexts while their wait dependencies
  are unsatisfied or their tracked signals are pending. Pending native records
  use capacity reserved before submission.

The dependency signal returned while its observed value was still 0. The native
wait call also returned before the second queue released its target. The bridge
remained able to receive that release; it did not block the connection in a CPU
wait or write completion values itself.

## Evidence and reproduction

[Retained receipts, audits and provenance](evidence/GPU-TIMELINE-2026-10-12.json)
include both timeline runs, both OpenGL regression runs, 42 source hashes, the
native executable hash, guest artifact hashes and hashes of the raw local
captures. Private images contain locally installed vendor binaries and are not
redistributed.

The source is in [experimental/windows-native-gpu](../experimental/windows-native-gpu).
Set `WDDM_BRIDGE_TEST_GPU_TIMELINE=1` when calling
`build_runtime_diagnostic.sh` with the explicit `4k` profile. Pack it as a `copy`
workload with the guest kernel module and contract program. Use the owned
runtime harness with the native 4K bridge and the same driver opt-ins used for
the [kernel component](LIVE-GPU-KERNEL-APERTURE.md). This does not modify an
installed Omarchy guest disk.

Audit each raw capture using:

```sh
python3 experimental/windows-native-gpu/verify_gpu_timeline_capture.py \
  --report /path/to/driver.json --output /path/to/new-timeline-audit.json
python3 -m unittest discover -s experimental/windows-native-gpu -p 'test_gpu_timeline_milestone.py'
```

The verifier requires the matching `driver.log` and `driver.host.log` beside the
report, correlates native signal/command completion, checks future dependency
ordering and independently calculates both copy hashes. The older general
workload verifier does not recognize this diagnostic's extra operations; its
exit 1 is retained and is not presented as acceptance. The dedicated timeline
and OpenGL/FD audits establish only their stated scopes.

Validation: MSVC `/W4 /WX` standard, 4K and stream builds and seven contracts per
build, including 398
shared-fence protocol checks; 203 Linux ioctl fault cases; 21 enabled OpenGL and
20 enabled stream transport cases plus disabled-profile checks; 94 existing
milestone tests; 10 Linux OpenGL packaging tests; and the new capture-corruption
tests (103 milestone tests in total).

## Remaining work

These shared fences use `NoGPUAccess`. Their native queue packets and scheduler
completion are real, but this does not establish a GPU-addressable shared fence
or direct GPU writes into its storage. The explicit hardware-queue wait endpoint
has protocol tests; the live runtime exercised the context-stream wait endpoint.

The diagnostic still has fixed object and per-session signal limits. It does not
establish an unbounded compositor session, memory accounting for a complete
desktop, failure recovery, Mesa semaphore FD import, Linux `sync_file`, DRM/GBM
or dma-buf integration. Its 65,536-byte copies are not new 4K rendering evidence;
the earlier [4K shader evidence](LIVE-GPU-4K.md) remains separate. Full Hyprland,
Vulkan, CUDA, video and ≥93% native performance remain unverified. The roadmap
therefore remains [4/8 verification stages](GPU-BACKEND-PROGRESS.md).
