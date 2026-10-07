# Windows NVIDIA backend investigation

Status on 2026-10-07: native host experiment implemented and physically tested;
the requested driver-level Linux guest backend is **not implemented**.
This experiment does not accelerate the Omarchy VM. The launcher continues to
use WINQ-EMU VirGL/Venus and its existing CPU fallback.

## Desktop requirement

Try Omarchy runs an Arch/Hyprland desktop inside QEMU with WHPX and an SDL window.
The outcome must be an accelerated compositor and applications presented in that
window, with input, resizing and existing host integration. An offscreen render,
CUDA query, native host window or encoded stream alone does not satisfy this.
Upstream disables Venus automatically on NVIDIA hosts because its measured
Vulkan path can hang the compositor or QEMU; forcing it on is not a solution.

## Reuse of virtio-nvgpu

Reviewed source revision:
[`21a4f075d5c01c0865c809a8592e40b860435768`](https://github.com/nestrilabs/virtio-nvgpu/tree/21a4f075d5c01c0865c809a8592e40b860435768).
The checked-out source takes precedence over cached README versions.

| Component | Reuse on Windows |
| --- | --- |
| 16-byte little-endian header: type, handle, signed status, padding | Useful framing, independently implemented in the experiment. |
| Control/event queues, bounded dispatch and handle ownership | Useful architecture. No Windows virtqueue adapter is implemented here. |
| Device ID 45, version/GPU slots/FD translation config | Existing Linux guest contract, not an ID assigned to this experiment. Advertising it without implementing its ABI would mislead the guest. |
| OPEN/CLOSE/IOCTL/MMAP/MUNMAP and proc/sys operations | Encode Linux NVIDIA semantics; cannot be remapped by renaming host calls. |
| `HostDriver::ioctl(RawFd, request, arg)` | Calls Linux `libc::ioctl`; unsuitable for Windows handles and WDDM semantics. |
| Shared GPU window, UVM aperture and readiness events | Need Windows allocation, mapping, residency and synchronization support. A shared host HANDLE does not establish a guest mapping. |
| Guest DRM render node and dma-buf import/export | Need compatible guest userspace and presentation; a render node alone is not a KMS display. |
| Vulkan Video streaming output | The upstream architecture's frame-output approach. The local desktop requires a separate scanout/presentation bridge. |

Evidence: [`messages.rs`](https://github.com/nestrilabs/virtio-nvgpu/blob/21a4f075d5c01c0865c809a8592e40b860435768/protocol/src/messages.rs),
[`virtio.rs`](https://github.com/nestrilabs/virtio-nvgpu/blob/21a4f075d5c01c0865c809a8592e40b860435768/device/src/virtio.rs),
[`host.rs`](https://github.com/nestrilabs/virtio-nvgpu/blob/21a4f075d5c01c0865c809a8592e40b860435768/device/src/nvidia/host.rs),
and [`architecture`](https://github.com/nestrilabs/virtio-nvgpu/blob/21a4f075d5c01c0865c809a8592e40b860435768/ARCHITECTURE.md).

## ABI barrier

Unmodified NVIDIA Linux userspace expects Linux RM object classes, ioctl
payloads, descriptor events, mmap behavior and compatible driver versions.
Windows' NVIDIA driver participates in WDDM instead. `DeviceIoControl` does not
implement a Linux ioctl number. `D3DKMTEscape` carries vendor-private data; it is
not a documented translation of Linux NVIDIA RM operations. Generated Linux
ABI tables describe input structures but do not supply the missing translation.

Microsoft documents [WDDM GPU paravirtualization](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-paravirtualization)
and [D3DKMTEscape](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtescape).
NVIDIA's [open kernel modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
are Linux modules. No documented Windows-native backend compatible with
virtio-nvgpu's Linux guest ABI was found in the reviewed projects/interfaces.
This is a finding from this investigation, not proof that no proprietary or
future implementation could exist.

Passing arbitrary guest requests to Windows vendor-private escapes would not
resolve this ABI. The experiment rejects original Linux operation IDs 1..8 with
a signed negative `EOPNOTSUPP` before any GPU submission.

## Existing Windows alternatives

| Approach | Fit |
| --- | --- |
| WINQ-EMU VirGL/Venus | Already integrated with the full desktop. API forwarding; NVIDIA Vulkan limitations remain. |
| WSL2 `/dev/dxg` GPU-PV | Real driver-level Windows-host virtualization, with userspace built for that interface. Upstream's full Hyprland test against WSLg failed to start. |
| Hyper-V GPU partitioning | Different hypervisor/device integration. Documented server/GPU/guest combinations do not establish a drop-in QEMU/WHPX consumer-laptop backend. |
| New Windows RM translator | Preserves the existing NVIDIA Linux guest only if RM/UVM/mappings/events/DRM interoperability are actually implemented. That mapping is unresolved. |
| New WDDM bridge over virtio | Could reuse queue architecture. Requires adapted guest kernel/userspace, Windows driver services and QEMU transport. A D3D12 API command service alone is API forwarding. |

References: [DirectX on Linux architecture](https://devblogs.microsoft.com/directx/directx-heart-linux/),
[WHP APIs](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/hypervisor-platform),
[Hyper-V GPU partitioning](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/gpu-partitioning),
and [upstream WSL desktop evidence](WSL-GPU-EXPERIMENT.md).

## Implemented and tested

[`experimental/windows-native-gpu`](../experimental/windows-native-gpu) contains
a bounded/versioned dispatcher and real D3D12 host renderer. It selects NVIDIA
hardware without software fallback; opens/queries/closes the adapter through
documented read-only D3DKMT calls; submits GPU clears; copies into a Win32
flip-model swapchain; and imports a shared texture and fence into a second
D3D12 device on the same host GPU. It synchronizes, reads back and checks every
pixel with one unit of RGBA8 quantization tolerance.

Private lab operations `0x1000` (capabilities/version handshake) and `0x1001`
(typed clear/present) use borrowed framing. They are not implemented by upstream
virtio-nvgpu. Dispatch consumes in-process byte spans; it is not IPC or a
virtqueue. Host HANDLE sharing is not a guest BAR or dma-buf implementation.
Capability bits describe host presentation and host shared readback only.
Bad lengths, handles, status, padding and versions cannot reach GPU work.

Physical test: RTX 5090 Laptop GPU, Windows driver `32.0.16.1742`, WDDM enum
`3200`. The self-test presented 90 frames across three colors and verified every
640x360 pixel through the second device for each color. D3DKMT open/query/close
all succeeded. This is not a performance benchmark, cross-process sharing test
or evidence of Linux guest acceleration. The record is
[`WINDOWS-NATIVE-GPU-2026-10-07.json`](evidence/WINDOWS-NATIVE-GPU-2026-10-07.json).

## Remaining implementation

First demonstrate real guest allocation, mapping, submission, events and buffer
sharing against either a substantiated Linux RM translator or an explicitly
different guest ABI such as a WDDM bridge. The host-only D3D12 experiment does
not substitute for this prerequisite.

Then implement the QEMU virtio adapter/lifecycle, guest kernel and userspace
against the pinned guest image, scanout into SDL with fences, resize/input and
reset/crash recovery. Validate actual Hyprland plus EGL/OpenGL/Vulkan workloads
and measured latency in the VM. Until those pass, retain the upstream desktop
path. The requested driver-level Windows backend remains unfinished.
