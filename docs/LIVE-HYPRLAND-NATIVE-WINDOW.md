# Actual Hyprland output in a native Windows test window

Updated 2026-10-15. **Two actual Hyprland frames pass strict native-window display
verification once. The immediate repeat fails at its second display-statistics
check. Reliable presentation and normal Try Omarchy launcher integration remain
unverified.** Progress remains 4/8 stages and 0/4 desktop acceptance gates.

This is the exact second 640×360 DXGI backbuffer from the passing run, decoded
from native BGRX bytes. It is a GPU readback, not an OS window screenshot. It has
no resizing, recoloring, cropping or generated pixels. The diagnostic guest
lacks usable fonts; full cursor/text correctness has no independent oracle.

![Actual Hyprland native-window backbuffer; strict display checks pass once, repeat fails](images/hyprland-native-window-backbuffer-2026-10-15.png)

## What changed

The existing Windows presenter assumed the shader diagnostic's fixed RGBA
layout. It now accepts bounded compositor dimensions and BGRA/RGBA layouts,
with a distinct compositor window class. The optional compositor path retains
one NVIDIA D3D12 device, queue, allocator, command list, readback resource and
swapchain across both output buffers. It validates the resource's actual layout
and immutable physical GPU, waits on the owned EGL producer fence on the GPU,
copies directly into the current DXGI backbuffer and captures that backbuffer
for verification. It returns the source to COMMON before completing the handoff.
There is no guest CPU map or CPU upload.

The same owned resource/fence tickets and two-attempt capture bound remain.
No new guest-selected Windows handles or window identifiers are accepted.
The normal launcher does not select this prototype. The existing host opt-in
`--driver-present-shared` selects the experimental native window when the
capture-mode compositor probe runs. Capture-only mode retains its earlier
ephemeral consumer and precise audit scope. A close request on the compositor
window cancels further copies; destruction waits for submitted GPU retirement.

## Actual evidence

The [retained development evidence](evidence/HYPRLAND-NATIVE-WINDOW-DEVELOPMENT-2026-10-15.json)
contains raw pixels, native records, guest receipts, exact source/build inputs
and all three outcomes. These working-tree experiments are based on 8289b28;
they are not clean-revision product acceptance. NVIDIA binaries and private
guest images remain local.

| Run | Observed result |
| --- | --- |
| 22, native window | Both 640×360 frames match the previous compositor readback byte for byte. Independent background regions match the pinned `114477` reference. Both Present calls, queue retirement, advancing DXGI present counts 1/2 and advancing display QPC timing pass. The same PID, swapchain and GPU context are retained. |
| 23, changed-background repeat | Both backbuffer images exactly match the earlier `335577` control. The second Present succeeds and its queue retires, but DXGI statistics stay on count 1 with unchanged display timing after 158 DWM flushes. The host records a failed compositor capture; the strict display auditor rejects the run. Correct pixels and retired GPU work do not convert it into display success. |
| 24, capture-only regression | The new native binary with presentation disabled passes both original capture audits and preserves the exact earlier raw hashes. |

All three runs record seven compositor commits and 29 retired guest GPU
commands. All nine sync-files, 74 native buffers, 142 fence mappings, 178
allocation mappings, 204 allocation slots and 320 broker views release before
VM exit. Guest, broker, native host and QEMU exit normally without a forced
stop. The failed repeat remains a failed display workload despite clean teardown.
The 256-allocation limit is unchanged; startup still peaks at 252.

The passing run checks 133,120 known-background pixels per frame independently.
Its complete raw bytes also match the earlier separately captured output.
Neither comparison supplies a full cursor/text scene oracle. Native copies,
backbuffer pixels, Present acceptance, GPU retirement and display timing remain
separate evidence. Microsoft documents that [DXGI frame statistics can be
unreliable in some configurations and may need DwmFlush](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics).
That documentation does not prove why this repeat stalled or excuse its failure.

## Validation and next work

Strict default and 4K-lab MSVC builds pass, including the retained wire and file
controls. The new display auditor checks actual retained success/failure data
and rejects stale statistics, nonadvancing timing, foreign presenter identity,
unretired work, mismatched bytes/hash, hidden windows and missing release.
It always leaves normal launcher integration, stability, full scene correctness,
desktop acceptance and near-native performance unverified.

```sh
python3 experimental/windows-native-gpu/verify_hyprland_native_window.py \
  --report RUN/driver.json --expected-background-rgb 114477 \
  --output RUN/native-window-audit.json
```

The older general OpenGL wrapper still returns 1 for this compositor workload;
that result is retained. The dedicated auditor accepts only the bounded result
above. The repeat's failure is not suppressed or reclassified.

Next is resolving the display stall, integrating the GPU presentation loop into
the normal Windows launcher/QEMU window, and supplying normal Omarchy assets
and real application scenes. Sustained resource/queue capacity, input, recovery,
4K compositor operation and matched performance remain substantial work.
