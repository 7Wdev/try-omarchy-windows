# Native GPU waits through Linux sync-file import

Two fresh ordinary-user QEMU/WHPX guests passed native sync-file import and GPU
ordering on 2026-10-13. A pending Linux FD resolves to an authenticated native
fence and its full 64-bit target, **4,294,967,367**. A real GPU wait gates dependent
work until an independent GPU queue releases it. A completed sync-file still
imports after the original source, native FD and first imported alias are gone.

This advances the fence path needed by Mesa. **EGL native-fence integration,
DRM/GBM buffers and accelerated Hyprland remain unverified.** The roadmap stays
4/8 verification stages and 0/4 desktop gates. The first verified backend-rendered
Hyprland session will be captured, shown and added to
[PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2) immediately.

## Hardware evidence

[Two full captures, disabled control and source/artifact pins](evidence/NATIVE-SYNC-LINEAGE-2026-10-13.json)
retain the actual results. Both render clients run as UID/GID 65534 with zero
capabilities and `no_new_privs`; controller/native-port access and direct kernel
create/resolve calls are denied. The root broker authenticates the actual native
capability FD and binds it to the exact CPU fence word authorized by the native
reply. No raw Windows HANDLE or user-supplied native ticket replaces that check.

Each run verifies:

- A valid zero-target, already-completed Linux sync-file and CLOEXEC descriptors.
- Pending native import, a real GPU queue wait, and an acknowledgement that
  remains zero before independent GPU release and becomes one afterward.
- Completed import after the original source, exported FD and first alias are
  released; the sync-file retains the genuine native reference.
- Foreign FD rejection, canceled-source status `-ENODEV`, and rejected import
  of that canceled sync-file. No CPU write substitutes for GPU completion.
- Three created/released sync-files, two completions, one expected error, and
  zero held/pending objects; all native/kernel/VM cleanup completes normally.

The runtime emits three native queue wait calls for the SDK's single logical
wait. All three dependencies complete. Native records separately verify the
dependent hardware-queue signal was pending and the independent context GPU
signal reaches the 64-bit target with `cpuValueWrittenByBridge=false`.
Both 130×73 OpenGL frames pass independent pixel audits, all 19 native commands
retire, and all 189 CPU leases balance: 93 explicit revocations and 96 retained
until producer exit. Native/guest/QEMU exits are zero without forced shutdown.

The guest also runs kernel isolation/rollback tests: the existing 583-check
memory profile, object/resource FD contracts, and the new lineage contract
(34 checks in these captures). The disabled control rejects lineage with
`EOPNOTSUPP`, creates no sync-files, and exits the whole probe with failure as
expected. A GUID-link build failure is retained separately and ran no GPU work.
The older general runtime wrapper still returns 1 for this additional workload;
the dedicated auditor verifies the stated scope and does not rewrite that result.

## Implementation and remaining limits

Additive guest ioctls create a linked sync-file and resolve its native lineage.
The kernel retains both the source CPU lease and genuine `WDDM_OBJECT_SYNC` file.
Resolution requires the original native-object controller owner and returns a
new CLOEXEC capability FD plus target. Foreign/composite fences, unlinked
exports and errored fences fail. A pending revoked lease is rejected even before
the polling worker has recorded its error; a successful completed fence can
outlive its CPU lease. Copyout failure releases retained references before a
poll is queued. RCU/workqueue teardown preserves module/file lifetimes.

Broker protocol 3 appends operations 11/12. Existing operations and default
behavior are preserved. The bridge exposes opt-in C hooks
`wddm_bridge_export_native_sync_file` and `wddm_bridge_resolve_sync_file`; callers
own returned descriptors. Kernel controllers remain private to the broker.

This supports our single native fence lineage within one broker session.
Foreign/merged Linux sync-files, independent GPU clients and general EGL
conformance are not supported by this result. Cancellation after a GPU wait has
already been submitted needs recovery integration; import-time rejection alone
does not cancel queued Windows GPU work. Completion still uses the diagnostic
one-millisecond/jiffy poll, five-second deadline and 256 held-fence limit.
Sustained/event-driven synchronization, stress, batching and performance need
further work before claiming a production compositor path.

The next integration is Mesa's actual `PIPE_FD_TYPE_NATIVE_SYNC` import/export
and EGL callbacks. Native D3D12 timeline FDs and texture capabilities are not
Linux dma-bufs. Aquamarine/Hyprland still need a validated graphics device,
allocator/buffer metadata, compatible EGL imports and continuous Windows display.
Vulkan, CUDA, video, NVIDIA features, concurrent workloads and ≥93% matched
native performance remain separate unfinished goals.

## Reproduction

Use the existing pinned Mesa/NVIDIA guest components and matching guest kernel
headers. Build the probe with `WDDM_BRIDGE_TEST_NATIVE_SYNC_LINEAGE=1` and
`WDDM_BRIDGE_TEST_BROKER_ROUTE=1`, supplying the Mesa, DirectX-Headers and libdxg
source paths. Package the kernel module/contract, broker and explicit
`--guest-native-sync-files` option. Default images keep the module feature off.
Private runtime images/binaries stay local; no guest module is loaded on the host.

After an owned disk-free hardware run, audit the retained logs:

```sh
python3 experimental/windows-native-gpu/verify_native_sync_lineage_capture.py \
  --report RUN/driver.json --output RUN/audit.json
```

Seven evidence-test groups corrupt real captures to reject missing/reordered
lifecycle, CPU-written/ungated signals, premature native close, invalid privilege
or cleanup receipts and bad shader pixels. CI compiles the probe/module and
audits retained captures without running vendor binaries on its runners.
