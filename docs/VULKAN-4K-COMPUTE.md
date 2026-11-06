# Verified 4K Vulkan compute — October 28, 2026

Two fresh QEMU/WHPX guests now execute a Vulkan compute shader on NVIDIA at
**3840×2160**, twice per guest. GPU transfer readback matches an independent
integer oracle at **all 8,294,400 words per round**. A separate GPU corruption
control fails at the final word as expected. This is offscreen compute, with
**no Vulkan graphics rasterization, window presentation or desktop acceptance**.

The original Mesa 26.0.8 Dozen ICD, genuine NVIDIA Linux UMD and Windows driver
621.01 / 32.0.16.2101 run on the RTX 5090 Laptop GPU. Software/non-NVIDIA devices
are rejected. No source disk, visible renderer/SDL window or host input route
is requested. Official launcher and keyboard/cursor code remain unchanged.

## What the shader test proves

The GLSL shader writes a packed integer RGBA pattern into a device-local buffer
whose selected memory type is **not host-visible**. The probe never maps or
CPU-initializes that storage. A GPU copy transfers its contents into a separate
coherent readback allocation. Explicit compute→transfer→host barriers and a
successful fence wait precede CPU access. The probe then checks every returned
word against its CPU formula. A separate Python reference computes the expected
complete-buffer hashes:

| Round | Words verified | GPU and independent reference FNV-1a |
| --- | --- | --- |
| 0 | 8,294,400 | `650630d8645436e5` |
| 1 | 8,294,400 | `3c21b7a6986969e5` |

Both rounds and hashes pass in runs **2 and 3** using identical images and
probe/ICD binaries. Each run completes eight native submissions, eight native
hardware-queue signals and four successful device-health queries. All tracked
native objects and mappings retire before natural host/VM exit, with no native
failure, pending-work, timeout, forced-stop or deferred-release counters.
Four kernel eventfd notifications also retire without error in each run.
These short runs establish neither device-loss recovery nor sustained health.

The readback data contains one word per pixel position. This is a storage-buffer
compute workload, not a captured Vulkan render target or displayed frame. No
rendered-image or presentation evidence is inferred from the expected pattern.

## Retained allocation failure and controlled capacity change

Run **1** uses the same image but the earlier 128-slot CPU aperture. The first
GPU dispatch and fence wait succeed, then `vkMapMemory` fails with **-5**.
The native admission receipt shows a **32 MiB** mapping needing 32 slots, with
98 slots already occupied and only 30 contiguous slots free. It is refused
before taking the native lock. Logical Vulkan data size remains 33,177,600 bytes.

Runs 2 and 3 select the already-supported **256 virtual CPU slots**, keeping
128 fence slots, the **96 MiB aggregate CPU mapping guard** and the **256 MiB
reported GPU-usage guard** unchanged. This changes virtual aperture capacity;
the workload, GPU storage size, readback size and byte guards stay fixed.
Peak CPU mappings are **61,452,288 bytes** and reported GPU usage is
**110,112,768 bytes**, below those guards. This does not fix slot granularity
or prove arbitrary application allocation/recovery behavior.

Run 1's mapping acknowledgements and eventual cleanup pass, but **four GPU
reservations remain until VM exit** after the UMD reports device removal.
The strict before-VM native cleanup check rejects that run. Its readback failure
and reservation-recovery failure remain preserved. No result is promoted to
whole-run acceptance merely because the host and VM eventually exit cleanly.

## GPU corruption control

Run **4** changes only the embedded shader/probe payload. It deliberately writes
zero to the final output word while leaving the CPU oracle unchanged. The
actual GPU readback is rejected at **x=3839, y=2159, round=0**, with observed zero
and expected **4,280,660,732**. No successful round or completion marker is
emitted. Native objects/mappings and both kernel notifications retire normally.

The default shader and probe remain byte-identical after adding the default-off
control. Both SPIR-V variants pass `spirv-val` for Vulkan 1.1; the negative
shader is valid shader code with deliberately incorrect output. The complete
normal/corrupt images have identical payload-content hashes except `guest-probe`.

## Source and evidence

- [GLSL shader](../experimental/windows-native-gpu/vulkan_compute.comp),
  [probe](../experimental/windows-native-gpu/vulkan_compute_probe.cpp) and
  [GPU workload/oracle](../experimental/windows-native-gpu/vulkan_compute_workload.h).
- [Isolated compiler](../experimental/windows-native-gpu/build_vulkan_compute_probe.py)
  builds normal/corrupt probes from existing tools and Vulkan headers; it never
  launches the workload or installs tools.
- [Independent CPU reference](../experimental/windows-native-gpu/vulkan_compute_reference.py)
  produces expected hashes only.
- [File-only verifier](../experimental/windows-native-gpu/verify_vulkan_compute.py)
  checks source, probe/SPIR-V/image hashes, genuine runtime guards, raw native
  GPU receipts, every round, cleanup and both retained failure controls.
- [Sanitized evidence](evidence/VULKAN-4K-COMPUTE-2026-10-28.json) contains scoped
  results and private receipt hashes. NVIDIA libraries, images and raw native
  command streams remain private.

The old 4K cleanup oracle still requires its original 128-slot geometry. The
new compute verifier checks the explicit 256/128 geometry and every mapping's
before-VM acknowledgement without changing that historical oracle. The generic
initialization harness still rejects all four runs; it does not cover this
compute workload. Scoped compute verification preserves those original reports.

Windowed Vulkan still needs GPU buffer sharing and synchronization with Hyprland;
stock Dozen's Linux WSI software path is insufficient. Vulkan rasterization,
window presentation, broad applications/conformance, recovery and performance
remain open. Backend **4/8**, product **0/4**, diagnostic scene **3/3** remain
unchanged. CUDA, video, DLSS, hardware ray tracing and ≥93% matched-native
performance remain independent required goals.
