# Native Windows GPU experiment

Host renderer and partial WDDM allocation bridge for the 7Wdev fork. A QEMU/WHPX
Linux guest creates Windows GPU allocations, transfers bounded data, makes
them resident and maps GPU addresses. Shared guest RAM and bounded, fenced GPU
copies passed physical acceptance. Opt-in virtual context lifecycle support is
also tested, including locally captured Linux NVIDIA initialization data.
Live NVIDIA Linux runtime initialization now reaches native paging and graphics
context creation through QEMU, then fails at the unsupported live allocation
interface. Direct guest fence reads and acknowledged unmap passed physical testing. Omarchy
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

The framing reference is pinned in `sources.json`. No upstream implementation
is vendored from virtio-nvgpu; these new files use the repository's MIT license.
The separately vendored JSON parser retains its own MIT license and exact
source hashes, and native builds package that license with the executable. Future copied
upstream code must preserve its per-component license: protocol, device backend
and guest kernel module have different terms.
