# NVIDIA backend progress

Updated **2026-10-19**. This page tracks the experimental QEMU/WHPX Windows-native
NVIDIA backend. The released Try Omarchy launcher continues using the upstream
graphics runtime.

![4K GPU capture: 3/3 scene elements verified twice; backend stages 4/8; desktop product gates 0/4](images/gpu-backend-progress.svg)

## What is verified now?

Two fresh guests each pass **five NVIDIA application launch/close cycles**
while the same Hyprland compositor keeps running. The real Omarchy 4.0.4
desktop is captured at **3840×2160** with shipped wallpaper, Quickshell bar
and a readable GPU application. All six retired client actors and every backing
mapping are reclaimed before VM exit. Twelve GPU handoffs, complete first/last
backbuffer audits, natural shutdown and native cleanup pass per run.

[Actual image, reproducible evidence and retained failures](LIVE-OMARCHY-GPU-LIFECYCLES.md)
bind every application and the shell to authenticated NVIDIA renderers, real
resource FDs and independent recipient NT references. Removing unconditional
CPU scans of mapped GPU memory reduces application retirement to 0.28–0.53
seconds while keeping the complete frame audits. The 192 MiB CPU mapped-data
and 1 GiB GPU-VA/reported-usage guards are unchanged. Ten ordinary frames per
run avoid CPU upload/readback; two use diagnostic GPU readback.

**3/3 diagnostic scene elements** means wallpaper, bar and readable GPU
application are present in the accepted 4K captures. **4/8 backend stages** and
**0/4 desktop product gates** retain their original acceptance definitions.
These are different checklists, not engineering-effort percentages. Capturing
the desktop does not establish normal launcher presentation or interactive use.

The Windows diagnostic client area is 1280×720. Physical 4K scanout, all-frame
display, normal launcher/UWSM/input, long-run stability and performance are
unverified. The new checkpoint has **zero mappings deferred to VM exit**;
normal launcher/input, continuous multi-window presentation, longer workloads
and recovery are next. The [earlier capture checkpoint](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-OMARCHY-4K-REFERENCE-FIX.md)
retains its original scope and failures.

## Backend verification stages

| Stage | Status | Evidence or exit criteria |
| --- | --- | --- |
| 1. Fork and upstream maintenance | Demonstrated | Preserve upstream history and fixes on `master`; review experimental work in [PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2). |
| 2. Windows bridge and D3D12 initialization | Demonstrated in the lab | Genuine Linux NVIDIA runtime initializes through QEMU and creates owned Windows KMT objects. |
| 3. 4K guest shaders and shared GPU textures | Demonstrated in the lab | [Independent 4K shader pixels, GPU copies and fence retirement](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-4K.md). |
| 4. Repeated guest rendering | Demonstrated in bounded diagnostics | [64-frame guest runs](https://github.com/7Wdev/try-omarchy-windows/blob/2290aa6209289dddfbae54b542c524927414c020/docs/LIVE-GPU-GUEST-STREAM.md), safe allocator/texture reuse and explicit cleanup. This is not long-run desktop or all-frame display acceptance. |
| 5. Guest graphics/device integration | In progress | Native render-node/GBM, EGL fences, separate GPU clients, 4K captures and five application lifecycles with live reclamation pass twice. Supported device integration, recovery and sustained work remain required. |
| 6. Interactive Omarchy/Hyprland desktop | In progress: diagnostic 4K desktop captured | Validate the normal launcher session, continuous correctly displayed frames, real apps, keyboard/cursor input, focus, resizing, fullscreen and multiple windows. |
| 7. Required APIs and NVIDIA features | Pending acceptance | OpenGL diagnostics pass; test general OpenGL, Vulkan, CUDA, video and NVIDIA capabilities in real guest applications. |
| 8. Reliability and ≥93% native performance | Pending acceptance | Matched guest/native benchmarks through 4K, concurrent host/guest GPU workloads, long runs, bounded memory and device/VM recovery. |

## Desktop product acceptance gates

**0/4 complete.** The first two gates have partial diagnostic evidence and are
shown in amber. A verified image is a real milestone within these gates; it does
not close the broader product requirements below.

| Gate | Current evidence | What closes the gate |
| --- | --- | --- |
| Guest graphics device, buffers and synchronization | In progress: separate NVIDIA clients, native resource FDs, GPU waits and five application lifecycles pass twice; every backing mapping is reclaimed live. | A supported guest device path with correct sustained ownership, synchronization and recovery beyond the bounded diagnostic. |
| First Hyprland frame in the Windows app | In progress: actual 4K wallpaper, bar and readable GPU application captured twice. Separate diagnostic presentation is not normal launcher acceptance. | Boot the normal guest session, independently verify a correctly displayed compositor frame in the Windows app, and publish its image in PR #2. |
| Usable interactive desktop | Pending acceptance: one application is present in the captured scene; continuous multi-window/input operation is unverified. | Continuously display real apps and multiple windows with working input, focus, resizing and fullscreen through 4K. |
| Stable and performant product | Pending acceptance: no matched near-native result or long desktop run has passed. | Sustain workloads, recover from failures, keep memory bounded and meet ≥93% of matched native performance. |

## Remaining work

1. Integrate the supported guest device path, normal UWSM session and Windows
   launcher/input, with independently verified continuous presentation.
2. Extend the bounded live-retirement result to concurrent application windows,
   longer workloads and failure recovery.
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
