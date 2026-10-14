# Native resource DMA-BUF ownership

The optional guest kernel component can export a genuine Linux DMA-BUF that
retains an authenticated native D3D12 resource capability. The renderer uses
the existing ordinary-user graphics broker. A retained or transferred DMA-BUF
keeps the native export alive until its last file reference closes; the existing
resource resolver unwraps only this module's own DMA-BUF exporter.

This is an ownership boundary for the future compositor buffer path. It does
not yet supply DRM PRIME, a render node, GBM allocation, EGL image import,
implicit synchronization, foreign-device DMA mapping, CPU access, or a Hyprland
desktop. A logical byte extent is not a physical allocation size, stride, or
modifier. No near-native performance claim follows from the diagnostic.

## Interface and lifetime

`enable_native_buffers=1` is required at module load. The default is disabled.
The private packer enables it only with `--guest-native-buffers`, which requires
an explicit guest module and contract binary. Keep the kernel and module
versions matched and hash-pinned; never bypass vermagic checks.

Broker protocol 3 adds operations 13–15: export a native buffer from one
authenticated resource FD plus a 64-bit logical extent, query one buffer's
extent, and read buffer lifetime counters. Use matching broker/client builds.
The exported file is close-on-exec. Failed requests retain the caller's input
FD. The shim exposes `wddm_bridge_export_native_buffer`,
`wddm_bridge_native_buffer_info`, and `wddm_bridge_native_buffer_stats`.

Only the privileged controller may create or query a wrapper, and its source
must be a live resource capability owned by that controller and registered by
the broker's native export store. Fences, arbitrary files, foreign controllers,
revoked sources, and nested wrappers are rejected. Controller exit invalidates
live wrappers. Failed copyout rolls back both descriptor and quota reservations.
The kernel retains the underlying file separately from the DMA-BUF FD, so dup
and SCM_RIGHTS preserve ownership. Native imported GPU resources have their
own lifetime after the DMA-BUF and original resource handles close.

The diagnostic allows 64 live wrappers, a maximum 256 MiB logical extent per
wrapper, and 1 GiB total logical extents. These bounds limit descriptors and
declared metadata; they do **not** enforce GPU allocation residency or physical
memory quotas. CPU mapping/synchronization and foreign-device attachment fail
explicitly. The DMA-BUF reservation object is not yet connected to native GPU
fences, so consumers must not infer implicit synchronization from this export.

## Hardware diagnostic

Build with `WDDM_BRIDGE_TEST_RESOURCE_FDS=1` and
`WDDM_BRIDGE_TEST_RESOURCE_NATIVE_BUFFERS=1`, the pinned Mesa native-sync build,
the ordinary-user broker, and the matching kernel module. This also enables
Mesa's verified native batch-fence path. The independent resource test creates
a 3840×2160 RGBA8 shared texture, wraps its native FD, closes the original FD,
duplicates and closes the DMA-BUF, and imports the same native resource through
the broker. It checks two full-image color passes, including a write after the
original source resource is released. Five readback bands verify every pixel
with a bounded 7,864,320-byte staging buffer; this is not a 4K compositor or FPS
benchmark. The two separate 130×73 OpenGL shader frames retain their independent
pixel audit.

### Verified on 2026-10-14

Two fresh final-source guests (`bfe61f8`, Mesa 26.0.8, kernel
7.2.8-arch1-2, NVIDIA GeForce RTX 5090 Laptop GPU) pass the dedicated auditor.
[Complete records and artifact hashes](evidence/NATIVE-BUFFER-2026-10-14.json)
include both runs, the disabled control, the EGL regression, and the initial
kernel-version rejection before any GPU work. Vendor binaries and private
guest images are not included.

- Each enabled run passes 200 native-buffer contract checks and the existing
  583 aperture checks. All 72 created DMA-BUFs release: 71 contract objects
  and one real native GPU texture wrapper. Zero bytes or descriptors remain.
- Both 8,294,400-pixel color rounds pass the full-image digest, including after
  source release. Both separate OpenGL shader frames pass with maximum channel
  error 1. All 49 native GPU commands, 15 hardware signals and six GPU wait
  dependencies retire. Five Mesa batch sync-files signal and release.
- All 165 memory leases revoke explicitly before VM exit. The ordinary-user
  renderer, native host and owned QEMU exit normally, without forced stops.
- The disabled control returns unsupported for export and info, retains the
  input FD on failure, creates no DMA-BUFs and performs no guest resource-FD
  import. Its two shader frames still pass. The separate host metadata probe
  may open a native resource; it is not a guest DMA-BUF import.
- The existing EGL native-fence regression passes with the buffer feature
  enabled. Its 71 kernel-contract buffers also release, and the future GPU
  wait, completion/error semantics and full cleanup remain verified.

The generic legacy runtime verifier returns nonzero for these newer profiles;
its old acceptance contract is not used to promote this milestone. The dedicated
auditors check actual guest/native receipts, independent pixels and complete
cleanup. Corruption tests reject missing/reordered ownership evidence, wrong
pixels, unretired commands, leaks and the mismatched-kernel launch.

`WDDM_BRIDGE_TEST_RESOURCE_NATIVE_BUFFERS_DISABLED=1` builds the explicit
disabled control. Pair it with a module loaded without native buffers: export
and info must return unsupported, input ownership must survive both failures,
and no native DMA-BUF may be created or imported. Mesa synchronization stays
enabled independently. `verify_native_buffer_capture.py` audits the complete
guest/native/VM records and the selected profile without altering old evidence.

Next integration work is the actual Mesa/GBM buffer allocation and EGL image
path, including verified formats, offsets, strides, modifiers and synchronization.
The first verified backend-rendered Hyprland session must be captured and added
to PR #2 immediately. This layer advances stage 5; desktop gates remain open.
