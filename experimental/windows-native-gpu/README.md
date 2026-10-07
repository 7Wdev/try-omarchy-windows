# Native Windows GPU experiment

Host-side rendering and protocol experiment for the 7Wdev fork. It is not wired
into QEMU or Linux and is not a completed virtio-nvgpu Windows backend. Read the
[investigation](../../docs/WINDOWS-NVIDIA-BACKEND.md).

Requirements: x64 Windows 10/11, NVIDIA GPU with D3D12, Visual Studio C++ Build
Tools and Windows SDK. No added library dependencies or driver installation.

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
explicitly record that guest acceleration and virtio transport are absent.
No VM or existing Omarchy installation is touched.

The framing reference is pinned in `sources.json`. No upstream implementation
is vendored; these new files use the repository's MIT license. Future copied
upstream code must preserve its per-component license: protocol, device backend
and guest kernel module have different terms.
