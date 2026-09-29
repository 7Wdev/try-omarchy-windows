# Windows sleep handling

The launcher registers its tray window with
[RegisterSuspendResumeNotification](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registersuspendresumenotification).
Microsoft documents this opt-in for notifications before the Desktop Activity
Moderator suspends desktop applications during Modern Standby. Ordinary sleep
uses the same `WM_POWERBROADCAST` suspend/resume handler. Registration failure
is logged and ordinary sleep broadcasts remain available.

On suspend, the launcher checks the runtime state and pauses only a running
VM. It keeps that QMP connection until resume, then resumes only the paused VM
it still owns. Duplicate notifications do not repeat successful operations.
A pre-existing manual pause, shutdown, restore or guest suspend is preserved.
Manual STOP/RESUME events during the interval relinquish automatic ownership.
A replacement VM cannot inherit an old connection's resume obligation.

Commands have a two-second budget per transition. A disconnected connection,
lost command reply or unconfirmed pause leaves automatic ownership cleared;
the VM may stay paused and the log asks for manual resume. A rejected resume
command can be retried on a subsequent resume notification while the original
connection still proves ownership. Tray shutdown closes that connection and
unregisters once. Early startup retains the supervisor's QMP quiet period:
if guest controls are not ready at suspend, no pause is attempted.

## Verification limits

The focused Windows tests cover duplicate and repeated transitions, manual
pause, non-running states, manual changes during sleep, missing controls,
startup/shutdown, replacement runtimes, rejected commands, lost replies and
registration/cleanup failures. A diskless Windows QEMU fixture checks actual
`prelaunch`, `running` and `paused` states. A hidden native receiver checks the
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
