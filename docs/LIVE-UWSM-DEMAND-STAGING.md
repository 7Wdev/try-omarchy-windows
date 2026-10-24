# Guarded staging and terminal compositor cleanup

Two fresh QEMU/WHPX guests, runs **81 and 82**, pass the full 4K NVIDIA
desktop, UWSM session, pixel and resource-lifetime audits. Each runs five GPU
application cycles under the same compositor, then releases every actor before
the native controller disconnects. This is a bounded diagnostic checkpoint.

![Unedited run 82, frame 12, captured from the NVIDIA GPU backbuffer](images/omarchy-4k-terminal-staging-gpu-2026-10-19.png)

The frame contains the shipped wallpaper, Omarchy bar and readable Alacritty
application at **3840×2160**. Hyprland, Quickshell and all five application
processes have authenticated NVIDIA renderer and native resource-FD evidence.
The image is a direct conversion of the complete GPU buffer, without edits.
The separate Windows diagnostic client remains 1280×720.

## Allocation and cleanup changes

The presentation ring retains three slots. It prepares one staging texture at
startup and allocates the others only when an actual submission needs their
slots. Allocation runs on the serialized native request thread, with the existing
usage guard before and after creation. A texture is published only after both
checks succeed. Failed admission releases the unpublished local resource.

This avoids preparing **66,846,720 bytes** (about 64 MiB) at startup. Run 81
eventually needs all three textures; run 82 needs two. This change does not
guarantee a smaller peak when the full ring is used.

A separate trusted controller operation, RetireStoppedGroup, closes the
primary compositor's remaining ownership. The ordinary actor-retirement operation
still rejects the primary. Before the terminal request, the broker closes its
listener, joins all client workers, verifies the actual pinned primary pidfd
has exited and checks the shared-capability registry is empty.

The native coordinator rejects the terminal transition while any application
session remains, the primary is connected or shared references remain. It then
seals the group against registration, aliases and actor calls. Actual native
presentation queues and fences drain before primary objects are retired using
the existing sixteen-object batches and dependency phases. A failed presentation
drain is fatal; busy objects stay owned. The operation adds no ordinary-client
opcode and does not change the guest kernel device protocol.

## Measured results

| Per fresh guest | Run 81 | Run 82 |
| --- | ---: | ---: |
| Native GPU commands retired | 1,248 | 1,257 |
| GPU application launch/close cycles | 5 | 5 |
| GPU handoffs / complete frame audits | 12 / 2 | 12 / 2 |
| Peak reported GPU usage, bytes | 1,046,568,960 | 1,009,213,440 |
| Staging textures created and released | 3 | 2 |
| Non-primary wire objects reclaimed live | 2,422 | 2,267 |
| Additional primary objects retired terminally | 1 | 1 |
| Retained actors / wire objects at disconnect | 0 / 0 | 0 / 0 |
| Mappings or native children deferred to VM exit | 0 | 0 |

Both runs preserve the complete source-disk digest, exit naturally, report no
failed residency/submission or GPU-state waits, and pass complete native cleanup.
The **192 MiB CPU mapped-data** and **1 GiB GPU-VA/reported-usage guards** are
unchanged. A hard whole-process physical GPU quota remains unimplemented.
Ten ordinary handoffs avoid CPU upload/readback; two use bounded diagnostic
readback for complete pixel verification.

Strict default and 4K MSVC builds, Linux coordinator controls and **389 milestone
regressions** pass. The six new evidence tests reconstruct both full GPU buffers,
bind source/binary/image/auditor digests and reject missing or reordered guards,
incomplete reclamation, early terminal transitions, forged pidfd/drain state,
unsealed groups, oversized batches, missing GPU witnesses and changed pixels.

[Reconstructable evidence](evidence/OMARCHY-4K-DEMAND-STAGING-2026-10-19.json)
retains run 79's primary reservation deferred to VM exit. Run 80 is a passing
pre-terminal staging baseline with its original auditor rejection retained:
the new auditor initially counted queue retries as accepted frames and assumed
a full-frame readback heap, although readback uses tiles. Correcting those two
new checks does not alter the published GPU/pixel/lifecycle auditors.

The [earlier UWSM checkpoint](LIVE-UWSM-GPU-SESSION.md) retains run 78's
reported-usage-guard failure. Neither failure is relabeled as passing. Runs 81
and 82 exercise terminal cleanup of the primary's remaining adapter; they do
not independently reproduce run 79's orphan reservation.

## Remaining acceptance

Two bounded fresh guests do not establish long-run memory stability, failure
recovery or a stable product. Normal first-boot, supported device integration,
Windows launcher/input, continuous multi-window display, physical 4K scanout,
resize/fullscreen, general OpenGL, Vulkan, CUDA, video, NVIDIA features and
matched-native performance remain open. Progress remains **4/8 backend stages**
and **0/4 desktop product gates**.
