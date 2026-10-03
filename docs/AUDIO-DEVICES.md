# Windows audio device choices

`v0.1.0` introduced **Sound output** and **Microphone** selectors to
**Devices**. Each direction can use **Windows default** or a device enumerated
by the selected runtime's SDL library. The `v0.2.0` release applies choices at
VM startup. The public `v0.3.0` release adds live switching with r20c: host
Settings and Omarchy's guest audio switcher can change playback and recording
choices while the VM runs, and selected devices persist across guest reboots.
The microphone access gate still applies at the next VM start.

The bundled r19 runtime in `v0.2.0` includes
`0013-select-sdl-audio-devices.patch`, as did the prior r18 runtime. The older
v20 runtime lacks this patch; when selected instead, its selectors stay
disabled and Windows defaults remain in use. A separately managed external
runtime may also lack the patch.

## Behavior

- Output and input use separate choices. Changing Try Omarchy's selection does
  not change the Windows system default or another application's route.
- **Allow microphone access** remains the recording gate. Disabling it leaves
  playback enabled and prevents a saved microphone choice from enabling input.
- A selected device that cannot open at startup falls back to the Windows
  default for that direction, with a warning in `vm/qemu-stderr.log`.
- If neither the selection nor the default opens, the existing launcher audio
  fallback applies. A non-SDL or older runtime logs that it cannot apply saved
  selections and uses defaults.
- Reopen Settings to refresh the device list. A disconnected choice is retained
  until changed. On the older r19 runtime, hot-unplug and default-device changes
  during playback still depend on SDL/Windows; restart the VM if routing is not
  recovered. r20c polls live routes and falls back if a selected endpoint
  disappears. Physical hotplug acceptance remains untested.
- Preferences retain SDL device names and, when a name uniquely matches an active
  Core Audio endpoint, its stable Windows endpoint ID. The ID resolves the current
  friendly name at each start, so ordinary renames and reboots do not discard the
  selection. Ambiguous duplicate names remain name-based. Live guest-driven
  switching ships in `v0.3.0` with r20c; public `v0.2.0` remains a previous
  startup-only release. This is not full Mac audio parity.

`audio-preferences.json` and the separate `audio-endpoints.json` live beside
`settings.json` and are included in current backups and recovery copies. Keeping
the IDs separate lets older launchers continue reading the name preferences during
rollback. Older launchers ignore the ID file. Device enumeration does not open
playback or recording streams. Diagnostic bundles report whether a selection
exists and omit device names and endpoint IDs.

## r20c live-route support

The r20 source recipe adds a private `vm/audio-control` directory. When the
runtime contains `0016-live-sdl-audio-routes.patch`, the launcher writes separate
output and input routes before QEMU starts. Saving audio choices in Settings
writes atomically replaced route files; the active SDL backend polls them and
reopens a changed route without restarting the guest. The microphone permission
gate still requires a new VM start because QEMU creates its input voices at
launch. Unsupported runtimes retain the startup-only behavior above.

The r20c runtime shipped as `runtime-v1-r20c` in `v0.3.0`. Its loopback-only
virtio serial catalog and guest PipeWire service offer active Windows endpoints
that SDL can identify unambiguously. Choosing one in Omarchy saves its stable
endpoint ID and updates QEMU's live route; Settings choices are polled back into
the guest. Microphone-off removes host input choices from the guest picker.
The [signed and public acceptance record](evidence/V030-SIGNED-CANDIDATE-2026-09-24.md)
passed playback, capture, live route changes from the guest and host Settings,
saved-choice persistence across guest restart, idle release, rollback, and the
public update path. The [signed packaged candidate checks](evidence/LIVE-AUDIO-R20-2026-09-23.md)
also passed the rebuilt image's playback, capture, route fallback, microphone
permission, idle release, and Windows boot checks. Public `v0.2.0` used r19 and
startup-only choices. Two physical endpoints per direction and hotplug remain
untested.

## Sample rates

The launcher reads each effective endpoint's shared-mode mix format from
`PKEY_AudioEngine_DeviceFormat` and supplies `out.frequency` and `in.frequency`
to QEMU's SDL backend at each VM start, including a guest reboot. Saved endpoint
IDs first resolve to current SDL names. A missing selection or unreadable rate
uses the Windows default for that direction, then 48000 Hz if lookup fails.
Older runtimes that cannot select devices use default endpoint rates. Input
frequency is omitted when microphone access is disabled. Reading endpoint
properties opens no playback or recording stream; rate failures do not stop boot.

The live-route runtime preserves its original callback format and QEMU's mixer
buffers when reopening an endpoint. SDL/WASAPI converts to the new device format
if a live selection, default change or hotplug changes its rate. Restart Omarchy
to match that new rate directly. Changing QEMU's mixer format during a stream is
not supported by the current control protocol. The same fixed-format reopen
behavior exists in Mac's SDL route patch. Matching rates avoids one resampling
stage; it does not increase volume or remove guest-side format conversion.

## Volume and quiet playback

Playback passes through these controls in order:

```text
guest app -> visible Windows route remap sink -> ALSA virtio transport sink
          -> QEMU mixer -> SDL/WASAPI session -> Windows endpoint
```

Guest patch 0091 creates the remap sinks but leaves their underlying ALSA sink
volume alone. The pinned WirePlumber 0.5.18 configuration has a
[default sink gain of 0.064](https://github.com/PipeWire/wireplumber/blob/0.5.18/src/config/wireplumber.conf),
which Pulse controls display as 40%. That leaves about 24 dB of attenuation when
the visible route is at 100%, unless a saved transport volume overrides it.
There is no explicit 40% or 50% volume command in the Windows guest patches.
Omarchy uses a software ALSA mixer, so the transport volume remains a real gain.

Patch 0120 keeps the identified `VirtIO SoundCard` transport at unity while the
route remaps are active. It preserves transport mute, visible route volume,
application volume and microphone gain. On orderly bridge shutdown, including
SIGTERM, it restores the previous channel volumes only if the transport is still
at unity; a manual change survives. A forced kill cannot run that cleanup. Errors
leave routing working and log that transport volume could not be set.
Compatibility revision 46 carries the fix to existing disks. Other ALSA devices,
including passed-through USB audio, are not amplified by this fix.

The pinned QEMU
[virtio-sound frontend](https://github.com/cmspam/winq-emu-qemu/blob/2ce303cfbbc8b0e4a7a3c66e27a094a980426d73/hw/audio/virtio-snd.c)
passes PCM bytes to `audio_be_write` and does not set a volume. Its SDL mixing
backend starts with unity gain; the route patches add no attenuation. SDL/WASAPI
opens a shared Windows audio session without overriding its volume. Windows can
remember a lower per-app session volume, independently of the endpoint slider.
The read-only `probe-audio-sessions.exe QEMU_PID` helper now reports both session
and endpoint volume/mute when their APIs succeed. Missing fields mean the API
was unavailable, not unity gain.

Guest master, Windows per-app session, and Windows endpoint controls remain
independent, as they are on Mac. Mirroring the guest's already applied gain into
the host session would attenuate twice. Replacing guest gain would need reliable
bidirectional ownership across remaps, apps, mute, restart and route changes;
the current bridge only transports route selections. Changing the system endpoint
volume would also affect other Windows applications. This change therefore does
not synchronize the sliders or override Windows session preferences.

This establishes and fixes a hidden transport gain in the source path, but does
not prove that it was the reporter's saved state in
[#277](https://github.com/omacom/try-omarchy-windows/issues/277). Confirm actual
transport, route, application and host session levels on the affected laptop
before treating that report as resolved.

For physical acceptance:

- Play and record through 44.1 kHz and 48 kHz endpoints. Check startup frequency
  values in `vm/shell.log`, separate saved playback/capture choices, unavailable
  selections and microphone-off behavior.
- Switch both directions from host Settings and the guest picker while streaming,
  including 44.1 to 48 kHz, defaults and unplug/replug. Check continuity and confirm
  a restart selects the new rate directly.
- On a fresh guest and an updated persistent disk, use `pactl --format=json list
  sinks` and `pactl list sink-inputs` to check the visible route, virtio transport
  and app gains. Set the visible route to 100% and verify transport unity. Check
  that ordinary guest volume/mute controls still work and bridge shutdown restores
  the prior transport gain without discarding manual changes.
- Compare the same speech and music in Windows and Omarchy on the same endpoint,
  with player normalization/enhancements accounted for and matching app levels.
  Begin at a comfortable Windows volume, then compare 100% levels. Run the session
  probe during playback to record QEMU session and endpoint gains, including the
  Windows communications ducking setting. Listening and the affected laptop's
  enhancement path cannot be established by cross-compilation or contract tests.

## Validation

The [September 21 physical acceptance](evidence/AUDIO-PARITY-2026-09-21.md)
passed built-in speaker/microphone routing, startup fallback and microphone-off
checks on the r16 engineering runtime. The [September 22 integration pass](evidence/PARITY-MASTER-INTEGRATION-2026-09-22.md)
passed stable endpoint enumeration and persistence on the same laptop. A physical
rename and two-device switching remain untested.

Local regression coverage includes preference round-trip/corruption, backup
round-trip, direction separation, microphone disablement, inherited environment
cleanup and old-runtime capability gating. The runtime build compiles the exact
patched route function against a fake SDL device opener to cover independent
routes, failed selections, failed defaults and invalid UTF-8.

Native Windows tests use:

```powershell
$env:TRYOMARCHY_UI_TEST='1'
$env:TRYOMARCHY_LAUNCHER_TEST_EXE='<candidate launcher>'
$env:TRYOMARCHY_AUDIO_TEST_QEMU='<runtime>\bin\qemu-system-x86_64w.exe'
& '<candidate tests>' -test.v '-test.run=TestNativeAudioDeviceEnumeration|TestAudio'
```

Run these on the signed-in desktop with no other launcher window. Native tests
check SDL and stable endpoint enumeration, disabled choices with an older runtime,
independent choices with a supporting runtime, persistence on reopening, and
return to defaults.
The settings test uses a temporary installation and does not boot a VM.

Hardware acceptance for each release candidate must additionally boot the existing guest with the rebuilt
runtime, play a short test sound, exercise recording with microphone permission
off/on, and test a nonexistent device's startup fallback. Two physical endpoints
are needed to prove routing away from the system default. Track those results
separately from source-level tests and successful compilation.

`runtime-build/probe-audio-sessions.c` is a read-only acceptance helper: while
the guest plays or records, it reports Windows endpoint sessions belonging to a
specified QEMU process. It uses Microsoft's documented
[session enumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator)
and [process identity](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getprocessid)
APIs. A reported active session is routing evidence, not proof that a human heard
the speaker or that a physical microphone produced intelligible audio.
