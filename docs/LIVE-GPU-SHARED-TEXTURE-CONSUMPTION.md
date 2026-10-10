# Windows GPU consumption of guest shader textures

Two fresh owned QEMU/WHPX runs on 2026-10-10 handed two distinct guest-rendered
NVIDIA shader frames to a retained native Windows D3D12 consumer. Windows copied
each imported shared texture into its own DEFAULT texture on the GPU, then
independently copied that texture to a diagnostic readback buffer. Every native
readback byte matched the corresponding guest shader frame exactly.

This extends the [shared resource import checkpoint](LIVE-GPU-SHARED-RESOURCE.md)
with a completed resource-state/fence handoff and real Windows GPU consumption.
The consumer has no swapchain or window yet, so presentation and accelerated
Omarchy/Hyprland remain unfinished. The launcher does not select the bridge.

[Hardware evidence](evidence/QEMU-LIVE-GPU-SHARED-CONSUMER-2026-10-10.json)
records the accepted runs, controls, complete native ownership and retirement
receipts, source/executable hashes and hashes of the original local logs.

## Handoff and retained consumer

After verifying each shader frame, the guest submits an explicit
`RENDER_TARGET → COMMON` transition and signals a separate workload fence.
Once that native fence completes, it requests consumption while keeping its
texture, command queue and fences alive.

The negotiated `ConsumeSharedTexture` operation carries typed resource and
synchronization IDs, a fence target and a reserved zero field. No pixel payload,
host pointer or NT handle crosses this operation. The server requires an owned
shared resource and a monitored `NoGPUAccess` fence from the same device. It
checks that the target was accepted as a native queue signal, reads the native
fence directly, rejects stale targets and requires every pending guest command
and signal to have retired before consuming the texture.

The native consumer opens the texture on the same physical NVIDIA adapter,
closes its temporary NT handle and retains the imported resource, device,
DIRECT queue, allocator/list, DEFAULT texture, readback buffer and completion
fence across both frames. These objects are created once.

Windows transitions the imported texture from `COMMON` to `COPY_SOURCE`, copies
it into the native DEFAULT texture and returns the source to `COMMON`. It copies
the native texture into diagnostic readback, returns the native texture to
`COMMON` and signals its own GPU fence. Only after completion does it map and
export the actual bytes and acknowledge the guest. The guest can then transition
back to `RENDER_TARGET` and draw the next frame. There is no CPU image upload.

The consumer objects are released before native resource destruction, after
GPU retirement. A native copy that cannot establish retirement terminates the
worker with backing retained for OS teardown; it cannot report normal cleanup.

## Physical results and controls

Each accepted run retires 14 native guest commands, four hardware queue signals
and two native consumer copies. One import/consumer serves both frames and is
released once. All 124 guest allocations, both resources and 89 synchronization
objects are destroyed. Peak live allocations remain 108, with at most three
guest commands pending. Native and QEMU capacity limits were not increased.

| Frame | Actual guest FNV-1a | Actual native consumer FNV-1a |
| --- | --- | --- |
| 1, blue background | `7ee55ce8ce7f7ee1` | `7ee55ce8ce7f7ee1` |
| 2, black background and rotated colors | `fb1b3577b0f6bcbf` | `fb1b3577b0f6bcbf` |

The verifier compares all 37,960 dense RGBA bytes per frame directly; matching
hash markers alone cannot satisfy acceptance. Every frame contains 2,680 shaded
pixels and 6,810 background pixels. Both fresh runs reproduce this previously
published final frame in the native consumer:

![Actual shader frame also reproduced by the Windows GPU consumer](images/nvidia-guest-shader-triangle-2026-10-10.png)

Actual GPU output enlarged 6× with nearest-neighbor sampling. Dense final RGBA
SHA-256: `fd894f123883a31e65cea02dc84eb9a52f0356511b4a85eb41977d8c46814353`.
This is verified readback, not a screenshot of a displayed guest window.

| Physical control | Result |
| --- | --- |
| Two fresh consumer runs, two different frames each | Accepted |
| Earlier shared-resource workload, consumer opt-in disabled | Accepted |
| Consumer guest image, native consumer opt-in absent | Rejected, no native copies |
| Consumer image requested as the earlier shared-import workload | Rejected despite correct copied pixels |

All six runs, including the preliminary run below, have normal owned VM exit
and verified teardown. The first copied both frames correctly but its verifier
incorrectly assumed 15 guest submissions. The observed 14 were all retired.
Its original rejected report remains preserved. The final guard correlates
every actual acceptance/retirement receipt without fixing the UMD batching
count. The two accepted reports come from fresh runs after that correction.

MSVC `/W4 /WX`, GCC warnings-as-errors, native and Linux wire ownership checks,
184 Linux fault cases, 75 native CLI rejection cases and 34 milestone guard
groups pass. Controls reject foreign/nonshared resources, wrong-device fences,
unsupported fence types, malformed packets/native replies, changed or incomplete
pixels, missing completion/release and incorrect workload selection. The earlier
intermittent import failure remains documented in the import checkpoint; these
short runs do not establish reset or import stability.

## Reproduction and presentation

Build with the existing native `build.ps1` and
`experimental/windows-native-gpu/build_runtime_diagnostic.sh`. Pack a fresh local
image with `make_runtime_initramfs.py --runtime-workload consume` and select
`--runtime-workload consume` in `test_owned_runtime_qemu.py`. Add
`--driver-consume-shared` to the explicit capabilities from the
[shared resource diagnostic](LIVE-GPU-SHARED-RESOURCE.md), including shared
resources, asynchronous submission and synchronization. Proprietary runtime
libraries and private guest images remain local.

The interposer's lab hook selects its unique owned shared resource and most
recently signaled owned fence. It is not a general Linux sharing-FD or DRM API.
Consumption is bounded to two 130×73 frames. It synchronizes on the CPU and
waits for diagnostic readback, so this path makes no performance claim.

The next step is swapchain presentation from the retained native GPU texture,
then shared GPU fence scheduling, multiple frames in flight and general guest
kernel/DRM integration. Production display must avoid diagnostic readback and
synchronous round trips per frame. CUDA, OpenGL, Vulkan, video, interactive
Omarchy/Hyprland, recovery and the minimum 93% native performance requirement
remain unverified. See [product acceptance](PRODUCT-ACCEPTANCE.md).

Microsoft describes [multi-queue resource access and fence synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization#multi-queue-resource-access)
and [shared committed resources](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps).
