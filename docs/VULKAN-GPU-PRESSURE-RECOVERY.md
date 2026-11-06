# Vulkan GPU buffer pressure recovery

Verified **October 29, 2026** on the RTX 5090 Laptop GPU, NVIDIA Windows driver
32.0.16.2101 (621.01), QEMU/WHPX and the private matching NVIDIA Linux runtime.
This fixes a specific allocation failure for opt-in, dedicated external Vulkan
GPU buffers. **Daily-use desktop acceptance remains open: 4/8, 0/4, 3/3.**

## Result and failure comparison

Previously, exhausting the diagnostic memory allowance could terminate the
Windows bridge. Catching that refusal kept the host alive but still caused
the NVIDIA guest D3D12 device to be removed. The new preflight rejects known
pressure before Dozen calls the guest D3D12 allocator, returning
`VK_ERROR_OUT_OF_DEVICE_MEMORY` to the caller.

| Private run | Configuration | Actual result |
| --- | --- | --- |
| Memory 2 | Original host; eight reservation records | Reservation-count refusal (`EMFILE`); guest device loss; four reservations survive until VM exit. Retained failure. |
| Memory 3 | Original host; existing sixteen-record agreement | Third 64 MiB buffer hits the metadata budget; host exits 1; VM requires forced stop. Retained failure. |
| Memory 4 | Catch known metadata memory errors on host | Host exits naturally, but guest device is lost; idle returns -2 and four reservations survive until VM exit. Retained failure. |
| Memory 5 | Same new image as the candidate; previous host | Unsupported preflight falls back to the legacy allocation path and reproduces guest device loss. Retained failure. |
| Memory 6 and 8 | New host, shim and Dozen preflight | Two independent guests refuse the third 64 MiB buffer, release held buffers, successfully retry the exact buffer, finish both original GPU data rounds, and clean up before VM exit. |
| Memory 7 | New components; original normal workload | Both complete GPU data rounds and live cleanup pass. |

Each pressure guest allocates, binds, fills and fences two real **64 MiB GPU-only
buffers**. The next request observes **244,264,960 bytes** of reported host-process
GPU usage; adding 67,108,864 bytes exceeds the unchanged **268,435,456-byte** guard.
The failed allocation returns a null output while the original exported caller FD
remains valid. After freeing the two held buffers, device idle succeeds. Retrying
the same Vulkan buffer, allocation size and memory type succeeds, including another
full GPU fill and retired fence.

The original FD then imports successfully after its creator has already been
freed. Both 64 KiB GPU fill/export/import/copy/readback rounds check **every one of
16,384 words**, with the unchanged independent integer references
`a39e4f675023a325` and `62b2ec7741bea325`. The 64 MiB fills prove allocation,
binding, execution and fence retirement; their complete contents are not read back.

Each pressure run correlates all **13 native submissions, 20 hardware-queue
signals and 10 device-health queries**. All ten kernel notifications retire.
Broker references drain to zero; native objects and aperture mappings retire
before natural host/VM exit. There is no guest device-removal message.

## Implementation and bounds

The new stateless wire operation `QueryNativeGpuBufferAdmission` (`0x20d0`)
accepts the existing public buffer description after capability and session
validation. It checks reported usage plus the requested byte count. The reply
contains no native object, handle or reserved capacity. Old operation numbers and
capability tuples stay unchanged. A valid unsupported reply permits legacy
fallback; malformed replies fail the connection. Memory refusals remain retryable.

The same-thread buffer declaration is cancelled on preflight failure. Dozen patch
0008 maps an actual `ENOMEM` preparation failure to Vulkan out-of-device-memory
before `CreateCommittedResource`; other preparation errors retain their separate
external-handle failure. The default-off typed-buffer hook remains explicit.

The host also catches only known, fully unwound metadata memory failures:
`E_OUTOFMEMORY`, `STATUS_NO_MEMORY`, host allocation failure, or an observed quota
refusal. Unknown budget observations and unrelated native failures remain errors.
That host catch alone does **not** prove guest recovery, as runs 4 and 5 show.

This preflight is **advisory**. It reserves no capacity, uses a necessary public-size
lower bound, and cannot prevent another allocation changing usage before the
native callback. Actual callback budget checks remain in place. Concurrent
allocation races, late native refusals and general application recovery remain open.

The CPU mapping guard stays **96 MiB**; reported GPU usage and GPU-mapped-byte
guards stay **256 MiB**. The pressure guest selects the existing capacity-v2
agreement with sixteen reservation records and a 16 GiB virtual reservation bound;
it creates no OpenGL context. Increasing virtual address record capacity does not
increase those physical/mapping byte guards.

## Regressions and evidence

Fresh FD-zero and FD-exhaustion guests each pass both full GPU data rounds. The
deliberate GPU corruption is rejected at word 16,383 with actual zero versus
expected 2,353,232,594. Both original 4K compute rounds verify all **8,294,400 words
per round**. Both original 3840×2160 shader frames pass the complete pixel oracle
and shared-texture handoff; their bytes match the earlier original shader images.
These regressions run without presentation or input.

The strict default and 4K Windows builds pass. CPU controls cover known-error
unwinding, malformed/unsupported wire replies, the exact patched SDK helper and
negative evidence mutations. The file audit binds frozen/current compiled inputs,
executable hashes, private image changes, the signed runtime snapshot, actual GPU
data/retirement receipts and every retained pressure failure. Historical generic
initialization acceptance stays false for the Vulkan workloads; the old harness
and its signal-count scope remain unchanged. The pressure audit independently
requires all twenty signals using the existing bounded correlation API.

Run the file-only audit against the private artifacts:

```powershell
python experimental/windows-native-gpu/verify_vulkan_gpu_pressure.py --lab <private-lab> --output <new-receipt.json>
```

[Sanitized evidence](evidence/VULKAN-GPU-PRESSURE-2026-10-29.json) contains hashes
and results. Proprietary runtime files, VM images and raw GPU streams stay private.
This establishes neither Vulkan rasterization/window presentation nor normal
launcher/input integration, sustained desktop stability, broad API support or the
requested ≥93% matched-native performance. Those remain required product work.
