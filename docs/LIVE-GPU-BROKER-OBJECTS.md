# Native GPU capability FDs through the guest broker

On 2026-10-13, the opt-in broker passed real NVIDIA texture and timeline-FD
workloads from UID/GID 65534, with zero effective capabilities and no access
to the privileged controller or native transport. Two further guests proved
that a real exported resource reference survives its producer process while
a separate ordinary process holds its kernel capability FD.

This advances guest graphics integration. **No Hyprland session has rendered
through this backend.** The first verified compositor session will be captured,
shown to the user and added immediately to [PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2).
The roadmap remains **4/8 verification stages and 0/4 desktop gates**.

## Actual hardware results

The [immutable evidence](evidence/BROKER-OBJECTS-2026-10-13.json) contains the
guest logs, native receipts, reports, independent audits, source pins and private
artifact hashes. Proprietary runtime binaries and private initramfs files are
excluded. The hardware is the same RTX 5090 Laptop GPU and installed NVIDIA
runtime as the [earlier broker rendering proof](LIVE-GPU-BROKER-ROUTING.md).

| Fresh guest | Verified workload | Native commands | Final cleanup |
| --- | --- | --- | --- |
| run-8 | Ordinary-user texture FD export/import; all 8,294,400 pixels checked in each red/green round | 24 | Guest, broker, native bridge and QEMU exit 0 |
| run-11, run-12 | Same full 4K storage checks, plus a second resource FD held beyond producer exit | 24 each | Last recipient close triggers one native reference close; all exits 0 |
| run-13, run-14 | Genuine Mesa timeline FD imports, two GL GPU signals, and three future GPU waits released by a second queue | 19 each | All native work retires; all exits 0 |

Every successful guest also passes the independent pixel oracle for both
130×73 OpenGL shader frames, 581 kernel memory-lease checks, 561 object checks
and 15 resource-FD checks. None requires a forced VM stop. The 4K workload uses
one 33,177,600-byte readback buffer per round; its driver-padded mapping is
33,226,752 bytes. The physical CPU mapping limit remains 96 MiB, with 256 virtual
aperture slots. Readback is a diagnostic, not a proposed desktop display path.

The recipient does **no GPU work**. Its pidfd observes producer exit, then it
holds the capability for approximately two seconds before closing it. The
broker's retained-reference drain starts before that hold receipt and closes
the native reference only after the recipient begins its last FD close. This
proves FD retention and native reference cleanup, not rendering or importing
from a second GPU client.

## Ownership implementation

Broker protocol 3 separates CPU memory leases from retained native NT objects.
The broker registers tickets only from successful native export replies. It
publishes a typed kernel capability with a unique cookie, then validates the
actual FD with the kernel on every native metadata query or import. A successful
earlier resolve or a client-supplied integer cannot authorize a later import.
The received FD stays owned through the native transaction.

While an FD is live, an ordinary client cannot close its backing native
reference. A last-FD-close kernel receipt makes that reference eligible for
exactly one native close. An unpublished export can be rolled back without
leaking its native reference. Wrong-type/foreign-FD resolution errors leave the
connection usable; malformed protocol and unauthorized native operations stop
the channel.

The broker checks producer pidfd exit even if a descendant inherits the socket.
It closes the memory controller at producer exit, while retaining the separate
object controller and native transport until recipient FDs close. Each diagnostic
session still has a single GPU client and native Windows worker. Store limits
remain 16 references per type and 32 in aggregate.

## Retained failures and regression checks

- run-7 exported and imported the real texture, then rejected its 33 MB CPU lock:
  the broker had been compiled with the default 4 MiB mapping profile. The build
  recipe now selects the same explicit 4K profile as the resource diagnostic's
  shim and native bridge. Tests exercise both profiles; the default remains
  bounded to 4 MiB per mapping. The failed guest exits 125; native/VM cleanup
  succeeds without a forced stop.
- run-9 passed storage checks but its five-second recipient deadline expired
  before producer teardown. It supplies no post-exit retention proof and is
  rejected by the lifetime auditor, despite its top-level guest exit 0. The
  bounded deadline is now 20 seconds and emits a failure receipt on timeout.
  Both fresh repeats pass the required retention/close ordering.

The portable tests exercise forged tickets, foreign and wrong-type FDs,
cookie substitution, duplicate publication, unauthorized native close, real
SCM_RIGHTS and inherited-socket lifetime. Native operations and kernel ioctls
in those portable tests are explicitly mocked. The real GPU captures are
audited separately, and corruption tests reject missing/repeated receipts,
early closes, wrong pixels, changed counts and false scope claims.

Re-audit a retained private capture with:

```sh
python3 experimental/windows-native-gpu/verify_broker_objects_capture.py --report RUN/driver.json --output RUN/audit.json
python3 experimental/windows-native-gpu/verify_broker_objects_capture.py --recipient --report RECIPIENT_RUN/driver.json --output RECIPIENT_RUN/audit.json
python3 experimental/windows-native-gpu/verify_broker_objects_capture.py --fences --report FENCE_RUN/driver.json --output FENCE_RUN/audit.json
```

These use explicit broker, protocol and workload selections. Existing direct
transport and protocol-2 evidence retain their strict defaults. The legacy
runtime wrapper is not an acceptance auditor for these new scopes; its exit 1
is retained separately from the successful guest/native/VM results.

## Remaining desktop work

Independent GPU clients still need separate Windows process contexts and a
coordinator that securely transfers native resource/fence references between
them. These FDs are not dma-bufs, DRM PRIME objects or Linux sync_files. DRM/GBM,
compositor-compatible buffers, presentation and Hyprland integration remain
open, followed by interactive and long-run tests up to 4K. Vulkan, CUDA, video,
concurrent Windows/guest use and ≥93% matched native performance are unverified.
The diagnostic quotas and deadlines do not establish a sustained production
runtime or a hard whole-process GPU-memory limit.
