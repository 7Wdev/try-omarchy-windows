# Real Wayland scene through the Windows NVIDIA bridge

Updated 2026-10-16. Real Hyprland 0.56.2 composes an actual Foot Wayland
terminal with readable fonts and changing PTY content through the Windows
NVIDIA backend. Two fresh disk-free QEMU/WHPX guests each complete 300 native
window submissions. This advances desktop integration; the full Omarchy
session and normal launcher are still incomplete.

![Actual frame 300 from run 44, decoded from the native GPU backbuffer](images/hyprland-wayland-scene-frame-300-2026-10-15.png)

The image is the actual 640×360 GPU backbuffer, decoded without resizing or
recoloring. The warnings remain visible because this is a diagnostic session.
Foot renders a shared-memory Wayland surface in software; **Hyprland composes
the scene on NVIDIA**. This does not prove application GPU acceleration.

| Fresh run | Native submissions | Retired guest GPU commands | Retired sync-files | Final DXGI count | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| 42 | 12 | 81 | 36 | 4 | Scene, scoped pixel audit and cleanup pass; display unverified |
| 43 | 300 | 948 | 613 | 297 | Changing scene, scoped pixel audit and cleanup pass; display unverified |
| 44 | 300 | 951 | 614 | 117 | Fresh repeat passes; display unverified |

The submission spans are 12.10/11.62 seconds for runs 43/44. These are short
development runs, not long-run stability or performance benchmarks. The owned
native diagnostic still has a 60-second deadline. Every ordinary frame uses
GPU copies without CPU readback/upload; only frames 1 and 300 are audited.
The extra GPU staging copy and synchronous ingress acknowledgement remain.

## Changes needed to reach this scene

The old profile exhausted its 256 live vendor objects while composing Foot.
The resulting allocation error preceded NVIDIA device removal and a compositor
reset. Profile 5 is an exact, opt-in negotiation: 512 live vendor objects,
1,024 wire objects, 64 live GPUVA reservations and rolling command/request
admission. Submission/signal/request lifetime counters use 64-bit values;
overflow, replay and malformed negotiation controls remain enforced.
Legacy profiles retain their original caps.

Live pending-command/signal limits remain 16, synchronization objects 192,
CPU aperture bytes 96 MiB, mapped GPU bytes 256 MiB, reservation bytes 16 GiB,
and allocation/fence aperture slots 256 each. The GPU driver-reported usage
budget is distinct from a hard quota on private vendor allocation bytes;
that hard quota remains unimplemented. The successful scene peaks at 257
live vendor objects and about 166.8 MiB reported GPU usage. Raising one cap
does not establish capacity for a full desktop or independent GPU clients.

Fontconfig now resolves Noto Sans and JetBrains Mono Nerd Font in the private
guest. Matching UTF-8 locale and compose data, a usable PTY, and Foot's
`colors-dark` configuration fix the retained application readiness failures.
The probe requires an actual mapped Wayland client, font receipts and child
readiness before forwarding frames; it closes the app and verifies its exit
marker before compositor teardown. The public scene script, configuration
and fontconfig file are reviewable under `experimental/windows-native-gpu`.
Vendor binaries, fonts and runtime images remain local.

## Evidence and validation

[Immutable development evidence](evidence/HYPRLAND-WAYLAND-SCENE-DEVELOPMENT-2026-10-15.json)
(SHA-256 `ba67b9fa015a5400f9b1529c2b70430c76f54181babf20dbd657c40c7b7d902f`)
retains runs 37, 38, 40–44: source/build/input digests, manifests, lifecycle and
native frame receipts, failed-control diagnostics and compressed raw frames.
Run 37 verifies the font-only regression; run 38 preserves allocation exhaustion.
Runs 40/41 retire over 1,000 GPU commands with clean teardown but fail Foot
readiness, so they are rejected as application scenes. Run 39 failed a runner
allowlist before a VM started. Prior published evidence is unchanged.

Each first/last scene audit verifies complete native/file digests, 7,168 known
blue background pixels, 8,960 terminal-background pixels and three independent
RGB swatches, each covering 525 pixels. Client metadata requires a mapped,
visible, non-Xwayland Foot surface at the expected geometry. The last images
differ from the first in both 300-frame runs; the terminal counter visibly
advances. Full glyph correctness and every intermediate frame's scanout are
not independently verified.

All 285 local milestone tests pass, including 16 new scene evidence tests.
They reject wrong profiles/counts, missing or hidden clients, wrong geometry,
missing font/readiness/close receipts, unretired work, ordinary-frame readback,
modified bytes and missing colors even when native/file digests agree.
Strict default and 4K-lab MSVC builds, GCC protocol/concurrent logging tests,
five C11 probe builds, legacy negotiation controls and 21 desktop-profile
transport cases pass. The default build rejects the desktop lab profile.

## Distance to the requested desktop

We have a real compositor and a real application surface rendering together.
The next visible milestone is the normal Omarchy scene in the Windows app:
boot its normal session, load its theme/configuration and start its apps,
integrate presentation/input with the launcher, and support independent GPU
clients. Reliable display, resize/fullscreen, recovery, long runs and 4K
compositor tests then need acceptance evidence. CUDA, Vulkan, video/NVIDIA
features, concurrent host/guest work and ≥93% matched-native performance
remain separate requirements of the full product.

The checklist stays **4/8 verification stages and 0/4 desktop gates**. These
counts do not measure engineering effort, and there is no verified completion
date yet. See [the roadmap](GPU-BACKEND-PROGRESS.md) and
[product acceptance](PRODUCT-ACCEPTANCE.md).
