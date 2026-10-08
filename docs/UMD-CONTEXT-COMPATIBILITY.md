# Linux NVIDIA UMD context compatibility

On 2026-10-08, a Linux guest in QEMU/WHPX successfully created six WDDM
synchronization contexts and five NVIDIA virtual contexts through the Windows
worker. The five NVIDIA requests used initialization data captured locally from
the installed WSL user-mode driver (UMD). The guest received the full private
reply buffers, destroyed four explicitly, and left one for disconnect cleanup.
All context, device, allocation and section counts returned to zero. The same
run passed six fenced GPU buffer copies totaling 262,443 bytes.

[QEMU evidence](evidence/QEMU-WDDM-CONTEXT-2026-10-08.json) records the exact
worker, guest image, kernel and QEMU hashes. This is a captured-input context
fixture. A live graphics runtime has not run through this bridge, and it does
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

The next integration must create UMD-requested allocations and their GPU
address mappings in the Windows worker, return those addresses to the live
Linux UMD, and then use the queue data it generates. Context creation alone is
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
