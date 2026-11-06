# Vulkan memory FD ownership — October 28, 2026

Follow-up: [actual GPU payload import/readback and its retained failures](VULKAN-GPU-BUFFER-ROUNDTRIP.md).

Two fresh QEMU/WHPX guests now export **32 distinct close-on-exec descriptors**
from a dedicated **64 KiB GPU-only buffer**. Every descriptor still identifies
the same kernel object after the creator's Vulkan buffer and memory are freed.
All native resources and mappings retire before natural host/VM exit. Separate
regressions preserve both complete **3840×2160 compute rounds**.

This checkpoint verifies FD export and reference lifetime. The follow-up above
verifies scoped GPU import/readback with retained failures. Vulkan rasterization,
Wayland presentation and daily-use acceptance remain open.
Progress counts stay **backend 4/8, desktop product 0/4, diagnostic scene 3/3**.
All runs were windowless, with no source disk, host input or visible renderer.
The official launcher and keyboard/cursor implementation remain unchanged.

## Two independent issues, with retained controls

The original Mesa 26.0.8 Dozen export function gives its owned descriptor to the
caller and replaces it with `-1`. A second export can return `VK_SUCCESS` with
an invalid descriptor. Its truthiness test also rejects descriptor zero. The
[Vulkan specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetMemoryFdKHR.html)
requires a new reference on each export and close-on-exec where supported.

The Linux-only patch duplicates with Mesa's existing `os_dupfd_cloexec`, keeps
the original owned reference, admits descriptor zero, checks descriptor range,
and preserves the owner/output on failure. Descriptor exhaustion returns
`VK_ERROR_TOO_MANY_OBJECTS`. Windows export code remains unchanged. The existing
FD-property query is also unchanged: querying `OPAQUE_FD` through
[`vkGetMemoryFdPropertiesKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetMemoryFdPropertiesKHR.html)
is prohibited by VUID 00674.

The native bridge initially rejects allocation before Dozen can export anything.
Windows `OpenSharedHandle` actually succeeds, but the existing bridge probe
accepts only textures, and original guest runtime metadata is cached only for
explicit texture declarations. The bridge now separately validates bounded
buffer descriptions and preserves the original guest runtime metadata for
shared allocations. Buffer admission requires successful same-adapter native
import, an allowed actual resource description, and release of the temporary
NT handle/resource. The texture checks and byte budgets remain unchanged.

| Run | Native bridge | Dozen ICD | Actual result |
| --- | --- | --- | --- |
| 1 | Original | Original | Allocation refused before FD export |
| 2 | Original | Patched | Same allocation refusal; export bug still masked |
| 3 | Original | Patched | Original 4K compute probe passes both complete rounds |
| 4 | Buffer admission | Patched | All 32 exports and post-free FD lifetime pass |
| 5 | Buffer admission | Patched | Original 4K compute probe passes both complete rounds |
| 6 | Buffer admission | Patched | Fresh repeat of all 32 exports and lifetime passes |
| 7 | Buffer admission | Original | Allocation/bind succeed; second export returns an invalid FD and the probe rejects it |

The negative rows remain rejected, including their guest nonzero exits. Runs
1/2 retain one `failedSharedResourceProbes` count. Run 7 retains the exact export
failure. Generic initialization reports also remain rejected: their workload
oracle does not cover these tests. A separate verifier audits the actual scope.

Runs 4/6 each complete five native **initialization** submissions, six native
queue signals, three device-health queries and three kernel notifications.
The probe submits no application shader. There is one native shared handle,
one broker publication/reap/close and 32 process FD references; these are
different layers of reference ownership. `importedSharedBuffers=1` records
the Windows validation probe, **not a guest Vulkan FD import**.

All seven runs retire their mappings before VM exit. Positive FD repeats have
no failure, pending-work, forced-stop, timeout or deferred-release counters.
They use 256 virtual CPU slots, 128 fence slots and the unchanged **96 MiB CPU
mapping / 256 MiB reported GPU-usage guards**. Peak usage is 25,341,952 CPU bytes
and 40,448,000 reported GPU bytes. Actual hardware coverage is the 64 KiB plain
transfer buffer. The portable description guard's 64 MiB/UAV limits are not
hardware coverage claims.

## Reproducible source checks and precise evidence

Eight CPU cases compile the exact original/patched Dozen function and real Mesa
duplication helper against Vulkan types. They use real Linux descriptors,
descriptor zero, exhaustion, owner close and exec. The original executes all
eight cases with seven failures; the patched function passes all eight. A
retained earlier adapter compile failure is excluded. CI requires the actual
receipt, binary/include hashes and all eight executed rows, so a compile error
cannot count as the expected baseline failure.

- [Exact-source Linux patch](../experimental/windows-native-gpu/mesa/0005-linux-dozen-memory-fds.patch),
  [digest-pinned applier](../experimental/windows-native-gpu/mesa/apply_mesa_dozen_memory_fd.py),
  [CPU contract](../experimental/windows-native-gpu/mesa/test_dozen_memory_fd.py) and
  [executed-case audit](../experimental/windows-native-gpu/mesa/verify_dozen_memory_fd_contract.py).
- [Native buffer guard](../experimental/windows-native-gpu/shared_buffer_profile.h) and
  [14 rejection controls](../experimental/windows-native-gpu/shared_buffer_profile_test.cpp).
- [Actual Vulkan probe](../experimental/windows-native-gpu/vulkan_memory_fd_probe.cpp) and
  [export/lifetime workload](../experimental/windows-native-gpu/vulkan_memory_fd_workload.h).
- [Scoped matrix audit](../experimental/windows-native-gpu/verify_vulkan_memory_fds.py),
  [FD evidence rules](../experimental/windows-native-gpu/vulkan_memory_fd_evidence.py),
  [11 negative evidence tests](../experimental/windows-native-gpu/test_vulkan_memory_fd_evidence.py) and
  [sanitized hardware receipts](evidence/VULKAN-MEMORY-FDS-2026-10-28.json).

The candidate ICD is a fresh build from the pinned Mesa 26.0.8 archive and the
same SDK sources. Private images deliberately replace its bytes at the original
guest ICD lookup path; the original host binary stays unchanged. Image audits
allow only the specified probe/ICD payload differences. NVIDIA runtime files,
kernel, broker, shim and root init stay identical. The unmodified original 4K
compute probe and shader pass both rounds with both native bridge versions.
An additional texture regression runs the unchanged original 4K shader image
against the new bridge. Both actual RGBA captures are byte-identical to the
previous bridge's frames, pass all 8,294,400 pixels against the original oracle,
and complete the native GPU handoff and before-VM cleanup. It opens no window
and does not add desktop/presentation acceptance.
Private NVIDIA ELFs, initrds, raw native streams and VM images are not published.

Next are actual FD import with GPU data readback, image/rasterization sharing
and Wayland synchronization. Normal launcher integration, official input,
sustained multi-application use, pressure/crash recovery and matched-native
performance remain open. This checkpoint is not a stable-release notification.
