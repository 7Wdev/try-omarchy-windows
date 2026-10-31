# Live Windows paging fences in QEMU

The later [live allocation route](LIVE-ALLOCATION-BRIDGE.md) builds on this
paging milestone. The evidence below retains its original allocation boundary;
with the additional opt-in, initialization now reaches GPU-address mapping.

The NVIDIA Linux runtime now creates a Windows WDDM paging queue and graphics
context through the diagnostic bridge. On the RTX 5090 Laptop GPU with driver
`32.0.16.1742`, the disk-free QEMU/WHPX guest directly loaded the native paging
fence 10,000 times, with zero QEMU MMIO callbacks. Its initial observed value
was 7000. This is a live driver-owned page, not a copied completion counter.
The runtime's 3,204-byte graphics context initialization reached
`D3DKMTCreateContextVirtual` unchanged, without a captured private fixture.

The [hardware report](evidence/QEMU-LIVE-PAGING-2026-10-08.json) records one map,
one host unmap acknowledgement, normal QEMU exit, 19 successful adapter queries,
one live context and zero remaining native objects. The
[memory/copy regression](evidence/QEMU-PAGING-MEMORY-REGRESSION-2026-10-08.json)
also passed the earlier allocation, GPU VA, shared-memory, context and query
paths: six fenced GPU copies totaling 262,443 verified bytes. That separate
regression uses captured context/query fixtures; the live runtime test does not.

**D3D12 initialization still fails with `0x80004001`.** The next live interface
is `CreateAllocation` (ioctl 6); synchronization creation, residency/eviction,
GPU VA operations and queue submission in the Linux runtime ABI also remain.
The existing bounded standard-allocation/copy fixture does not implement the
UMD's vendor-private allocation contract. No Omarchy desktop acceleration or
near-native performance is established, and the launcher does not select this
backend.

## Ownership and wire contract

Only `driver-bridge.exe --run-qemu` advertises capability bit 128. It requires
the explicit `--driver-contexts --driver-queries` opt-ins, owns QEMU's Windows
process handle, and creates the VM suspended inside a private job before
resuming it. A fixed inherited handle refers to the native driver owner;
only that handle, the fresh log and NUL input are inherited. The job kills its
child if the owner disappears. Normal teardown still explicitly waits for
child exit before releasing native pages; this is not a proof of every parent
crash or Windows GPU reset scenario.

The host opens an ephemeral loopback QMP endpoint and validates the machine's
random name. It uses a pinned JSON parser with an 8 KiB message limit, nesting
bound, duplicate-key rejection and exact string request/reply IDs. Control
requests have five-second deadlines. Guest packets cannot specify a Windows
address or send QMP commands. These local endpoints are diagnostic interfaces,
not authenticated production boundaries against another process of the same
Windows user.

Private wire operation `0x2040` creates a paging queue for a typed device ID.
On success it returns the 16-byte ordinary reply followed by a 16-byte
`PagingReply`: `u32 synchronizationId`, `u32 reserved=0`, `u64 fenceOffset`.
The queue ID is in the reply header. Both IDs are connection-local, consume
the 64-object quota, and remain distinct from native KMT handles. The offset
is eight-byte aligned and below the 256 KiB aperture. Native failures return
their NTSTATUS with no IDs or extra payload. Invalid native results destroy
the queue rather than expose an incomplete object. The synchronization ID is
borrowed from the queue and is invalidated only after successful queue
destruction; it is not independently destroyed with the KMT sync API.

The owner maps an empty slot in the private `1234:11fd` PCI fence hub through
QMP and waits for acknowledgement before replying to the guest. Each map uses
a globally increasing generation. Host commands reject occupied slots and
stale generations; unmap must match the current slot's generation. The guest
discovers the device and layout through sysfs, enables its PCI BAR, maps just
one 4 KiB page read-only, and verifies the emulated-read counter stays unchanged
through 10,000 fence loads before exposing its own CPU pointer to the UMD.

Queue destruction first unmaps the guest CPU view, then sends the typed queue
destruction request. The owner waits for QMP's completed memory-listener
transaction before calling `D3DKMTDestroyPagingQueue`. Failed control stops
and reaps the owned VM before releasing its native source page. Transport EOF
or failure likewise stops the VM before session cleanup. The runtime diagnostic
has bounded requests/time and a 40-second guest process watchdog. The Python
harness retains a verified child process handle and terminates/reaps that
child before killing its native owner on timeout; it deliberately retains the
owner when it cannot establish child exit.

This mapping is process-local `LD_PRELOAD` plumbing for a root diagnostic
process. It is not a guest kernel driver, DRM node, eventfd implementation,
production fencing API, or graphics submission interface. The direct-read
test does not prove a live GPU submission advanced this fence, and this result
does not cover slot-reuse stress, GPU resets or arbitrary guest process crashes.

## Reproduce with private installed libraries

Build the native worker with `experimental/windows-native-gpu/build.ps1`.
Build the Linux runtime diagnostic and private image as described in
[the runtime report](LIVE-NVIDIA-RUNTIME.md). Never upload that image or the
installed NVIDIA binaries. The QEMU executable must include the dynamic hub;
the physically tested build is
[run 37759077873 at 60e35ae](https://github.com/7Wdev/try-omarchy-windows/actions/runs/37759077873).
Its artifact SHA-256 is
`1eb0c9aa73dd8bc465cd02394bb4675639c875b6f5327c78bc781ad4a612befe`;
the tested executable is
`382ab0992db24bda9bcea365a58b3bd84c1c8736439fa9f6f461ce2d132e1e28`.
The downloaded package includes `qemu-corresponding-source.tar.gz` and its
dependency source inventory; retain them with any redistributed binary.

```powershell
python experimental/windows-native-gpu/test_owned_runtime_qemu.py `
  --qemu C:\local\qemu-hub\bin\qemu-system-x86_64.exe `
  --firmware C:\local\qemu-hub\share\qemu `
  --kernel C:\local\vmlinuz-linux `
  --initramfs C:\local\private-runtime.cpio.gz `
  --bridge C:\local\native-build\driver-bridge.exe `
  --report C:\local\fresh-paging-report.json `
  --expected-unimplemented-ioctl 6
```

`diagnosticAccepted=true` requires incomplete initialization at the observed
allocation boundary, live UMD/device/context creation, direct fence reads,
unmap acknowledgement and clean native/VM exit. It must not be used as the
application's GPU readiness flag. CI compiles both platforms and exercises
wire/QMP validation plus fourteen fake-worker fault cases without proprietary
binaries or hardware. Physical acceptance is separate from those tests.
