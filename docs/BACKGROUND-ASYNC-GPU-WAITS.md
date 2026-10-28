# GPU fence notifications — October 28, 2026

The opt-in Linux eventfd bridge now passes six real NVIDIA GPU wait controls.
It also cancels a pending wait on process exit. **Live native crash recovery
still fails in the process-exit profile:** native objects and mappings remain
until QEMU exits. This is synchronization progress toward the windowed Omarchy
backend; daily-use acceptance remains **0/4** and backend verification **4/8**.

The tests ran through QEMU/WHPX, the genuine NVIDIA Linux UMD and the Windows
NVIDIA driver on the RTX 5090 Laptop GPU, driver 621.01 / 32.0.16.2101. No desktop
window, host input, source disk or presentation route was requested. These runs
execute GPU queue signals and initialization work, with no shader workload.

## Normal lifetime: six controls pass

| Control | Actual observation |
| --- | --- |
| Pending wait | Registration returns before its dependency; no event arrives until a real GPU queue signal. |
| ALL | Signaling the first of two fences does not notify; signaling the second does. |
| ANY | Signaling one fence notifies while the other GPU fence remains zero. |
| Descriptor reuse | Reusing the numeric descriptor does not redirect notification from the original eventfd context. |
| Capacity | 64 pending registrations succeed; the 65th is refused; all accepted waits retire after the GPU signal. |
| Full eventfd counter | A saturated Linux notification counter does not block the kernel worker; GPU completion produces the documented overflow observation. |

Normal run 1 records **69 created, 69 notified, 69 released, zero errors and
zero held/pending waits**. Seven native context GPU signals retire at targets
1, 3, 7, 9, 11, 29 and 43. Their native receipts explicitly report no CPU write
to a GPU fence. Two additional command submissions are runtime initialization,
not shader acceptance. These are context signals; hardware-queue signal and
asynchronous context-signal paths are not exercised here.

All native mappings retire before VM exit: 67 fence mappings, 46 allocation
mappings and 53 allocation-slot mappings have matching unmap acknowledgements.
The existing full cleanup oracle passes. No native device-health queries occur
in this workload, so it adds no device-loss or sustained-health coverage.

## Process exit: kernel cancellation passes; live native recovery does not

Run 2 registers an unsignaled future target of 999 and exits without signaling
it. The kernel records **one created, zero notified, one cancellation/error,
one released and zero held/pending waits**. Cancellation is not reported as
successful GPU retirement.

The broker still reports **104 retained views**, with zero normal view-release
receipts. Kernel leases are revoked, but native mappings are safely retained
until VM exit: **60 fence, 44 allocation and 51 allocation-slot mappings**.
Four hardware queues, 67 synchronization objects, six GPU reservations and
44 CPU locks also retire after VM exit. Eventual cleanup and natural host/VM
exit pass. The existing broker-teardown and before-VM mapping-retirement
oracles continue rejecting this run. They have not been relaxed.

This failure must be resolved through verified actor retirement and revocation
acknowledgements. Releasing native CPU aliases early would bypass the safety
protocol. This single-client lab profile does not establish desktop application
crash recovery.

## Implementation and limits

`WDDM_BRIDGE_ASYNC_CPU_WAITS=1` enables the guest shim path; the kernel also
requires `enable_native_sync_files=1`. Both are explicit experimental opt-ins.
Default callers retain the previous asynchronous-wait rejection policy.
The new lease ioctl validates descriptor type, owner, range, flags and all
points before publishing work. It retains the actual eventfd context and reads
owned GPU fence pages without CPU signaling them. A legal future target does
not acquire an artificial completion timeout.

Resources are bounded to 64 registrations and 64 points per registration, also
subject to the shared 256-view native-sync limit. Pending waits poll at a
minimum one-jiffy interval on the existing workqueue. This is a bring-up
implementation, with no interrupt-efficiency or near-native performance claim.
Closing only an eventfd does not cancel its retained context; target retirement
or lease revocation is needed. Device-loss propagation remains unverified.

Both runs also pass **31 kernel contract checks** covering malformed requests,
wrong descriptors, foreign owners, writable allocation leases, revoked
authority and partial-registration rollback. Those controls publish no wait,
notify no event and perform no GPU MMIO.

## Evidence

[Sanitized run evidence](evidence/BACKGROUND-ASYNC-GPU-WAITS-2026-10-28.json)
binds the original private reports, build receipts, source and component hashes.
The [file-only verifier](../experimental/windows-native-gpu/verify_background_async_waits.py)
checks both full normal cleanup and the retained exit failure, including raw
native context-signal receipts. It never launches a VM. Build receipt digests
retain the original raw source bytes; comparison with public text checkouts
permits Git line-ending normalization only.

The generic initialization harness still rejects both runs. Its ordinary
initialization workload does not cover these controls; separate scoped evidence
does not turn that verdict into whole-run or desktop acceptance. The historical
harness and original cleanup verifiers remain unchanged.

The guest module, both shim profiles, parent init and both GPU probes compiled
with warnings treated as errors. The default transport fault suite passed
217 tests. Portable evidence tests additionally reject missing/duplicated cases,
invented GPU retirement, CPU-written fences, and erased exit-retention counters.
Private NVIDIA libraries, VM images and raw native streams are not published.

Next API work includes actual Vulkan shader execution with an independent
result oracle and GPU-backed window-buffer integration. The normal launcher,
official physical input, longer browsing/multiwindow sessions, recovery and
matched-native performance still need product acceptance.
