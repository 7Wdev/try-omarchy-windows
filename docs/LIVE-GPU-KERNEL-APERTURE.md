# Guest kernel memory-lease evidence

On 2026-10-12, two fresh owned QEMU/WHPX guests loaded the new
`wddm_aperture` Linux module and rendered the real Mesa/NVIDIA OpenGL workload
through kernel-managed native-memory mappings. Each run passed 581 kernel
contract checks, both independent 130x73 pixel audits, all 18 native command
retirements, and normal kernel-module, guest and host cleanup.

This implements a kernel component of the graphics memory path. It does not
implement `/dev/dxg` graphics ioctls in the kernel, a DRM render node, dma-buf,
KMS, a compositor, or the accelerated Omarchy desktop. The roadmap stays 4/8.

The [retained evidence](evidence/KERNEL-APERTURE-2026-10-12.json) contains both
runs' pixels, correlated native receipts, kernel checks, binary hashes and
normalized source hashes. The rendered frames match the
[previous OpenGL image](images/nvidia-guest-opengl-2026-10-11.png).

## What changed

The earlier diagnostic opened the private QEMU PCI BARs through root-only
sysfs files. The new optional path binds the two private PCI hubs to a Linux
driver. A trusted broker with `CAP_SYS_RAWIO` authorizes a bounded slice using
`/dev/wddm-aperture`. The kernel returns a close-on-exec anonymous FD with its
own inode. Recipients can map that slice without obtaining whole-BAR access.

Fence leases are read-only. Allocation leases are writable, page-aligned and
bounded by the advertised aperture and a 96 MiB aggregate CPU-view limit.
Concurrent overlapping writable leases are rejected. There are at most 256
leases. Cookies are monotonic, belong to a controller FD and cannot revoke
another controller's lease. Offset, length, version, flags and reserved fields
are checked; failed copyout rolls back the lease and FD reservation.

Revocation removes every recipient VMA's PTEs before allowing range reuse.
Later access faults with `SIGBUS`; the FD cannot recreate a revoked mapping.
Controller close revokes its outstanding leases. The driver's PCI removal
path also revokes leases before releasing the BAR. Active PCI removal during
rendering still needs a separate hardware test.

The C++ shim explicitly selects this path with
`WDDM_BRIDGE_KERNEL_APERTURE=1`; failure does not fall back to sysfs. Existing
diagnostics retain their original opt-in behavior. Control marshalling still
runs in the diagnostic process interposer. This run's whole OpenGL process
remains privileged; the separate FD recipient test runs as UID/GID 65534.

These FDs represent CPU views of native memory. They are **not dma-buf FDs**
and cannot yet be passed to a compositor as graphics buffers. A privileged
broker must authorize them from validated host mapping responses; the driver
does not independently communicate with Windows to establish a host lease.

## Retained results

| Check | Both final runs |
| --- | --- |
| Guest kernel | `7.2.8-arch1-2`, matching headers extracted read-only from the installed guest image |
| Isolation contract | 581 checks; malformed requests, bounds, quotas, overlap, copyout rollback, foreign/stale cookies, mapping permissions and controller close |
| FD transfer | SCM_RIGHTS to an unprivileged process; mapping succeeds, write/execute escalation fails, creation without capability fails |
| Cross-process revocation | Revoked recipient mapping faults with SIGBUS; remapping the FD fails |
| Kernel/native correlation | 71 fence leases and 91 allocation leases match every native mapping group |
| OpenGL renderer | `D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)`, Mesa 26.0.8 |
| Pixels | Two 130x73 frames; exact coverage/background/alpha, maximum RGB error one byte |
| GPU work | 18 native commands retired; peak 3 pending |
| Direct memory access | Zero emulated fence/allocation MMIO reads and writes |
| Guest cleanup | Zero active leases and CPU-view bytes; module unload succeeds |
| Host/VM cleanup | Exit 0 without forced VM stop; all native objects and mappings released |

The kernel module SHA256 is
`37f19773c7e5f04057b5853f78c0c189f0b61b77e453c90eb43fe7e4bce90bd9`.
The private image SHA256 is
`0a183309c16ca864cde168960a3a8215f161b3806b6b3f9451dfcc483ad20962`.
The image includes installed vendor binaries and must remain local.

The module was built with workspace Ubuntu GCC 16.0.1 and pahole 1.31.
The guest kernel records GCC 16.2.1 and pahole 1.32; these differences were
reported by the build. The actual matching-kernel load and both rendering runs
passed. This establishes the tested combination, not compatibility with other
kernels. CI additionally compiles against its prepared Linux headers and
never loads the module on the runner.

An earlier run correctly rejected executable mapping with `EPERM` at the
anon-inode noexec mount, while the contract expected `EACCES` from the driver.
That run submitted no GPU work and was not accepted. The contract now accepts
both permission-denial paths; execution remains prohibited.

## Reproduce privately

Build the guest-only module and static contract probe against prepared headers
that exactly match the guest kernel release:

```sh
sh experimental/windows-native-gpu/build_guest_aperture.sh <guest-kernel-build> <fresh-module-output>
```

Follow the [OpenGL private-image procedure](LIVE-GPU-OPENGL.md), additionally
passing `--guest-aperture-module <output>/wddm_aperture.ko` and
`--guest-aperture-contract <output>/aperture-contract` to the packer. Both inputs
are required together. Guest init loads the module, runs the isolation probe,
requires the kernel path for rendering, checks zero leases and unloads it.
Neither build script loads a module on Windows or the host WSL kernel.

The original D3D12-only acceptance guard remains false for this different
frontend. Use the independent combined verifier:

```text
python experimental/windows-native-gpu/verify_kernel_aperture_capture.py --report <driver.json> --output <fresh-audit.json>
```

General graphics ioctl/device integration, shared GPU resources and fences,
DRM/GBM/display integration, real Hyprland, broader APIs, long-run recovery and
the >=93% native performance target remain unfinished. This CPU-memory lease
component advances that integration without establishing those requirements.
