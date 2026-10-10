# Native Windows GPU experiment

Experimental Windows NVIDIA driver bridge for the 7Wdev fork. The genuine Linux
NVIDIA runtime in an owned QEMU/WHPX guest now passes
[full D3D12 device and copy queue initialization](../../docs/LIVE-D3D12-INITIALIZATION.md)
and [GPU buffer copies with verified guest readback](../../docs/LIVE-GPU-BUFFER-COPY.md).
Two normal RTX 5090 Laptop runs each copy two distinct 64 KiB patterns through
UPLOAD, DEFAULT and READBACK resources; every byte matches. Native command and
queue-signal fences retire, with normal probe/VM exit and verified cleanup.
This does not establish accelerated Omarchy, the requested APIs or the minimum
93% native performance.

The [shader diagnostic](../../docs/LIVE-GPU-SHADER-TRIANGLE.md) passes two fresh
physical runs with every pixel checked in two distinct rounds. A separate
[shared-texture route](../../docs/LIVE-GPU-SHARED-RESOURCE.md) also passes two
fresh runs: the guest draws into a shared texture and Windows independently
imports and releases that resource before drawing. Select `--runtime-workload shared`
in both the private-image packer and owned-runtime verifier, plus the separate
`--driver-shared-resources` native opt-in. Native GPU consumption and desktop
presentation remain unfinished.

The diagnostic adds owned, nonshared single-allocation resources, the observed
NVIDIA DEFAULT-resource DriverProtection value, direct CPU fence waits and a
bounded GPU2 context-signal route. `--runtime-workload copy` must be selected
both when packing the private guest image and when verifying it. The default
remains initialization only. The copy guard rejects incomplete, duplicated or
incorrect readback results and requires matching native resource/signal evidence.

The [graphics clear workload](../../docs/LIVE-GPU-CLEAR.md) now passes two
physical runs: a DIRECT queue renders two distinct RGBA8 images, with every
pixel verified after GPU readback. Separate asynchronous command acceptance and
hardware-queue signaling preserve ownership until actual native retirement.
The earlier [graphics queue controls](../../docs/LIVE-GRAPHICS-QUEUE-CONTROL.md)
remain lifecycle tests. Initialization and copy controls cannot satisfy the
clear workload verifier. The separate shader and shared-resource guards require
their own drawing/import evidence; desktop presentation remains unfinished.

The optional `--hwqueue-no-broadcast-wait-eof-test` stops the clear workload
after its first combined `NoBroadcastSignal | NoBroadcastWait` queue (flags 6).
It requires submission, retirement and a separate run from other EOF controls.
Its verifier requires unchanged native flags, acknowledged earlier unmaps and
verified native cleanup after owned VM exit. It cannot certify rendered pixels.

The bridge preserves source sentinels, TDR fence flags, native allocation
subranges and verified in-process context priority. Those baseline GPU mappings have a separate
32 MiB cap; CPU views retain their 16 MiB live-byte cap and 4 MiB per-view limit.
The [CPU aperture](../../docs/LIVE-CPU-APERTURE-CAPACITY.md) defaults to 16 slots
and supports 32/64 slots. The optional 128-slot graphics startup profile
negotiates a 32 MiB live vendor CPU-view budget and a separate 64 MiB GPU-map
budget and a 128-allocation live-object limit; other profiles retain 16 MiB CPU,
32 MiB GPU and 96 allocation objects. Per-request allocation lists remain bounded
to 96 entries.
It also selects 128 read-only fence pages; other profiles retain 64.
This profile passes the bounded clear diagnostic. [Contiguous views](../../docs/LIVE-CPU-ALLOCATION-SPANS.md)
span adjacent slots without a QEMU rebuild. Native pools allow 96 independent
synchronization objects and 256 aggregate wire objects; the
default read-only fence BAR has 64 pages. Earlier hardware acceptance includes repeat, owned
VM exit, old-guest, capacity-rejection and restored CPU write controls.

[Allocation retirement](../../docs/LIVE-ALLOCATION-RETIREMENT.md),
[bounded command submission](../../docs/LIVE-COMMAND-SUBMISSION.md),
[GPU reservations](../../docs/LIVE-GPU-RESERVATION.md) and
[Zero address state](../../docs/LIVE-GPU-ADDRESS-STATE.md) preserve ownership and
verify native progress. General submission, production asynchronous transport and signal/wait,
eviction, guest kernel/DRM support and presentation remain unfinished. The
launcher does not select the experiment. Read the
[investigation](../../docs/WINDOWS-NVIDIA-BACKEND.md) and
[product acceptance gates](../../docs/PRODUCT-ACCEPTANCE.md).

Requirements: x64 Windows 10/11, NVIDIA GPU with D3D12, Visual Studio C++ Build
Tools and Windows SDK. The QMP controller uses the pinned MIT nlohmann/json
single header bundled under `third_party`; no driver installation is required.

```powershell
experimental/windows-native-gpu/build.ps1
experimental/windows-native-gpu/build/native-gpu.exe --probe
# Opens a short-lived window, submits 90 frames and verifies shared pixels.
experimental/windows-native-gpu/build/native-gpu.exe --self-test --report native-gpu-report.json
```

The build runs packet contract tests. GPU tests require an interactive Windows
desktop and real NVIDIA hardware; hosted CI only builds the experiment.
`--probe` reports adapter/device availability without claiming rendering.
Failures have nonzero exit status; software fallback is refused. Reports
explicitly record that guest acceleration and a custom virtio GPU are absent.
The host rendering test does not boot a VM. The separate disk-free QEMU control
test and its build instructions are in [WDDM bridge](../../docs/WDDM-BRIDGE.md).
The [Linux UMD context experiment](../../docs/UMD-CONTEXT-COMPATIBILITY.md)
records the context acceptance and hardware queue dependency investigation.
The [adapter query bridge](../../docs/ADAPTER-QUERY-BRIDGE.md) adds an independent
`--driver-queries` opt-in, bounded large-buffer transactions, and a reusable
Linux query client. Physical QEMU acceptance includes the NVIDIA runtime's
50,616-byte initialization query. The [live runtime diagnostic](../../docs/LIVE-NVIDIA-RUNTIME.md)
adds explicit local UMD selection and documented PV adapter flag adaptation,
without modifying vendor-private replies. Its
[owned-QEMU paging mode](../../docs/LIVE-PAGING-BRIDGE.md) maps native fences
through a host-only QMP channel and retains pages until unmap acknowledgement
or confirmed owned VM exit. It is a partial initialization
experiment, not a graphics kernel driver or a usable accelerated backend.
The [live allocation route](../../docs/LIVE-ALLOCATION-BRIDGE.md) adds the
separate `--driver-allocations` opt-in and typed standalone allocation ownership.
Its reported-memory guard is not a hard allocation-byte quota; it remains
restricted to the local runtime diagnostic.
The [live GPU-address route](../../docs/LIVE-GPUVA-BRIDGE.md) adds
`--driver-gpuva`, typed queue/allocation ownership, preserved positive NTSTATUS
and direct guest verification of paging completion. The page limit bounds
mapped GPU VA rather than opaque vendor allocation bytes.
The [live residency route](../../docs/LIVE-RESIDENCY-BRIDGE.md) adds
`--driver-residency`, owned allocation lists, preserved paging/count/trim
outputs and a conservative bound on residency attempts.
The [native Lock2 investigation](../../docs/NATIVE-LOCK2-INVESTIGATION.md)
adds a standalone CPU-sharing probe and a separate writable QEMU allocation
hub. The [live CPU allocation route](../../docs/LIVE-CPU-ALLOCATION-BRIDGE.md)
adds `--driver-cpu` only to an owned QEMU runtime. Physical tests passed direct
reads, first/last-word stores restored by Windows, repeated guest references,
capability-disabled behavior and native cleanup after VM exit with a lock held.
The [live hardware queue route](../../docs/LIVE-HARDWARE-QUEUE-BRIDGE.md) adds
documented allocation-token translation and the `--driver-hwqueues` opt-in.
The live UMD builds its private queue data from a translated allocation alias;
public operations still resolve to typed owned wire objects. Direct queue
fences, queue-before-allocation cleanup and guest exit with a queue held passed
physical testing. Exact committed CPU subregions of 4, 16, 64 and 128 KiB work,
including subregions inside larger reservations. Live runtime writes reached
eight views. The [live synchronization route](../../docs/LIVE-SYNCHRONIZATION-BRIDGE.md)
adds `--driver-syncs`, typed monitored-fence/mutex ownership, direct fence reads,
acknowledged destruction and native cleanup after abrupt guest exit.
The separate `--driver-submit` route validates ownership, residency and a
retained CPU command mapping before native submission. It synchronously waits
for retirement as a diagnostic. The optional `--driver-async-submit` selects a
separate negotiated queue operation, returning after KMT acceptance and recording
actual hardware retirement independently. The verifier requires every accepted
command to retire and complete native cleanup. This removes a demonstrated
startup dependency stall and supports the verified graphics diagnostics. Production queues
must also preserve asynchronous
execution and overlap. General allocation layouts, broader synchronization operations,
scanout and desktop integration remain unfinished.
The separate `--driver-retirement` option uses documented VidMm destruction,
keeps `AssumeNotInUse` zero, requires acknowledged CPU unmap and preserves
ownership on native failure. CPU-slot exhaustion returns allocation failure
before taking a native lock or issuing a QMP mapping.
The [product acceptance gates](../../docs/PRODUCT-ACCEPTANCE.md) require CUDA,
OpenGL, Vulkan, video acceleration, a stable interactive Omarchy desktop and at
least 93% of native performance. No current test proves those gates.

The framing reference is pinned in `sources.json`. No upstream implementation
is vendored from virtio-nvgpu; these new files use the repository's MIT license.
The separately vendored JSON parser retains its own MIT license and exact
source hashes, and native builds package that license with the executable. Future copied
upstream code must preserve its per-component license: protocol, device backend
and guest kernel module have different terms.
