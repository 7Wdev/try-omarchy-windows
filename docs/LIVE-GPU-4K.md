# 4K shader rendering and display diagnostics

The owned QEMU/WHPX guest renders two distinct 3840×2160 NVIDIA shader frames.
An independent integer rasterizer verifies all 8,294,400 pixels per frame, with
exact triangle coverage, background and alpha, and at most one UNORM color step
of interpolation error in the measured frames. A Windows GPU consumer imports
the guest texture, copies it to a retained native resource and copies it into
DXGI backbuffers. Actual backbuffer bytes match the native copy byte for byte.

One source-built presentation run completes both DXGI statistics receipts.
Two subsequent runs, including the reviewed build, retain the first frame's
statistics after the second successful `Present` and queue retirement. Their
strict presentation result remains **failed**. These runs establish correct 4K
shader pixels and resource copies, but do not establish display stability,
sustained performance or accelerated Omarchy/Hyprland.

![Actual second 4K guest shader frame](images/nvidia-guest-shader-4k-2026-10-11.png)

The image is actual GPU readback from the successful source-built run, encoded
losslessly at 3840×2160. It is not an enlarged 130×73 image or a window screenshot.
The diagnostic window is 780×438; this does not prove native 4K monitor scanout.
The dense second-frame RGBA SHA-256 is
`0a5b6e1ed3c42b5a4ac4d668b4607a7938a2144e7daeb98177fcd79add032abf`.

[Evidence and original-file hashes](evidence/QEMU-LIVE-GPU-4K-2026-10-11.json)
preserve the successful run, failures, reviewed build and standard-profile
regression. Private libraries, guest images and opaque metadata are not included.

## Explicit lab profile

`build.ps1 -HighResolutionDiagnostic` creates a separate `build-4k` output by
default. The guest build takes an explicit fourth argument, `4k`. Both compile
with `WDDM_BRIDGE_4K_LAB=1`; this is a narrowly bounded lab profile, not a general
resolution negotiation protocol. Capability bit 31 distinguishes it from the
standard build. Its native texture declaration accepts only 3840×2160 RGBA8,
one mip/array/sample and `ALLOW_RENDER_TARGET`. The standard build accepts its
original 130×73 profile and retains its original budgets.

| Limit | Standard 128-slot profile | Separate 4K lab |
| --- | ---: | ---: |
| Live vendor CPU views | 32 MiB | 96 MiB |
| Single CPU/GPU mapping | 4 MiB | 64 MiB |
| Live mapped GPU bytes | 64 MiB | 256 MiB |
| Reported GPU usage guard | 64 MiB | 256 MiB |
| Dense RGBA per frame | 37,960 bytes | 33,177,600 bytes |

The 4K host rejects startup unless native metadata, shared consumption, 128 CPU
slots and an owned QEMU runtime are explicitly requested, along with their
existing dependency gates. The guest rejects a 4K native declaration when the
host lacks bit 31. Template allocation size and copy footprints remain bounded.
The driver-reported usage guard is not a hard quota on every vendor allocation.

Large reference/readback images now use heap storage. A previous private trial
attempted to export each 4K frame as about 66 MB of UART hex text and exceeded
the transport timeout. The 4K profile instead keeps guest pixel verification and
hashes, then writes native dense RGBA into an explicitly supplied directory.
`CREATE_NEW` prevents overwriting previous captures, failures retain partial
files, and scoped unmapping covers exceptions during file export. VM teardown
also emits its final ownership receipt during exception unwinding.

These changes remove the serial capture bottleneck from the diagnostic. They
do not remove its full-frame CPU verification, synchronous handoffs, readbacks
or presentation pauses, and consequently do not constitute a frame-rate test.

## Reproduce

Build with the existing pinned headers/runtime prerequisites documented in the
[native GPU experiment](../experimental/windows-native-gpu/README.md):

```powershell
experimental/windows-native-gpu/build.ps1 -HighResolutionDiagnostic
```

```sh
sh experimental/windows-native-gpu/build_runtime_diagnostic.sh \
  /path/to/DirectX-Headers /path/to/libdxg /path/to/private-4k-build 4k
```

Pack that probe and shim with `make_runtime_initramfs.py --runtime-workload native`
and locally installed, compatible NVIDIA runtime libraries. Keep the resulting
image private. Create a fresh absolute capture directory and set
`WDDM_BRIDGE_CAPTURE_DIRECTORY` in the host process environment. Run
`test_owned_runtime_qemu.py` with the private image, the 4K host, `--runtime-workload native`,
`--driver-native-metadata`, `--driver-shared-resources`, `--driver-consume-shared`,
the existing submission/synchronization/retirement/CPU/GPU-address opt-ins and
`--driver-cpu-slots 128`. Add `--driver-present-shared` to request the diagnostic
window. Use a fresh report/log prefix for every run.

The original runner still applies its strict 130×73 acceptance criteria and
therefore returns rejection for this different profile. Keep its report intact;
evaluate the separate 4K evidence with:

```powershell
python experimental/windows-native-gpu/verify_4k_capture.py `
  --report C:/lab/run/driver.json --capture-directory C:/lab/run `
  --oracle experimental/windows-native-gpu/build-4k/rgba-oracle.exe `
  --output C:/lab/run/verified.json
```

The verifier independently recomputes integer rasterization, both hashes and
every actual pixel; correlates guest/native fences, metadata and imports; checks
backbuffer equality; and requires normal owned-VM exit and complete cleanup.
It records rendering, handoff and presentation separately. A failed full
diagnostic returns nonzero while preserving its report. Missing or stale
statistics cannot become a presentation pass merely because pixels are correct.

## Display trace finding

An Intel-signed PresentMon 2.6.0 trace, restricted to the private renderer's PID
and with input tracking disabled, captured one `Composed: Flip` row with
`Dropped=0` and 18.8831 ms from `Present` to display. It corresponds to the
second actual presentation in that two-frame experiment, whose DXGI statistics
remained at count 1. PresentMon's
[v2.6.0 output code](https://github.com/GameTechDev/PresentMon/blob/v2.6.0/PresentMon/OutputThread.cpp)
seeds the first presentation without emitting a row. Thus this trace does not
independently verify both frames. It provides evidence that a stale counter can
reject a frame that reached the display; it is not evidence of stable delivery.

The source now records the process ID, swapchain identity, QPC frequency and
timestamps immediately before/after each `Present`. The optional offline
`--presentmon trace.csv` check requires an exact unique PID/swapchain/call-interval
match, rejects duplicates and unrelated rows, and leaves missing frames
unverified. The earlier private trace lacks those new call intervals, so it is
preserved as investigative evidence rather than retrofitted into that stricter
verifier. ETW is an optional lab tool, not a production runtime dependency.

## Next engineering work

Sustained rendering needs bounded outstanding submission/signal credits and a
separate total-run budget. The current lifetime caps (16 submissions/signals,
two shared consumes) are diagnostic limits. Keep native retirement and ownership
checks while allowing retired entries to be reused. Add a matched native Windows
baseline and GPU timestamp queries; measure shader work, transport and display
separately. Full-frame CPU verification and visible pauses belong outside the
timed loop. A GPU-to-GPU display path must handle occlusion, resize, disconnect
and reset without making progress depend on optional DXGI statistics.

Guest kernel/DRM integration, Omarchy/Hyprland, CUDA, OpenGL, Vulkan, video
acceleration and the minimum 93% native-performance requirement remain open.
The full [product acceptance gates](PRODUCT-ACCEPTANCE.md) still apply.
