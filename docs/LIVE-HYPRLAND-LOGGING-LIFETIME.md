# Hyprland shutdown ownership

Updated 2026-10-15. **The bounded actual-compositor diagnostic now exits cleanly
in three fresh QEMU/NVIDIA runs. Compositor pixels and Windows presentation
remain unverified.** Progress remains 4/8 verification stages and 0/4 desktop
acceptance gates. This is a lifecycle fix within the existing integration work.

The earlier [compositor-buffer checkpoint](LIVE-HYPRLAND-COMPOSITOR-BUFFERS.md)
returned 0 from Hyprland's main function, then segfaulted during process
finalization. Two additional failed controls preserve that failure. A private
fault trace locates the instruction in Mesa's `d3d12_bo_unreference`: releasing
a resource reads a vtable in the already-unmapped `libd3d12core.so`. Stack
candidates and the observed `dlclose` order identify Aquamarine output-buffer
destruction after the D3D12 loader's finalizer.

Aquamarine 0.15.1 stores a global strong `backendInUse` reference for callbacks
from libseat and libinput. `CSession::attempt` sets it before opening a seat;
even an unsuccessful seat attempt retains the entire backend. This prevents
Hyprland's normal backend reset from releasing its output buffers and allocator.
The additive [logging ownership patch](../experimental/windows-native-gpu/aquamarine-0.15.1-logging-lifetime.patch)
uses a weak reference and locks a temporary strong reference during each logging
callback. The callbacks no longer extend backend lifetime to library finalization.
No D3D12 module is pinned, no resource release is skipped, and no signal is hidden.

## Actual controls and repeats

The immutable [development evidence](evidence/HYPRLAND-LOGGING-LIFETIME-DEVELOPMENT-2026-10-15.json)
retains failed runs 13–14 and successful lifecycle runs 15–17, including raw
receipt excerpts, capture and input digests, selected native/VM records and
fault module offsets. Private runtime binaries and guest images remain private.
These development runs use the 356a6f1 base with the recorded working-tree changes;
they are not clean-revision desktop acceptance.

| Runs | Result |
| --- | --- |
| 13–14, previous library | Seven owned buffer commits and 28 retired native commands, followed by finalizer SIGSEGV. Native/VM cleanup succeeds after guest termination. Lifecycle rejected. |
| 15, fixed library with fault tracing | Seven commits, 29 retired native commands and process exit 0. All mappings and broker views explicitly release before VM exit. No finalizer fault occurs. |
| 16–17, fixed library and published probe | Two fresh untraced guests reproduce clean main/process/broker/native/VM exit. Each explicitly releases all 142 fence mappings, 178 allocation mappings, 204 virtual allocation slots, 320 broker views, 74 native buffer references and seven completed sync-files. No forced VM stop or deferred VM-exit reclaim occurs. |

The [actual-library ownership probe](../experimental/windows-native-gpu/aquamarine_logging_lifetime_probe.cpp)
forces an invalid libseat backend without opening a seat or GPU. Logging occurs
while the backend is alive. After a failed session attempt and removal of the
application's final reference, the previous library retains the backend and the
probe exits 4; the fixed library releases it and exits 0. Both tests use the
same executable and retained runtime closure. Source application, idempotence
and rejection of modified source also pass against the official pinned archive.

## Reproduce the source and audit

Apply both additive patches to official Aquamarine 0.15.1 commit
`f31c47a1b9d300847d8fc3108ad959448103dfc1`:

```sh
python3 experimental/windows-native-gpu/apply_aquamarine_native_render_patch.py SOURCE
python3 experimental/windows-native-gpu/apply_aquamarine_native_render_patch.py SOURCE --patch logging-lifetime
```

The helper verifies exact before/after file hashes and patch digests. The
original native-render patch and its lock are unchanged. The
[new lock](../experimental/windows-native-gpu/aquamarine-logging-lifetime.lock.json)
pins `Session.cpp` separately. Both patches retain Aquamarine's BSD license.

Build the ownership probe against the matching patched library and headers:

```sh
c++ -std=c++23 -O2 -Wall -Wextra -Werror \
  experimental/windows-native-gpu/aquamarine_logging_lifetime_probe.cpp \
  $(pkg-config --cflags --libs aquamarine) -o aq-logging-lifetime-probe
./aq-logging-lifetime-probe
```

The normal OpenGL-workload verifier still rejects these compositor diagnostics:
their workload and evidence differ from its two-frame shader test. Its exit 1
is retained rather than changed to success. The separate
[lifecycle auditor](../experimental/windows-native-gpu/verify_hyprland_lifecycle_capture.py)
checks the actual unprivileged compositor sequence, owned buffer metadata,
native GPU work, sync-file retirement and explicit teardown. Its output always
leaves pixels, Windows presentation, desktop and performance unverified.
Ten audit regressions include both retained failures, all three clean runs,
truncation, duplicate commits, missing unmaps, deferred reclaim, GPU failures,
live native objects and forced VM termination.

```sh
python3 experimental/windows-native-gpu/verify_hyprland_lifecycle_capture.py \
  --report CAPTURE/driver.json --output CAPTURE/lifecycle.json
```

## Remaining first-frame work

The three-buffer output is still 640×360. Buffer commits prove ownership and
lifecycle, not the contents of the image. Next are actual compositor pixel
capture and GPU-backed Windows presentation. A sustained resource/queue profile,
input, real applications, 4K desktop operation, recovery, CUDA/Vulkan/video and
the ≥93% matched-native target require their own implementation and acceptance.
The first independently verified Hyprland frame displayed through the Windows
backend will be captured and added to PR #2 immediately.
