# Hyprland compositor-buffer development

Updated 2026-10-15. **Actual Hyprland now initializes its NVIDIA renderer and
commits owned output buffers. Compositor pixels and Windows presentation remain
unverified.** The final diagnostic in this original checkpoint crashes during
library finalization. The later [logging ownership fix](LIVE-HYPRLAND-LOGGING-LIFETIME.md)
passes three fresh compositor lifecycle runs; the failed records below are preserved.
This closes no desktop acceptance gate: progress remains 4/8 stages and 0/4
desktop gates. There is no defensible first-Windows-frame ETA yet.

The real retained Hyprland 0.56.2 runtime
(`efb50993780079460b0cbed1363e2166a2de1d9f`) runs as UID/GID 65534 in the
disk-free QEMU/WHPX guest. It reports **OpenGL ES 3.1 Mesa 26.0.8** and
**D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)**. The Aquamarine prototype creates
a three-buffer 640×360 swapchain, exports its native DMA-BUFs, and imports them
into the renderer's separate device through EGL. An observer validates the
actual owned native buffer FD at output commit. Its records explicitly say
`pixelsVerified=false` and `windowsPresentation=false`.

The [development records](evidence/HYPRLAND-COMPOSITOR-BUFFERS-DEVELOPMENT-2026-10-15.json)
retain all seven runs, including failures, full local-capture digests, binary
digests and available working-tree source hashes. Private binaries and images
are not redistributed. These working-tree captures are based on 172d12f;
they are not presented as clean-revision desktop acceptance.

| Run | Evidence and limitation |
| --- | --- |
| 6 | Expanded mappings clear graphics initialization: 142 fence and 163 allocation leases. Startup aborts because the small image lacks XKB data. Native/VM cleanup succeeds. |
| 7 | Adding matching XKB data starts the control socket. GBM allocation fails because the preparation hook permits only one device. No usable output buffer exists. |
| 8 | The ownership fix allocates three native buffers and configures the swapchain. The first render attempt exhausts 16 GPUVA reservations. Native cleanup fails; this remains a failed run. |
| 9 | Profile 4 allows the required reservations. EGL image import fails because allocator and renderer hold different adapter handles for the same GPU. Native/VM cleanup succeeds. |
| 10 | The same guest image with the host identity fix imports all three buffers and submits GPU work. It then overruns finite diagnostic command/allocation limits and fails cleanup. No frame acceptance is claimed. |
| 11 | Actual buffer-commit observation permits an early exit request: six commits and 24 retired commands. A forced compositor termination interrupts teardown and exposes a lost-reply race in the broker. |
| 12 | The reply-disconnect fix and longer graceful allowance produce seven commits, seven signaled/released sync-files and 28 retired commands. Main returns 0, then a library finalizer segfaults. Broker/native/VM exit 0, no forced VM stop occurs, and all kernel/native state is released. The compositor lifecycle still fails. |

## Implemented ownership and capacity

Native resource metadata is now staged per thread against a snapshot of owned
device/adapter pairs, then consumed by the actual allocation call. Foreign,
newly created, destroyed or reassigned devices are rejected. Failed attempts
consume their declaration; cancellation and the 16-pending-preparation bound
remain enforced. The older fixed-texture preparation contract is unchanged.

Profile 4 separately negotiates 32 GPUVA reservations and 768 aggregate objects.
Its reserved-address byte limit remains 16 GiB; physical GPU/CPU byte limits,
192 sync objects, 256 vendor allocations, 16 hardware queues, 16 pending
commands/signals and the finite 256-command/50,000-request budget stay bounded.
Profiles 1–3 retain their established contracts. This is a startup/render
diagnostic, not a sustained compositor profile.

Only profile 4 permits resource query/import across distinct adapter handles
after the native driver verifies they refer to its immutable selected GPU.
The guest cannot supply a Windows LUID or raw native handle. Unknown/foreign
adapters and older-profile cross-adapter requests remain rejected. Resources
and tickets remain scoped to the authenticated worker.

A broker reply lost after a valid request now follows the normal disconnect
lifetime path. The broker retains authority until its authenticated producer's
pidfd confirms exit. Malformed requests and unauthorized memory access still
fail. A real Unix socket/credential test reproduces this race.

## Source and validation

The [pinned Aquamarine patch](../experimental/windows-native-gpu/aquamarine-native-render.lock.json)
targets official 0.15.1 commit `f31c47a1b9d300847d8fc3108ad959448103dfc1`.
Its BSD license is retained. Apply it to that source checkout with
`python3 experimental/windows-native-gpu/apply_aquamarine_native_render_patch.py SOURCE`.
The helper checks exact before/after file hashes, verifies its patch digest,
supports idempotent application and rejects modified/partially patched source.
The optional [startup probe](../experimental/windows-native-gpu/hyprland_startup_probe.c)
observes real buffer commits and requests exit after at least two; asynchronous
startup and IPC allow additional commits before exit. It does not present or
verify pixels. The prototype is not installed by the normal Windows launcher.

Local validation passes: strict Windows native build and wire tests; 45
preparation ownership checks on MSVC and GCC; exact profile 3/4 negotiation and
reservation boundaries; resource import checks for same/foreign GPU identities;
1,636 shared-fence checks; all four OpenGL transport profiles and disabled
controls; the Linux ioctl fault suite; real privileged Unix broker routing,
rejection, retained-view lifetime and lost-reply tests; exact/idempotent
Aquamarine patch application and a corrupted-source rejection control. The
Aquamarine prototype builds and links against the actual retained runtime ABI.
The new public probe compiles with C11 warnings as errors.
All 211 retained milestone tests also pass on Windows after these changes.

All four hosted workflows for the earlier 172d12f capacity checkpoint pass,
including QEMU build 38642267443. Its artifact digest and corresponding source
are verified. Hosted CI, native push and native PR checks for the 356a6f1
integration checkpoint pass. The PR portable job passed on its second attempt
after the first failed only on Mesa archive download timeouts. Checks for the
later logging ownership fix are tracked separately.
GitHub had disabled Actions on the fork; the existing workflow triggers were
reviewed and Actions restored. The native validation workflow also supports
manual runs. A fresh push resumes the hosted integration checks.

Next work is compositor pixel capture and Windows presentation, and a sustained
resource/queue profile. The finalizer crash is addressed by the later logging
ownership checkpoint. Input, real applications,
4K desktop operation, recovery, required APIs and ≥93% matched-native performance
remain separate product acceptance requirements. The first verified frame
displayed through the Windows backend will be captured and added to PR #2.
