# NVIDIA backend progress

Updated 2026-10-12. **4 of 8 verification stages demonstrated.** This is a
checklist count, not 50% of engineering effort or a product-readiness estimate.
The largest integration tasks are still ahead.

Latest lab result: [Mesa OpenGL shader rendering](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-GPU-OPENGL.md)
now passes two independent 130×73 pixel audits through the Windows NVIDIA
bridge. This is progress within the remaining API/device work; it does not
complete a new roadmap stage.

The [first guest kernel memory-lease component](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-GPU-KERNEL-APERTURE.md)
now passes 581 isolation checks and two fresh OpenGL runs with complete kernel
lease cleanup. It supplies restricted CPU-view FDs, not DRM or dma-buf buffers;
stage 5 still requires the graphics ioctl/device and compositor integration.

The [native fence-sharing diagnostic](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-GPU-SYNC-FD.md)
also passes two fresh guest runs: kernel FDs retain a native fence after its
original object/device are destroyed, allow import and re-export, and clean up
every reference. Separate native probes verify 16 GPU queue signals.
The [genuine guest D3D12 timeline diagnostic](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-GPU-TIMELINE.md)
now also passes two fresh runs with native fence import, eight retired GPU
signals, and a future queue wait that gates a dependent signal until a second
queue releases it. Two separate OpenGL/FD regressions pass with this change.
Mesa semaphore FD import and compositor integration remain pending; this
advances stage 5 without completing it.

![Four of eight verification stages demonstrated](images/gpu-backend-progress.svg)

The normal upstream Try Omarchy desktop is already available. This roadmap
tracks the new QEMU/Windows-native NVIDIA backend requested for this fork.
Its results currently come from isolated diagnostic workloads.

| Stage | Status | Evidence or exit criteria |
| --- | --- | --- |
| 1. Fork and upstream maintenance | Demonstrated | Preserve upstream history and fixes on `master`; keep experimental work reviewable in [PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2). |
| 2. Windows bridge and D3D12 initialization | Demonstrated in the lab | Genuine Linux NVIDIA runtime initializes through QEMU and creates owned Windows KMT objects. |
| 3. 4K guest shaders and shared GPU textures | Demonstrated in the lab | [Independent 4K pixel checks, GPU copies and fence retirement](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-GPU-4K.md). |
| 4. Repeated guest rendering | Demonstrated in the bounded lab | [64-frame guest runs](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-GPU-GUEST-STREAM.md), safe allocator/texture reuse, first/last audits and complete cleanup. This does not establish long-run stability or all-frame display. |
| 5. Guest graphics/device integration | Pending | Replace the diagnostic interposer with a usable guest device/driver path. Integrate graphics buffers, memory ownership, synchronization and the compositor's DRM/GBM/display requirements. |
| 6. Interactive Omarchy/Hyprland desktop | Pending | Boot Omarchy through the new backend and validate real apps, cursor/keyboard input, focus, resizing, fullscreen, multiple windows and correct continuously displayed frames. |
| 7. Required APIs | Pending | Test OpenGL, Vulkan, CUDA and video acceleration in real guest applications. D3D12 shader success does not establish support for these APIs. |
| 8. Reliability and ≥93% native performance | Pending | Matched guest/native benchmarks across resolutions up to 4K, long runs, stable memory use, and device/VM failure recovery. |

## How much work remains?

We have working evidence for the driver bridge and a controlled graphics path.
We do not yet have the Linux graphics/display integration that lets Hyprland use
that path. Reaching the first real Hyprland frame is the next major integration
milestone, followed by interactive use and sustained correctness.

The remaining desktop work has these acceptance gates. They are not equally
sized, and some implementation can overlap.

| Gate | Current evidence | What closes the gate |
| --- | --- | --- |
| Guest graphics device, buffers and synchronization | Partial: a kernel memory-lease component, imported native fences with GPU queue ordering, and isolated Mesa OpenGL draws. | The compositor can access the backend through a supported guest device path, import/export its buffers, and synchronize their use correctly. |
| First Hyprland frame | Pending. No Omarchy/Hyprland session has rendered through this backend. | Boot the real guest session and independently verify a correctly displayed compositor frame in the Windows app. |
| Usable interactive desktop | Pending. Bounded shader diagnostics do not exercise a desktop session. | Continuously display real apps and multiple windows, with working input, focus, resizing and fullscreen up to 4K. |
| Stable and performant product | Pending. No matched near-native result or long desktop run has passed. | Sustain desktop workloads, recover from failures, keep memory bounded, and meet ≥93% of matched native performance on the agreed workloads. |

The first three gates describe the route to a usable desktop. The fourth is
required for the stable, near-native product goal. Vulkan, CUDA and video
acceleration also need separate implementation and real application tests;
a visible desktop alone will not complete those API requirements.

There is no defensible completion date yet. Kernel/device integration and the
guest graphics APIs are substantial systems work; their compatibility has not
been established by the current diagnostic. Counting completed stages cannot
estimate the duration of those tasks.

Before calling the desktop usable, the backend must correctly present
continuous frames, handle buffer lifetime and synchronization, and survive
normal desktop operations. Before calling it stable or near native, it must
pass longer workloads and matched performance tests. CUDA and video require
their own implementation and application evidence as well.

## Current unresolved findings

- **Display proof:** DXGI statistics vary between otherwise successful runs.
  Present acceptance and retired work are tracked separately from scanout.
- **Memory:** the bounded stream retains fixed pending-work limits, but a hard
  whole-process GPU memory quota is not implemented. The native consumer and
  swapchain also consume GPU memory. Long-run accounting needs work.
- **Throughput:** the first guest stream serializes host consumption and guest
  reuse. It is a correctness baseline; the proven GPU-side waits still need
  integration into a synchronized buffer ring before measuring near-native throughput.
- **Synchronization frontend:** Mesa 26.0.8's D3D12 path lacks the FD-import
  callback needed by the GL semaphore FD frontend. The new opaque kernel FDs
  establish ownership, not usable compositor semaphore or sync_file support.
- **Product integration:** no accelerated Hyprland desktop, general OpenGL,
  Vulkan, CUDA or video test has passed through this backend yet.

The [experimental PR](https://github.com/7Wdev/try-omarchy-windows/pull/2) retains
the implementation and detailed evidence. The released app continues using the
upstream graphics runtime while this backend is developed.
