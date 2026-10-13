# EGL native-fence callbacks through the Windows NVIDIA bridge

Verified 2026-10-13 in **two fresh, disk-free QEMU/WHPX guests** on the
NVIDIA GeForce RTX 5090 Laptop GPU. This implements and tests Mesa's actual
`PIPE_FD_TYPE_NATIVE_SYNC` callbacks and `EGL_ANDROID_native_fence_sync` for
our owned, single native-fence lineage. It advances guest graphics integration;
it does **not** demonstrate a Hyprland session or complete a desktop gate.

[Immutable captures and source/artifact digests](evidence/EGL-NATIVE-SYNC-2026-10-13.json)
retain both successes, a disabled control, the legacy timeline regression,
three failed hardware attempts, a build failure and a sandbox launch failure.
Private NVIDIA binaries and initramfs images remain local.

## What passed

Both enabled guests run the graphics client as UID/GID 65534, with no effective
capabilities and no access to kernel controllers or the native worker port.
The authenticated protocol-3 broker retains those privileges.

- Actual EGL creation/export, two distinct CLOEXEC descriptor duplicates,
  successful EGL import and completion after destroying the original EGL sync.
- Zero-target import and an initially pending target **4,294,967,387**. EGL
  client wait reports timeout and EGL status remains unsignaled before release.
- `eglWaitSyncKHR` inserts real Windows GPU waits. A subsequently exported EGL
  fence and a separate SDK acknowledgement both remain pending. An independent
  copy queue releases the source; both complete. Native records independently
  show the pending signals, release and retirement, with no bridge CPU writes.
- Completed reimport after releasing the original native source, source FD and
  first imported EGL sync. Failed foreign-FD import preserves caller ownership.
- Source revocation produces standard sync-file status `-ENODEV`. An already
  imported EGL fence stays unsignaled and client wait does not report success;
  a subsequent import fails with `EGL_BAD_ATTRIBUTE`, preserving the input FD.
- Real current-worker memory-budget queries, synchronization-only residency
  signals, and one native Evict/MakeResident API cycle after GPU dependencies
  retire. This does **not** prove forced physical page-out or contents recovery.

In each enabled run, all **20 native command submissions**, eight hardware
queue signals, three context signals and nine GPU wait dependencies retire.
Both 130×73 shader images pass independent reference-pixel audits, with maximum
color error of one. All 193 memory leases revoke explicitly before producer
exit. Ten sync-files, including Mesa's internal batch fences, balance at
teardown: nine successful, one expected revocation error, ten released,
zero held/pending. Native, guest and QEMU exits are zero, without forced stops.

The disabled guest keeps the EGL extension absent and creates zero sync-files;
both shader images still pass. The separately built legacy timeline probe also
passes its import/signal/future-wait and pixel audits. Those default-path tests
retain the existing 96 CPU views until producer exit, with verified kernel/VM
cleanup. Enabled EGL runs release every view explicitly.

The older general initialization wrapper returns 1 for these OpenGL workloads;
its result is retained as `legacyRuntimeWrapperAccepted=false`. The dedicated
auditors verify the scope above without rewriting that wrapper result.

## Implementation and failure fixes

The additive patch targets pinned Mesa 26.0.8 after the existing timeline patch.
`D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE=true` and both bridge hooks are required
before advertising native-fence support. Normal startup keeps this option off.
The full non-GLVND D3D12 Mesa build passes locally; zero-fuzz application,
idempotence and changed-source rejection also pass on a separate pinned tree.

Internal Mesa batch fences must use the same genuine sync-file path. The old
Linux path requests an unsupported asynchronous CPU event. The new path caches
one native capability for Mesa's shared screen fence, owns independent sync-file
descriptors/native references, polls standard sync-file status and treats errors
as failure. A fence exported after a server-side wait emits a fresh queue signal
even when there are no new GL commands, preserving the dependency.

Real batch retirement exposed missing memory-management operations. An optional
wire capability now forwards owned `QueryVideoMemoryInfo` and Evict requests.
Budget reporting is limited to the current worker and a diagnostic 256 MiB
reported budget; **this is not a hard allocation-byte quota**. Eviction validates
owned/deduplicated allocations, requires retired pending GPU work, and protects
the last proven residency reference of a CPU-mapped allocation. Reference
accounting is conservative for partial failures. Synchronization-only contexts
may signal owned residency fences; this does not authorize command submission,
hardware-queue creation or GPU waits from those contexts.

The failed captures remain visible. The first exposed the old async CPU-event
path; the next two stalled on residency handling and failed native cleanup.
Their results are rejected by the new auditor. A corrected unit-test setup
preceded the final native build; the failed build ran no GPU workload.

## Limits and next work

This is an experimental, session-owned native-fence implementation, not general
EGL conformance. Foreign/composite sync-files, independent guest GPU clients,
DRM PRIME/dma-buf formats and modifiers, GBM allocation/import and Hyprland's
graphics device path remain unfinished. Revocation rejection does not implement
cancellation/recovery for an already queued Windows GPU wait.

The diagnostic still uses one-millisecond/jiffy completion polling, a five-second
deadline, bounded requests/commands and a single broker client. Sustained,
event-driven synchronization and production memory-pressure recovery require
further work. These small shader tests do not add a new 4K, display, stability
or performance claim to the separate earlier 4K evidence.

Next: implement and test compositor-compatible buffer ownership/import, then
sustained compositor work and continuous Windows presentation. Capture the first
verified backend-rendered Hyprland session and update PR #2 immediately when it
exists. Vulkan, CUDA, video, NVIDIA features, simultaneous host/guest workloads
and ≥93% matched native performance remain separate acceptance requirements.

## Reproduce the scoped diagnostic

Apply the pinned base and additive patches in a separate Mesa tree:

```sh
python3 experimental/windows-native-gpu/mesa/apply_mesa_fence_fd.py MESA_SOURCE
python3 experimental/windows-native-gpu/mesa/apply_mesa_native_sync.py MESA_SOURCE
```

Build a private D3D12-only Mesa install, then use `build_opengl_probe.sh` with
`WDDM_BRIDGE_TEST_EGL_NATIVE_SYNC=1`, `WDDM_BRIDGE_TEST_BROKER_ROUTE=1` and the
existing Mesa/DirectX-Headers/libdxg source variables. Package the private
runtime, current broker/shim, matching guest module/contracts and explicit
`--guest-native-sync-files` flag. Build the disabled control with
`WDDM_BRIDGE_TEST_EGL_NATIVE_SYNC_DISABLED=1` and omit that module flag.
Use the owned, disk-free QEMU harness and the native high-resolution diagnostic
build; do not load the guest module on the Windows host.

```sh
python3 experimental/windows-native-gpu/verify_egl_native_sync_capture.py \
  --report RUN/driver.json --output RUN/audit.json
# Add --disabled only for the explicitly disabled control.
```

Nine corruption-test groups audit the retained captures and reject missing or
reordered receipts, truncated/CPU-written/ungated signals, premature close,
invalid memory-budget/eviction records, bad privilege/cleanup evidence and bad
pixels. CI compiles the probes and audits evidence without running vendor GPU
binaries on its runners.
