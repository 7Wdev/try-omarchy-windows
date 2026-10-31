# Launcher graphics argument boundary

Verified **October 29, 2026**. The native bridge now uses a separate argument
builder for its fence hub, allocation hub, guest transport and SDL GPU relay.
The launcher integration composer can replace the upstream GPU device while
preserving all other arguments. **It is not activated by the released launcher.**
Backend/desktop acceptance stays **4/8, 0/4, 3/3**.

The argument boundary is necessary because the prototype owns QEMU, its inherited
source-process handle, native allocations and private QMP controls. The launcher
currently starts QEMU directly and uses its PID for window, keyboard, cursor and
bridge supervision. Changing a rendering option alone cannot transfer that
ownership safely.

## Actual launcher source check

A Go file overlay runs one pure export test against the current launcher package.
It calls the actual `buildQemuArgs`, rather than reproducing its arguments in a
separate implementation. All **430 launcher source files remain unchanged**.
The native composer verifies the resulting vectors:

| Launcher profile | Composer result |
| --- | --- |
| Single SDL GPU output, guest cursor | Accept; preserve all arguments except the graphics device. |
| Single SDL GPU output, host cursor preference | Accept; preserve that cursor preference and all other non-graphics arguments. |
| CPU renderer | Explicitly reject. |
| Venus selected | Explicitly reject. |
| Multiple outputs | Explicitly reject. |

Both accepted vectors retain the original writable-disk, kernel, initramfs,
command line, CPU/RAM, keyboard/tablet, QMP roles, audio, networking, camera,
other bridge and window options. Only the upstream graphics device changes to
`-vga none`, followed by the native graphics transport devices. This establishes
argument preservation, not input delivery, boot compatibility or presentation.

Thirty-five portable controls also cover default/expanded device arguments,
commas in audit paths, incompatible or duplicate graphics devices, conflicting
channels, missing option values and malformed endpoint values. Existing native
runtime argument order is preserved by the extraction. Strict default and 4K
Windows builds pass. The composer accepts endpoints already prepared by the
native owner; it does not authenticate a process or grant handle authority.

## GPU regressions and remaining integration

With unchanged private guest images, the new native executable passes one complete
GPU allocation-pressure recovery, both original 4K compute rounds and both original
3840×2160 shader frames. Existing data/pixel oracles and byte guards stay fixed.
The shader captures match the earlier images byte for byte. Native objects and
aperture mappings retire before natural VM exit. These checks have no display,
input or guest disk; there is no new live SDL regression.

The file-only audit binds the actual compiler inputs, launcher-generated vectors,
unchanged images and prior accepted control receipts. A private compiler wrapper's
receipt step failed because a PowerShell hashing cmdlet was unavailable. Its
completed strict compiler artifact was hashed with Python and successfully tested;
the original receipt-tool failure remains retained separately.

[Sanitized evidence](evidence/LAUNCHER-GRAPHICS-ARGUMENTS-2026-10-29.json) records
the exact proof and limits. Run the source-level check without starting a VM:

```powershell
python experimental/windows-native-gpu/test_launcher_graphics_arguments.py --go <go.exe> --composer-test <qemu-launcher-arguments-test.exe> --output <fresh-directory>
```

Next, a prepare/attach/stop handshake must bind the actual QEMU process, inherited
source-process handle and native owner's lifetime, retain the launcher's QMP roles,
and reap the guest before releasing native GPU objects. The matching normal guest
bootstrap/session still needs integration. Ordinary input, sustained browsing,
general pressure recovery, broader APIs and matched-native performance remain
required product work.
