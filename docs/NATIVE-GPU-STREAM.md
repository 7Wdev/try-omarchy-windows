# Continuous native Windows shader reference

`native-stream-probe` provides a Windows NVIDIA control experiment for the
bridge's graphics work. It uses the same checked-in triangle shaders, root
constants, RGBA8 format and 3840×2160 viewport as the guest diagnostic. It draws
directly into native DXGI backbuffers. QEMU and the guest driver bridge are not
in this path, so its result cannot establish guest acceleration or the requested
93% native performance ratio.

Two source-retained runs verify 1,200 and 360 measured frames after 60 warm-up
frames each. All 1,262 and 422 GPU submissions retire normally. Two additional
full-image audits per run pass the independent integer pixel oracle, with exact
coverage/background/alpha and maximum color error of one step. This checks the
audit frames; it does not independently read every frame in the timed loop.

| Run | Measured frames | Clear/draw GPU median | GPU p95 | Paced wall time |
| --- | ---: | ---: | ---: | ---: |
| Source-retained run 3 | 1,200 | 0.038896 ms | 0.057504 ms | 5.004390 s |
| Final source run 4 | 360 | 0.038688 ms | 0.045088 ms | 1.499874 s |

These are small shader-work measurements on an RTX 5090 Laptop GPU, Windows
driver 32.0.16.1742. They do not measure a complete application's rendering cost,
uncapped throughput, bridge transport or display latency. Wall time includes
vertical synchronization, frame pacing and diagnostic instrumentation. The
simple shader and short runs do not establish thermal or long-term stability.

## Ownership and timing

The two backbuffers each have an owned command allocator and a retirement value.
An allocator is reset only after its previous GPU work retires. DXGI's waitable
frame-latency object paces each frame, including the first, with maximum latency
one. Waits pump the owned window's messages and have bounded deadlines. Closing
the window cancels the run and drains submitted GPU work before resource release.
Unrecoverable retirement failures retain resources until process teardown.

The run budget is 60–3,600 measured frames plus 60 warm-up frames and two audits.
Timestamp-query storage and frame records are bounded by this budget. GPU
timestamps surround the clear and draw commands; timestamp resolution, barriers,
readback and presentation are outside that interval. Pixel copies, the independent
CPU oracle and capture file writes occur outside the timed loop. No per-frame
image transfer or visible hold is used in that loop.

Microsoft documents the [frame-latency wait handle](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)
and [queue timestamp frequency and queries](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing).
The measured clear/draw interval is only one component of a future matched
native/guest benchmark. Shared consumption, transport, API workloads and desktop
presentation need their own measurements and correctness gates.

## Display remains unverified

Both native runs return successful `Present` calls and sequential last-present
counts. Their frame statistics nevertheless stop at count 63. The point check at
the window center never resolves to the owned root window, even with the explicit
`--topmost` option. The final run confirms an interactive window station and
matching thread/input desktop names. These observations do not identify the root
cause or prove that every frame reached the display.

The native control shows that stale statistics are not unique to the guest
bridge. It does not exonerate all bridge behavior or fix the guest presentation
failure. Microsoft also documents [limitations of frame statistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics).
Keep the existing strict guest acceptance guard. Establish unobscured output and
correlated display evidence before claiming reliable delivery.

The first private reference trial retired 420 submissions, then rejected its
audit copy because the new probe omitted the 512-byte footprint base offset from
its readback allocation. That probe bug was fixed before subsequent runs. The
failure and original-file hashes remain in the [evidence](evidence/NATIVE-STREAM-2026-10-11.json).
Run 2 predates the visibility records and retained source snapshot; its successful
pixels and submission result are investigative evidence, not a final-verifier pass.

## Reproduce the 4K control

Build with Visual Studio C++ tools and the Windows SDK:

```powershell
experimental/windows-native-gpu/build.ps1 -HighResolutionDiagnostic
```

Run on an interactive NVIDIA Windows machine. Use a fresh, absolute capture
directory for each attempt; existing capture files are rejected without overwrite.
`--topmost` keeps only this short-lived test window topmost without activating it.

```powershell
$runDirectory = Join-Path $PWD ('native-stream-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $runDirectory | Out-Null
$env:WDDM_BRIDGE_CAPTURE_DIRECTORY = $runDirectory
& experimental/windows-native-gpu/build-4k/native-stream-probe.exe --frames 1200 --topmost `
  1> (Join-Path $runDirectory 'stream.jsonl') 2> (Join-Path $runDirectory 'host.jsonl')
Set-Content (Join-Path $runDirectory 'exit.txt') $LASTEXITCODE
python experimental/windows-native-gpu/verify_native_stream.py `
  --directory $runDirectory --oracle experimental/windows-native-gpu/build-4k/rgba-oracle.exe `
  --output (Join-Path $runDirectory 'verified.json')
```

The verifier recomputes timing summaries from every GPU timestamp pair, checks
frame/presentation sequence and normal cleanup, runs the independent rasterizer
on the actual audit files, and matches native capture hashes. Missing, malformed,
duplicated or inconsistent evidence fails. Its separate result flags explicitly
leave guest bridge, complete display delivery and native performance ratio false.
The small standard build compiles the probe too; this verifier deliberately
accepts only the documented 4K reference profile.

## Next bridge work

The guest bridge still has lifetime limits of 16 submissions/signals and two
shared consumes. A sustained guest path must distinguish a bounded number of
outstanding commands from a separately bounded total run budget, reuse only
retired command storage and retain generation/ownership checks. Raising lifetime
caps alone does not provide backpressure or safe reuse. Correctness readbacks and
presentation statistics must not become dependencies in the production frame loop.

The full [product requirements](PRODUCT-ACCEPTANCE.md), including Omarchy/Hyprland,
CUDA, OpenGL, Vulkan, video, recovery and at least 93% native performance, remain open.
