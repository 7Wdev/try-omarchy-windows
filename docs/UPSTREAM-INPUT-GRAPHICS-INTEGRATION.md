# Keep official input; integrate the graphics backend

The product should reuse the official Try Omarchy keyboard and cursor logic.
The work belongs at the graphics/presentation boundary, with the existing
launcher and QEMU SDL window handling input.

The official repository was fetched and inspected at commit
[`3b497519`](https://github.com/omacom/try-omarchy-windows/tree/3b497519f5f4b320b33aa7bfaf8e3ec12ecb2b64).
The [source-preservation receipt](evidence/UPSTREAM-INPUT-PRESERVATION-2026-10-20.json)
records canonical Git-byte hashes for the unchanged input entry points, patches
and controls. The complete `runtime-build` tree is also identical at this
checkpoint. Branding/packaging differences elsewhere in the fork are separate.

## Existing route to retain

| Component | Official implementation | Integration requirement |
| --- | --- | --- |
| QEMU command line and guest devices | `app/qemu.go` creates `virtio-keyboard-pci`, `virtio-tablet-pci` and the SDL frontend. GPU mode uses `sdl,gl=on`; guest cursor display and close behavior retain the existing preferences. | Keep these devices, SDL input handling and launcher preferences. |
| Reserved Windows keys | `app/winkey.go`, `app/keyboard_preferences.go`, `app/main.go` and `app/qemu.go` coordinate the existing Windows hook and QMP reserved-key routing. | Reuse this exact route, including focus, held-key state, Alt+Tab and external-hook behavior. |
| Mouse, cursor and window lifecycle | `app/cursor_clip.go`, `app/cursor_clip_windows.go`, `app/qemu_window_events_windows.go` and the SDL runtime patches. | Preserve clipping, guest cursor updates, active-window routing and SDL window lifetime handling. |
| Runtime fixes and controls | Existing stale-window input, precise scroll, external keyboard-hook, guest cursor and touchpad patches; `test-sdl-input.py`, `test-sdl-cursor.py`, `test-sdl-keyboard-hook.py` and related controls. | Retain their source and tests while changing graphics. |

The upstream launcher already uses QMP for certain reserved keys. That is part
of the input route to preserve. Our fixed QMP self-test sequence is a separate
lab diagnostic and can continue to verify the guest PTY and GPU-rendered response.

## Graphics integration boundary

The current bridge presents through a separate experimental Windows window.
Its optional native input handlers remain lab code. The preferred product path
is the official launcher-owned **QEMU SDL frontend**, using the existing
`QemuConsole`/display scanout lifecycle for the NVIDIA-rendered output.

The graphics integration must:

1. Publish GPU-rendered buffers into the existing SDL/OpenGL scanout path, with
   explicit ownership and GPU fence synchronization.
2. Keep display surface/console dimensions and cursor metadata correct so the
   unchanged input handlers receive valid window and coordinate information.
3. Preserve the official window identity and lifecycle used by launcher focus,
   cursor clipping, reserved-key handling, multi-display and shutdown behavior.
4. Validate keyboard, pointer, cursor and focus through that official window,
   alongside the 4K GPU-content and cleanup checks.

The [host SDL/D3D12 interop component](LIVE-SDL-D3D12-INTEROP.md) now passes two
bounded GPU controls with separate producer/consumer fences. Its connection to
QEMU's actual console/scanout path still needs implementation and hardware
validation. A CPU screenshot upload would not establish the requested
GPU presentation or performance result. The existing diagnostic window and
QMP tests do not prove integration with the official frontend.

The [300-frame application-resize checkpoint](LIVE-UWSM-SUSTAINED-RESIZE.md)
advances graphics/resource validation without changing the official input code.
Product input acceptance remains open until the new graphics path works
through the retained frontend.
