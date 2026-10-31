# GPU checks without using the Windows desktop

A paired NVIDIA GPU clear/readback check now runs through QEMU with
`-display none`. It requests no window presentation, keyboard or mouse route,
source disk or desktop session. The render client uses UID 65534 and the
authenticated broker/kernel-aperture path, with raw sysfs fallback disabled.
The existing 4K diagnostic capacity profile provides 1 GiB of guest RAM; this
particular test surface is **130×73**, not a 4K desktop frame.

Both the original 10,000-read shim and the sampled two-read shim verify all
**9,490 pixels in each of two GPU readbacks**, including their independent
expected hashes and actual fence values. Each run completes seven native GPU
submissions and 264 paging-retirement checks. Both the host and VM exit normally,
with all tracked native objects and aperture mappings retired before VM exit.
The driver, QEMU, runtime and source guards remain checked.

Both runs emit 437 mapped-memory diagnostic records. They describe **4,370,000
reads** in the original selection and **874 reads** in the sampled selection.
The private images differ only in the shim; the shader/runtime libraries,
probe, init, broker and kernel module are identical. These are actual diagnostic
counts, not a performance ratio. No GPU timing or matched-native benchmark is
claimed. This fixed-function clear also does not establish shader drawing or
daily desktop compatibility.

[Structured paired evidence](evidence/BACKGROUND-GPU-CLEAR-2026-10-27.json) binds
the original artifacts. The [file-only auditor](../experimental/windows-native-gpu/background_clear_pair_audit.py)
can reproduce the scoped result from the private artifact directories; it opens
no VM or window. Proprietary libraries and private images remain excluded.

## Retained failures and oracle scope

The first attempt selects the desktop-profile executable without its required
permissions and is refused before VM creation. The standard-build attempt then
fails to unpack the current runtime image in its 512 MiB guest, before GPU work.
Both attempts remain recorded. Using the existing 1 GiB profile permits the
paired check to complete without requesting presentation.

The original whole-run reports for runs 3/4 remain **rejected**. Their generic
verifier includes the older direct-port label, 10,000-read parsing and capacity
assumptions. The new scoped auditor independently requires the complete broker
isolation/teardown witnesses, matched native retirement, whole clear readbacks,
exact selected read count and live cleanup. It preserves the rejected reports.
Its first revision also rejects two valid zero-target already-resident paging
checks; the final audit accepts those under the existing fence contract while
continuing to reject stale values and the device-loss sentinel. Raw logs are
unchanged. The initial metadata's profile/geometry mix-up is retained separately;
the corrected receipt explicitly identifies the 130×73 surface.

The explicit `broker_runtime_qemu.py` adapter recognizes a complete single-client
broker route as well as the older direct route. The historical runtime harness
remains byte-identical to its recorded source, preserving the earlier milestone
replays. The adapter verifies that source digest and changes the initialization
predicate in a separate module instance. Missing, duplicate, reordered, privileged,
leaking, failed or mixed-route witnesses are rejected. Existing device/queue
result and owned-cleanup requirements remain. Five broker evidence tests
and all 23 existing runtime milestone tests pass. This helper update does not
turn the original whole-run reports into passing desktop evidence.

Further GPU compute/shader checks can use this background route. Interactive
input, scanout, full desktop sessions, pressure recovery and the ≥93% native
performance target still require their own evidence. Backend/product checklist
counts remain **4/8** and **0/4**.
