# Sparse aperture integration for the real Omarchy guest

The [newer explicit GPU-VA profile](LIVE-NORMAL-OMARCHY-4K-READBACK.md) produces
the first two verified full-size 4K GPU compositor readbacks. Its reported-usage,
normal-shutdown, missing-shell and display gates still fail. The version-8
failure below remains retained and unchanged.

**The requested GPU-only 4K desktop frame is still unverified.** The new
512-slot CPU aperture works on actual WHPX/NVIDIA hardware. The retained
3840×2160 attempt then reaches the separate GPU virtual-mapping quota and
aborts, leaving native GPU waits pending. Neither pixels nor clean teardown
pass for that attempt. Quickshell's independent GPU connection also remains
unsupported. These failures remain visible in the
[complete evidence](evidence/NORMAL-OMARCHY-SPARSE-APERTURE-2026-10-16.json).

## Exact profile and ownership

Version 8 requires a fixed 32-byte receipt: version 8, kernel capacity ABI 1,
192 MiB CPU-map capacity, 512 allocation slots, 256 fence slots and a zero
reserved word. The guest kernel needs both immutable load-time parameters
`enable_desktop_4k_cpu=1 enable_sparse_aperture=1`. Discovery returns flags 3;
the authenticated broker independently checks its controller, and the Windows
owner verifies its actual configured layout. The new export is
`wddm_bridge_begin_opengl_desktop_sparse_render`.

The 512-slot layout reserves 512 MiB of guest MMIO address space; only authorized
native pages are mapped. It does not allocate that amount of physical GPU
memory. CPU mappings remain bounded at 192 MiB, individual views at 64 MiB,
GPU virtual mappings and reported GPU usage at 512 MiB each. Pending submission
and signal counts remain 16; live wire/vendor/sync objects remain 1,024/512/192.
Kernel leases retain their independent 512 limit.

Versions 1–7 retain their 256-slot receipt ceiling, including the original
version-7 exact capacity contract. Native and guest reply validators, broker
view grants and actual allocation mappings widen only after version 8 succeeds.
The default executable rejects `--driver-cpu-slots 512`; the high-resolution
executable also requires explicit 256 fence slots and an owned QEMU runtime.
Older profiles cannot initialize against its 512-slot runtime. Generation
ownership, overlap rejection and VM-before-backing-release remain enforced.

## Hardware results

The [QEMU build and section-memory test](https://github.com/7Wdev/try-omarchy-windows/actions/runs/38919124772)
pass at `3e63de34d7c43aec5b48d94bc879fea8be40dedc`. The downloaded artifact
matches GitHub's SHA-256 digest; its complete corresponding allocation-device
source matches that revision after newline normalization. The archive and
executable digests are retained, along with DLL/source receipts. Section-memory
CI alone is not treated as a WHPX or graphics acceptance test.

In the hardware-only 4K run, all **1,101 kernel aperture checks** pass. The actual
NVIDIA runtime verifies the exact version-8 contract. Two 33,226,752-byte CPU
views map directly; the second starts at offset 241,172,480 and ends at
274,399,232, crossing the previous 268,435,456-byte aperture ceiling. Its first
and last words receive 10,000 direct guest loads. There are 265 slot mappings,
four explicit removals and zero emulated MMIO reads/writes. The previous
slot-quota rejection is gone.

The next 8,160-page GPU-VA mapping would exceed 512 MiB with 130,090 pages
already mapped. It fails with `errno=24`. CPU usage is 124,198,912 bytes and
reported GPU usage peaks at 399,089,664 bytes, both within their independent
guards. Hyprland aborts after **28 retired GPU commands**, producing **zero
audited backbuffers**. Native teardown retains two GPU signals and three GPU
waits; `driverCleanupVerified=false` and the host exits 1. Guest kernel cleanup
and normal VM poweroff do not erase that native failure. No software Qt
override is used. The source disk digest remains unchanged.

A fresh version-5 regression uses the same new QEMU and kernel binaries with
the preserved 256-slot layout and default 96 MiB CPU limit. It passes **300
frames / 945 retired GPU commands** and the strict scene auditor accepts both
actual backbuffers. All 142 fence views, 185 allocation views and 213 slot maps
release explicitly before normal VM shutdown; native cleanup passes. The Foot
application uses software rendering and Hyprland composes it on NVIDIA. This
retains compositor regression scope and does not prove an all-GPU desktop.

## Remaining work

The next resource change needs a distinct bounded GPU-VA accounting contract;
version 8's published limit must remain fixed. Native abort cleanup also needs
to cancel or destroy owned device work safely after proven VM exit, without
fabricating retired fence values. The observed pending waits cannot simply be
discarded to make a cleanup report pass. Independent GPU clients, shared-FD
lifetimes and aggregate budgets remain required for Quickshell and applications.

Default/high-resolution MSVC builds, the slot boundary/reuse/stale-owner tests,
native wire and broker capacity controls, 105 default CLI rejections, seven
high-resolution startup rejections, legacy transport controls and real Unix
credential/FD-lifetime checks pass. Seven new evidence tests preserve both the
failed 4K control and passing legacy run. Private vendor runtimes and source
images stay local. Full desktop, stable input/scanout/launcher integration,
application APIs and ≥93% matched-native performance remain open; acceptance
counts remain **4/8 verification stages and 0/4 desktop gates**.
