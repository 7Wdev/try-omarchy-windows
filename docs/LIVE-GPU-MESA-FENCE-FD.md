# Mesa native timeline FDs and OpenGL GPU waits

On 2026-10-12, two fresh QEMU/WHPX guests passed genuine Mesa OpenGL semaphore
calls through the Windows-native NVIDIA bridge on an RTX 5090 Laptop GPU.
The private Mesa 26.0.8 patch supplies its missing Linux D3D12 fence-FD callback.
This advances synchronization integration within stage 5; the roadmap remains
**4/8 verification stages demonstrated**.

This is a **private native D3D12 timeline FD contract**, using
`GL_NV_timeline_semaphore` parameters and the GL external-semaphore entry points.
It does not establish binary Vulkan opaque-FD interoperability, Linux `sync_file`,
dma-buf, compositor synchronization, extension conformance or a Hyprland session.
Normal startup leaves this experiment disabled.

## What passed

Each new guest rendered two real 130×73 GLSL frames through Mesa's D3D12 driver
and the Linux NVIDIA runtime. Independent pixel checks verified every pixel,
exact coverage/background/alpha and a maximum color-channel error of one byte.
The second GPU readback is shown below; it has the same dense RGBA SHA256
`fd894f123883a31e65cea02dc84eb9a52f0356511b4a85eb41977d8c46814353`
as the [earlier shader capture](LIVE-GPU-OPENGL.md).

<img src="images/nvidia-guest-opengl-2026-10-11.png" width="520" alt="Actual second NVIDIA guest OpenGL shader frame, original size 130 by 73">

The helper borrows Mesa's real D3D12 device through
`MesaGLInteropEGLQueryDeviceInfo`, creates an NT-shareable native fence, and
exports two authenticated guest kernel FDs. One creates a D3D12 control alias;
the other is imported through `glImportSemaphoreFdEXT`. Successful GL import
consumes the FD, and the original fence is released before the GPU signals.

The real GL calls then:

1. Signal timeline value 1 and observe completion at 1.
2. Wait for future value 2 and queue a dependent signal at 3.
3. Observe that the fence remains at 1 after 50 ms.
4. Release value 2 from an independent D3D12 copy queue and observe completion at 3.

The native trace independently confirms that all three internal hardware-queue
waits and the dependent GL signal were accepted **before** the releasing queue's
signal. The dependent signal returned while the observed fence remained at 1.
Both GL signals, the independent release and all 19 native commands retired.
The bridge did not write completion values or block the connection in a CPU wait.
These shared fences retain `NoGPUAccess`; GPU-addressable shared fence storage
is not established by this test.

Binary import and operations on an unimported semaphore return
`GL_INVALID_OPERATION`. Failed binary and foreign-FD imports preserve caller FD
ownership; a failed replacement preserves the previously imported fence.
Timeline type changes and queries work before import, and a type change after
import is rejected. Both runs release two exports, two imports and two native
shared handles, retire 83 kernel fence leases and 104 allocation leases, unload
the module, and exit QEMU and the worker normally without a forced stop.

## Implementation and bounded budgets

The [pinned patch and instructions](../experimental/windows-native-gpu/mesa/README.md)
include exact original/patched source digests, the official archive digest, and
zero-fuzz application with verification. A separate full Mesa build passed;
the original Mesa installation was preserved. The patch also repairs the
timeline type getter, permits pre-import type changes, guards missing imports,
and transfers FD ownership only after a successful import.

The explicit OpenGL version-2 negotiation allows 384 aggregate objects,
16 hardware queues and 192 vendor allocations. Legacy sessions retain their
previous limits. Sync objects remain capped at 96; CPU slots at 128; pending
commands at 16; total commands at 64; and requests at 10,000. GPU/CPU byte budgets
are unchanged. Tests reject altered, unsupported, late and repeated profiles,
and reject work beyond the negotiated caps before calling the native driver.

Early runs exposed the old aggregate-object and hardware-queue caps. A direct
release queue also exhausted CPU aperture slots. The final test uses a copy
queue and passes within the existing CPU-slot and byte limits; it does not
establish support for arbitrary additional queues or desktop-sized workloads.

## Evidence, validation and remaining failure

[Retained captures and provenance](evidence/MESA-FENCE-FD-2026-10-12.json) contain
both new timeline runs, a patched-Mesa run with the experiment disabled, one
successful legacy FD regression, 32 normalized source hashes, patch/runtime
digests, executable/image hashes and raw local capture hashes. Private images
contain installed NVIDIA files and stay local.

The disabled run independently verifies both shader frames and 18 native
commands, with no semaphore extension advertised. The legacy image also passes
its original pixel, FD ownership and cleanup audit against the new host.
However, an earlier run of that unchanged legacy image completed its workload
and then terminated abnormally during process teardown (`BRIDGE_RUNTIME_EXIT=128`).
The old host and a repeated new-host run passed. **The teardown cause remains
unresolved**, and the failed run is retained separately rather than counted as
a success. Native and VM cleanup passed even on that failure. Stable teardown
must be established before product acceptance.

A subsequent [late-finalizer investigation](LIVE-GPU-EXIT-LIFETIME.md) confirms
and fixes premature CPU-view teardown with a deterministic failing control and
three passing fresh guests. Two further Mesa timeline regressions pass. The
original exit-128 run had no signal receipt, so its attribution remains unproven;
the follow-up does not establish general teardown or desktop stability.

Validation passed: standard, 4K and stream MSVC `/W4 /WX` builds with seven native
contracts each; 203 Linux ioctl fault cases; 21 enabled transport cases for each
OpenGL profile plus both disabled-profile checks; 112 milestone tests including
corruption of the new real captures; and 10 private-image packaging tests.
CI verifies the pinned patch twice and compiles both probe components without
executing a GPU workload or loading the kernel module on the runner.

Audit raw captures with:

```sh
python3 experimental/windows-native-gpu/verify_mesa_fence_fd_capture.py \
  --report /path/to/driver.json --output /path/to/new-mesa-audit.json
python3 -m unittest discover -s experimental/windows-native-gpu -p 'test_mesa_fence_fd_milestone.py'
```

The matching `driver.log` and `driver.host.log` must accompany the report. The
auditor correlates actual native signals, waits, FD lifetime, pixel readbacks,
command retirement and complete teardown. The older general verifier still
returns 1 for these extra operations; that result is retained and is not
presented as acceptance. Dedicated auditors establish only their stated scopes.

DRM/GBM device integration, compositor buffers, Linux explicit-sync semantics,
continuous display, Vulkan, CUDA, video and ≥93% native performance remain open.
The next desktop milestone remains the [first correctly displayed Hyprland frame](GPU-BACKEND-PROGRESS.md).
