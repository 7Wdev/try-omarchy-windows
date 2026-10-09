# First live NVIDIA command submission from QEMU

The live NVIDIA Linux runtime submitted its own 4 KiB GPU command buffer to
the Windows NVIDIA driver through the owned QEMU/WHPX bridge. Windows observed
the hardware-queue progress fence advance from 0 to 5. The guest then directly
read 5 from the shared read-only fence mapping, with 10,000 loads and no
emulated MMIO reads. The metadata-only
[physical evidence](evidence/QEMU-LIVE-COMMAND-2026-10-09.json) pins the exact
tested sources and binaries.

This is a bounded initialization command, not a rendered desktop or a CUDA,
OpenGL, Vulkan or video acceleration test. D3D12 device creation still returns
`80004005`; diagnostic allocation limits, private escapes, eviction, guest
kernel integration and presentation remain unfinished. The full
[product requirements](PRODUCT-ACCEPTANCE.md), including at least 93% of native
performance for the requested APIs, remain unverified.

## Submission contract

Capability `32768`, operation `0x2062` and the separate `--driver-submit` opt-in
enable the diagnostic route. It requires an owned QEMU instance with hardware
queues, sync objects, CPU mappings, GPU mappings, residency and allocation
translation enabled. It cannot be combined with diagnostic CPU stores or the
CPU/hardware-queue exit controls.

The fixed 32-byte descriptor carries GPU address, progress fence value, command
length, private data length, primary count and reserved zero. Command addresses
and lengths must be page-aligned, nonzero and bounded by the existing 1 MiB
allocation mapping limit. Private data is optional and bounded at 4,000 bytes.
The complete packet fits the 4 KiB framing limit. This route accepts no
primaries. A zero-count guest primaries pointer is ignored and never read or
forwarded, matching the
[WSL kernel submission path](https://github.com/microsoft/WSL2-Linux-Kernel/blob/d504d40ab83839cb61942a812a99d183c42227b9/drivers/hv/dxgkrnl/dxgvmbus.c).

Both the session and native driver require a typed owned hardware queue and
exactly one command allocation on its device. The command range must fit its
GPU mapping and retained CPU lock. All owned vendor allocations on that device
must have completed residency. Foreign or unmapped addresses, stale handles,
out-of-bounds ranges and unsupported counts fail before native submission.
The opaque private data is forwarded unchanged; private offsets and command
bytes are not decoded or patched.

The host reconstructs
[D3DKMT_SUBMITCOMMANDTOHWQUEUE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_submitcommandtohwqueue)
and invokes
[D3DKMTSubmitCommandToHwQueue](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsubmitcommandtohwqueue).
Fence values must increase per queue and exceed its observed initial value.
The diagnostic permits 16 submission attempts. Native failure NTSTATUS is
preserved; positive unexpected status, raw handles and malformed completion
values are rejected.

After a successful native call, this diagnostic waits up to five seconds for
the queue's progress fence and returns its completed value. A timeout or invalid
fence terminates the diagnostic while retaining native objects until the owned
VM is reaped. Timeout/reset fault recovery has not been physically verified.
On success, the guest checks the same target through its mapped fence before
returning to the runtime.

This synchronous wait establishes compatibility and completion. Production
submission must preserve GPU/CPU overlap through shared descriptor queues and
direct completion signaling; this diagnostic loop does not satisfy the
performance requirements.

## Physical results and reproduction

The RTX 5090 Laptop test used one 4 KiB command at offset zero in a 16 KiB
CPU-locked allocation, with 1,880 live private bytes and progress target 5. One
native call completed, with no failed submission or timeout. Three queues,
eight monitored fences, eight mutexes and 16 allocations were released cleanly.
All 13 fence mappings received acknowledged unmap; two allocation CPU locks
were retained until confirmed VM exit. The submission-disabled control made
zero native submission calls and kept ioctl 52 unsupported.

MSVC/GCC protocol tests pass with warnings treated as errors. All 94 Linux
fault cases pass, including disabled submission, invalid descriptors, foreign
queue identities and an inaccessible zero-count primaries pointer. Protocol
tests cover ownership, residency, CPU lock requirements, buffer overruns,
monotonic fences, malformed native output, preserved native failures and the
attempt limit. Live command data comes from the runtime, without captured
request replay or forwarding to another VM's `/dev/dxg`.

Use the [existing owned-runtime instructions](LIVE-HARDWARE-QUEUE-BRIDGE.md),
adding `--driver-syncs --minimum-monitored-fences 8 --minimum-sync-mutexes 8
--driver-submit --minimum-submissions 1` to `test_owned_runtime_qemu.py`.
The observed first unsupported ioctl after accepted submission is 13. The
runtime image, vendor libraries and private driver bytes remain local; use a
fresh report path for each run.
