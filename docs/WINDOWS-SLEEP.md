# Windows sleep handling

The launcher registers its tray window with
[RegisterSuspendResumeNotification](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registersuspendresumenotification).
Microsoft documents this opt-in for notifications before the Desktop Activity
Moderator suspends desktop applications during Modern Standby. Ordinary sleep
uses the same `WM_POWERBROADCAST` suspend/resume handler. Registration failure
is logged and ordinary sleep broadcasts remain available.

On suspend, the launcher checks the runtime state and pauses only a running
VM. Power handling uses a dedicated private QMP monitor so shutdown, USB and
forwarding controls remain available while it holds a connection. It keeps that
connection until resume, then resumes only the paused VM
it still owns. Duplicate notifications do not repeat successful operations.
A pre-existing manual pause, shutdown, restore or guest suspend is preserved.
Manual STOP/RESUME events during the interval relinquish automatic ownership.
A replacement VM cannot inherit an old connection's resume obligation.

Each QMP step has its own two-second budget. A failed stop acknowledgement,
pause confirmation, or resume confirmation retains a recovery obligation for
that runtime. An interrupted monitor cannot prove intervening manual changes,
so the launcher does not automatically resume over a replacement connection.
Instead the tray shows **Omarchy paused after sleep**, offers **Resume Omarchy**,
and automatically saves diagnostics under the installation's `diagnostics`
folder. The notice includes the snapshot path, or explains a snapshot failure.
Resume inspects the same runtime before continuing it and confirms it is running.
A replacement VM never inherits this action. Tray shutdown cancels outstanding
power commands and unregisters once. Early startup retains the supervisor's QMP
quiet period: if guest controls are not ready at suspend, no pause is attempted.

A resume broadcast corrects the guest clock even when the suspend broadcast was
missed. Paired resume notifications are coalesced, and the follow-up clock,
battery and app-catalog retry uses a timer without blocking periodic updates.

## Windows restart and sign-out

The tray registers a short shutdown-block reason when Windows asks whether the
session may end, then responds immediately. Only a confirmed `WM_ENDSESSION`
requests guest poweroff. Negotiation, resuming a paused CPU, the ACPI power button
and waiting for a clean guest shutdown share one ten-second deadline. The block
reason stays registered throughout that wait. This allows margin above measured
KVM shutdown times for slower WHPX hosts and stays below the 25-second cap.
Windows may force termination sooner if the user chooses Shut down anyway;
sign-out is never held indefinitely. A canceled end-session query leaves the
guest running. An already exited guest returns immediately.

The launcher records an unclean marker before each VM start and only records a
clean exit after a guest-originated QMP shutdown event and process exit. On the
next launch after an unclean exit, it offers the existing snapshot recovery UI.
Declining the offer boots normally so the guest can check its disk. A power-button
acknowledgement alone is not a clean-exit record.

## Verification limits

The pure state-machine tests run on Linux and Windows and cover duplicate and
repeated transitions, manual pause, non-running states, manual changes during
sleep, replacement runtimes, rejected commands, lost replies and per-step
timeouts. Windows-only tests cover startup/shutdown, missing controls and
registration/cleanup failures. A diskless Windows QEMU fixture checks actual
`prelaunch`, `running` and `paused` states, independent tools access while
paused, and intervening manual state changes. A hidden native receiver checks the
Windows notification registration API. These checks do not suspend Windows.

The [v0.6.1 record](evidence/V061-SIGNED-CANDIDATE-2026-09-29.md) proves the
separate guest watchdog fix under a controlled five-minute freeze. It does
not prove physical Modern Standby or resolve the XWayland authorization report.
[#216](https://github.com/omacom/try-omarchy-windows/issues/216) stays open for
the reporter's long-sleep result and X11 diagnostics if authentication fails.

On an isolated candidate installation on an S0 Low Power Idle host, check:

- Confirm `powercfg /a` lists Modern Standby and capture candidate/runtime/guest
  versions and hashes. Confirm the guest has the shipped watchdog drop-in.
- Sleep for at least five minutes, wake, and repeat. Confirm one successful
  pause/resume per cycle, guest clock sync, responsive graphics/input, unchanged
  service PIDs and no failed units. Also test lid close, startup and shutdown.
- Pause manually before sleep and confirm wake leaves the VM paused. Resume
  manually, then repeat normal sleep.
- Launch X11 applications before and after sleep. If authentication fails,
  collect `$XAUTHORITY`, `pgrep -a Xwayland`, `/tmp/.X11-unix` permissions and
  `xhost` output, and distinguish ChatGPT from other X11 apps.
- Repeat ordinary S3 sleep on a suitable host. Native synthetic transitions
  alone do not prove physical S3 acceptance.

The available AMD laptop supports S3 and not S0 Low Power Idle. Do not change
its power model or treat a simulated freeze as Modern Standby acceptance.
