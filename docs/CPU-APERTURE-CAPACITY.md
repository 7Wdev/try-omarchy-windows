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

**Physical 256-slot GPU acceptance is pending.** The matching QEMU build must
finish before the same full-buffer image can be tested on that layout. The
full-buffer helper is separately selected with
`WDDM_BRIDGE_TEST_RESOURCE_FULL_READBACK=1` alongside the resource-FD build.
Its auditor requires `--full-readback`, the exact 256-slot layout, unchanged
96 MiB budget, a genuine full-buffer kernel lease, both complete 4K pixel
checks, GPU retirement and native/kernel/VM cleanup. Banded evidence cannot
be promoted into full-buffer acceptance.

Even a successful comparison would establish this bounded readback workload.
General fragmentation, long desktop sessions, cross-process graphics
ownership, DRM/GBM, Hyprland and near-native performance need further work.
