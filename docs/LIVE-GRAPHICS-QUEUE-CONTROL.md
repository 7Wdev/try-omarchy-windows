# NVIDIA graphics queue control through QEMU

Historical checkpoint: the later [GPU clear milestone](LIVE-GPU-CLEAR.md) now
passes rendered-pixel verification. The controls below deliberately stop earlier.

The genuine NVIDIA Linux runtime can now create a Windows hardware queue with
`NoBroadcastSignal` through the QEMU/WHPX bridge. Two physical RTX 5090 Laptop
controls on 2026-10-09 accepted the flag unchanged and reclaimed every native
object after the owned VM exited. Initialization and GPU buffer-copy regression
controls still pass. **Guest graphics rendering is unverified.**

The [hardware evidence](evidence/QEMU-LIVE-GRAPHICS-QUEUE-CONTROL-2026-10-09.json)
separates these controls from an earlier stalled graphics attempt. The controls
exit immediately after the first flag-2 queue, before completing DIRECT queue
creation or submitting the clear workload. Their accepted diagnostic reports
explicitly leave initialization, buffer-copy and rendered-pixel success false.
The earlier [initialization](LIVE-D3D12-INITIALIZATION.md) and
[verified GPU copies](LIVE-GPU-BUFFER-COPY.md) remain separate milestones.

## Flag forwarding and lifetime

The prior bridge accepted only hardware queue flags zero. The graphics runtime
also requests the documented `NoBroadcastSignal` bit, value 2. Microsoft defines
this as excluding an internal queue from a D3D12 broadcast signal in
[D3DDDI_CREATEHWQUEUEFLAGS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_createhwqueueflags).
The bridge advertises a separate capability, rejects an unnegotiated request,
and forwards the complete flags field unchanged to native `D3DKMTCreateHwQueue`.
The native SDK bit layout and returned flags are checked. Other bits and flag
combinations remain rejected; the timeout-disable bit is unsupported.

The explicit `--hwqueue-no-broadcast-eof-test` control requires the `clear`
private image, submission and allocation-retirement opt-ins. It stops the Linux
probe immediately after successful creation and direct progress-fence mapping
of the first flag-2 queue. It performs no further graphics initialization.
Native objects and mappings remain owned until QEMU has exited.

Unlike the earlier first-queue EOF test, this point follows COPY queue teardown.
The verifier counts earlier successful fence unmaps and CPU span unmaps, checks
their QMP acknowledgements, and checks that the remaining objects are released
after VM exit. Existing early-EOF zero-acknowledgement requirements remain in
force. Normal execution still requires complete acknowledged teardown.

Each final physical control creates five hardware queues. One is destroyed by
the guest; four, including the flag-2 queue, are retained until VM exit. All five
are then destroyed. One preexisting 4 KiB COPY queue teardown packet retires at
native target 5. It is not a graphics clear command. No forced VM stop, native
failure counter or remaining live object is reported in these controls.

## Clear workload implemented, but not accepted

The new explicit `--runtime-workload clear` probe requests a DIRECT queue, a
130-by-73 RGBA8 render target and a readback buffer. Its intended two rounds
clear the whole texture and an inner rectangle with distinct primary colors,
transition the texture to COPY_SOURCE, copy it into readback memory, signal the
queue and compare every one of 9,490 pixels. The final round exports only the
application's dense RGBA rows for independent host checking.

Readback addressing respects the returned row pitch and a nonzero 512-byte
placement offset. The buffer includes that offset in addition to the required
span returned by `GetCopyableFootprints`, following Microsoft's
[resource helper](https://github.com/microsoft/DirectX-Headers/blob/adbd6f3ba40795c46a8d0f33af00bcb57ff0f0a4/include/directx/d3dx12_resource_helpers.h).
The verifier requires all API statuses, both independent color hashes, both
retired queue fences, native submissions, every exported final pixel and
complete cleanup. Synthetic tests reject partial, duplicate, stale, altered or
device-loss evidence. These tests do not prove GPU rendering.

An earlier physical attempt with flag forwarding reached slots 62 and 63 of
the selected 64-slot CPU aperture, then rejected the next 4 MiB Lock2 request
with `ENOMEM`. A subsequent 32 KiB driver command did not produce a retirement
record before the owner deadline. The VM and native owner were later observed
to have exited, but the observer did not obtain a native cleanup receipt. That
attempt is rejected, with cleanup unknown. No pixels or successful DIRECT queue
HRESULT were recorded. It used earlier diagnostic binaries, not the final
EOF-control build.

The observer now streams native errors into a local log, uses daemon pipe
readers, and preserves a failed JSON report when observation or a deadline
fails. Its existing timeout path retains the exact QEMU process handle and
verifies child exit before terminating a native page owner. The accepted
controls exercise normal observation; they do not establish recovery from a
stalled graphics submission.

## Validation and next gate

MSVC and GCC warning-as-error builds, 167 Linux fault cases, 53 native CLI
rejection cases and nine milestone guard groups pass. An old native bridge
rejects the unnegotiated queue flag with verified cleanup. A buffer-copy image
requested as `clear` is rejected even though its own copy succeeds. Neither
negative control can claim rendered pixels.

The next hardware gate is to support the graphics startup allocation footprint
with explicit bounded capacity and then resolve any remaining submission or
queue dependency failure. Capacity alone has not been shown to fix the stalled
command. No QEMU capacity, native byte quota, timeout, driver installation or
launcher selection was changed here. Presentation, real Omarchy/Hyprland
integration, CUDA/OpenGL/Vulkan/video, asynchronous production transport,
reset recovery and the 93% performance target remain unfinished under
[PRODUCT-ACCEPTANCE.md](PRODUCT-ACCEPTANCE.md).
