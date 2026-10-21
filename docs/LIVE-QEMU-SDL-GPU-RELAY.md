# NVIDIA GPU textures in QEMU's SDL frontend

Updated **2026-10-21**. QEMU's actual SDL OpenGL root context imports and
displays a shared 3840×2160 D3D12 texture produced by a separate Windows process.
RGBA and GPU-converted BGRA pass complete source and pre-swap window pixel
checks on the **NVIDIA GeForce RTX 5090 Laptop GPU**, OpenGL 4.6 NVIDIA 617.14.

This is a paused, disk-free **host control**. The real Omarchy compositor has
not yet been connected to this relay. The [earlier real 4K desktop](LIVE-UWSM-SUSTAINED-RESIZE.md)
retains its separate presentation path and evidence. Counts remain **4/8 backend
stages and 0/4 desktop product gates**.

## Hardware results and retained failure

| Relay run | Compiled revision | Source | GPU handoffs | Independent complete pixel audit |
| --- | --- | --- | ---: | --- |
| 1–2 | `4503762` | RGBA8 | 2 each | Pass, two fresh processes |
| 3 | `4503762` | BGRA8 | 2 | **Rejected: red/blue channels swapped** despite protocol success |
| 4 | `4503762` | RGBA8 | 300 | Pass; 298 ordinary handoffs without CPU texture upload/readback |
| 5 | `39f4b02` | BGRA8 | 2 | Pass after GPU channel conversion |
| 6 | `39f4b02` | BGRA8 | 300 | Pass; 298 ordinary handoffs without CPU texture upload/readback |
| 7 | `39f4b02` | RGBA8 | 2 | Pass, compatibility with the same new build |

Every accepted run independently verifies every byte in the complete first/last
source and actual SDL pre-swap backbuffers, both **3840×2160**. These are changing
quadrant controls, not Omarchy screenshots. The failed BGRA captures are retained
unaltered and the independent auditor still rejects them. BGRA matters because
the real compositor's AR24/XR24 buffers use this channel order.

The [reconstructable evidence](evidence/QEMU-SDL-GPU-RELAY-2026-10-21.json)
contains all seven invocations, full QMP/native streams, distinct compiled
sources and artifact receipts, frozen controllers/SDK producer, deduplicated
losslessly compressed raw buffers, independent audits and the rejected result.
Eight regressions replay the accepted runs and reject stale/corrupt/incomplete
pixels, unretired fences, mixed builds/input sources, missing acknowledgements,
incomplete cleanup and unsupported product claims.

## GPU ownership and unchanged input

The host-only `wddm-sdl-relay` console imports one named D3D12 texture and
**separate producer/consumer fences** in a shared OpenGL 4.5 core context. Both
contexts must match the producer's hardware NVIDIA adapter LUID. The producer's
three non-inheritable exported NT handles close after import, before rendering.
Each actual producer and consumer fence must reach its own expected frame value.

A persistent local GPU texture feeds QEMU's existing console/SDL scanout, so
spontaneous redraws do not race producer reuse. GPU synchronization covers the
initial clear and both contexts' subsequent use. BGRA normalization uses a fixed
compute shader; it performs no CPU channel swap or texture upload. Only the two
explicit audit frames read back full source and SDL window pixels.

Detach retires GPU work, disables scanout and releases all four imported GL
objects, local texture, conversion program and child context. Stale generations,
out-of-order frames and reattachment are rejected. Each accepted producer and
QEMU exits naturally with code zero. Producer peak reported GPU usage is
**77,041,664 bytes**, under its unchanged 1 GiB guard. QEMU's own reported usage
and the combined process budget are **not verified** by these controls.

The compiled build retains all **24 official runtime patches**, the official
virtio keyboard/tablet and `sdl,gl=on,show-cursor=off,window-close=off` options.
Input-source hashes remain unchanged. The new callback and relay affect graphics;
the SDL event/input implementation is retained. A bare QEMU control does not
instantiate the launcher's Windows hook or establish physical keyboard/mouse use.

## Build and remaining integration

Both jobs of the [BGRA candidate build](https://github.com/7Wdev/try-omarchy-windows/actions/runs/39566528969)
pass. The downloaded artifact matches GitHub's ZIP SHA-256
`86f8a801a184182dc1cde56c0e7241f1958bbd3195f7823707ea16a23c2226c1`.
The receipt pins every extracted file and the corresponding compiled source.
Content-checked compiler caching is enabled, but this build has zero cache hits
after a toolchain inventory change; no build-time speedup is claimed.

Next: connect real guest compositor buffers to this tested relay, retaining the
official keyboard/cursor path, then validate normal launcher sessions and
continuous multi-window interaction. This host control does not establish guest
scanout through SDL, physical 4K scanout, all-frame display, frame rate, long-run
recovery, general Vulkan/CUDA/video support or ≥93% matched-native performance.
