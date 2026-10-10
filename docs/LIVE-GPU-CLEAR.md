# NVIDIA guest graphics pixels through QEMU

Two owned QEMU/WHPX runs on 2026-10-10 rendered and verified two distinct
130-by-73 RGBA8 images on the physical NVIDIA RTX 5090 Laptop GPU. All 9,490
pixels in each round matched an independent reference. The genuine NVIDIA
Linux D3D12 runtime creates a DIRECT queue, render target, RTV heap and readback
buffer through the Windows KMT bridge. Both runs exit normally with all native
commands retired and all bridge-owned objects released.

[Hardware evidence](evidence/QEMU-LIVE-GPU-CLEAR-2026-10-10.json) records both
repeats, copy and initialization regressions, a combined internal-queue VM exit
control, and a rejected copy-image/clear-verifier mismatch. The prior
[graphics queue controls](LIVE-GRAPHICS-QUEUE-CONTROL.md) are historical tests
that stopped before graphics initialization.

## Queue dependencies and signals

Graphics initialization submits work to several internal queues. Waiting for
each command to retire before accepting the next command blocked dependencies
that the runtime had not yet submitted. The separate negotiated
`--driver-async-submit` operation returns after native KMT acceptance and retains
command mappings and dependencies until native progress fences prove retirement.
The legacy synchronous operation retains its original behavior. Enqueue
receipts alone cannot satisfy the verifier.

The new capability-gated `QueueHwQueueSignal` operation translates owned queue
and synchronization IDs to
[D3DKMTSubmitSignalSyncObjectsToHwQueue](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_submitsignalsyncobjectstohwqueue).
It supports one owned NoGpuAccess monitored fence and up to eight distinct
hardware queues belonging to the same device. Flags zero or the documented
`AllowFenceRewind` value 4 are forwarded unchanged; this diagnostic still
requires monotonically increasing targets. It never writes the CPU fence value.
Accepted signals remain owned until actual native fence retirement. There are
at most 16 command submissions and 16 hardware queue signals per diagnostic.

Each clear repeat accepts and retires 13 commands totaling 311,296 bytes and
two hardware queue signals. The peak of three pending commands demonstrates
that command acceptance is asynchronous. Both final workload fences reach
targets 1 and 2. Every command and signal has matching native acceptance and
retirement receipts; all pending and failure counters are zero at cleanup.

The explicit 128-slot startup profile provides 128 read-only fence pages,
128 live allocation objects, 32 MiB of live CPU views and 64 MiB of mapped GPU
VA. The runs peak at 108 live allocations, 26,996,736 CPU bytes and 10,243 GPU
pages. Defaults and per-request limits remain bounded separately.

## Pixel verification

Round one clears red with a cyan rectangle; round two clears blue with a yellow
rectangle. Dense RGBA hashes are `c53e73aa3030bd71` and `8bce2f687f7dcf61`.
The readback uses a nonzero 512-byte placement offset and a 768-byte row pitch.
The host independently checks the expected hashes and every exported final pixel.
The first physical run produced correct pixels but its original report rejected
the new asynchronous log markers. That report remains preserved. The corrected
verifier requires independent native retirement before accepting those markers;
two fresh runs then pass without altering captured pixel data.

MSVC and GCC warning-as-error builds pass, as do 175 Linux ioctl fault cases,
71 native CLI rejection cases and 17 verifier guard groups. The guards reject
missing retirement, changed pixels, duplicate or stale results, failed API
statuses, device loss and incomplete cleanup. The copy regression checks both
64 KiB patterns through GPU DEFAULT memory; initialization remains a separate gate.

This is a bounded graphics diagnostic. Shader drawing, presentation, a guest
kernel/DRM backend, Omarchy/Hyprland integration, CUDA, OpenGL, Vulkan, video,
reset recovery and the minimum 93% native performance target remain unfinished.
Earlier rejected synchronous/capacity runs have unknown cleanup and are not
counted as success. Vendor runtime images remain private and are not distributed.
See [product acceptance](PRODUCT-ACCEPTANCE.md).
