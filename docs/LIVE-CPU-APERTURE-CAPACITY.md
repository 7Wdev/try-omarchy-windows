# Configurable CPU allocation aperture

The QEMU bridge now supports 16, 32 or 64 CPU allocation slots, with 16 as the
compatible default. On the RTX 5090 Laptop, the live NVIDIA Linux runtime used
21 slots and directly accessed five mappings beyond the previous 16 MiB aperture.
The expanded cases had no CPU-slot quota failures. Initialization advanced from
`8007000e` to `80070057`, where two monitored-fence requests use the documented
`NoGPUAccess` flag. That fence mode is not implemented at this revision.

The [physical evidence](evidence/QEMU-LIVE-APERTURE-2026-10-09.json) records six
accepted diagnostics with exact source, native binary, QEMU and private image
hashes. Complete runtime initialization, CUDA, OpenGL, Vulkan, video acceleration,
interactive Omarchy and the 93% performance requirement remain unverified.

## Capacity contract

The native option `--driver-cpu-slots 16|32|64` requires `--driver-cpu` and an
owned QEMU runtime. For expanded configurations, the controller passes QEMU's
`wddm-allocation-hub,slot-count=N` property. The guest discovers the actual slot
count from PCI configuration and bounds mappings against that layout. Default
startup omits the new property and remains compatible with the older QEMU binary.

Each slot provides at most 1 MiB of exact committed native allocation pages.
The BAR reserves address space; it does not allocate 64 MiB of backing RAM.
Only owned, validated pages are mapped. Capacity is fixed at device realization;
generation checks, overlap rejection, no-execute WHPX mappings and acknowledged
unmap before native unlock are preserved. Malformed capacity options fail before
runtime startup. The independent vendor allocation limit remains 32 objects;
mapped GPU addresses retain their 16 MiB budget.

## Physical controls

Both 32 and 64 configurations created and CPU-mapped 21 allocations, including
slots 16 through 20. They completed one native 4 KiB command at fence 5, reclaimed
five allocations with two hardware queues alive, acknowledged all 21 CPU unmaps
and released all native objects. The 64 configuration does not prove simultaneous
use of 64 slots: the runtime used only 21.

The older QEMU binary and the new binary at the default 16 slots both reproduced
two CPU-slot quota failures. A 32-slot control with allocation retirement disabled
kept five CPU locks until confirmed VM exit and released them successfully.
A guest-exit control kept one hardware queue and one CPU lock alive until VM exit;
cleanup released them without claiming guest unmap acknowledgements.

MSVC and GCC checks passed with warnings treated as errors, all 98 Linux fault
cases passed, ten native malformed-option cases passed and QEMU rejected capacity
values 0, 8, 17, 33 and 65. The pinned Windows QEMU build and its hosted shared-memory
checks passed. Its downloadable artifact includes corresponding GPL source.

For the [owned runtime procedure](LIVE-HARDWARE-QUEUE-BRIDGE.md), add
`--driver-cpu-slots 32 --minimum-expanded-cpu-mappings 1
--expected-cpu-slot-quota-rejections 0` and use the QEMU build linked in the
evidence. Keep vendor libraries and private runtime images local. These tests
still expect incomplete runtime initialization; they verify this capacity change
and cleanup rather than product readiness.
