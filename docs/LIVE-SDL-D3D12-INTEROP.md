# NVIDIA D3D12 textures in an SDL OpenGL context

**Subsequent checkpoint, 2026-10-21:** [actual QEMU SDL cross-process GPU import,
RGBA/BGRA pixels and two 300-handoff controls now pass](LIVE-QEMU-SDL-GPU-RELAY.md).
The host-component and startup results below retain their original scope.

Two fresh host controls pass on the RTX 5090 Laptop GPU with NVIDIA 617.14,
using the same SDL 2.32.10 DLL as the pinned QEMU lab runtime. Each imports a
named **3840×2160 D3D12 GPU texture**, verifies all its pixels, blits it on the
GPU into a hidden 1280×720 SDL backbuffer and verifies that complete buffer.
The second frame changes every quadrant, so a stale first frame cannot pass.

This verifies a host component needed to put the guest graphics into the
official QEMU SDL frontend. It does not yet prove that frontend, a guest frame
through it, physical display or input, or performance. The existing
[real Omarchy desktop images](LIVE-UWSM-SUSTAINED-RESIZE.md) keep their separate
guest-rendering scope. Counts remain **4/8 backend stages and 0/4 desktop gates**.

## Shared resource and synchronization contract

The OpenGL device LUID must match the hardware NVIDIA DXGI adapter. The control
requires the EXT memory-object and semaphore Win32 extensions. A random
128-bit session prefix names one shared resource and **two distinct fences**:

1. D3D12 finishes the producer commands, returns the texture to COMMON and
   signals producer fence N.
2. OpenGL waits for N on that producer fence before reading the imported
   texture and blitting it into the SDL backbuffer.
3. OpenGL signals consumer fence N after consumption. D3D12 observes that
   separate fence before the control reuses the texture and command allocator.

One shared timeline would be unsafe for multiple in-flight frames: producer
progress could falsely satisfy a consumer-retirement target. The two directions
therefore use independent native fence objects. This bounded control waits on
the CPU for each consumer completion; it is not the production scheduling or
throughput implementation.

The control imports by UTF-16 object name rather than passing foreign process
handle numbers. All three original NT handles are non-inheritable and are
closed after import; the imported objects retain their own references. Names
use the session-local namespace and default process DACL. This is not isolation
against another process running as the same user.

For D3D12 resource import, size zero is the compatibility value specified by
the [Khronos Win32 external-object extension](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_external_objects_win32.txt).
The [external-object specification](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_external_objects.txt)
defines the memory, semaphore and texture-layout operations. The small checked-in
header contains only the required Khronos ABI declarations; no SDK download or
third-party GL implementation is needed to compile the control.

## Actual results

| Per fresh host control | Run 6 | Run 7 |
| --- | ---: | ---: |
| Full 4K source and complete SDL-buffer comparisons | Pass | Pass |
| Frames with changed GPU quadrant pixels | 2 | 2 |
| Independent producer/consumer targets observed | 1, 2 / 1, 2 | 1, 2 / 1, 2 |
| Diagnostic readbacks | 4 | 4 |
| CPU texture uploads | 0 | 0 |
| Peak reported local + nonlocal GPU usage, bytes | 157,356,032 | 157,356,032 |
| Unchanged usage guard, bytes | 1,073,741,824 | 1,073,741,824 |
| Original exported NT handles closed | 3 | 3 |
| Explicit GL, D3D12 and SDL teardown; natural exit zero | Pass | Pass |

The [reconstructable evidence](evidence/SDL-D3D12-INTEROP-2026-10-20.json)
contains all four raw RGBA captures per run, native records, frozen source,
header/build receipts, SDL dependency hash and independent pixel/fence audits.
These quadrant controls are not desktop screenshots. The SDL window stays
hidden and no physical scanout claim is made.

## QEMU preparation and unchanged input

The subsequent **shared-context mode passes twice (runs 8–9)**. Named texture
and semaphore imports happen in a distinct OpenGL 4.5 core context shared with
the existing SDL root. The non-shareable FBO, GPU wait, complete source/window
readbacks, blit and consumer signal run in that root context. Every pixel and
both fence directions pass; peak reported usage is 157,892,608 bytes. A fresh
single-context compatibility control also passes (run 10). The original two
runs, source snapshot and auditor remain unchanged.
[Separate shared-context evidence](evidence/SDL-SHARED-CONTEXT-2026-10-20.json)
retains its own build receipt and complete-buffer references. **Cross-process
import, actual QEMU scanout and physical display/input remain unverified.**

A separate source worktree at the locked QEMU revision passes application of
the lab patches followed by all **24 unchanged official runtime patches**.
The existing upstream pointer, cursor, keyboard-hook and scanout controls all
pass against that patched source; their hashes and outputs are retained in the
evidence. The original modified dependency checkout is preserved.

The new [SDL/OpenGL build variant](../experimental/qemu/README.md#sdlopengl-build-candidate)
enables the compiled GL path, reruns those controls and retains input-source
hashes/configuration. The section-only variant remains available separately.
Both [Windows variants build and pass their memory controls](https://github.com/7Wdev/try-omarchy-windows/actions/runs/39480803808).
The downloaded OpenGL ZIP matches GitHub's SHA-256 digest; all 209 extracted
files have retained hashes. Its configuration enables SDL and OpenGL, its SDL
DLL exactly matches the hardware controls above, and its four input-source
hashes match the independently checked source worktree.

A local **paused, disk-free QEMU** starts with both **TCG and WHPX**, SDL/GL enabled and the official
virtio keyboard/tablet devices. An owned loopback QMP connection verifies VM
identity, paused status, absolute tablet and the actual SDL display options,
then obtains a natural zero exit. The first batched-stdin fixture timed out
after 25 seconds; its cause remains unestablished. The retained subsequent
socket fixture passes without relaxing the bounded startup or ownership checks.
[Build, artifact and local startup evidence](evidence/QEMU-SDL-OPENGL-CANDIDATE-2026-10-20.json)
also records **431 passing local milestone tests**.

This startup result does not identify the QEMU window's GPU renderer. Actual
NVIDIA adapter identity, cross-process GPU import and guest texture scanout
through that frontend remain unverified. Neither variant is selected by the
released launcher.

Next is a graphics-only QEMU console/scanout relay with explicit session,
buffer-generation and fence ownership. Context sharing, display dimensions,
cursor metadata, disconnect/device-loss cleanup and input through the existing
SDL window still require implementation and hardware testing.
