# Real render node, GBM allocation and GPU sampling

The Windows NVIDIA bridge now passes two fresh, disk-free QEMU/WHPX guests
using an actual Linux render node and GBM/EGL allocation. These are development
captures based on `5440a87` with exact working-tree source hashes retained in
[the evidence](evidence/GBM-NATIVE-RENDER-DEVELOPMENT-2026-10-14.json).
No desktop gate is complete: the roadmap remains **4/8 stages and 0/4 gates**.

Each ordinary-user process opens `/dev/dri/renderD128`, selects Mesa's private
D3D12 DRM loader and allocates two simultaneous GBM buffers. The 1281×719 BGRA
and 257×131 RGBA buffers export genuine owned DMA-BUFs, import into EGL and
retain GL textures after the caller FDs, original BOs and EGL images are gone.
Solid-color readbacks verify all 954,706 storage pixels. A separate shader samples
the first imported texture into the second buffer; every pixel in its 130×73
viewport matches the independent triangle oracle within one channel value.
This sampled viewport is a bounded correctness check, not a full 4K workload.

Both runs retire all 39 native GPU commands. All 73 DMA-BUF references and
165 memory leases release before VM exit; no mapping release is deferred until
after VM exit. The render module unloads, its node disappears, and the guest,
Windows bridge and QEMU all exit normally without a forced stop.

The new guest-only module defaults off. Its render node exposes DRM identity
and a fixed, read-only private query. Kernel PRIME, GEM, KMS, CPU mapping and
syncobj are explicitly unavailable; it does not pretend to implement them.
Owned-buffer sharing and GPU calls continue through the authenticated broker.
The packer requires native buffers, native sync-files and the graphics broker
together, verifies the module ELF identity and matching kernel release, and
includes no render module by default.

The additive [fourth Mesa patch](../experimental/windows-native-gpu/mesa/native-render-patch.json)
requires all three pinned predecessors. Its applier verifies source and patch
digests, dry-runs with zero fuzz and verifies the resulting source. A repeat
verifies the exact patched tree. Render-FD ownership is established before
screen capability initialization; the owned FD closes during screen teardown.

Two actual development failures remain in the evidence. Missing
`FOP_UNSIGNED_OFFSET` initially made the kernel reject node opens with `EINVAL`.
After fixing that, screen initialization dereferenced an absent software winsys
while querying its FD. Moving FD initialization earlier fixed that crash.
Neither failure is counted as an accepted run.

A separate legacy-profile run with this Mesa build, no render module and no
native buffers renders both shader frames and exits normally. Its native and VM
cleanup is verified, but some mappings release after VM exit, as permitted by
that older profile. It does not meet the new GBM run's explicit-release criterion.

Hyprland still needs its Aquamarine allocator/output connection, a sustained
render profile and reliable Windows presentation. General DRM/PRIME interoperability,
implicit synchronization, multiple independent guest GPU clients, 4K desktop
stability and near-native performance remain unverified. The first verified
Hyprland frame will be captured and added to PR #2 immediately when it exists.
