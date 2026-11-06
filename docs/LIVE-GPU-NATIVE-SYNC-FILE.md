# Native GPU completion through Linux sync-file FDs

Follow-up: [owned sync-file native import and GPU waits](LIVE-GPU-NATIVE-SYNC-LINEAGE.md)
now pass two fresh guests. This page retains the earlier export milestone and
its evidence; EGL and compositor integration remain open.

On 2026-10-13, two fresh QEMU/WHPX guests passed GPU-driven Linux sync-file
export and transfer as UID/GID 65534. An ordinary recipient sees the FD pending
before an NVIDIA GPU queue signal, then sees it complete at **4,294,967,354**.
Both guests independently verify their OpenGL shader pixels, retire all 18
native commands and the separate GPU signal, release every sync-file and native
reference, unload the kernel module and exit normally.

This implements a synchronization building block. EGL native-fence export/import,
DRM/GBM, compositor buffers, continuous display and Hyprland remain open. The
roadmap stays **4/8 verification stages and 0/4 desktop gates**. Capture/show the
first verified backend-rendered Hyprland session and update
[PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2) immediately.

## Actual evidence

[Retained captures and source/artifact pins](evidence/NATIVE-SYNC-FILE-2026-10-13.json)
include two final successes, the disabled-feature control and three earlier
failed probes. Private NVIDIA runtime images and executables stay local.

Each successful guest creates five Linux sync-files: an already-completed value,
a future GPU target transferred with SCM_RIGHTS after the parent closes its FD,
two pending values revoked with their source lease (including an abandoned FD),
and a deadline test. Standard `poll` and `SYNC_IOC_FILE_INFO` verify readiness,
completion and error status. Totals are **two successful signals, three errors,
five releases and zero held/pending objects**. Revocation returns `-ENODEV`;
the diagnostic deadline returns `-ETIMEDOUT`. Native receipts separately prove
the GPU queue signal, retirement and `cpuValueWrittenByBridge=false`.

The root broker authorizes the exact monitored-fence word from a successful
native reply. The client receives a restricted sync-file FD and cannot open
the controller/native port or create a kernel export directly. Its capabilities
remain zero with `no_new_privs`. There is one guest GPU client. All 165 CPU leases
balance: 69 explicit revocations and 96 retained through process exit.

The kernel memory contract records 583 checks: the existing 581 plus two checked
lease revocations in the new test. The independent auditor explicitly selects
that profile; older/default captures still require exactly 581. Object-FD tests
and the new rejection/rollback/direct privilege checks also run in the guest.

## Ownership and limitations

`enable_native_sync_files=1` explicitly enables the guest module feature; default
images leave it disabled. A controller creates a `dma_fence` and `sync_file`
bound to a valid read-only native fence lease, aligned offset and 64-bit target.
The driver checks validity and reads the actual native progress word under the
same lock used for revocation, preventing reads from a recycled native mapping.
The bridge resolves a genuine native capability FD before borrowing its existing
owned CPU lease. A raw D3DKMT fence is not interchangeable with D3D12-created
shared-fence runtime metadata; the retained failed import demonstrates that.

Polling holds its own fence/source reference until completion or error. Closing
a pending FD does not free a live worker. Last-reference teardown waits for an
RCU grace period, then unmaps and releases resources in workqueue context; the
module reference survives that cleanup. The source lease remains revocable:
destroying it cancels pending exports with an error. General independent-client
imports and retention beyond source/process teardown need further integration.

The kernel bounds held sync-files at 256 and polls at a one-millisecond/jiffy
interval with a five-second diagnostic deadline. The hardware runs verify the
bounded lifecycle, not quota saturation, scheduler latency or sustained load.
An interrupt/event-driven completion path and batching need performance testing
before claiming the requested ≥93% native target.

Linux replaces signaled fence names with generic strings in the tested kernel.
The original probe incorrectly rejected those standard completion names. The
corrected probe requires the native exporter identity while pending and verifies
the standard completion/error status after signaling, consistent with the
[kernel implementation](https://github.com/torvalds/linux/blob/v6.18/drivers/dma-buf/dma-fence.c).
The original metadata failures stay failed captures.

## Reproduction

Build the guest module with matching kernel headers and the existing private
Mesa/OpenGL guest components. Set `WDDM_BRIDGE_TEST_NATIVE_SYNC_FILES=1` and
`WDDM_BRIDGE_TEST_BROKER_ROUTE=1` when building the probe; supply the existing
Mesa, DirectX-Headers and libdxg sources. Package with the kernel module/contract,
graphics broker and explicit `--guest-native-sync-files` option. No driver is
installed on Windows or loaded into the WSL host.

Run in the existing owned disk-free QEMU harness with the explicit OpenGL
profile, then independently audit:

```sh
python3 experimental/windows-native-gpu/verify_native_sync_file_capture.py \
  --report RUN/driver.json --output RUN/audit.json
```

The older general wrapper returns 1 for this additional workload; that outcome
is retained. The dedicated auditor verifies only this export/poll/transfer scope.
General sync-file import, EGL native-fence conformance, dma-buf/GBM, Hyprland,
Vulkan/CUDA/video, NVIDIA features, concurrent host/guest workloads, stability
and matched native performance remain unverified.
