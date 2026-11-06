# CPU aperture capacity investigation

The 4K resource-FD test exposed virtual-slot exhaustion before it reached the
96 MiB CPU mapping budget. The full-buffer build requests a 33,177,600-byte
readback instead of five small bands. On the original 128-slot layout it
imports the native texture successfully, then fails at `ReadbackMap` after
one slot-quota rejection. The retained host peak is 31,526,912 CPU bytes.
The owned guest exits 1; native worker and QEMU exit 0, unforced, with complete
kernel/native cleanup. This is a reproduced failure, not a passed workload.

The new explicit `--driver-cpu-slots 256` layout reserves a 256 MiB guest
virtual PCI aperture. Its slots remain initially unmapped; only an owned
native allocation can supply backing pages. It keeps the 4K profile's
96 MiB mapped-byte budget, 64 MiB per-view maximum, existing object/command
budgets, and separate 128-page read-only fence hub. The 16/32/64/128 layouts
remain selectable with their existing budgets. It does not increase the
NVIDIA hardware's physical memory capacity or partition the GPU.

A regression reproduces the virtual limit with 100 small CPU views and a
full 3840×2160 RGBA8 view. The old layout cannot fit the contiguous view;
the new one fits, retires and reuses it while keeping the same physical byte
budget. Bounds, generation ownership, rollback and VM-exit-first cleanup
checks remain in place. The new guest kernel module compiles, and the owned
128-slot comparison passes its 581 memory, 561 object and 15 resource-FD
contracts before reproducing the full-buffer failure.

**Two fresh 256-slot GPU runs now pass the bounded full-buffer workload.**
Both use exactly the same native bridge, guest image and kernel as the retained
128-slot failure. Each checks all 8,294,400 pixels of a source-written red frame
through the imported resource, releases the source, then checks all pixels of
an imported-resource-written green frame. The readback is one 33,177,600-byte
buffer per round, with two retired hardware-queue signals and 24 retired native
commands per guest. Neither run has a slot rejection. Peak mapped CPU bytes
are 65,802,240; the physical mapping limit remains 100,663,296 bytes (96 MiB).
Native worker and owned QEMU exit 0, unforced, and the kernel module unloads
with zero live leases/bytes. Each guest passes 581 memory, 561 object and 15
resource-FD contracts and exits 0.

The retained [evidence](evidence/CPU-CAPACITY-2026-10-12.json) contains both
positive captures, the failed comparison, source/binary hashes, and the pinned
[QEMU build](https://github.com/7Wdev/try-omarchy-windows/actions/runs/38295227410).
Its ZIP digest was checked against the artifact metadata, and its corresponding
source contains the exact committed allocation-map and hub changes.

The full-buffer helper is separately selected with
`WDDM_BRIDGE_TEST_RESOURCE_FULL_READBACK=1` alongside the resource-FD build.
Its auditor requires `--full-readback`, the exact 256-slot layout, unchanged
96 MiB budget, a genuine full-buffer kernel lease, both complete 4K pixel
checks, GPU retirement and native/kernel/VM cleanup. Banded evidence cannot
be promoted into full-buffer acceptance. The actual native region includes
driver padding: 33,226,752 committed bytes. The audit matches that bounded
native region to the kernel lease, guest mapping and native-lock receipts
before accepting the first storage check; it does not infer allocation size
from the tightly packed pixel count.

Run the comparison auditor from the repository root:

```sh
python3 experimental/windows-native-gpu/verify_cpu_capacity_capture.py \
  --evidence docs/evidence/CPU-CAPACITY-2026-10-12.json
```

The general legacy runtime wrapper still rejects this newer resource-FD
diagnostic; its `diagnosticAccepted=false` is retained. The standalone audit
accepts only the bounded workload described here.

This comparison establishes this bounded readback workload. General aperture
fragmentation, long desktop sessions, cross-process graphics
ownership, DRM/GBM, Hyprland and near-native performance need further work.
