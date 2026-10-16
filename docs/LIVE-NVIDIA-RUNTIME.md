# Live NVIDIA Linux runtime in QEMU

Historical initialization checkpoint: subsequent work passes
[full D3D12 initialization](LIVE-D3D12-INITIALIZATION.md) and
[verified guest shader rendering](LIVE-GPU-SHADER-TRIANGLE.md). The results and
incomplete initialization boundary below describe the earlier test.

The disk-free QEMU/WHPX guest now loads this machine's installed NVIDIA Linux
user-mode driver and routes its actual initialization calls through virtio
serial to the native Windows worker. On the RTX 5090 Laptop GPU, it completed
19 native adapter queries, including the 50,616-byte NVIDIA-private query,
created a native WDDM device and paging queue, directly read its Windows fence
10,000 times without emulation, and created a native graphics context from the
live UMD's 3,204-byte initialization data. With the explicit allocation opt-in,
it also creates and synchronously destroys one Windows video-memory allocation
using the live UMD's 586-byte private input/output buffer.
With the additional GPU-address opt-in, it maps 16 pages, preserves the native
`STATUS_PENDING` result, and directly observes the completed Windows paging
fence. The mapping run performs 20,000 direct fence loads in total.
With the residency opt-in, it makes the allocation resident, preserves the
pending result and directly observes fence `7002`. Windows reports 64 KiB
of GPU-memory usage. The residency run performs 30,000 direct fence loads.
This test uses no captured private input fixtures
and has no real WSL `/dev/dxg` available in the guest.

**D3D12 device initialization is incomplete.** The runtime fails with
`0x80004001` after reaching `Lock2` (ioctl 37). The paging interface
now returns a queue-owned typed synchronization ID and a directly mapped CPU
fence. CPU allocation access, synchronization creation, eviction and broader GPU-address operations in
this runtime API, an optional feature query and registry queries remain unsupported.
All native objects were released and the guest and worker exited successfully.
[Residency hardware evidence](evidence/QEMU-LIVE-RESIDENCY-2026-10-08.json) records
the exact executable, image and kernel hashes and the negative initialization
result separately from diagnostic acceptance. The
[earlier device-only result](evidence/QEMU-LIVE-NVIDIA-RUNTIME-2026-10-08.json)
is retained alongside the [paging result](evidence/QEMU-LIVE-PAGING-2026-10-08.json)
and [allocation result](evidence/QEMU-LIVE-ALLOCATION-2026-10-08.json), alongside
the [GPU-address result](evidence/QEMU-LIVE-GPUVA-2026-10-08.json),
as earlier initialization boundaries. The
[allocation contract and memory-budget limitation](LIVE-ALLOCATION-BRIDGE.md)
describe this deliberately restricted interface. The
[GPU-address contract](LIVE-GPUVA-BRIDGE.md) adds owned mappings and guest
paging retirement verification.
The [residency contract](LIVE-RESIDENCY-BRIDGE.md) adds bounded typed lists,
memory-budget outputs and the live residency paging operation.

This is progress toward interactive Omarchy acceleration. It does not render,
submit guest GPU command buffers, run Hyprland, verify desktop stability or
measure performance. The launcher does not select it.

## Implemented diagnostic

`linux_ioctl_bridge.cpp` is an explicitly preloaded, process-local experiment,
not a Linux kernel module. Opening `/dev/dxg` creates a tracked memfd; matching
ioctls go to the bridge rather than the genuine WSL device. Descriptor identity
is checked, so closing/reusing a descriptor cannot redirect unrelated files.
The supported x64 subset is adapter enumeration/open/close, adapter queries,
device creation/destruction, paging queue/fence lifecycle in the owned-QEMU
mode, virtual context creation/destruction, and opt-in single standalone
video-memory allocation with typed list destruction and an opt-in, bounded
single GPU-address mapping per allocation, plus opt-in vendor allocation
residency with owned lists. Native
objects remain connection-local IDs. Unsupported calls fail with `ENOSYS`.

The adapter name is a consistent guest-local LUID. Adapter queries preserve
native data, with two documented guest adaptations: selecting the explicitly
supplied Linux UMD basename in the UTF-16 driver-store filename, and adjusting
public adapter flags for a render-capable PV guest. The flag behavior follows
Microsoft's [pinned WSL adapter-query source](https://github.com/microsoft/WSL2-Linux-Kernel/blob/d504d40ab83839cb61942a812a99d183c42227b9/drivers/hv/dxgkrnl/dxgvmbus.c#L4276).
Vendor-private query and context buffers are not patched. The diagnostic
advertises zero guest display sources; a future SDL scanout interface must
provide actual guest outputs.

Use exactly one transport: `WDDM_BRIDGE_PORT` selects the QEMU virtio port;
`WDDM_BRIDGE_WINDOWS_WORKER` starts one owned Windows executable from WSL using
binary pipes and `posix_spawn`. The entire ioctl/query transaction is serialized
on the connection. Replies and the version/capability handshake are validated;
RPCs have 10-second deadlines. Malformed responses disconnect the worker.
SIGPIPE is handled per thread without changing the application's disposition.
EOF releases the worker session. The WSL diagnostic also waits for its owned
interop child and reports whether it was reaped; it does not assume killing a
Linux relay kills a Windows process.

This interposer covers this probe's three-argument ioctl calls only. It does
not support general no-argument ioctl forms, fd duplication, kernel mmap/poll
semantics, production guest page pinning, or DRM/EGL integration. Arbitrary
guest pointers are never sent to Windows; the guest copies bounded data into
the typed protocol. It is deliberately excluded from the application runtime.

## Reproduce privately

Use the pinned `microsoft/DirectX-Headers` and `microsoft/libdxg` checkouts in
`sources.json`. WSL is used only as a source of already installed local runtime
libraries and as the build environment:

```sh
sh experimental/windows-native-gpu/build_runtime_diagnostic.sh \
  /local/DirectX-Headers /local/libdxg /local/runtime-build
python3 experimental/windows-native-gpu/make_runtime_initramfs.py \
  --init /local/runtime-build/runtime-init \
  --probe /local/runtime-build/runtime-probe \
  --shim /local/runtime-build/linux-ioctl-bridge.so \
  --linux-umd /usr/lib/wsl/drivers/LOCAL-DRIVER-DIRECTORY/libnvwgf2umx.so \
  --extra-runtime /usr/lib/wsl/lib/libnvidia-gpucomp.so \
  --output /local/runtime-build/private-runtime.cpio.gz
```

Choose a Linux UMD compatible with the host's installed NVIDIA driver. The
archive preserves the driver's local path because DXCore maps driver-store
filenames into `/usr/lib/wsl/drivers`. It includes the locally installed D3D12,
DXCore, NVIDIA compiler and ELF dependencies, and is bounded to 256 MiB before
compression. The compiler is loaded dynamically and is absent from `ldd`'s
dependency list; omitting it made the runtime fail early with `0x8007000e`.
Optional `--strace` and `--dependency-directory` support a caller-supplied
scratch tracer for failed syscalls only.

**Keep this image private.** It contains vendor binaries. The tool never
uploads anything, refuses existing output paths, records a local hash manifest,
and marks redistribution as unapproved. CI compiles the interposer and tests
owned fake workers; it neither packs nor publishes installed NVIDIA binaries.

On the Windows host, use `test_owned_runtime_qemu.py` with `--qemu`, `--firmware`,
`--kernel`, `--initramfs`, `--bridge`, a fresh `--report` path and
`--driver-allocations --minimum-vendor-allocations 1 --driver-gpuva
--minimum-vendor-gpuva-maps 1 --driver-residency --minimum-vendor-residency-requests 1
--expected-unimplemented-ioctl 37`.
Without the residency opt-in, the same image stops at ioctl 11. Without the
GPU-address opt-in, it stops at ioctl 12; without the
allocation opt-in, it stops at ioctl 6.
The native driver owner starts the hidden,
disk-free QEMU process, with no NIC, guest disk or host share. Use the QEMU build
with the dynamic fence hub; the [paging protocol and full command](LIVE-PAGING-BRIDGE.md)
describe its ownership contract. Acceptance requires live Linux UMD presence,
native device/context creation, direct fence reads, successful host unmap
acknowledgements, matching native allocation creation/destruction counts,
GPU-address mapping, residency and paging waits, direct guest retirement observations,
zero live mapped pages/residency attempts, the first unsupported Lock2 call after residency, a failed D3D12
result and clean native teardown. `diagnosticAccepted` is not an application
GPU-readiness signal. The older `test_runtime_qemu.py` still tests the explicitly
disabled paging baseline with a separately owned VM and worker.

Portable fault tests cover missing configuration, wrong protocol version,
closed/reused descriptors, malformed replies, closed workers and valid public
flag adaptation, disabled paging, unsupported priorities/adapter indices,
malformed paging replies and queue cleanup when the PCI hub is absent. They
also verify the owned worker exits after EOF. These tests
run in CI using the pinned public headers and no GPU hardware.

## Next driver interface

Live CPU allocation access through Lock2 is the next interface. Broader GPU virtual-address
operations, allocation-token translation, hardware queue creation/submission
and synchronization remain. Their successful integration must precede DRM/Mesa
and fenced SDL presentation, interactive Hyprland acceptance and performance
comparisons. The captured queue diagnostic's private-field patch is not a
production relocation rule.
