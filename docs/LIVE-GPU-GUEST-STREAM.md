# Repeated 4K guest rendering through QEMU

On 2026-10-11, the separate NVIDIA bridge lab completed 64 consecutive guest
shader frames at 3840×2160. Two final-source runs also copied every frame into
Windows DXGI backbuffers and received/retired all 64 Present calls. The first
and last frames passed independent pixel checks and matched the backbuffers.
The owned host and VM exited normally with no live bridge objects or mappings.

This establishes a bounded repeated-rendering diagnostic. The Omarchy/Hyprland
desktop still cannot use this backend. All-frame display verification, long-run
stability, API compatibility and the ≥93% native-performance target remain open.

## What changed

The original diagnostics retain their 16-submission/16-signal lifetime budgets,
two handoffs, and strict DXGI statistics checks. A separate build profile requires
an explicit `BeginStreaming` exchange (`0x205c`) before the first shared resource.
The response is a fixed 32-byte version-1 profile: 64 frames, at most 16 outstanding
commands and 16 outstanding signals, lifetime limits of 1024 native commands and
256 signals, and 50,000 requests. Earlier initialization attempts remain counted.
Windows checks that startup commands/signals have retired before negotiation.
Malformed, unsupported, repeated and late negotiation is rejected.

`ConsumeStreamingTexture` (`0x205d`) uses the existing typed resource and owned
NoGpuAccess fence descriptor. Frames must arrive exactly in order from 1 to 64.
The host requires the native fence to be retired and all guest submissions/signals
to have drained before touching the texture. Both sides restore `COMMON` before
the guest reuses it. A hash is returned only for audit frames 1 and 64; zero is the
explicit no-readback result for frames 2–63.

The guest reuses one allocator/list only after retirement and host consumption.
Each frame uses one application-level ExecuteCommandLists call, including the
handoff transition. The NVIDIA UMD emits additional native submissions: **199
native commands and 64 hardware-queue signals** retired in each successful run.
Peak outstanding native work was **3 commands and 1 signal**, below the fixed
16/16 caps. The preallocated pending arrays reject overflow before calling KMT.

There is no full-frame CPU upload. Only frames 1 and 64 copy to CPU readback for
audits; the 62 intermediate frames draw and copy on the GPU. Host consumption
and guest reuse are serialized in this baseline. It does not yet provide the
overlapped buffer ring and GPU-side cross-process waits needed for throughput.

## Evidence and limits

[The retained evidence](evidence/GUEST-STREAM-2026-10-11.json) contains source
hashes, binary/image/kernel hashes, cleanup receipts, independently checked
audit hashes, and a real-run receipt fixture for corruption tests. Private NVIDIA
runtime images and vendor binaries remain local.

- The first attempt failed negotiation because D3D12 initialization had already
  submitted one command. It produced zero shader handoffs; host and VM cleanup
  completed. Negotiation now collects and checks retirement without resetting
  counters. That failure remains in the evidence.
- The corrected consumption run and two preliminary DXGI runs completed 64
  handoffs. Two final-source DXGI repeats also passed the independent verifier.
- Each audit checked all 8,294,400 pixels. Maximum channel error was 1, within
  tolerance 2. Audit RGBA SHA-256 values were
  `2b197b65be21948a220fd21ea4d8c1d767fe4f568cc5e832da5acce53ed8cd85`
  and `0a5b6e1ed3c42b5a4ac4d668b4607a7938a2144e7daeb98177fcd79add032abf`.
- Present acceptance and retired GPU work **do not prove every frame reached
  the screen**. DXGI statistics progressed differently between repeats. The
  stream verifier always reports `allFramesDisplayedVerified=false`; the older
  strict presentation verifier is unchanged. No new ETW display pass is claimed.
- CPU mappings, GPU virtual-address mappings, object counts and pending work
  remain bounded. This is **not a hard physical GPU memory quota**. Native
  process GPU usage peaked at 351,068,160 bytes with the consumer and swapchain,
  exceeding the 256 MiB allocation-preflight threshold. That threshold is
  sampled before vendor allocations, not an enforceable whole-process budget.
  Long-run memory behavior and accounting must be resolved before product use.
- No guest/native performance ratio was measured. The native reference in
  [NATIVE-GPU-STREAM.md](NATIVE-GPU-STREAM.md) has a different submission and
  synchronization path, so its timing cannot establish the ≥93% target.

The [existing 4K PNG](images/nvidia-guest-shader-4k-2026-10-11.png) decodes to
exactly the same second-audit RGBA bytes. It is an actual rendered frame.

## Build and verify

Use the separate Windows build:

```powershell
experimental/windows-native-gpu/build.ps1 -StreamingDiagnostic
```

Build and pack a private guest using the documented local runtime prerequisites:

```sh
sh experimental/windows-native-gpu/build_runtime_diagnostic.sh "$HEADERS" "$LIBDXG" "$OUTPUT" stream
python3 experimental/windows-native-gpu/make_runtime_initramfs.py \
  --init "$OUTPUT/runtime-init" --probe "$OUTPUT/runtime-probe" \
  --shim "$OUTPUT/linux-ioctl-bridge.so" --linux-umd "$LOCAL_NVIDIA_UMD" \
  --runtime-workload native-stream --output "$FRESH_PRIVATE_IMAGE"
```

Run the owned-QEMU harness with the same opt-ins used by the native-metadata
diagnostic, `--driver-async-submit`, and `--runtime-workload native-stream`.
Optional `--driver-present-shared` adds the DXGI backbuffer path. Set a fresh
`WDDM_BRIDGE_CAPTURE_DIRECTORY` before starting. The old small-profile verifier
deliberately returns nonzero for this new workload. Its report must then pass:

```powershell
python experimental/windows-native-gpu/verify_guest_stream.py `
  --report "$REPORT" --capture-directory "$CAPTURES" `
  --oracle experimental/windows-native-gpu/build-stream/rgba-oracle.exe `
  --output "$FRESH_VERIFICATION_JSON"
```

CI builds the standard, 4K and stream profiles without vendor binaries or GPU
hardware. It runs transport corruption and receipt rejection tests; hardware
results above were collected separately on the RTX 5090 Laptop GPU.
