# NVIDIA backend progress

Updated **2026-10-21**. This page tracks the experimental QEMU/WHPX Windows-native
NVIDIA backend. The released Try Omarchy launcher continues using the upstream
graphics runtime.

![4K Omarchy frame achieved; QEMU SDL host GPU relay verified; backend stages 4/8; desktop gates 0/4](images/gpu-backend-progress-real-sdl-2026-10-21.svg)

## What is verified now?

**Latest display follow-up:** real Omarchy guest buffers now reach QEMU SDL on
NVIDIA. Run 102 passes 300 handoffs, exact complete 4K source pixels, independently
scaled full SDL window pixels and live cross-process GPU cleanup. The actual SDL
backbuffer is 2560×1566. **The repeat fails at frame 207 with device loss.** Median
log-observed frame delivery is 69.15 ms, while ingress-to-SDL receipt is 5.07 ms;
physical scanout/input latency and near-native performance remain unmeasured.
[Actual image, reconstructable evidence and retained failures](LIVE-OMARCHY-QEMU-SDL.md).

**Latest guest follow-up:** two fresh guests pass **300 GPU handoffs with real
application resizing**, independently verified enlarged/restored scene pixels,
QMP input response and live cleanup. Two earlier 300-frame input-response
baselines remain separate. [Images, raw evidence and scope](https://github.com/7Wdev/try-omarchy-windows/blob/c6b554458583d14ea9fc6ef432b75d2a0aacc2a4/docs/LIVE-UWSM-SUSTAINED-RESIZE.md).
The product should preserve the official keyboard/cursor code and QEMU SDL
input path. Forty input-related files are unchanged; the real guest is now
connected to SDL. The user reports cursor movement, with automated full-input
and normal-launcher acceptance still open. [Source-preservation record and integration boundary](https://github.com/7Wdev/try-omarchy-windows/blob/c6b554458583d14ea9fc6ef432b75d2a0aacc2a4/docs/UPSTREAM-INPUT-GRAPHICS-INTEGRATION.md).
The staging trims still occur after the streams; hardware reallocation while
rendering remains open. The earlier results below keep their own scope.


Two fresh guests pass **UWSM in a real PAM/logind session**.
Independent supervisor queries verify the active compositor service, exact
MainPID/cgroup and active graphical session target. Each passes **five NVIDIA application launch/close cycles**
while the same Hyprland compositor keeps running. The real Omarchy 4.0.4
desktop is captured at **3840Ã—2160** with shipped wallpaper, Quickshell bar
and a readable GPU application. Every actor, including the primary compositor,
is reclaimed before native disconnect; every backing mapping is reclaimed before VM exit.
Twelve GPU handoffs, complete first/last
backbuffer audits, natural shutdown and native cleanup pass per run.
Demand-allocated staging avoids preparing about 64 MiB at startup while retaining all
three ring slots. Both runs stay below the unchanged 1 GiB reported-GPU-usage guard.
Hyprland also enumerates QEMU's keyboard and tablet through the guest input seat.
The later **run 93** verifies a fixed QMP keyboard sequence in the real Alacritty
PTY, the expected pointer motion and the terminal's green confirmation in the
complete NVIDIA backbuffer. Its fresh **repeat 94 fails** the unchanged 1 GiB
GPU-usage guard during shutdown and requires VM termination. This is one accepted
input-response diagnostic, not repeated stability. Physical Windows-window
keyboard/mouse delivery remains unverified.
This bounded repeat does not establish long-run memory stability.

**The subsequent staging-reclamation change passes two fresh guests (95â€“96).**
Each passes the complete QMP input-response, 4K GPU pixels and live-cleanup audit
under the same usage guard, after actually releasing 31.875 MiB of idle staging.
Both GPU copy fences retire before release; three-slot capacity is preserved.
Neither run reallocates a trimmed slot during rendering. Continued hardware
trim/reallocation and longer workloads remain required.
[Two-run evidence, actual frame and scope](https://github.com/7Wdev/try-omarchy-windows/blob/bf60d2848f831be892f8c5e69b686bcedb98644e/docs/LIVE-UWSM-STAGING-RECLAIM.md).

[Actual UWSM image, reproducible evidence and retained failures](https://github.com/7Wdev/try-omarchy-windows/blob/2abc463db5cdb8c2b62b3c0526fe60932f9acde7/docs/LIVE-UWSM-NATIVE-INPUT-DISCOVERY.md)
bind every application and the shell to authenticated NVIDIA renderers, real
resource FDs and independent recipient NT references. The
[earlier CPU-scan removal checkpoint](https://github.com/7Wdev/try-omarchy-windows/blob/cb505099ef461603c45ccc8b322031546966ee36/docs/LIVE-OMARCHY-GPU-LIFECYCLES.md)
measures 0.28â€“0.53-second application retirement while keeping complete frame audits. The 192 MiB CPU mapped-data
and 1 GiB GPU-VA/reported-usage guards are unchanged. Ten ordinary frames per
run avoid CPU upload/readback; two use diagnostic GPU readback.

**The first GPU-rendered 4K Omarchy/Hyprland frame is achieved.**
[Actual input-response image, raw evidence and failed repeat](https://github.com/7Wdev/try-omarchy-windows/blob/0774e40f0dd394aedb90a5d341b87cc42a00cc91/docs/LIVE-UWSM-QMP-INPUT-RESPONSE.md)
preserve the scope of that later checkpoint.

**3/3 diagnostic scene elements** means wallpaper, bar and readable GPU
application are present in the accepted 4K captures. **4/8 backend stages** and
**0/4 desktop product gates** retain their original acceptance definitions.
These are different checklists, not engineering-effort percentages. Capturing
the desktop does not establish normal launcher presentation or interactive use.

The Windows diagnostic client area is 1280Ã—720. Physical 4K scanout, all-frame
display, normal first-boot/launcher/input, long-run stability and performance are
unverified. Both passing UWSM runs have **zero mappings or native children deferred to VM exit**.
Earlier usage-guard and primary-reservation failures remain retained and rejected.
GPU memory pressure, physical Windows input, normal launcher integration, continuous multi-window presentation, longer workloads
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
| 6. Interactive Omarchy/Hyprland desktop | In progress: two 300-frame 4K application-resize/input-response runs pass; separate QEMU SDL host GPU import/BGRA controls pass | Connect the real guest compositor to the tested SDL relay with retained input, then validate normal launcher sessions, continuous display, focus, fullscreen and multiple windows. |
| 7. Required APIs and NVIDIA features | Pending acceptance | OpenGL diagnostics pass; test general OpenGL, Vulkan, CUDA, video and NVIDIA capabilities in real guest applications. |
| 8. Reliability and â‰¥93% native performance | Pending acceptance | Matched guest/native benchmarks through 4K, concurrent host/guest GPU workloads, long runs, bounded memory and device/VM recovery. |

## Desktop product acceptance gates

**0/4 complete.** The first three gates have partial diagnostic evidence and are
shown in amber. A verified image is a real milestone within these gates; it does
not close the broader product requirements below.

| Gate | Current evidence | What closes the gate |
| --- | --- | --- |
| Guest graphics device, buffers and synchronization | In progress: separate NVIDIA clients, native resource FDs, GPU waits and five application lifecycles pass twice; every backing mapping is reclaimed live. | A supported guest device path with correct sustained ownership, synchronization and recovery beyond the bounded diagnostic. |
| First Hyprland frame in the Windows app | In progress: actual 4K wallpaper, bar and readable GPU application captured twice. Separate diagnostic presentation is not normal launcher acceptance. | Boot the normal guest session, independently verify a correctly displayed compositor frame in the Windows app, and publish its image in PR #2. |
| Usable interactive desktop | In progress: two bounded QMP keyboard/pointer and GPU-rendered application responses pass after staging reclamation. Physical Windows input and continuous multi-window operation are unverified. | Continuously display real apps and multiple windows with working input, focus, resizing and fullscreen through 4K. |
| Stable and performant product | Pending acceptance: no matched near-native result or long desktop run has passed. | Sustain workloads, recover from failures, keep memory bounded and meet â‰¥93% of matched native performance. |

## Remaining work

1. Connect real guest compositor buffers to the tested QEMU SDL relay, retaining its
   keyboard/cursor logic; extend hardware trim/reallocation and longer rendering
   without relaxing the usage guard.
2. Integrate the supported guest device path, normal first-boot session and Windows
   launcher/input, with independently verified continuous presentation.
3. Extend the bounded live-retirement result to concurrent application windows,
   longer workloads and failure recovery.
4. Exercise multiple applications, resize/fullscreen, longer runs and bounded
   whole-process memory use through 4K.
5. Benchmark matched native/guest workloads and concurrent Windows/guest GPU use.
6. Implement and validate the required Vulkan, CUDA, video and NVIDIA features
   with real applications. A D3D12 or OpenGL diagnostic does not establish them.

There is no defensible completion date yet. Stage counts cannot estimate the
duration of the remaining driver, compositor and product work.

## Evidence history

Earlier results keep their original scope and failures:

- [Actual QEMU SDL NVIDIA host relay, BGRA correction and retained pixel failure](https://github.com/7Wdev/try-omarchy-windows/blob/6db01ffa69db405f9c76fb51da335bcf0513e8f6/docs/LIVE-QEMU-SDL-GPU-RELAY.md).
- [Idle staging reclamation and two passing 4K input-response runs](https://github.com/7Wdev/try-omarchy-windows/blob/bf60d2848f831be892f8c5e69b686bcedb98644e/docs/LIVE-UWSM-STAGING-RECLAIM.md).
- [QMP input delivery and actual 4K GPU response, including the failed repeat](https://github.com/7Wdev/try-omarchy-windows/blob/0774e40f0dd394aedb90a5d341b87cc42a00cc91/docs/LIVE-UWSM-QMP-INPUT-RESPONSE.md).
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
