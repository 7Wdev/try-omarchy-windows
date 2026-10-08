# QEMU Windows section memory lab

This engineering runtime adds `memory-backend-win32-section` to the exact
WINQ-EMU QEMU revision pinned by the project. The build and memory acceptance
[passed CI](https://github.com/7Wdev/try-omarchy-windows/actions/runs/37688925268);
it is not selected by the launcher and does not supply guest GPU rendering.

The new backend uses `CreateFileMappingW` and `MapViewOfFile` for shared RAM.
A small QEMU API extension preserves `RAM_SHARED` when an external pointer
backs a memory region, instead of changing a RAMBlock after registration.
QOM releases the memory region before the backend unmaps its view. Migration
is explicitly blocked. Existing mappings are refused; section names must use
the session-local prefix and a random 128-bit suffix. Windows' default process
DACL controls access; this lab is not an isolation boundary against other
processes running as the same user.

`test_section.py` starts a paused, disk-free QEMU with TCG, writes through an
external Windows mapping, and checks the bytes through QEMU's physical-memory
read command. It also checks invalid size/share/name, section collisions,
immutable properties, migration rejection and mapping cleanup. No NVIDIA GPU
is required for this memory test. The separate NVIDIA WDDM acceptance also
passed guest writes, host writes, residency, GPU address mapping and six GPU
copies in actual guest RAM. See [evidence and limitations](../../docs/WDDM-BRIDGE.md).

Build in MSYS2 UCRT64 using the packages listed in
`.github/workflows/wddm-qemu-lab.yml`:

```sh
bash experimental/qemu/build_qemu.sh qemu-lab
```

```powershell
python experimental/qemu/test_section.py `
  --qemu qemu-lab/bin/qemu-system-x86_64.exe `
  --firmware qemu-lab/share/qemu
```

The artifact includes SDL and WHPX, corresponding modified QEMU source,
configuration, and MSYS2 package versions. It omits the normal runtime's
VirGL/Venus and host integrations and is suitable only for this lab. QEMU and
the modifications to its existing files retain QEMU's GPL terms; the new
independently written backend and helper scripts use the project MIT license.
Bundled DLLs retain their own package licenses.

This fixture has no balloon device, hotplug, confidential memory or migration.
Adding it to a general VM requires a defined memory layout and allocation
lifetime contract before guests can register RAM ranges with Windows. A
section-backed CPU mapping alone does not produce GPU commands, Linux DRM or
SDL scanout.

## Read-only driver fence aperture

The recipe also adds `wddm-fence-lab`, a diagnostic PCI device with one 4 KiB
read-only BAR. Its source process handle and idle fence address come only from
the owning Windows test process. A WHPX listener maps that page directly using
`WHvMapGpaRange2`; ordinary QEMU accesses use a bounded `ReadProcessMemory`
callback. The PCI configuration reports the fence offset, expected idle value,
and callback count, so a Linux acceptance test can distinguish direct guest
loads from emulation. It exposes no host pointer in PCI configuration.

This extension follows the successful standalone
[WHP mapping/protection tests](../../docs/evidence/WHP-WDDM-FENCES-2026-10-08.json).
The [QEMU build](https://github.com/7Wdev/try-omarchy-windows/actions/runs/37746324169)
and [Linux guest integration](../../docs/evidence/QEMU-WDDM-FENCES-2026-10-08.json)
passed: four queue cases, 80,000 direct idle-fence loads, zero emulated reads,
and successful teardown. The native queue probe's `--qemu-fences` mode holds
the driver objects until the owned VM exits. Build the separate `fence-test.cpio`
with `experimental/windows-native-gpu/build_guest.sh`; reproduction arguments
are in [the compatibility report](../../docs/UMD-CONTEXT-COMPATIBILITY.md).
The device
uses private lab PCI identifiers, disables hotplug and migration, and requires
WHPX. The parent must keep driver objects alive until QEMU has exited. It is
not a graphics device, command-submission interface or production fence ABI.

## Dynamic fence hub

`wddm-fence-hub` adds 64 initially unmapped, read-only 4 KiB slots in a PCI BAR.
The owning host controls the `fence-mapping` QOM property over QMP with
`map:slot:source-address:generation` and `unmap:slot:generation`. Only its fixed
inherited source-process handle is used. Commands are bounded, require an
aligned readable source, reject occupied slots and stale generations, and
acknowledge unmapping only after the memory listener transaction completes.
The guest sees slot offsets and reads; it cannot provide a host address.
Migration and hotplug are blocked. `fence-state` reports mapped count,
generation and emulated-read count without source addresses.

The [live paging integration](../../docs/LIVE-PAGING-BRIDGE.md) passed physical
QEMU/WHPX acceptance: NVIDIA's Linux runtime created a Windows paging queue,
read its live fence 10,000 times without emulation, created a graphics context
from live private data, and acknowledged unmapping before native teardown.
The Windows owner retains each native fence through successful QMP unmapping,
or stops and reaps its owned QEMU process before releasing driver objects.
Portable command/generation tests cover stale and occupied slots. Slot-reuse
stress, GPU resets, runtime allocations/submission and desktop acceleration
remain unverified or unimplemented; the result is partial initialization.
