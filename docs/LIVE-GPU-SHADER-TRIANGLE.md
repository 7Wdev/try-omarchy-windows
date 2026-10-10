# NVIDIA guest shader rendering through QEMU

Two fresh owned QEMU/WHPX guests on 2026-10-10 rendered vertex/pixel shader
triangles on the physical NVIDIA RTX 5090 Laptop GPU through the Windows KMT
bridge. Both runs passed two distinct 130×73 RGBA8 rounds. Every pixel was
checked independently on the host, all 13 native commands and two hardware
queue signals retired, and every bridge-owned object was released normally.
The native worker and Linux ioctl bridge are unchanged from the verified
[render-target clear checkpoint](LIVE-GPU-CLEAR.md).

![Actual NVIDIA guest shader triangle, round two](images/nvidia-guest-shader-triangle-2026-10-10.png)

Actual final-round GPU readback, enlarged 6× with nearest-neighbor sampling.
Both fresh runs produced identical dense RGBA bytes, with SHA-256
`fd894f123883a31e65cea02dc84eb9a52f0356511b4a85eb41977d8c46814353`.
The image contains the shader's interpolated vertex colors. No reference or
generated image was substituted for GPU output.

[Hardware evidence](evidence/QEMU-LIVE-GPU-SHADER-TRIANGLE-2026-10-10.json)
records both shader runs, a passing clear regression, a rejected clear-only
image requested as shader drawing, source/executable hashes and native
acceptance/retirement receipts. All four guests and their workers exit normally
with clean teardown. The negative control cannot set the shader success field.

## Graphics pipeline and verification

The genuine installed NVIDIA Linux D3D12 runtime creates a DIRECT queue, a root
signature with one vertex-stage root constant, and a graphics pipeline containing
validated shader-model-6.0 DXIL. It draws a three-vertex triangle, transitions the
RGBA8 render target to COPY_SOURCE, copies it into a pitched readback buffer and
waits for its workload fence. The second round resets the allocator/list and
changes both the shader color rotation and background. This exercises shader
execution, interpolation and an explicit shader parameter across list reuse.

The guest checks a floating-point barycentric reference. Separately,
`triangle_evidence.py` uses integer edge equations at pixel centers. All vertices
lie exactly on the subpixel grid and no pixel center lies on an edge, so coverage
is unambiguous. Every round contains 2,680 shaded pixels and 6,810 background
pixels; none are skipped. Only interpolated triangle RGB permits an error of
two 8-bit UNORM steps. Background RGB and alpha must match exactly. The observed
maximum error is one step in both rounds, consistent with interpolation rounding.

| Round | Reference FNV-1a | Actual GPU FNV-1a | Workload fence |
| --- | --- | --- | --- |
| 1 | `4bcc063a79d16bd1` | `7ee55ce8ce7f7ee1` | 1 |
| 2 | `5e4404d75aeb0297` | `fb1b3577b0f6bcbf` | 2 |

Both rounds export every actual RGBA row. The host recomputes their hashes and
checks each channel against the integer reference. It also requires successful
pipeline/resource API results, the 512-byte readback placement offset and valid
768-byte pitch, both draw records, ordered complete rows and workload fences.
Queue-acceptance log markers are accepted only after independent native progress
fences prove retirement of every command. Signal retirement cannot be replaced
by a CPU fence write.

The runs peak at three pending commands and 108 live vendor allocations.
All 124 allocations created during each run are destroyed. The existing
128-slot diagnostic capacity profile suffices; this change increases no native
object, memory or submission limit.

## Building the shader source

The checked-in `triangle_shaders.hlsl` is compiled by
`experimental/windows-native-gpu/compile_triangle_shaders.py`. Supply an installed
official DXC and write to a fresh path, then review the generated header:

```powershell
python experimental/windows-native-gpu/compile_triangle_shaders.py --dxc "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe" --output triangle_shaders.regenerated.h
```

This checkpoint uses SDK DXC `1.8.2502.11`, profiles `vs_6_0` and `ps_6_0`, and
flags `-O3 -Ges -Qstrip_debug -Qstrip_reflect`. A second compilation reproduces
the checked-in header byte for byte. Compiler, validator and shader source hashes
are recorded in the evidence. Compiler/runtime binaries are not redistributed.
The existing runtime build includes the shader workload. Select
`--runtime-workload triangle` in both `make_runtime_initramfs.py` and
`test_owned_runtime_qemu.py`, using the same explicit native capabilities as the
clear diagnostic. The packed NVIDIA runtime image remains private.

GCC warning-as-error builds and 23 verifier guard groups pass. The guards reject
missing shader/pipeline results, altered coverage, stale or flipped frames,
incorrect shader hashes, incomplete rows, widened tolerances, failed API
statuses and missing retirement evidence. Previous MSVC/GCC, 175 ioctl fault
and 71 native CLI checks cover the unchanged native bridge.

The preliminary legacy shader-model-5 bytecode attempt returned `E_NOTIMPL` at
pipeline creation. Validated DXIL resolved that boundary. A subsequent run
correctly rejected the diagnostic's assumption that `SV_VertexID` includes the
draw start offset; the final workload uses an explicit root constant instead.
Both rejected reports remain preserved and both have verified cleanup. See
Microsoft's [shader compiler](https://github.com/microsoft/DirectXShaderCompiler),
[root signatures](https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-a-root-signature)
and [vertex ID semantics](https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/).

This remains a bounded graphics diagnostic. GPU presentation, the guest
kernel/DRM backend, interactive Omarchy/Hyprland, CUDA, OpenGL, Vulkan, video,
reset recovery and the minimum 93% native performance target remain unfinished.
Readback is used to verify correctness here; it is not the planned desktop
presentation path. See [product acceptance](PRODUCT-ACCEPTANCE.md).
