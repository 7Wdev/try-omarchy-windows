# QEMU socket reconnect startup failure

The first real VM attempts through the new original-argument profile reach
verified Go/native/QEMU process binding. They do **not** complete a normal boot
or render a new desktop frame. They expose a socket reconnect crash in the
pinned experimental QEMU runtime. A five-line upstream backport is now included
in that runtime's build recipe; its rebuilt binary still needs verification.
Counts remain **4/8, 0/4, 3/3** and native activation stays unset.

The existing Go argument producer includes reconnecting camera and authentication
channels. These channels must tolerate an unavailable listener. The pinned QEMU
base tries to unregister a yank function after an asynchronous connection fails,
although it registers that function only after a successful connection. The
unmatched unregister calls `abort()`. QEMU already has an
[upstream fix, ddf5738503e2](https://github.com/qemu/qemu/commit/ddf5738503e2ba6adb74359abf3b6e7aa81af420),
which removes that error-path unregister. The successful connection, ordinary
disconnect and reconnect paths remain intact.

## Observed failure and retained limits

The first private attempt rejects an overlong test-only Unix QMP socket path.
The existing launcher already bounds these paths; the next attempt uses a short
private directory and retains the original four QMP roles. QEMU then creates SDL
and NVIDIA OpenGL surfaces but exits with **0xC0000409** before kernel serial
output. Go publishes the actual bound QEMU PID, rejects stopping the native owner
by a confused PID, reaps QEMU through its held handle and closes the cancellation
event. Private source-image and installed-driver checks pass before and after.

A debugger attached only to the owned test process tree captures the QEMU fault
in `ucrtbase.dll`, with fast-fail parameter **7**. A private raw stack-address scan
against the exact executable's COFF symbols identifies addresses in
`yank_unregister_function.cold` and `qemu_chr_socket_connected`; it is not a full
validated stack unwind. The exact corresponding source contains the upstream
failure path. A separate paused, disk-free, display-free reconnect test reproduces
the same **0xC0000409** exit. No GPU workload or desktop rendering is needed to
reproduce this QEMU failure.

The VM attempts use the unchanged private guest-204 diagnostic RAM overlay and
an explicitly read-only base image, with runtime paths and the probe's boot
command line substituted into the exported Go argument profile. They establish
neither ordinary account/session startup nor full launcher acceptance. A private
debugger attempt also retains its observer failure; it is excluded from successful
isolation or VM evidence.

## Background isolation

The private runner creates a random Windows desktop, passes its name through
`STARTUPINFO.lpDesktop`, and checks that it never receives user input. It requests
no desktop-switch right and never selects that desktop or sends input. The
parent's desktop remains unchanged. The successful isolation controls include an
owned window and terminated child process trees; VM attempts retain the actual
SDL window's PID on that private desktop. Owned jobs are empty before the runner
closes its desktop. Windows' internal UI helper windows can also appear there;
these controls do not establish physical scanout or interaction acceptance.

This follows Microsoft's documented
[child desktop selection](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfow),
[desktop access rights](https://learn.microsoft.com/en-us/windows/win32/winstation/desktop-security-and-access-rights)
and [`UOI_IO` input-state query](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getuserobjectinformationw).

## Backport and regression

`experimental/qemu/socket-reconnect-yank.patch` contains the upstream five-line
deletion, applied to the pinned base without changing any other bytes in that
file. The fixed file's Git blob is
`99e01a4875a27a57e3bd9ef0c063cb2809c0c12e`. An initial local comparison against the
upstream master blob is retained as rejected: the pinned stable base differs,
and local Git newline conversion also changed the file. The final comparison
uses LF bytes and verifies the exact deletion against the actual pinned source.

The build applies the backport to both experimental QEMU variants. The SDL
provenance records its patch and compiled-source digests; verification checks
that the source stays unchanged during compilation. All 24 official runtime
patches and official input-source checks remain in place.

The new CI regression waits for an actual refused connection callback, then
requires a late listener, peer disconnect/reconnect, owned QMP yank/reconnect and
natural QEMU exit. It uses paused TCG, no display, no disk and no host input.
The old binary fails with **3221226505 / 0xC0000409** before it can report the
refused connection. A green rebuilt regression will still need the new profile's
background VM boot, matching normal guest bootstrap and complete product checks.

[Structured evidence and artifact digests](evidence/QEMU-SOCKET-RECONNECT-2026-10-29.json).
