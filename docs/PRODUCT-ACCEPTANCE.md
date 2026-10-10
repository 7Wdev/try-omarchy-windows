# Requested product and performance gates

The requested product is an interactive Omarchy/Hyprland desktop in QEMU on
Windows with NVIDIA GPU acceleration, CUDA, OpenGL, Vulkan and video
acceleration. It must remain compatible with upstream fixes and deliver at
least 93% of native performance. Initialization diagnostics, host rendering and
a single guest GPU command do not satisfy these requirements.

No current result proves this product or performance threshold. The launcher
does not select the experimental backend. The current diagnostic supports
separately negotiated asynchronous submission and directly shared native
fences, with bounded native objects. It verifies [guest shader drawing](LIVE-GPU-SHADER-TRIANGLE.md),
but remains a compatibility and ownership probe. The production transport and
desktop presentation path still need implementation and validation.

## Functional acceptance

| Requirement | Evidence required before completion |
| --- | --- |
| Interactive desktop | Boot the real Omarchy image with the new backend; prove Hyprland and applications render on the NVIDIA GPU in the existing VM window; test input, resizing, clipboard and host integrations |
| CUDA | Run real guest kernels, allocation/copy/stream/event workloads and representative application workloads through the Windows driver; verify outputs and advertised features; prove no dependency on another VM's `/dev/dxg` |
| OpenGL | Guest EGL/OpenGL rendering and presentation with hardware acceleration; run conformance appropriate to the advertised version, representative applications and compositor workloads |
| Vulkan | Guest physical-device enumeration, compute/graphics/transfer queues, synchronization, memory sharing and window presentation; run validation/conformance for the advertised version and real workloads |
| Video acceleration | Hardware decode and encode, output correctness and GPU-to-display integration; publish the tested codec/profile/bit-depth/resolution matrix and measure copies between decode, rendering and presentation |
| Interoperability | Verify shared resource/fence lifetimes across the supported graphics, compute and video paths; advertise only tested interoperability |
| Stability | Repeated boot/shutdown, resize and mixed API workloads, sustained rendering, disconnect/crash cleanup and GPU timeout/reset recovery; no resource leaks or silent software fallback |
| Upstream updates | Preserve the fork's upstream synchronization, test changes against the new backend and retain custom commits when updating |

API versions, extensions and video formats must be inventoried and tested.
Missing features remain unfinished work; they must not disappear from the
completion audit because one convenient benchmark passes.

NVIDIA's [CUDA on WSL guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html)
documents a Linux CUDA library backed by the Windows driver, but also lists
managed-memory and OpenGL/CUDA interoperability limitations. It does not
establish compatibility of these libraries with this QEMU bridge.
[Mesa's D3D12 driver](https://docs.mesa3d.org/drivers/d3d12.html) is a candidate
graphics front end, not evidence that this guest has OpenGL, Vulkan or video
support. Each API needs its own integration and verification.

## Production transport and bottleneck checks

The driver bridge should retain native GPU allocations and shared mappings
across frames. Bulk resources and command buffers belong in bounded shared
memory; virtqueues carry descriptors, ownership changes and doorbells. Native
fences provide completion without reading each value through an emulated MMIO
or control RPC. Submission should enqueue work without a host wait on every
command. Waits belong at actual application dependencies and resource reuse.

Keep independent queues available for graphics, compute, transfer and video;
avoid a global worker lock that serializes unrelated GPU work. Batch descriptor
notifications where latency permits. Use explicit backpressure and completion
events, with generation ownership and validated retirement before reuse.
Sharing must preserve access permissions and isolation, including while a VM
crashes or its driver resets. Presentation should share GPU resources and fences
with the host window; CPU readback or encode/decode of every desktop frame is
not an acceptable default display path.

Measure submission latency, time waiting for the host, CPU utilization,
allocation/residency churn, interrupt/doorbell rates, copied bytes and queue
depth. Attribute any measured regression to its transport, driver, API front
end or presentation stage. Changing this design is acceptable when measurements
show a better implementation that still satisfies the complete product.

## At least 93% of native performance

The initial reproducible baseline is the same physical GPU running the same
workload natively on Windows, with the same driver, output, resolution, quality
settings and power conditions. Record the baseline platform explicitly; a
native Linux baseline would require a separately controlled measurement.

For throughput, compare `guest throughput / native throughput`. For time to
complete identical work, compare `native time / guest time`. Each agreed
representative workload must reach at least `0.93`; a fast CUDA result cannot
hide a slower Vulkan or video result. Include small submissions and transfers
as well as large GPU-bound workloads, so batching cannot conceal excessive
latency. Report median, tail latency, variability and thermal conditions, with
interleaved native/guest runs and repeated samples.

The benchmark matrix must include CUDA kernels and transfers, OpenGL/Vulkan
rendering and presentation, video decode/encode at matched quality, and mixed
desktop/application activity. Check correctness before scoring performance.
Publish workload versions, commands, native and guest raw measurements and
calculated ratios. Exclude unsupported/fallback runs from performance claims
and mark the associated functional requirement incomplete. Object-lifetime
tests and diagnostic fence-load counts are not benchmark evidence.
