# Linux NVIDIA UMD context compatibility

The later [live hardware queue bridge](LIVE-HARDWARE-QUEUE-BRIDGE.md) now lets
the live UMD create native queues with its own private data. The captured
dependency probes described below remain separate historical controls.

On 2026-10-08, a Linux guest in QEMU/WHPX successfully created six WDDM
synchronization contexts and five NVIDIA virtual contexts through the Windows
worker. The five NVIDIA requests used initialization data captured locally from
the installed WSL user-mode driver (UMD). The guest received the full private
reply buffers, destroyed four explicitly, and left one for disconnect cleanup.
All context, device, allocation and section counts returned to zero. The same
run passed six fenced GPU buffer copies totaling 262,443 bytes.

[QEMU evidence](evidence/QEMU-WDDM-CONTEXT-2026-10-08.json) records the exact
worker, guest image, kernel and QEMU hashes. This is a captured-input context
fixture. The separate [live runtime diagnostic](LIVE-NVIDIA-RUNTIME.md) now
reaches native paging and graphics context creation from the installed NVIDIA
Linux UMD, including unchanged live context initialization data, but does
not establish rendering, hardware queue submission, Hyprland acceleration,
desktop stability or near-native performance.

## Reference and experiment

`umd_context_probe.cpp` creates a D3D12 device and copy queue on NVIDIA through
the **existing WSL `/dev/dxg`**. Its paired `umd_context_trace.cpp` observes only
that test process's ioctl calls. It records context and queue initialization
inputs locally, along with public metadata and aggregate operation counts.
Driver-private byte buffers and GPU addresses are not published in evidence.
The tracer is a diagnostic helper for this probe, not a general ioctl proxy.

The reference succeeded on the RTX 5090 Laptop GPU (vendor 0x10de, device
0x2c58, Windows driver 32.0.16.1742). Five private-data contexts targeted nodes
0, 4, 4, 3 and 4. Four used 3204 private bytes and HwQueueSupported; the node 3
context used 3573 bytes and zero flags. A sixth context was synchronization-only.
The Windows native probe also accepted all five captured context inputs.
The [native diagnostic report](evidence/NATIVE-WDDM-CONTEXT-2026-10-08.json)
records context success, queue failure and destruction statuses separately.
The [context-disabled regression report](evidence/HOST-WDDM-CONTEXT-BASELINE-2026-10-08.json)
confirms the ordinary shared copy and reset/truncation cleanup path still passes.

The next call exposed an allocation dependency. Replaying each captured
180-byte hardware queue input in a fresh Windows process returned
`STATUS_INSUFFICIENT_RESOURCES` (0xc000009a). The read-only trace found a field
at byte offset 24 that falls in a previously mapped 64 KiB GPU allocation in
the WSL process. That address is not backed by the corresponding allocation in
the replay process. This is evidence of a dependency that the bridge must
preserve, **not proof that this field is the sole cause of the failure**.
The protocol does not add a hardcoded NVIDIA-private relocation rule.

The subsequent dependency diagnostic passed four native hardware queues.
Recreating each 64 KiB allocation and mapping it to the captured GPU address
alone still failed. The trace additionally identified a documented
`TRANSLATEALLOCATIONHANDLE` call before each queue. Its returned handle appeared
at private-data offset 36. Calling that translation for the **new Windows
allocation** and using its result resolved queue creation in all four cases.
Each returned a valid CPU/GPU progress-fence mapping, initially zero. All
queues, allocations, paging queues, contexts, devices and adapters were
destroyed successfully; see [native queue evidence](evidence/NATIVE-WDDM-QUEUE-2026-10-08.json).

This diagnostic intentionally patches one captured field for the observed
driver version. It submits no GPU commands and is not part of the bridge
protocol. The reusable interface is Microsoft's
[allocation-handle translation operation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ne-d3dukmdt-_d3dddi_driverescapetype),
which returns a handle suitable for the driver's kernel callbacks. A live UMD
must receive this token and construct its own private data. The trace found
67 allocation requests during device/copy-queue initialization, each with one
allocation, 586 private bytes, zero global/runtime data, no supplied system
memory and the OverridePriority flag. This observed profile is not a universal
NVIDIA ABI.

An isolated Windows Hypervisor Platform test also mapped those driver-owned
fence pages into a separate guest process using Microsoft's
[WHvMapGpaRange2](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/funcs/whvmapgparange2).
Across all four queue cases, eight guest loads matched the real paging and
queue fences, eight attempted writes exited on read-only protection, and four
unmapped controls exited on missing mappings. The paging values were nonzero;
the queue values were zero because this test submitted no commands. Each
child removed its mapping and partition before the parent destroyed the driver
objects. See [WHP fence evidence](evidence/WHP-WDDM-FENCES-2026-10-08.json).
This establishes cross-process mapping on this machine, not QEMU integration
or live GPU completion under a guest workload. The tested API provides a
possible route to expose fences without copying each poll through an RPC.

The separate QEMU integration subsequently passed four Linux guest runs using
the read-only PCI aperture. Each read both driver fences 10,000 times, for
80,000 verified loads across the four queues. The QEMU emulation counters
did not change. All guest/QEMU exits and native destruction calls succeeded;
see [QEMU fence evidence](evidence/QEMU-WDDM-FENCES-2026-10-08.json). This is
direct access to idle driver-owned fence pages, not completion of submitted
guest GPU work. The new runtime also passed the prior shared-memory GPU copy
and context fixture acceptance; see
[regression evidence](evidence/QEMU-FENCE-COPY-REGRESSION-2026-10-08.json).

The next integration must create UMD-requested allocations and their GPU
address mappings in the Windows worker, return those addresses to the live
Linux UMD, implement typed allocation-handle translation, and then use the queue
data it generates. Context creation alone is
not sufficient. Monitored fences, allocation CPU mappings, process ownership,
driver queries and completion must share that same lifetime model.

Microsoft's [GPU paravirtualization contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-paravirtualization)
requires PV-aware private data and explicit marshaling. The
[WSL context path](https://github.com/microsoft/WSL2-Linux-Kernel/blob/d504d40ab83839cb61942a812a99d183c42227b9/drivers/hv/dxgkrnl/dxgvmbus.c)
copies private data both into and out of the host call. The bridge now preserves
that in/out behavior with bounded buffers and connection-local context IDs.

## Reproduce locally

Use the DirectX-Headers and libdxg commits in
[`sources.json`](../experimental/windows-native-gpu/sources.json). On the
NVIDIA Windows machine's existing WSL distribution, with its installed D3D12,
DXCore and NVIDIA Linux driver files, run:

```sh
sh experimental/windows-native-gpu/build_umd_context.sh \
  /path/to/DirectX-Headers /path/to/libdxg /path/to/fresh-local-capture
sh experimental/windows-native-gpu/build_guest.sh
python3 experimental/windows-native-gpu/make_test_initramfs.py \
  --init experimental/windows-native-gpu/build/guest-init \
  --probe experimental/windows-native-gpu/build/guest-probe \
  --context-fixture /path/to/fresh-local-capture/driver-contexts.bin \
  --output /path/to/local-context-test.cpio
```

The capture directory must be fresh. Keep captured driver bytes and this
initramfs local; they depend on the installed driver and GPU. The ordinary
`build_guest.sh` image includes no captured vendor data. Driver files are not
bundled or redistributed by this project.

Build the Windows worker with `experimental/windows-native-gpu/build.ps1`.
Run `test_qemu.py` with the paths described in [WDDM-BRIDGE.md](WDDM-BRIDGE.md),
the local context initramfs, `--driver-contexts`, and
`--expected-driver-contexts 5` (replace 5 with the packer's actual count).
Add `--shared-memory` and the experimental QEMU runtime/firmware paths to
exercise shared copies in the same run. Without a private fixture,
`--driver-contexts` checks six synchronization contexts and cleanup only.

For the separate Windows context/queue dependency diagnostic:

```powershell
experimental/windows-native-gpu/build/context-native-probe.exe `
  C:\local-capture\wsl-context-input-1.bin `
  C:\local-capture\wsl-queue-input-1.bin
```

Omit the queue argument for context-only acceptance. A queue failure is a
nonzero result and its NTSTATUS is preserved in JSON. The diagnostic destroys
each successfully created queue, context, device and adapter before exit.

The allocation-aware native queue diagnostic additionally takes the allocation
initializer and mapping captured by the tracer:

```powershell
experimental/windows-native-gpu/build/queue-dependency-probe.exe `
  C:\local-capture\wsl-context-input-1.bin `
  C:\local-capture\wsl-queue-input-1.bin `
  C:\local-capture\wsl-allocation-input-1.bin `
  C:\local-capture\wsl-allocation-map-1.bin
```

Select corresponding files using the profile's `contextSequence` and
`allocationSequence` metadata. The recorded run used context/allocation pairs
1/1, 2/6, 3/15 and 6/52. Both the 64 KiB allocation size and the observed
180-byte queue layout are diagnostic constraints; this is not a generic UMD
queue implementation.

Add `--whp-fences` to the allocation-aware diagnostic to run five tiny,
disk-free WHP guests for that queue: paging-fence read, queue-fence read,
unmapped control, and read-only write checks for both fences. Windows Hypervisor
Platform must already be available. The test creates hidden child processes
with an explicit inherited-handle allowlist, a five-second VP cancellation
deadline and a ten-second child deadline. The parent holds all backing objects
until the children have exited. No host process addresses or private driver
buffers are written into the evidence report.

For the QEMU fence integration, build the recipe in `experimental/qemu`
and run `build_guest.sh` to produce the separate `fence-test.cpio`. Replace
`--whp-fences` with the following six arguments:

```powershell
--qemu-fences C:\qemu-lab\bin\qemu-system-x86_64.exe `
  C:\qemu-lab\share\qemu C:\local-kernel\vmlinuz-linux `
  C:\local-build\fence-test.cpio C:\local-results\new-fence-run.log
```

The log must not already exist. The parent starts one hidden, disk-free QEMU
with two read-only PCI apertures and an explicit inherited-handle allowlist.
The Linux init maps each BAR, checks 10,000 loads against the idle fence value,
and requires the emulation counter to remain unchanged. The test requires
both the guest success marker and a zero QEMU exit code, then destroys the
NVIDIA objects. A 60-second deadline kills only its owned QEMU child before
cleanup. An older QEMU without the device was verified to fail this harness
while all NVIDIA destruction calls still succeeded. The direct-load acceptance
does not cover live guest graphics commands or desktop acceleration.
