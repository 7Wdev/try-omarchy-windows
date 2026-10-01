# v0.7.0 signed candidate and release acceptance

v0.7.0 adds the Windows install walkthrough and native Linux importer, brings
the importer into guest exports, updates launcher branding, remembers USB
selections and follows Windows time-zone changes. The guest carries patches
through `0113`, compatibility revision 44. The GPU runtime remains r20c.

## Candidate

- [Prepare run 36798233073](https://github.com/omacom/try-omarchy-windows/actions/runs/36798233073)
  built the guest and importer, passed the guest contract and importer tests,
  authenticated the assets and booted the instant account headlessly. The
  package refresh in #247 resolved six Arch package version changes.
- The manifest pin is
  `aa790d2a369777bfba4c14e489f35eec0eb7aae55ae3bbd1580928904724b271`.
  The installed and published importer bundle is
  `561cf0164aa992c958aaec02dd58aecdfdb68d2233354498bb9a19100392f524`.
- The first physical walkthrough check found that its subprocess could not
  open while the trial owned the lifecycle port. Commit `3f553ca` lets this
  inspection open without taking lifecycle or recovery ownership. A native
  regression also checks that a pending checkpoint journal is unchanged.
- [Signing check 36799709678](https://github.com/omacom/try-omarchy-windows/actions/runs/36799709678)
  built that exact commit. Candidate SHA-256:
  `a3ecbe05c2948227e1684dd9ae1d768624390a0e08432d042e9017a285ab461c`.
  Authenticode was Valid with publisher Brandon South. Defender returned no
  detections with file exclusions ignored, no matching path exclusion and
  signatures 1.459.491.0. This is a file-scan result, not SmartScreen acceptance.
- Five native tests passed with that signed executable: opening the walkthrough
  during an active lifecycle, resource profiles, Settings action visibility,
  all setup prompts and setup button layout on a small work area.

## Physical Windows acceptance

The AMD Windows 11 laptop ran a disposable copy of an existing v0.6.2 trial,
under WHPX with virgl and Venus. Both guest and runtime assets were served over
loopback with the exact manifest pin. The original trial was preserved.

- Stopping the first candidate boot before userspace readiness left the guest
  update pending. The next launch restored revision 42, reached a healthy
  desktop with zero failed units, and made no payload downloads.
- After a clean poweroff, the next upgrade reached revision 44 and confirmed
  v0.7.0 only after userspace readiness. The document marker remained byte
  identical. The installed importer matched the release asset and reported
  v0.7.0. The audio sink was present.
- Guest reboot and launch after poweroff passed. Neither repeated the upgrade.
- Settings > Recovery > Install Omarchy opened while the trial was running.
  Its shutdown button powered off the guest and advanced to the install steps.
  The short import command and uninstall warning fit on the page.
- A separate fresh writable disk passed the share, account and shortcut
  prompts, then reached a GPU desktop with revision 44, zero failed units and
  no Hyprland configuration errors. This used authenticated cached factory
  assets; the full download and unpack path was exercised by the upgrade and
  release build. The fresh disk was removed afterwards.

The copy helper did not preserve file modification times, so the first copied
cache failed its receipt check. The old guest files were checked against their
authenticated hashes before restoring their original timestamps. One local
HTTP server also exited during an early download; the complete assets were
then served directly on the laptop. Neither failure reached a guest upgrade.

## Native Linux import

The owner-approved import into a separate account on the laptop's existing
encrypted Linux installation passed before release preparation. The importer
found the Windows trial, imported changed files and tools, and repeated without
further changes. The customized SD-card trial also passed with byte comparisons.
The owner observed both UAC cancellation and approval for Fast Startup.
The primary Linux account and source trials were preserved, and temporary Linux
accounts, mounts, SSH access and sudo rules were removed. See the
[physical migration record](MIGRATION-LAPTOP-2026-09-30.md).

Fresh native installation from USB, partition resizing and the imported test
account's desktop login remain untested. The owner explicitly accepted these
limits and chose to preserve the existing Linux installation.

## Publication

[Publish run 36801309267](https://github.com/omacom/try-omarchy-windows/actions/runs/36801309267)
passed, published v0.7.0 and promoted it to Latest. The tag points to the tested
commit `3f553ca31b86e699125949dbac5b7ab3add3ed55`. Current and legacy repository
download routes and both update feeds passed the workflow's public checks.

The public launcher SHA-256 is
`c20ebd8e7de87b36f25be9a449d28748c6cdca0b3834f46ed9e483d38d3b2b2d`.
It is a separate signed build of the tested source. On the laptop its
Authenticode signature was Valid, ProductVersion v0.7.0, and Defender reported
no detections with exclusions ignored and signatures 1.459.491.0.

The copied v0.6.2 launcher accepted the signed public update, installed the
exact public executable, kept the unchanged runtime and booted revision 44.
The system was healthy with zero failed units, the document marker was
unchanged, and both launcher and guest updates committed after userspace
readiness. The new receipt points to the public v0.7.0 assets. Poweroff passed.
The short import URL fetched the expected bootstrap hash and reported v0.7.0.

The owner requested a polished release announcement instead of changelog
bullets. The live release now uses the reviewed prose in
`.github/release-notes/v0.7.0.md`, with title "v0.7.0: From trial to install".
The release helper now rejects missing announcements instead of copying the
changelog. Its focused tests passed.

All 12 temporary Windows tasks, both disposable test directories and their
uninstall entries were removed. No test VM remained running. The original
trial's disk size, recorded modification time and launcher hash were unchanged.
Windows C: had 28.4 GiB free after cleanup. The existing native Linux installation
was preserved.

Private logs and screenshots are retained locally under
`/data/try-omarchy-v070-release-20260930/`. The social video uses clean demo
data and does not show a native Linux installation.
