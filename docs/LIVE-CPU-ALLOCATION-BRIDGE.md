# Live NVIDIA CPU allocation sharing in QEMU

The later [live hardware queue route](LIVE-HARDWARE-QUEUE-BRIDGE.md) advances
beyond this 64 KiB acceptance boundary. It adds translated allocation aliases,
hardware queues and bounded exact committed CPU subregions inside larger
reservations. The evidence below records the earlier CPU milestone.

The NVIDIA Linux runtime can now lock its native Windows allocation and obtain
a guest CPU pointer into the actual driver-owned pages. On the RTX 5090 Laptop
GPU, physical QEMU/WHPX acceptance passed a 64 KiB allocation, 10,000 direct
guest reads and normal guest-unmap/QMP-unmap/native-unlock cleanup. The next
unsupported initialization operation is hardware queue creation (ioctl 24).
D3D12 device creation still returns `80004001`; this does not accelerate
Omarchy/Hyprland or measure near-native performance.

The [metadata-only evidence](evidence/QEMU-LIVE-CPU-ALLOCATION-2026-10-08.json)
contains normal, explicit store/reference, disabled-capability, locked-allocation
EOF and existing memory/GPU-copy regression cases. Live tests use installed
Linux runtime binaries, with no captured private request replay and no WSL
`/dev/dxg` forwarding. The private initramfs and vendor binaries are not published.

## Owned memory contract

Capability `2048` and operations `0x2054`/`0x2055` are separately enabled by
`--driver-cpu`. This requires allocation and GPU-VA opt-ins and an owned
`--run-qemu` process; ordinary stdio/listen endpoints reject the CPU opt-in.
Requests identify a typed allocation and matching device, with flags zero.
Lock requires a zero-offset GPU mapping. Native failures remain NTSTATUS with
zero CPU output. Success returns byte count, slot offset and generation,
never a Windows CPU address.

The host retains the native lock before any validation or mapping can fail.
In this acceptance version, eligible memory starts at its region/allocation base, is aligned and committed
`MEM_PRIVATE`, has read/write protection optionally with write combining, and
has the exact byte extent of the bounded GPU mapping. A reserved tail of the
same reservation is permitted but never exposed. This uses
[VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery)
to check accessible pages; it does not decode opaque vendor allocation sizes.
Unsupported layouts are unlocked and rejected. CPU views are bounded to
1 MiB each and 16 MiB total; the existing reported-memory guard is still not a
hard vendor allocation-byte quota.

The private `wddm-allocation-hub` gives each allocation a generation-owned,
bounded writable WHPX slot with no execute permission. Linux maps its PCI BAR
with `MAP_SHARED` and read/write protection. It verifies direct access using
10,000 loads and the hub's emulated read/write counters. Repeated process-local
locks reuse the pointer with at most 64 references. An intermediate unlock
retains the view; the final unlock unmaps the guest view before native QMP
unmapping and `D3DKMTUnlock2`. Allocation destruction drains guest lock state
before synchronous native destruction. Failed native unlocks retain ownership
for retry. Malformed replies or mapping failures permanently close the transport.

On disconnect, the native server stops and reaps its owned QEMU before session
cleanup releases any driver pages. The physical EOF control exited the guest
probe while its CPU lock was still held: no guest/QMP unmap acknowledgement was
claimed, QEMU exited normally, then Windows unlocked and destroyed the allocation.
All native objects, CPU bytes and QEMU mappings were zero afterwards. Control
failures also stop the owned VM before freeing pages. Forced-stop/control-failure
injection and GPU reset recovery are not covered by this hardware acceptance.

## Hardware acceptance and scope

| Case | Result |
| --- | --- |
| Live initialization | One native 64 KiB CPU lock, 10,000 direct reads, acknowledged unmap, native unlock; then ioctl 24 |
| Explicit store/reference control | Same guest pointer on repeated lock; intermediate unlock retained it; first/last-word stores reached Windows and both original words were restored; zero MMIO reads/writes |
| CPU capability disabled | Remained at ioctl 37; zero native CPU locks or allocation mappings |
| Guest exits holding a lock | VM exited before native unlock/destruction; no unmap acknowledgement substituted for VM exit |
| Existing memory regression | Six allocation cycles, 393,216 CPU roundtrip bytes, six fenced GPU copies totaling 262,443 bytes, five vendor contexts and 22 vendor queries; full cleanup |

Store/reference checks are explicit diagnostics, not writes performed by the
NVIDIA UMD. They touch two words only after initialization has failed, verify
host visibility and restore those words before native unlock. The unmodified
live initialization test observed no changed CPU contents at this boundary.
This milestone ran protocol/bounds/ownership tests and 40 Linux interposer fault
cases without NVIDIA binaries. The hardware queue route expands this to 69
cases. This remains a local initialization diagnostic;
kernel/DRM integration, hardware queues, synchronization objects, command
submission, scanout, Hyprland, stability and performance work are still required.

## Reproduce locally

Build the native worker with `experimental/windows-native-gpu/build.ps1`.
Build the source-only guest shim/probe/init with `build_runtime_diagnostic.sh`,
then pack a fresh private runtime image with `make_runtime_initramfs.py` using
the installed NVIDIA UMD and its compiler dependency. Use the successful
[QEMU build](https://github.com/7Wdev/try-omarchy-windows/actions/runs/37822698638)
from commit `305d4b7345bf2198292b492122b414d3552a7dff`, including its corresponding
GPL source archive. Its binary hash is pinned in the evidence.

```powershell
python experimental/windows-native-gpu/test_owned_runtime_qemu.py `
  --qemu C:/local/qemu/bin/qemu-system-x86_64.exe `
  --firmware C:/local/qemu/share/qemu --kernel C:/local/vmlinuz-linux `
  --initramfs C:/local/private-runtime.cpio.gz `
  --bridge C:/local/driver-bridge.exe --report C:/local/fresh-cpu-report.json `
  --expected-unimplemented-ioctl 24 --driver-allocations --minimum-vendor-allocations 1 `
  --driver-gpuva --minimum-vendor-gpuva-maps 1 `
  --driver-residency --minimum-vendor-residency-requests 1 `
  --driver-cpu --minimum-vendor-cpu-locks 1
```

For separate explicit controls, add `--cpu-store-test` or `--cpu-eof-test`.
Use fresh report paths for every run. A capability-disabled control omits CPU
flags and expects ioctl 37. These VMs have no guest disk, NIC, host share or
existing Omarchy instance attached. The launcher remains on its existing path.
