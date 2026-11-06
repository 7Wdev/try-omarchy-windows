# Guest OpenGL shader evidence

On 2026-10-11, a private Mesa 26.0.8 D3D12 frontend created a desktop OpenGL
context and rendered two GLSL triangles inside the owned QEMU/WHPX guest.
The renderer reported `D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)` and
`4.6 (Core Profile) Mesa 26.0.8`. Both 130Ã—73 readbacks passed the independent
integer triangle reference. This verifies this workload, not OpenGL 4.6
conformance, 4K OpenGL, a compositor, or a desktop.

<img src="images/nvidia-guest-opengl-2026-10-11.png" width="520" alt="Actual second OpenGL GPU readback: interpolated color triangle on black">

The PNG contains the actual second GPU readback, enlarged here for visibility.
Its original dimensions are 130Ã—73. [Machine-readable receipts and pixels](evidence/OPENGL-2026-10-11.json)
retain both frames and the native command/cleanup records.

## What passed

| Check | Retained result |
| --- | --- |
| Frontend | Real Mesa EGL and desktop OpenGL; GLSL compiled by Mesa's D3D12 driver |
| Transport | QEMU virtio serial and the Windows native bridge; no forwarding to the host WSL `/dev/dxg` |
| Pixels | 9,490 pixels per frame, 2,680 triangle pixels; both frames independently checked |
| Maximum channel error | 1 byte in both frames; exact background, alpha and coverage |
| Native work | 18 commands accepted and retired; no failed submissions or retirement timeouts |
| Pending work | Peak 3 commands; limit 16 |
| Objects | Peak 123 vendor allocations; limit 128 |
| Reservations | 9 created; peak 4,840,947,712 bytes of GPU address space |
| CPU aperture | Peak 30,478,336 bytes; limit 96 MiB |
| Cleanup | Guest runtime exit 0, host exit 0, owned QEMU exit 0 without a forced stop; all native objects and guest mappings released |

The final run used bridge SHA256
`c2121ffb9629294c8fcd219db2deaaa32383c4f2c5e0106a5ea13d54176973f3`
and private image SHA256
`180fba18c15229f664947d053b940c91041c40c5c10864beca15008c960ab0a4`.
The evidence records the exact hashes; the private image contains installed
vendor binaries and must not be uploaded or redistributed.

## Changes that enabled the frontend

- Accept the documented `GLON12` context client hint, alongside `DX12`, with
  the existing context flag, device ownership and private-data bounds.
- Forward the two observed NT-shareable monitored-fence flag combinations
  (`131` and `1155`) unchanged to Windows. NT sharing correctly has no global
  D3DKMT handle. NT-handle export/import remains unimplemented.
- Validate residency on the submitted command allocation rather than rejecting
  a submission because an unrelated allocation is not resident. The command
  must still have exactly one owner, a resident GPU mapping and a valid CPU
  lock. Opaque UMD references remain subject to native VidMm validation.
- Add an explicit two-frame OpenGL negotiation before creating GPU objects.
  It permits 64 total commands and 16 reservations. The pending-command limit
  stays 16 and the address-space reservation byte limit stays 16 GiB. Older
  diagnostics retain their 16-command and 8-reservation limits.
- Pack an explicitly supplied private Mesa install and its ELF dependencies.
  The standard guest stays at 512 MiB; the explicit high-resolution lab uses
  1 GiB to fit the private initramfs and frontend.
- Account separately for acknowledged live-guest unmaps and mappings retained
  until the owned VM has exited. Native backing outlives those guest mappings.

The investigation retained the earlier failures: insufficient initramfs RAM,
rejected GLON12 hint, rejected shared-fence flags, the unrelated-residency
check, and the old reservation quota. They were not accepted as rendering
successes. A first successful two-frame run was repeated with explicit
VM-exit mapping accounting; both runs retired 18 native commands.

## Reproduce locally

Build a private, non-GLVND Mesa prefix with only the `d3d12` Gallium driver,
EGL/GBM enabled, and LLVM, GLX, Vulkan and video disabled. The retained source
is [Mesa 26.0.8](https://archive.mesa3d.org/mesa-26.0.8.tar.xz), archive SHA256
`caf1c0061a68e88dfa74967a7e780c0e85d65b6c4e334cd69095a5dc54ad78bc`.
Its DirectX-Headers subproject is commit
`64c650b0c6642ab0f3cdf674e38401e02ccd2460`.

Use `build_opengl_probe.sh` with that prefix; compile the Linux shim with
`WDDM_BRIDGE_4K_LAB=1` and the pinned DirectX-Headers/libdxg inputs used by
the other diagnostics. Build the Windows bridge with `-HighResolutionDiagnostic`.
Pack a fresh private image using `make_runtime_initramfs.py` with
`--runtime-workload opengl --mesa-runtime-directory <prefix>` and the locally
installed NVIDIA Linux UMD and GPU compiler. The runtime libraries remain
private local dependencies.

Run the owned harness with `--runtime-workload opengl` and the allocation,
mapping, residency, CPU, translation, queue, sync, reservation, GPU-state,
submission and retirement opt-ins. The 4K host profile still requires its
128 CPU slots, native metadata, shared-resource/consumer prerequisites and
a fresh capture directory. This OpenGL probe performs no texture handoff.

The original D3D12 harness acceptance deliberately remains false for this
different probe. Audit its report and adjacent guest/host logs with:

```text
python experimental/windows-native-gpu/verify_opengl_capture.py --report <driver.json> --output <fresh-audit.json>
```

The separate verifier requires both pixel frames, matching native command
retirements, genuine runtime provenance, the negotiated limits and complete
owned-VM cleanup. CI tests corrupt real receipts and run 21 fake-transport
cases, plus the standard-profile rejection case, without a GPU or vendor code.

## What remains

This is still a process interposer in a disk-free diagnostic guest. It has no
DRM render node or compositor display path, and has not run Hyprland. The
probe uses a surfaceless EGL pbuffer, blocking readback and two frames; it is
not a throughput benchmark. Some optional runtime ioctls remain unsupported,
and some native objects are retained safely until VM exit. These lifetimes
need further work for applications that repeatedly create and destroy devices.

Mesa advertises memory-object and semaphore extensions, but the bridge has
not implemented or validated their FD/NT-handle import/export path. An
advertised GL extension is not proof that this backend can use it correctly.
Vulkan, CUDA, video, 4K OpenGL, long-run stability, recovery and the â‰¥93%
native target remain unverified. The roadmap therefore remains **4/8 stages**.

Protocol flag meanings are documented by [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_synchronizationobject_flags).
Mesa documents the frontend in its [D3D12 driver guide](https://docs.mesa3d.org/drivers/d3d12.html).
