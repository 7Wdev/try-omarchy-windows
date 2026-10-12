# Native fence sharing through guest kernel FDs

On 2026-10-12, two fresh owned QEMU/WHPX guests exported a native NVIDIA
monitored fence as a kernel-authenticated Linux FD, transferred it through an
unprivileged child, and imported it into another device in the original broker
session. The original fence and device were destroyed before import. The alias
retained initial value 57 and could be re-exported after the original FD closed.
Both guests also passed the real Mesa/OpenGL pixel checks and normal cleanup.

This advances guest synchronization ownership. The roadmap remains **4/8**:
these opaque FDs are not Linux sync_file or dma-buf objects, and Mesa semaphore
FD import, guest GPU timeline updates and an accelerated Hyprland desktop
remain unverified. The child transfers the FD; native import happens in the
parent's existing broker session. Independent process/broker imports are not
implemented by this test.

The [retained evidence](evidence/SYNC-FD-2026-10-12.json) includes both guest
runs, two separate native GPU probes, normalized source hashes, binary hashes,
native receipts and independent audits. The OpenGL pixels reproduce the
[previous shader image](images/nvidia-guest-opengl-2026-10-11.png).

## Lifetime and protocol

The bridge explicitly negotiates a versioned shared-fence profile. Each session
allows at most 16 exported native references and 96 synchronization objects.
The wire carries bounded session tickets; native NT handles remain on Windows.
Tickets belong to the adapter, allowing their original fence and device to be
destroyed before an alias is imported into another owned device on that adapter.

The Linux module creates a close-on-exec anonymous object FD with immutable
controller identity, kind, cookie and ticket. Only a broker with CAP_SYS_RAWIO
can create it. Resolution checks the actual kernel file operations and
controller identity; copying ticket bytes into a memfd cannot forge it. Object
FDs expose no memory mapping or readable contents. There are at most 256 held
objects, including closed objects awaiting broker reap.

dup and SCM_RIGHTS hold the real file reference. Last close or explicit revoke
queues the ticket for its broker to reap and close the native NT reference.
Failed copyout rolls back creation or preserves a pending reap. Controller
close invalidates outstanding object FDs. Broker disconnect separately releases
native references; closing an NT handle does not destroy already imported KMT
aliases. This is a trusted-broker diagnostic, not a complete Linux graphics
device or cross-session handle service.

## Results

| Check | Each final guest run |
| --- | --- |
| Kernel object contract | 561 checks: malformed requests, forged files, controller isolation, dup lifetime, revoke, quota, copyout rollback and reap |
| Capability restriction | Unprivileged object creation denied |
| Existing memory contract | All 581 checks remain passing |
| Native handle lifetime | 2 exports, 1 import, 2 closes, zero live NT references |
| Import after source destruction | Initial value 57 preserved; imported alias re-exported |
| FD transfer | UID/GID 65534 child receives and returns FD using SCM_RIGHTS |
| OpenGL | Two 130x73 frames, independent pixel audits, maximum RGB error one byte |
| GPU commands | 18 retirements, peak 3 pending |
| Kernel/native memory correlation | 73 fence leases and 91 allocation leases |
| Exit | Zero held/active object FDs, zero memory leases, module unload, host and QEMU exit 0 without forced stop |

Separate **native Windows** probes import one GPU-accessible shared KMT fence
into a second KMT device and an ID3D12Fence. A real D3D12 queue signals values
1 through 16. Both CPU fence mappings and D3D12 completion agree for values
1 through 8; the imported mapping and D3D12 completion agree for 9 through 16
after the original fence and NT handle close. The imported KMT alias can be
re-exported and reopened as a D3D12 fence at value 16. No CPU stores write the
fence. Both probes release all native objects successfully.

The guest FD test currently uses NoGPUAccess and checks initial value 57.
Native Windows GPU signaling and guest FD lifetime are separate proofs;
combining their summaries does not establish guest GPU timeline updates.

## Remaining Mesa integration

In the tested Mesa 26.0.8 source, the D3D12 driver supplies
`screen.create_fence_win32` but does not supply `context.create_fence_fd`.
The GL FD semaphore import frontend calls the latter callback, while the
driver advertises fence signaling support. Directly invoking that unsupported
callback path would not be valid semaphore evidence. No GL semaphore FD import
success is claimed here.

A correct frontend/driver implementation must define compatible timeline or
binary semantics, import the real guest FD, and submit GPU queue waits and
signals. The bridge still lacks the required GPU queue wait operation. DRM/GBM
buffer sharing, continuous scanout, Hyprland, general application compatibility,
long runs and matched native performance measurements also remain ahead.

## Reproduce privately

Build the guest module as described in [kernel memory leases](LIVE-GPU-KERNEL-APERTURE.md).
To add the FD diagnostic to the private OpenGL probe, set these build variables:

```sh
export WDDM_BRIDGE_TEST_SYNC_FDS=1
export WDDM_BRIDGE_DXG_HEADERS=<pinned-libdxg-source>
export WDDM_BRIDGE_DIRECTX_HEADERS=<pinned-DirectX-Headers-source>
sh experimental/windows-native-gpu/build_opengl_probe.sh <mesa-prefix> <fresh-output>
```

Pack the probe, shim and matching guest module using the existing private-image
procedure. It contains installed vendor binaries and stays local. Audit the
captured run with:

```text
python experimental/windows-native-gpu/verify_sync_fd_capture.py --report <driver.json> --output <fresh-audit.json>
```

`sync-sharing-probe.exe`, built by the native build script, runs the separate
Windows GPU queue test. CI compiles that probe and the guest helper and runs the
190-check sharing protocol contract, capture corruption tests and kernel build;
it does not execute vendor GPU workloads or load the module on the runner.
