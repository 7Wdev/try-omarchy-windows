# Sampling mapped-memory diagnostics

An explicit guest build option, `WDDM_BRIDGE_SAMPLED_DIRECT_READS=1`, reduces each
mapped-fence or allocation-end diagnostic from 10,000 volatile reads to two.
The default and explicit value `0` retain 10,000 reads. Unknown selections are
compile errors. This is a candidate for reducing diagnostic overhead; it has
not received a live GPU/desktop trial or established a performance gain.

The saved run-249 trace contains 6,192 delivered diagnostic records describing
61,920,000 volatile reads: 3,990 paging-retirement checks, 1,421 allocation maps
and 781 fence maps. Applying two reads to that fixed record set would require
12,384 reads. This is a comparison of diagnostic counts, not a timing result
or a prediction of how often a future workload will call the bridge. The full
private trace is hash-bound by the existing [run-249 evidence](evidence/CONTINUOUS-INPUT-FAILURE-2026-10-27.json).

Each sampled allocation probe still touches both the first and last word. Fence
checks still use the latest real mapped value and reject a value below the
target or the device-loss sentinel. The before/after emulation counters,
mapping/lease ownership, alignment, capacity, release and revocation checks are
preserved. The actual fence-wait loop, its ten-second deadline, and GPU/native
submission are unchanged. Counters and receipts report the selected diagnostic
read count. No CPU fence value is synthesized and no GPU wait is skipped.

Background controls compile both selections and use the actual fence/allocation
mapping classes over authenticated Unix sockets with UID 65534 clients and real
memfd mappings. Fourteen scenarios cover valid reads, a future retirement
target, device loss, changed fence counters during mapping or retirement,
changed allocation read/write counters, duplicate/unknown objects and lease
release after rejection. Metadata and native fences are mocks; no GPU or guest
kernel module is involved. The existing ioctl transport fault suite also passes
against the sampled default shim.

```sh
sudo python3 experimental/windows-native-gpu/test_guest_direct_read_probe.py -v
```

Root is needed only to run the real UID 65534 IPC controls. The command opens no
desktop windows and starts no VM. The remaining CPU-only controls also run as an
ordinary user; the UID-transition test then reports a skip. CI runs all controls.

Strict builds cover default, sampled default, 4K, sampled 4K and sampled
application-object configurations. A hardware trial must still establish correct
4K application rendering, real GPU retirement and cleanup with the new selection.
Matched-native performance, input latency and daily-use readiness remain open;
the backend/product counts stay at **4/8** and **0/4**.
