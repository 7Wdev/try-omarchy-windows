# Guest shared texture and native Windows resource import

Two fresh owned QEMU/WHPX runs on 2026-10-10 passed shader drawing in a shared
NVIDIA texture and a separate Windows D3D12 import of that resource. Each run
verified both 130×73 RGBA8 shader rounds, retired all 13 native commands and
two hardware queue signals, exited normally and released every bridge-owned
object. This clears a prerequisite for a Windows GPU presentation path.

The subsequent [Windows GPU consumer checkpoint](LIVE-GPU-SHARED-TEXTURE-CONSUMPTION.md)
adds an explicit COMMON/fence handoff and verifies native GPU copies of two
different guest frames. The import-only workload described here retains its
original scope and resource-state behavior.

The native import occurs during allocation creation and is released before
guest drawing. These tests establish resource compatibility and guest shader
correctness separately. They do not establish that a Windows GPU consumer sees
the rendered pixels, synchronized handoff or a displayed frame.

[Selected hardware evidence](evidence/QEMU-LIVE-GPU-SHARED-RESOURCE-2026-10-10.json)
includes complete cleanup, command retirement, signal and import receipts,
source/executable hashes and hashes of the original local reports and logs.

## Bounded allocation route

The Linux NVIDIA runtime requests a single-allocation resource with allocation
flags `71`: `CreateResource | CreateShared | NonSecure | NtSecuritySharing`.
The runtime supplies 264 bytes of opaque resource metadata. The new negotiated
`CreateSharedVendorResourceAllocation` operation preserves this metadata and
the existing allocation-private input/output data through native
`D3DKMTCreateAllocation2`. The shim rejects changed runtime metadata in a reply.

The route requires `--driver-shared-resources`, explicit allocation opt-in and
an owned QEMU runtime. Existing nonshared resources use their original route.
Runtime metadata is bounded to 1,024 bytes, with a combined packet limit of
4,096 bytes including its header and descriptor. Allocation-private data keeps
its existing 4,000-byte bound. There is one allocation per resource, with
typed device/resource ownership, unchanged object and memory limits and native
destruction after actual command retirement. System-memory allocations,
per-resource driver-private data and resource reuse remain unsupported here.

The diagnostic worker calls native `D3DKMTShareObjects` for its owned resource,
then creates a Windows D3D12 device on the same adapter LUID and calls
`OpenSharedHandle` for `ID3D12Resource`. Both fresh runs return `S_OK`, with:

| Property | Imported value |
| --- | --- |
| Dimension | `TEXTURE2D` |
| Width × height | 130 × 73 |
| Format | `R8G8B8A8_UNORM` |
| Resource flags | `ALLOW_RENDER_TARGET` |
| Native resource import | `S_OK` |
| Separate heap-interface check | `E_NOINTERFACE` |

The unnamed, noninheritable NT handle stays inside the Windows worker. The
worker releases the imported resource and closes the handle before returning.
No raw NT handle, name or security descriptor is sent over the guest protocol.
This does not implement Linux sharing file descriptors, dma-buf or a DRM node.
The independent heap-interface check diagnoses the object type; it is not
required for resource import success.

## Shader correctness and controls

The shared workload uses the same validated vertex/pixel shaders and independent
integer coverage oracle as the [ordinary shader diagnostic](LIVE-GPU-SHADER-TRIANGLE.md).
All 9,490 pixels in each round are checked. Background and alpha match exactly;
interpolated triangle RGB permits two UNORM steps, with one step observed.
Both shared runs reproduce the previously published final-round image exactly:

![Actual guest shader pixels reproduced by the shared-texture runs](images/nvidia-guest-shader-triangle-2026-10-10.png)

Actual GPU readback enlarged 6× with nearest-neighbor sampling. Dense RGBA
SHA-256: `fd894f123883a31e65cea02dc84eb9a52f0356511b4a85eb41977d8c46814353`.
This image is correctness readback, not a screenshot of Windows presentation.

| Physical test | Result |
| --- | --- |
| Shared texture shader drawing and native import, first run | Accepted |
| Shared texture shader drawing and native import, fresh repeat | Accepted |
| Original nonshared shader image, sharing opt-in disabled | Accepted |
| Nonshared shader image requested as shared-resource workload | Rejected |
| Shared-resource image requested as ordinary shader workload | Rejected |

All five runs retire 13 commands and two hardware queue signals, destroy all
124 allocations and both resources, and finish with verified native cleanup.
The accepted shared runs peak at three pending commands and 108 live
allocations; native and QEMU capacity limits were not increased. The wrong-mode
controls cannot set either public shader/shared success field even when their
underlying shader pixels are correct.

An earlier native resource import returned `E_NOINTERFACE`; its rejected report
and clean teardown remain preserved. Its cause is unresolved. A subsequent
pre-guard run imported successfully but its old verifier rejected shared
resources; that original rejection is also preserved. The final two accepted
runs use fresh reports and the strict shared-workload guard. Adding a separate
heap-interface check did not demonstrate a fix for the earlier resource-import
failure. These limited runs do not establish import stability or reset recovery.

## Reproduction and remaining work

Build the native worker with the existing `build.ps1` and build the guest
probe/shim using the [runtime instructions](LIVE-NVIDIA-RUNTIME.md). Select
`--runtime-workload shared` both when packing a fresh private image with
`make_runtime_initramfs.py` and when running `test_owned_runtime_qemu.py`.
Use the explicit capabilities of the [graphics clear diagnostic](LIVE-GPU-CLEAR.md),
including asynchronous submission, and additionally select
`--driver-shared-resources`. No QEMU change is required beyond the recorded
128-slot fence/aperture build. Installed proprietary runtime libraries and
private images remain local and are not redistributed.

MSVC `/W4 /WX` and GCC warnings-as-errors builds pass. The native contract
checks, 181 Linux ioctl fault cases, 73 native CLI rejection cases and 28
milestone guard groups pass. The shared guards require exact API results,
resource metadata, close/release receipts, workload markers and independent
shader verification; plain shader drawing cannot satisfy shared acceptance.

The subsequent consumer checkpoint verifies a resource-state/fence handoff and
a retained Windows GPU consumer. Swapchain presentation remains unfinished. The
current guest leaves the target in `RENDER_TARGET` state after verification;
no cross-device consumption is attempted. A production guest kernel/DRM
transport, Linux sharing semantics, Omarchy/Hyprland integration, the requested
APIs and the minimum 93% native performance remain unfinished. CPU readback
serves this diagnostic's correctness check, not the planned desktop path.
See [product acceptance gates](PRODUCT-ACCEPTANCE.md).

Microsoft documents [D3D12 shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps),
[`OpenSharedHandle`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-opensharedhandle)
and [`D3DKMTShareObjects`](https://microsoft.github.io/windows-docs-rs/doc/windows/Wdk/Graphics/Direct3D/fn.D3DKMTShareObjects.html).
The [WSL dxgkrnl sharing implementation](https://github.com/microsoft/WSL2-Linux-Kernel/blob/linux-msft-wsl-6.6.y/drivers/hv/dxgkrnl/ioctl.c)
is a reference for the additional file-descriptor and lifetime semantics still
needed on the guest side; no kernel code was copied into this route.
