# Vulkan device bring-up — October 28, 2026

Original Mesa 26.0.8 Dozen now creates a Vulkan instance, selects the NVIDIA
discrete adapter, creates a logical device/compute queue and completes
`vkDeviceWaitIdle` through QEMU/WHPX and the Windows NVIDIA backend. These device
tests do not exercise shaders or presentation. A later, separate
[4K compute follow-up](VULKAN-4K-COMPUTE.md) now verifies shader execution and
integer readback; Vulkan window presentation remains open.

The unchanged [device probe](../experimental/windows-native-gpu/vulkan_device_probe.cpp)
loads one explicitly selected Dozen ICD, rejects software/non-NVIDIA devices
and checks that the genuine NVIDIA UMD is mapped. The GPU is the RTX 5090 Laptop
GPU on signed NVIDIA driver 621.01 / 32.0.16.2101. These background runs attach
no source disk and request no window, presentation or input route.

Baseline run 2 creates the instance/device but returns **-1** at device idle.
Run 3 uses the identical probe and Dozen library with the opt-in kernel eventfd
bridge and returns **VK_SUCCESS** at all five checked API stages. Comparing the
complete private images finds only three changed ELF payloads: the guest shim,
aperture module and kernel contract checker. The native-sync opt-in marker is
added. All other payloads, including probe, ICD and NVIDIA libraries, match.

Both runs retire five native initialization submissions and four hardware-queue
signals, perform two successful device-health queries, clean up their tracked
native objects/mappings before VM exit and exit naturally. The failed baseline
is retained. These initialization submissions are not Vulkan shader submissions.
Run 3 predates the richer eventfd statistics and failure controls documented in
[GPU fence notifications](BACKGROUND-ASYNC-GPU-WAITS.md); those controls use
separate D3D12 GPU workloads and do not retroactively add Vulkan coverage.

Stock Dozen's Linux Wayland WSI initializes its software presentation path.
That is not sufficient for this project's GPU-backed desktop requirement.
Windowed Vulkan needs validated GPU buffer sharing and synchronization with
the guest compositor; merely enabling Wayland or changing a software flag
does not provide that integration. Vulkan graphics rasterization, presentation, conformance,
application compatibility and performance remain open.

[Sanitized baseline/candidate comparison](evidence/VULKAN-DEVICE-BRINGUP-2026-10-28.json)
records the private receipt hashes and exact scope. The generic harness's
whole-run rejection remains preserved. This is API bring-up progress; backend
**4/8**, product **0/4** and diagnostic scene **3/3** counts stay unchanged.
