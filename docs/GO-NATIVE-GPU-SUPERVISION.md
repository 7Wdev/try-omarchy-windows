# Go supervisor integration

The launcher's normal supervision function now has a native-owner branch for
an explicitly prepared backend profile. It publishes the **actual QEMU PID**,
retains the VM's Windows process identity and cancels the native owner without
killing the GPU page owner first. Three hidden GPU workloads and two early
lifecycle controls pass through these same Go start/publish/stop/wait functions.

**Released settings do not select the branch.** The profile that combines the
original launcher arguments with the matching normal guest runtime is still
missing. This is not normal launcher/account-session acceptance. Backend and
desktop counts stay **4/8, 0/4, 3/3**.

## Changes at the supervision boundary

`main.supervise` uses `startSupervisedGuest`, `publishSupervisedGuest`,
`guest.wait` and `guest.kill`. The shutdown watchdog uses the same registered
stop operation. The default branch retains the original `exec.Cmd` start/wait
and process-kill behavior. An internal, unset `nativeGPUPrepare` callback is the
future prepared-profile boundary; there is no new user activation setting.

For a prepared native owner, Go creates one private inherited cancellation
event, reads the bridge's protocol-1 VM identity and independently holds the
QEMU process object. It checks the actual creator PID, canonical 64-bit creation
timestamps, executable file identity and process liveness. Protocol parsing
rejects duplicate, missing, unknown or case-variant field names, numeric
timestamps and implicit/null acceptance flags. Graphics, input and daily-use
acceptance must remain explicitly false in the identity message.

The original command's environment, directory and streams are captured before
profile preparation and restored onto the native command. Audio, keyboard and
pinch environment construction stays in the original launcher code. Only the
verified QEMU PID is published to its existing window lookup and guest bridge
checks. The native bridge PID is rejected as a stop target; an unexpected PID
cannot fall through to a process kill while a native guest is registered.

Cancellation signals the event. Go waits for the native owner, then verifies
exit through the held QEMU object before closing its entry. The native owner
continues to control GPU/aperture cleanup. This follows the documented
[Windows process-handle model](https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers).
Native commands with automatic `Cmd.Cancel` or `WaitDelay` termination are
rejected, because those mechanisms can kill the owner directly, as documented
by [Go's exec package](https://pkg.go.dev/os/exec#Cmd).

The protocol reader duplicates a caller-owned file stream before starting.
This matters because the original supervisor closes its log entry after start;
the reader needs its separate entry until native output drains. Configured
startup bounds are 45 seconds for identity plus 15 seconds for cancelled-start
cleanup. A cleanup timeout reports pending ownership and retains the background
reaper. An unresponsive-owner fault campaign is still required; these settings
are not proof of general failure recovery.

## Verification, October 29, 2026

The exact Go build passes six selected Windows test methods, including protocol
mutation subtests, cancelled-before-start, a real hidden default child, rejection
of automatic owner termination, and the existing QEMU bridge/peer tests. Windows
static checking passes. The private integration entry point is skipped by
ordinary tests and does not run launcher UI or host input.

Each of three real hidden QEMU/WHPX guests uses the unchanged native bridge,
signed NVIDIA 621.01 runtime snapshot, kernel, guest image, byte limits and original
data/pixel oracles from the preceding native lifetime checkpoint:

| Workload through the Go supervisor boundary | Result |
| --- | --- |
| Vulkan allocation pressure | Original refusal/free/same-buffer retry; both complete 16,384-word rounds; all 13 submissions and 20 signals retire. |
| Original 4K compute | Both complete 8,294,400-word rounds pass. |
| Original 4K shader | Both 3840×2160 frames pass every pixel and shared-texture handoff; captures match the preceding native-owner frames byte for byte. |

All three reject the native bridge PID as a stop target, close the caller's
original stdout entry after start and continue rendering/logging successfully.
The published PID is QEMU's PID, distinct from the bridge and Go controller.
Go observes natural native/VM exits of zero through its held objects. Native
objects and mappings retire before VM exit. The historical GPU harness is
unchanged. Its `hostBridgeExit` field observes the outer Go test process in these
runs; the separate Go receipt also binds the actual native exit and command.

Two separate controls stop before a guest graphics context is created:

- The Go shutdown route signals cancellation, reaps QEMU with code 90, observes
  native exit 1 and closes its event after exit verification.
- An observer holds the live Go/native/QEMU identities and interrupts only its
  own Go controller. The native creator-exit monitor reaps QEMU with code 90 and
  exits 1. The stop acknowledgement keeps driver-cleanup acceptance false.

These early controls do not establish recovery during active GPU work. No
visible renderer/VM, host input, desktop control or source disk was used.
Their separate final driver summaries show zero submissions and zero live,
failed or pending native counters; the stop acknowledgement itself remains
insufficient to establish cleanup.

The first Python adapter attempt contained a syntax error and stopped before
starting a child. Its original log is retained. Source review also found the
case-folded JSON and unexpected-stop-PID cases before publication; corrected
builds rerun the actual GPU checks. Earlier builds and their observations remain
retained and excluded from final acceptance.

[Sanitized audit evidence](evidence/GO-NATIVE-GPU-SUPERVISION-2026-10-29.json)
binds all 432 Go/module files in the build snapshot and current tree, the executable, original raw native
and GPU controls, full payload/pixel checks and the stop tests. Of the original
430 launcher files, **429 remain byte-for-byte unchanged**. An exact reverse
source check restricts `main.go` changes to the configuration and supervision
boundary above; original keyboard/cursor/window handlers and argument building
are preserved.

Next is the actual native argument profile: use the existing graphics composer
inside the owner while retaining the original kernel, disk, network, audio,
display/cursor choices and official QMP roles. Matching guest graphics bootstrap,
normal account/session startup, ordinary input, long browsing sessions, broad
application compatibility, failure recovery and matched-native performance are
still required. CUDA, video acceleration, DLSS, hardware ray tracing and the
93% performance target remain part of the full goal and are unverified.

The subsequent [original-argument profile checkpoint](ORIGINAL-LAUNCHER-GPU-PROFILE.md)
implements that argument/runtime path and an early VM binding before private QMP
readiness. Its current proof is CPU/helper coverage plus separate legacy GPU
regressions; VM execution in the new mode and normal session acceptance remain
unverified.
