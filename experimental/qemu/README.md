# QEMU Windows section memory lab

This engineering runtime adds `memory-backend-win32-section` to the exact
WINQ-EMU QEMU revision pinned by the project. It is being built and validated;
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
is required for this memory test. The later WDDM acceptance must additionally
prove guest writes, host writes, residency and GPU mapping of those guest pages.

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
