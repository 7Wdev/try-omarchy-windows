# Preserve the broker connection after capacity refusal

Updated **2026-10-23**. A valid resource-resolution request returning `ENOMEM`
previously disconnected both broker endpoints. A real Unix IPC control
reproduces that failure. The corrected endpoints preserve the authenticated
connection and caller-owned FD after `ENOMEM` / `ENOSPC`, then successfully retry
on the same connection. **The intermittent GPU reference-limit cause and visual
recovery after a real GPU admission failure remain unverified.**

## Correction

`ResolveObject` now treats capacity refusal as recoverable, consistently with
the existing allocation operations. Malformed requests, protocol errors,
unexpected native driver failures and transport timeouts remain fatal.

The coordinator formerly translated every negative native alias result into
`ENOMEM`. It now reserves that classification for `STATUS_NO_MEMORY`
(`0xc0000017`) and `STATUS_INSUFFICIENT_RESOURCES` (`0xc000009a`). Other native
failures become fatal `EIO`; the receipt retains the exact native status.
The status constants were checked against the installed Windows SDK header.

The correction changes no quota, wire layout, kernel module, rendering action,
Windows/QEMU input code or launcher reserved-key route. It releases no live
capability, bypasses no ownership check and fabricates no completion fence.

## What the controls prove

Strict default and 4K builds execute real Unix credentials, SCM_RIGHTS
descriptor passing and pidfd process-lifetime controls with unprivileged
clients. Native GPU operations and kernel object ioctls are **mocked** here.

- Before correction, the first valid `ENOMEM` reply stops the connection;
  the follow-up operation fails with `ESHUTDOWN`.
- After correction, `ENOMEM` and `ENOSPC` preserve the sender's FD and allow
  object-info, resolution retry and a subsequent driver call on the same
  authenticated connection. Ownership drains only after process exit.
- `EIO`, timeout and a malformed resolution lacking its required descriptor
  still stop the channel. Existing foreign-ticket, forged-cookie, read-only
  mapping, inherited-FD and disconnected-reply controls pass.
- Coordinator controls in default and CPU384 diagnostic builds refuse aliases
  before publication, retain the live source FD, then retry without a stale
  cache entry or premature native release.

The controls replay their original failed output and corrected results.
Atomic test receipt writes prevent concurrent child diagnostics from splitting
the evidence lines; earlier interleaved outputs are retained.

## Real GPU compatibility

Both endpoints were rebuilt into private **guest130**. Only the broker and
Linux ioctl shim payloads change from guest129. The same **Native86** binary,
profile 14/v9, kernel, CPU384 guard, 1 GiB GPU usage guard, per-actor
16-reference limit, two-window actions and input workload remain fixed.

**Run 150** independently passes **1,800 GPU handoffs over 32.17 seconds**,
complete first/final 3840×2160 source and 2560×1566 SDL pixels, four focus
checkpoints, independent application counters, input response, resizing,
live child and terminal retirement, and native cleanup before natural VM exit.
Maximum pixel channel error is **1**, within the unchanged tolerance of **2**.
It completes **5,954 native submissions** and leaves no pending work.

The actual renderer is NVIDIA GeForce RTX 5090 Laptop GPU through D3D12.
Median instrumented CPU handoff interval is **16.76 ms**, p95 **24.49 ms**.
These intervals do not measure displayed FPS, physical input latency, GPU
execution time or matched-native performance.

Run 150 does **not** encounter native alias refusal. It proves normal GPU
compatibility after the correction, not GPU-level refusal recovery. The
original failed run 144 remains rejected, and the reference-limit cause remains
open. The [published two-window frame](LIVE-MULTIAPP-CPU384.md) remains the
current PR image. Product acceptance stays **0/4**, backend verification **4/8**.

## Reproducible evidence

[Lossless control output, exact compile sources and reconstructable run-150 pixels](evidence/OMARCHY-ALIAS-CAPACITY-RECOVERY-2026-10-23.json)
are checked by the [separate transport/GPU auditor](../experimental/windows-native-gpu/verify_alias_capacity_recovery.py).
The frozen CPU384 auditor retains all pixel, fence, lifetime, input and budget
checks; only the exact candidate guest/manifest pins differ. Historical
auditors and previous evidence are unchanged.

[Local validation output](evidence/ALIAS-CAPACITY-RECOVERY-VALIDATION-2026-10-23.txt)
retains the failing-before control, passing default/4K authenticated RPC and
coordinator/routing controls, strict broker/shim compilation, and new archive
replays with evidence-corruption checks. The preceding CPU384 checkpoint
passed all **521** milestone checks and both native CI workflows.

Next: reproduce or inject a clearly labelled refusal during real GPU work,
verify compositor/output retirement and recovery, and determine whether the
16 references are legitimate concurrent ownership or delayed reaping. Normal
guest device/account/launcher startup, official physical input and scanout,
continuous stability, broad APIs and ≥93% matched-native performance remain
required. All changes stay in **PR #2**.
