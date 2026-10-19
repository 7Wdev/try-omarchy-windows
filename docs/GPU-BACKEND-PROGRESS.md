# NVIDIA backend progress

Updated **2026-10-19**. This page tracks the experimental QEMU/WHPX Windows-native
NVIDIA backend. The released Try Omarchy launcher continues using the upstream
graphics runtime.

![4K GPU capture: 3/3 scene elements verified twice; backend stages 4/8; desktop product gates 0/4](images/gpu-backend-progress.svg)

## What is verified now?

Two fresh guests capture the real Omarchy 4.0.4 desktop at **3840×2160** with
the shipped wallpaper, Quickshell bar and readable Alacritty application.
Hyprland, shell and application each have authenticated NVIDIA D3D12 renderer
and GPU resource-handoff evidence. Each run retires twelve compositor handoffs;
the complete first and last GPU backbuffers pass independent byte/pixel audits.
Application close, natural VM exit and native cleanup pass in both runs.

[Actual unedited image, raw evidence, measurements and failures](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-OMARCHY-4K-REFERENCE-FIX.md)
are pinned to hardware checkpoint `2290aa6`. The later test-only fixes through
`aec20fe` do not change that hardware scope. Native recipient-reference admission
and tiled readback fixes pass without raising the 192 MiB CPU mapped-data or
1 GiB GPU-VA/reported-usage guards. Ten ordinary frames per run avoid CPU
upload/readback; the two full-buffer audits use diagnostic GPU readback.

**3/3 diagnostic scene elements** means wallpaper, bar and readable GPU
application are present in the accepted 4K captures. **4/8 backend stages** and
**0/4 desktop product gates** retain their original acceptance definitions.
These are different checklists, not engineering-effort percentages. Capturing
the desktop does not establish normal launcher presentation or interactive use.

The Windows diagnostic client area is 1280×720. Physical 4K scanout, all-frame
display, normal launcher/UWSM/input, long-run stability and performance are
unverified. The accepted checkpoint still retains 142 fence, 206 allocation
and 267 CPU-slot mappings until VM exit. Safe live retirement/reuse and repeated
application lifecycles are the next work. New experiments must pass the complete
GPU, pixel and lifecycle audit before replacing this checkpoint.

## Backend verification stages

| Stage | Status | Evidence or exit criteria |
| --- | --- | --- |
| 1. Fork and upstream maintenance | Demonstrated | Preserve upstream history and fixes on `master`; review experimental work in [PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2). |
| 2. Windows bridge and D3D12 initialization | Demonstrated in the lab | Genuine Linux NVIDIA runtime initializes through QEMU and creates owned Windows KMT objects. |
| 3. 4K guest shaders and shared GPU textures | Demonstrated in the lab | [Independent 4K shader pixels, GPU copies and fence retirement](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-4K.md). |
| 4. Repeated guest rendering | Demonstrated in bounded diagnostics | [64-frame guest runs](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-GUEST-STREAM.md), safe allocator/texture reuse and explicit cleanup. This is not long-run desktop or all-frame display acceptance. |
| 5. Guest graphics/device integration | In progress | Native render-node/GBM, EGL fences, authenticated separate GPU clients and verified 4K Hyprland captures pass. Supported device integration, safe live reuse, recovery and sustained work remain required. |
| 6. Interactive Omarchy/Hyprland desktop | In progress: diagnostic 4K desktop captured | Validate the normal launcher session, continuous correctly displayed frames, real apps, keyboard/cursor input, focus, resizing, fullscreen and multiple windows. |
| 7. Required APIs and NVIDIA features | Pending acceptance | OpenGL diagnostics pass; test general OpenGL, Vulkan, CUDA, video and NVIDIA capabilities in real guest applications. |
| 8. Reliability and ≥93% native performance | Pending acceptance | Matched guest/native benchmarks through 4K, concurrent host/guest GPU workloads, long runs, bounded memory and device/VM recovery. |

## Desktop product acceptance gates

**0/4 complete.** The first two gates have partial diagnostic evidence and are
shown in amber. A verified image is a real milestone within these gates; it does
not close the broader product requirements below.

| Gate | Current evidence | What closes the gate |
| --- | --- | --- |
| Guest graphics device, buffers and synchronization | In progress: separate NVIDIA compositor/shell/application clients, native resource FDs, GPU waits and 4K captures pass. Some mappings remain until VM exit. | A supported guest device path with correct sustained buffer ownership, synchronization, live retirement/reuse and recovery. |
| First Hyprland frame in the Windows app | In progress: actual 4K wallpaper, bar and readable GPU application captured twice. Separate diagnostic presentation is not normal launcher acceptance. | Boot the normal guest session, independently verify a correctly displayed compositor frame in the Windows app, and publish its image in PR #2. |
| Usable interactive desktop | Pending acceptance: one application is present in the captured scene; continuous multi-window/input operation is unverified. | Continuously display real apps and multiple windows with working input, focus, resizing and fullscreen through 4K. |
| Stable and performant product | Pending acceptance: no matched near-native result or long desktop run has passed. | Sustain workloads, recover from failures, keep memory bounded and meet ≥93% of matched native performance. |

## Remaining work

1. Safely retire and reuse GPU application resources while the compositor and
   other clients keep running; pass repeated launch/close cycles and recovery.
2. Integrate the normal UWSM session, Windows launcher and input path, with
   independently verified continuous presentation.
3. Exercise multiple applications, resize/fullscreen, longer runs and bounded
   whole-process memory use through 4K.
4. Benchmark matched native/guest workloads and concurrent Windows/guest GPU use.
5. Implement and validate the required Vulkan, CUDA, video and NVIDIA features
   with real applications. A D3D12 or OpenGL diagnostic does not establish them.

There is no defensible completion date yet. Stage counts cannot estimate the
duration of the remaining driver, compositor and product work.

## Evidence history

Earlier results keep their original scope and failures:

- [Readable 4K captures and the earlier admission failure](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-OMARCHY-4K-READABLE-GPU-DESKTOP.md).
- [Wallpaper/application partial capture and slot-exhaustion findings](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-OMARCHY-4K-DESKTOP-CONTENT.md).
- [GPU shell-bar checkpoint](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-OMARCHY-4K-GPU-SHELL.md).
- [Real Omarchy root boot with a read-only base and RAM overlay](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-NORMAL-OMARCHY-ROOT-BOOT.md).
- [300-frame real Wayland scene, with a software-rendered Foot client](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-HYPRLAND-WAYLAND-SCENE.md).
- [Earlier compositor capture](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-HYPRLAND-COMPOSITOR-CAPTURE.md),
  [GBM rendering](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-GBM-NATIVE-RENDER.md) and
  [EGL native synchronization](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-EGL-NATIVE-SYNC.md).

Historical 256-object/startup-only limits and missing-shell/application findings
describe their original experiments. They are not the current 4K capture scope.
The [draft PR](https://github.com/7Wdev/try-omarchy-windows/pull/2) retains the
implementation, images and detailed verification history.
