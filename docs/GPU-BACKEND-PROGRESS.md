# NVIDIA backend progress

Updated 2026-10-11. **4 of 8 verification stages demonstrated.** This is a
checklist count, not 50% of engineering effort or a product-readiness estimate.
The largest integration tasks are still ahead.

Latest lab result: [Mesa OpenGL shader rendering](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-GPU-OPENGL.md)
now passes two independent 130×73 pixel audits through the Windows NVIDIA
bridge. This is progress within the remaining API/device work; it does not
complete a new roadmap stage.

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
  reuse. It is a correctness baseline; a synchronized buffer ring and GPU-side
  waits are still needed before measuring near-native throughput.
- **Product integration:** no accelerated Hyprland desktop, general OpenGL,
  Vulkan, CUDA or video test has passed through this backend yet.

The [experimental PR](https://github.com/7Wdev/try-omarchy-windows/pull/2) retains
the implementation and detailed evidence. The released app continues using the
upstream graphics runtime while this backend is developed.
