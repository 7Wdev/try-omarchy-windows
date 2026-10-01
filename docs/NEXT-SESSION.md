# Try Omarchy Windows continuation - September 30, 2026

The current normal release is [v0.7.0](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.7.0), published and Latest on September 30 (October 1 UTC). Settings > Recovery > Install Omarchy now guides installation next to Windows and importing the trial. The launcher checks BitLocker without PowerShell, and the import command uses `https://tryomarchy.com/import`. Branding, USB selection persistence and guest time-zone synchronization also ship. See the [signed candidate and public release record](evidence/V070-SIGNED-CANDIDATE-2026-09-30.md) and [publish run](https://github.com/omacom/try-omarchy-windows/actions/runs/36801309267).

[v0.6.2](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.6.2) shipped on September 29. New installs default to your own account. The quick-start account now accepts only SSH keys; existing quick-start guests receive that restriction on their first boot after updating. Accounts you created yourself keep password login. Setup also checks space for the unpacked guest before downloading. See the [changelog](../CHANGELOG.md#v062---2026-09-29) and [publish run](https://github.com/omacom/try-omarchy-windows/actions/runs/36624010076).

[v0.6.1](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.6.1) added a setting that sends Alt+Tab to Windows, stopped guest services restarting after Modern Standby, kept a monitor scale set in `monitors.lua` across reloads and refreshed guest packages; see the [v0.6.1 candidate and release record](evidence/V061-SIGNED-CANDIDATE-2026-09-29.md). Its acceptance included a public update from an installed v0.6.0 launcher. [v0.6.0](evidence/V060-SIGNED-CANDIDATE-2026-09-27.md) added drops into apps and 1Password unlock with Windows Hello.

[v0.3.0](https://github.com/omacom/try-omarchy-windows/releases/tag/v0.3.0) added live Windows audio device switching to the v0.2.0 features. The [signed and public acceptance record](evidence/V030-SIGNED-CANDIDATE-2026-09-24.md) links the public update check, exact hashes, physical upgrade, audio checks, reboot and rollback results. The user's normal Windows installation was not changed. The Intel/NVIDIA PC remains booted into Omarchy at the owner's request.

The record also notes one clean guest poweroff during an early candidate boot. It was not reproduced in later boots and its cause remains unknown; see the [acceptance evidence](evidence/V030-SIGNED-CANDIDATE-2026-09-24.md#review-of-the-earlier-0122-poweroff).

Thousands of users and relatively few bug reports are a useful positive signal for the everyday experience. Do not use broad Windows 10 or hardware coverage as an automatic gate for future 0.x releases. Fix reproducible reports as they arrive. Keep the realistic feature gaps active:

- [Windows Hello sudo #165](https://github.com/omacom/try-omarchy-windows/issues/165): shipped in v0.5.0 as an opt-in; see the [design](WINDOWS-HELLO.md) and the [laptop run](evidence/HELLO-SUDO-LAPTOP-2026-09-26.md). The 1Password unlock in [#176](https://github.com/omacom/try-omarchy-windows/issues/176) builds on it and shipped in v0.6.0.
- [True LAN bridge #166](https://github.com/omacom/try-omarchy-windows/issues/166): opt-in, reversible signed TAP plus wired-Ethernet path to evaluate. NAT and explicit LAN port forwarding work today. The laptop's only active connection is Wi-Fi, so changing its adapter bindings remotely is unsuitable.
- [Live audio switching #167](https://github.com/omacom/try-omarchy-windows/issues/167): shipped in `v0.3.0`. Host Settings and the Omarchy audio picker switch Windows playback and recording devices while the VM runs; saved choices persist across guest reboots, and microphone access changes apply at the next VM start. Only two physical endpoints per direction and hotplug acceptance remain. See the [signed and public acceptance record](evidence/V030-SIGNED-CANDIDATE-2026-09-24.md).
- [Embedded Windows app windows #160](https://github.com/omacom/try-omarchy-windows/issues/160): phase 1 launch-and-return ships in v0.2.0. Window capture, input, focus, scaling and lifecycle are a separate milestone.
- [Direct application drops #174](https://github.com/omacom/try-omarchy-windows/issues/174) shipped in v0.6.0: a drop lands in the app under the pointer and still saves to Downloads. [Intel/NVIDIA Venus #173](https://github.com/omacom/try-omarchy-windows/issues/173) tracks a concrete older-runtime Vulkan/Godot failure without switching the owner's PC away from Omarchy. [ARM64 #131](https://github.com/omacom/try-omarchy-windows/issues/131), [interface translation #127](https://github.com/omacom/try-omarchy-windows/issues/127), and USB device reports remain open or documented. [PR #184](https://github.com/omacom/try-omarchy-windows/pull/184) brought the pinch touchpad rules to existing guests, and physical pinch passed on the migrated laptop guest. The launcher turns pinch on for guest images that declare the device (patch `0095`), which v0.4.0 ships.

The [feature tracker](MAC-PARITY.md), [1.0 quality bar](RELEASE-READINESS.md), [runtime checklist](RUNTIME-VALIDATION.md), and [remote laptop instructions](REMOTE-LAPTOP-TESTING.md) provide details. Historical physical evidence remains under `docs/evidence/`.

For all GitHub mutations under `/home/bts/Projects`, invoke `/home/bts/.local/bin/gh`, verify `gh api user --jq .login` returns `btsouth`, and use the personal git identity. Do not touch historical `bts-cssi` authorship; the user explicitly chose to leave it alone.

## September 30 migration continuation

The importer in [#241](https://github.com/omacom/try-omarchy-windows/pull/241)
is merged. [#242](https://github.com/omacom/try-omarchy-windows/pull/242)
(guest export) and [#243](https://github.com/omacom/try-omarchy-windows/pull/243)
(Windows walkthrough) have passed the physical import and UAC checks below.
#243's implementation commit `85ca7f6` passes all four CI checks; its latest
documentation head `77fbce4` also passes (run `36767044791`).
The #243 follow-up reports unknown encryption status with a BitLocker
settings button and opens Disk Management using its full Windows system path.
The short prerequisite pages are compact; the install steps have enough text
space to show the entire import command and uninstall warning together.
Focused launcher tests, 13 native Windows tests, Linux and configured Windows
vet, Windows build and all 112 importer tests pass.

The Windows 11 VM check opened the actual walkthrough, Disk Management and
the install guide. Native fixtures covered all three prerequisites together,
decryption in progress and unknown encryption on a separate trial drive; the
unknown-state button opened BitLocker settings. No disk was encrypted or resized.
The Fast Startup button changed `HiberbootEnabled` from 1 to 0 and returned to
the install steps, using a temporary hibernation registry stand-in. Both power
values were restored. UAC stayed disabled as found, so there was no permission
prompt. The final unsigned build passed an explicit Defender file scan with
file exclusions ignored and signatures 1.459.485.0. This is not SmartScreen
or signed-release acceptance. Screenshots, logs, the build and cleanup evidence
are retained locally under `/data/try-omarchy-install-review-20260930/`.

The #242 existing-disk check now passes. A disposable copy of the customized
revision-39 trial booted with the exact CI candidate from run `36742607778`
(source `496c23d`), received revision 44 through the initramfs compatibility
repair, preserved its custom files and exported 159 files. The archive's
bundled importer matches the installed one, and the exporter still works
after reboot. A network-isolated omabox copied documents, a Git workspace,
Chromium bookmarks and test SSH keys with matching bytes. The broader import
also copied settings, then reported the unavailable mise downloads and the
old fixture's invalid `hl.bind` line. Those follow-up steps are not a clean
settings acceptance. Evidence is retained locally under
`/data/try-omarchy-export-upgrade-20260930/`; the original trial is unchanged.

The owner then booted and unlocked the existing laptop Linux installation.
Real UAC cancellation left Fast Startup enabled; approval turned it off and
returned to the install steps. The importer found the Windows trial automatically,
copied its five changed files and installed its mise tools. The customized SD-card
trial copied 123 changed files, with 112 data files matching the source byte for
byte. Both reruns made no further changes. The source mounts were read-only and
ID-mapped to the separate test account. All importer mounts and loop devices were
removed; 49 primary-account configuration entries remained unchanged. The test
account, temporary sudo rules, SSH key and firewall rule were removed, and SSH
was stopped without enabling it. See the [physical acceptance record](
evidence/MIGRATION-LAPTOP-2026-09-30.md).

#242 and #243 are merged and ship in v0.7.0. The signed candidate passed
native UI, Defender, physical guest upgrade, forced rollback, reboot, poweroff,
second launch and a fresh Windows trial. The walkthrough also opened while
Omarchy was running and shut it down through its own button. The owner accepted
the untested fresh native USB install, partition resizing and imported test
account desktop login, preserving the existing Linux installation.

The public release contains both importer assets. The short import URL resolves
to the authenticated v0.7.0 bootstrap and its `--version` check passes. Public
update acceptance and temporary Windows test cleanup are recorded in the
[v0.7.0 release record](evidence/V070-SIGNED-CANDIDATE-2026-09-30.md).

Bridge handoff: #233 and #234 merge cleanly into their stacked bases, but the
bridge control ID 2127 collides with master's USB selection. Reserve 2130 for
#243's Install Omarchy button. After the v0.7.0 lock refresh, the bridge patch must follow
patch 0113 and use compatibility revision 45 or later. No bridge branch was
changed during this review.
