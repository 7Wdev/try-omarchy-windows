# Native NVIDIA texture FDs through QEMU

Two fresh owned QEMU/WHPX guests export and import a genuine NVIDIA-backed
3840×2160 RGBA8 texture through a typed Linux FD. The guest writes the source
texture on the GPU, reads all 8,294,400 pixels through the imported resource,
releases the source, then writes and reads all pixels of the import again.
Both guests exit normally and release all native resources without a forced
VM stop. An actual GPU wrong-color control fails at the pixel check as intended.

This establishes **native resource FD storage sharing**. It does not establish
dma-buf/DRM PRIME, GBM integration, a Hyprland session, continuous presentation,
general OpenGL/Vulkan/CUDA/video support, NVIDIA feature coverage, stability
under sustained load, or ≥93% native performance. The README remains at four
of eight backend stages and zero of four desktop acceptance gates.

## What changed

The opt-in resource protocol uses operations `0x2090`–`0x2094` to negotiate,
export, close, query and import owned texture references. A connection can hold
at most 16 resource references. Imports are limited to one allocation, bounded
private metadata, the owning adapter, and the existing object, GPU memory,
CPU mapping and command budgets. Resource and synchronization references use
different wire and kernel FD types. Raw Windows handles and host CPU pointers
do not cross the wire.

The Windows worker uses genuine `D3DKMTShareObjects`,
`D3DKMTQueryResourceInfoFromNtHandle` and `D3DKMTOpenResourceFromNtHandle` calls.
An NT reference retains its own runtime metadata and remains owned after
source-resource destruction. Native imports enter the existing allocation
translation, residency, GPU address and destruction paths. Failed publication
rolls back native objects; disconnect cleanup releases outstanding references.

The Linux runtime's resource metadata is preserved independently of the
Windows runtime metadata. In these captures, the sizes are 264 Linux runtime
bytes, 280 Windows runtime bytes and 586 native driver bytes. The Windows data
is consumed on Windows; it is not substituted for the Linux resource descriptor.
The real Linux runtime supplies a four-byte optional mutex initializer for
this ordinary texture. The profile accepts that observed input while requiring
the native open to return no keyed mutex or sync object. Keyed mutex support
is not claimed.

The guest kernel extends the existing authenticated object FD with resource
kind 2. Ownership, exact type, ticket uniqueness, duplicate-FD lifetime and
last-close reaping apply to both kinds. A resource FD cannot be resolved as a
fence, mapped as a CPU buffer, or resolved through another controller. An
exported reference retains its owning adapter after the source device is gone.

Microsoft documents the native query and open structures here:
[resource query](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_queryresourceinfofromnthandle),
[resource open](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_openresourcefromnthandle).

## GPU checks and retained failures

The private diagnostic borrows the real Mesa D3D12 device and queue. It creates
a shared render-target texture, obtains a typed resource FD, imports it through
Linux D3D12 and closes the descriptor. It then performs:

| Round | GPU write | Read through | Source lifetime | All pixels | Full-image FNV-1a |
| --- | --- | --- | --- | --- | --- |
| 1 | Source cleared red | Imported resource | Source still alive | 8,294,400 × `ff0000ff` | `95c9d10519374325` |
| 2 | Import cleared green | Imported resource | Source released; FD closed | 8,294,400 × `00ff00ff` | `842d8f16d3d2a325` |

The CPU readback is a diagnostic. It reuses a 7,864,320-byte buffer in five
bands per round. Each band waits for a GPU queue retirement signal before
mapping and checking its pixels. The guest computes the digest from the
observed pixel bytes; the independent Python auditor computes the expected
digest for the complete image. It also validates the existing two 130×73
OpenGL shader frames against their independent pixel oracle. These clears do
not establish 4K OpenGL conformance or a 4K desktop presentation path.

Both final-source captures retire 48 native commands, pass the 581 aperture,
561 existing object and 15 new resource-FD contract checks, unload the kernel
module, and finish with zero live native objects, CPU mappings or FD references.
An intentionally blue source fails the red-pixel check with guest exit 1;
its native resources and owned VM still clean up normally.

The earlier full-buffer diagnostic also remains in the evidence as a failure:
a contiguous 33 MB CPU mapping exhausts the 128-slot aperture, and the UMD
reports device removal after that allocation failure. The final banded
diagnostic avoids that large lease. **General CPU aperture fragmentation and
capacity have not been fixed.** A successful banded check must not be used to
claim that arbitrary 4K readbacks or long desktop sessions are stable.

The legacy initialization wrapper returns 1 and retains
`diagnosticAccepted=false` for these captures. It expects a different runtime
initialization contract. Acceptance here comes from the explicit resource-FD
auditor; none of those legacy report fields were rewritten into successes.

## Evidence and reproduction

[RESOURCE-FD-2026-10-12.json](evidence/RESOURCE-FD-2026-10-12.json) retains two
complete final-source positive captures, the real wrong-color control, the
full-buffer failure, normalized source hashes and private binary/image hashes.
Vendor libraries and private runtime images remain local.

Build the Windows diagnostic with `build.ps1 -HighResolutionDiagnostic` and
the guest module/contract with `build_guest_aperture.sh` against matching guest
kernel headers. The resource probe is explicitly selected with
`WDDM_BRIDGE_TEST_RESOURCE_FDS=1` when calling `build_opengl_probe.sh`, with
`WDDM_BRIDGE_MESA_SOURCE` pointing to pinned Mesa 26.0.8 and
`WDDM_BRIDGE_DIRECTX_HEADERS` to the pinned DirectX-Headers checkout. The
explicit wrong-color build additionally sets
`WDDM_BRIDGE_TEST_RESOURCE_BAD_COLOR=1`; normal builds leave it unset.

Pack and run the private OpenGL guest through the owned-QEMU harness using
the existing native-metadata, shared-resource, asynchronous-submit and
128-slot CPU opt-ins. Audit its `driver.json`, `driver.log` and
`driver.host.log` together:

```powershell
python experimental/windows-native-gpu/verify_resource_fd_capture.py `
  --report <capture-directory>/driver.json --output <fresh-audit.json>
```

The wrong-color control requires the explicit `--wrong-color-control` option;
the normal auditor rejects it. Outputs must use fresh paths. Eight corruption
test groups reject wrong pixels, incomplete coverage, missing/reordered GPU
retirement, lifetime errors, metadata substitution, handle exposure, leaks,
abnormal exit and attempts to disguise unrelated failures as the control.

## Remaining desktop work

Native NT-backed resource FDs are not dma-buf objects. This profile is scoped
to one bridge connection/controller; cross-process compositor/client sharing
still needs a VM-wide lifetime and ownership design. The GBM/allocator path,
compositor buffer import, synchronization and continuously displayed frames
remain ahead of the first accelerated Hyprland frame.

The wider product goal also includes concurrent Windows/guest GPU use,
NVIDIA-specific feature coverage, CUDA, Vulkan, OpenGL and video acceleration.
Each needs real application tests. The ≥93% target requires matched native
benchmarks on the same GPU, driver, power conditions, resolution and host load;
no performance acceptance follows from these correctness checks.
