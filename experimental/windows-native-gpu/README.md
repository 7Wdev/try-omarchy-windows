# Native Windows GPU experiment

Host renderer and partial WDDM allocation bridge for the 7Wdev fork. A QEMU/WHPX
Linux guest creates Windows GPU allocations, transfers bounded data, makes
them resident and maps GPU addresses. Shared guest RAM and bounded, fenced GPU
copies passed physical acceptance. Opt-in virtual context lifecycle support is
also tested, including locally captured Linux NVIDIA initialization data.
Live NVIDIA Linux runtime initialization now reaches native paging and graphics
context creation, standalone video-memory allocation, GPU-address mapping,
residency, direct CPU allocation access, hardware queues and synchronization
objects through QEMU. Physical testing created eight monitored fences, eight
mutexes, three hardware queues and 16 allocations before
the diagnostic allocation-count limit. The [bounded command route](../../docs/LIVE-COMMAND-SUBMISSION.md)
has executed one live runtime initialization command and verified its progress
fence in Windows and the guest. General command submission remains unfinished. Direct guest
fence reads, synchronous allocation destruction and acknowledged unmap passed physical testing. Omarchy
desktop acceleration remains unimplemented. Read the
[investigation](../../docs/WINDOWS-NVIDIA-BACKEND.md).

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
for retirement as a diagnostic; production queues must preserve asynchronous
execution and overlap. General allocation layouts, synchronization signal/wait operations,
scanout and desktop integration remain unfinished.
The [product acceptance gates](../../docs/PRODUCT-ACCEPTANCE.md) require CUDA,
OpenGL, Vulkan, video acceleration, a stable interactive Omarchy desktop and at
least 93% of native performance. No current test proves those gates.

The framing reference is pinned in `sources.json`. No upstream implementation
is vendored from virtio-nvgpu; these new files use the repository's MIT license.
The separately vendored JSON parser retains its own MIT license and exact
source hashes, and native builds package that license with the executable. Future copied
upstream code must preserve its per-component license: protocol, device backend
and guest kernel module have different terms.
