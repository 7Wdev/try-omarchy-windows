# Live NVIDIA Linux runtime in QEMU

The disk-free QEMU/WHPX guest now loads this machine's installed NVIDIA Linux
user-mode driver and routes its actual initialization calls through virtio
serial to the native Windows worker. On the RTX 5090 Laptop GPU, it completed
19 native adapter queries, including the 50,616-byte NVIDIA-private query, and
created a native WDDM device. This test uses no captured private input fixtures
and has no real WSL `/dev/dxg` available in the guest.

**D3D12 device initialization is incomplete.** The runtime fails with
`0x80004001` at `CreatePagingQueue` (ioctl 7), whose required synchronization
handle and directly mapped fence are not implemented in this guest API. An
optional feature query (ioctl 73) and registry queries are also unsupported.
All native objects were released and the guest and worker exited successfully.
[Hardware evidence](evidence/QEMU-LIVE-NVIDIA-RUNTIME-2026-10-08.json) records
the exact executable, image and kernel hashes and the negative initialization
result separately from diagnostic acceptance.

This is progress toward interactive Omarchy acceleration. It does not render,
submit guest GPU command buffers, run Hyprland, verify desktop stability or
measure performance. The launcher does not select it.

## Implemented diagnostic

`linux_ioctl_bridge.cpp` is an explicitly preloaded, process-local experiment,
not a Linux kernel module. Opening `/dev/dxg` creates a tracked memfd; matching
ioctls go to the bridge rather than the genuine WSL device. Descriptor identity
is checked, so closing/reusing a descriptor cannot redirect unrelated files.
The supported x64 subset is adapter enumeration/open/close, adapter queries,
device creation/destruction, and virtual context creation/destruction. Native
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

On the Windows host, run `test_runtime_qemu.py` with `--qemu`, `--firmware`,
`--kernel`, `--initramfs`, `--bridge` and a fresh `--report` path. It starts only
its own hidden worker and disk-free VM, with no NIC, guest disk or host share.
The current acceptance requires observed Linux UMD presence during its live
private query, the successful native device creation, the expected unsupported
paging call, a failed D3D12 result, and clean native teardown. Its
`diagnosticAccepted` field is not an application GPU-readiness signal.

Portable fault tests cover missing configuration, wrong protocol version,
closed/reused descriptors, malformed replies, closed workers and valid public
flag adaptation. They also verify the owned worker exits after EOF. These tests
run in CI using the pinned public headers and no GPU hardware.

## Next driver interface

The existing native backend can create paging queues, and the separate QEMU
fence experiment can directly map driver-owned pages. They are not yet
connected for queues created dynamically by this live runtime. The next step
must return a typed synchronization ID and guest fence mapping, with a private
host-to-QEMU mapping handshake and an acknowledgement before replying to the
guest. Queue destruction must unmap that page before releasing its native
owner. Do not send the Windows CPU pointer to the guest or substitute a stale
copied counter for the live completion fence.

After that, live allocation/private-data handling, GPU virtual-address
operations, allocation-token translation, hardware queue creation/submission
and synchronization remain. Their successful integration must precede DRM/Mesa
and fenced SDL presentation, interactive Hyprland acceptance and performance
comparisons. The captured queue diagnostic's private-field patch is not a
production relocation rule.
