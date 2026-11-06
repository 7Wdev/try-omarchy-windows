# Actual 4K shader rendering during background tests

Both the original and sampled-read guest shims now complete **two actual
3840×2160 NVIDIA shader frames** through QEMU and the Windows-native bridge.
The guest uses the genuine NVIDIA D3D12 runtime; native submission/retirement,
typed resource metadata, shared-texture import and GPU copy are correlated with
the captured pixels. Native copies report no CPU texture upload. These runs
request no display presentation, keyboard/mouse route, desktop session or source
disk, so they can run while the Windows desktop remains available.

![Actual captured 4K NVIDIA shader frame, second color rotation](images/nvidia-4k-shader-background-2026-10-27.png)

This PNG encodes the actual second captured RGBA frame. Its decoded bytes match
the sampled run and baseline exactly, with SHA-256
`0a5b6e1ed3c42b5a4ac4d668b4607a7938a2144e7daeb98177fcd79add032abf`.
Its resolution is 3840×2160; the preview may be scaled by the viewer.

## What passed

The existing independent C++ integer rasterizer checks **all 8,294,400 pixels in
each frame**. Coverage and background/alpha are exact. Only interpolated RGB
inside the triangle permits the existing tolerance of two; the observed maximum
channel error is **one** in all six captures, including the adapter repeat. The second round changes the
shader colors, and both rounds have distinct verified hashes. Deliberately
corrupting a background alpha byte is rejected by the same compiled oracle.

The unchanged full 4K verifier also checks pipeline/HRESULT results, shader
bytecode hashes, the real readback footprint, native metadata/import, correlated
command and handoff fences, complete binary captures and owned cleanup. Each
run completes eight native submissions and two shared GPU copies. Both the host
and VM exit naturally, and all tracked native objects and aperture mappings
retire before VM exit.

A third run repeats the sampled image through the separate broker-aware runtime
adapter. It produces the same two captured frames and verifies broker
initialization while preserving the exact historical harness bytes. This also
checks the CI repair against real GPU work; the original baseline reports remain
unchanged.

The paired images differ only in the shim. Each run emits 447 diagnostic probe
records: **4,470,000 reads** with the legacy selection and **894 reads** with the
sampled selection. Corresponding captured frames are byte-identical. These are
read counts, with no timing or performance-ratio claim. Reported GPU usage peaks
at 253,648,896 bytes under the unchanged 268,435,456-byte diagnostic guard; pressure
recovery is not established by this workload.

[Structured evidence and artifact hashes](evidence/BACKGROUND-4K-SHADERS-2026-10-27.json)
are produced by the [paired auditor](../experimental/windows-native-gpu/background_shader_pair_audit.py),
which composes the existing 4K verifier with strict background, broker, sampling
and source/driver guards. Private runtime libraries/images and raw native command
streams remain excluded from the repository.

## Scope and regression controls

The original generic small-profile reports remain **rejected** and unchanged.
The existing separate 4K verifier returns `full4kDiagnosticVerified=true` for
all three runs. DXGI presentation and physical scanout are unverified because these
tests do not request a display. No actual GPU or desktop run is represented by
the portable mock tests alone.

New background-control tests reject presentation/input/disk options, missing or
duplicate profile parameters, incorrect read counts, malformed receipts, invalid
broker witnesses, and even failed presentation-event records. The existing
4K capture/oracle negative tests retain their original requirements.

The sampled selection still needs full Omarchy application/session integration
and ordinary-input/latency testing. Longer sessions, broader API/application
coverage, pressure recovery and the ≥93% matched-native target remain open.
Backend stages and desktop product gates remain **4/8** and **0/4**.
