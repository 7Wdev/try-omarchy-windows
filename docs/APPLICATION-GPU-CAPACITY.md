# GPU capacity for applications with multiple processes

The experimental coordinated backend can explicitly negotiate eight authenticated
application processes and 32 resource references per process. This addresses two
observed Chromium startup boundaries. It does not establish browser compatibility
or daily-use readiness. The normal launcher and broader application acceptance
remain open.

The owner enables process capacity with `--driver-actor-slots 8`. Resource capacity
additionally requires `--driver-resource-reference-slots 32`. Both are restricted
to the explicit full owned SDL desktop with a read-only source root. Ordinary
application clients cannot change these agreements.

| Trusted initialization payload | Process slots | Resource references per process | Resource profile |
| --- | ---: | ---: | --- |
| Existing 80 bytes | 4 | 16 | v1 |
| Application initialization, 88 bytes | 8 | 16 | v1 |
| Application reference initialization, 96 bytes | 8 | 32 | v2 |

The 176-byte information reply retains its layout. Flag 1 identifies the
eight-process agreement; flag 2 additionally identifies the resource-reference
agreement. Exact flags, versions, payload sizes and reserved zero fields are
validated together. An owner that permits the extended agreement still uses the
original limits when the controller sends an older initialization payload.

These changes distribute the existing shared pool differently. The native group
still allows 64 resource references and 64 fence references in total, with 16
fence references per process. Aggregate queues, synchronization objects, leases,
CPU mappings and reported GPU-usage guards retain their existing limits. Adding
process slots does not multiply memory budgets. Resource exhaustion and recovery
still need broader product validation.

The guest capability store derives its limits from the authenticated coordinator
reply. It checks capacity before asking the native owner for a recipient alias.
A capacity refusal preserves the live FD and existing references; forged
identities and malformed successful native replies remain protocol errors. The
host independently enforces per-process export/alias limits and its group limit.
Native references remain charged until the kernel confirms last-FD close or
the owned VM has exited.

Regression checks cover default agreements, malformed requests, eight independent
handle namespaces, the ninth-process refusal, 32 resource references, refusal of
a 33rd reference before native work, the fixed group limit, slot reuse after
close, and real Unix FD lifetimes. Mock checks identify themselves as such;
browser and GPU observations require separate evidence in
[the browsing progress report](GUEST-BROWSING-PROGRESS.md).
