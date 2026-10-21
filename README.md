<p align="center">
  <img src="app/OmarchyIcon.svg" width="88" height="88" alt="Try Omarchy icon">
</p>

<h1 align="center">Try Omarchy for Windows</h1>

**7Wdev development fork:** [build and upstream sync](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/FORK.md) ·
[NVIDIA development PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2).
The experimental QEMU/WHPX bridge now captures a real **3840×2160 Omarchy desktop on NVIDIA**:
the shipped wallpaper, Omarchy bar and readable Alacritty application appear together.
Full integration into the Windows launcher and a stable interactive desktop remain in development.

![First 4K Omarchy frame achieved; QEMU SDL host GPU import and BGRA conversion verified; backend 4/8 stages; desktop product 0/4 gates](docs/images/gpu-backend-progress-qemu-sdl-2026-10-21.svg)

**Latest display checkpoint — October 21, 2026:** the actual QEMU SDL frontend
now imports a shared **4K NVIDIA GPU texture from a separate Windows process**.
RGBA and GPU-converted BGRA pass independent complete source/window pixel
audits, including a **300-handoff BGRA run** with 298 ordinary GPU-only handoffs.
This paused, disk-free host control is the next display building block; the real
guest compositor still needs connection to it.
[Hardware evidence, retained BGRA failure and scope](https://github.com/7Wdev/try-omarchy-windows/blob/6db01ffa69db405f9c76fb51da335bcf0513e8f6/docs/LIVE-QEMU-SDL-GPU-RELAY.md).

**Latest guest desktop checkpoint — October 20, 2026:** two fresh guests (runs 99–100)
pass **300 actual 4K GPU handoffs** while the same NVIDIA application grows from
1440×960 to 1600×1000 and returns to 1200×800. Complete first/last backbuffers
independently verify the enlarged panel and restored wallpaper. Each also passes
five GPU application launch/close cycles, the QMP input-response diagnostic and
natural shutdown with no mappings or native children deferred to VM exit.

**Product priority: retain the official keyboard/cursor code and QEMU SDL input
path, including the launcher's reserved-key routing.** Forty input-related source,
patch and control files are unchanged from the inspected official commit.
The new host GPU relay retains that frontend's input implementation; real guest
graphics and normal launcher integration remain open.
[Verified upstream source map and integration boundary](https://github.com/7Wdev/try-omarchy-windows/blob/c6b554458583d14ea9fc6ef432b75d2a0aacc2a4/docs/UPSTREAM-INPUT-GRAPHICS-INTEGRATION.md).

![Actual unedited 4K NVIDIA desktop after application resizing and input response](https://raw.githubusercontent.com/7Wdev/try-omarchy-windows/c6b554458583d14ea9fc6ef432b75d2a0aacc2a4/docs/images/omarchy-4k-restored-application-gpu-2026-10-20.png)

This is run 100, frame 300, converted directly from the complete GPU backbuffer.
[Enlarged/restored images, four-run raw evidence and scope](https://github.com/7Wdev/try-omarchy-windows/blob/c6b554458583d14ea9fc6ef432b75d2a0aacc2a4/docs/LIVE-UWSM-SUSTAINED-RESIZE.md).
Hyprland and the applications run through UWSM in a verified PAM/logind session,
using a disposable seeded account and RAM overlay. The source disk is read-only.
The separate diagnostic Windows client is 1280×720; physical 4K scanout and
every-frame display remain unverified. The released launcher uses the upstream
graphics runtime.

| Progress measure | Verified status | What remains |
| --- | --- | --- |
| 4K diagnostic scene | **3/3:** shipped wallpaper, Omarchy bar, readable GPU application | Normal launcher presentation and usable desktop acceptance. |
| Bounded rendering/input response | **Two 300-frame resize runs pass**, plus two separate 300-frame input-response baselines | Long runs, physical input through the official frontend and simultaneous application windows. |
| QEMU SDL GPU display building block | **4K cross-process RGBA/BGRA host controls pass**, including 300 GPU handoffs | Connect real guest compositor buffers and validate the normal launcher/input path. |
| Backend stages | **4/8 demonstrated; stages 5–6 in progress** | Supported device integration, broader APIs, recovery and performance. |
| Desktop product gates | **0/4 complete; first three in progress** | Normal account/launcher integration, continuous multi-window display, reliability and matched-native performance. |

The **1 GiB reported GPU usage guard is unchanged**. Idle staging reclamation
passes, but the trims still occur after the streams; hardware reallocation of a
trimmed slot during continued rendering remains unverified. Earlier failures
and their original evidence remain retained.

**First 4K desktop frame: achieved. Next:** connect real guest GPU buffers to the
tested QEMU SDL relay while preserving official input, and exercise sustained
resource churn and simultaneous windows.
The checklist counts do not measure engineering effort.
[Gate definitions and evidence](docs/GPU-BACKEND-PROGRESS.md).

Vulkan, CUDA, video acceleration, broader NVIDIA features, concurrent Windows/guest workloads
and **≥93% matched-native performance** each still require real-application acceptance tests.

Download links below refer to official upstream releases.

<p align="center">The full Omarchy desktop, running in a window on your Windows PC.</p>

<p align="center">
  <strong><a href="https://github.com/omacom/try-omarchy-windows/releases/latest/download/TryOmarchy.exe">Download TryOmarchy.exe</a></strong>
  &nbsp;·&nbsp; <a href="https://github.com/omacom/try-omarchy-windows/releases/latest/download/TryOmarchy.exe.sha256">SHA256</a>
  &nbsp;·&nbsp; <a href="https://github.com/omacom/try-omarchy-windows/releases/latest">Latest release</a>
</p>

<p align="center">Windows 10 or 11 · x86_64 · Hardware virtualization required</p>

<picture>
  <source media="(prefers-reduced-motion: reduce)" srcset="docs/images/try-omarchy-windows-still.jpg">
  <img src="docs/images/try-omarchy-windows.gif" width="960" alt="The Omarchy pixel wordmark appears, then the Omarchy desktop is shown in a Try Omarchy window on Windows 11">
</picture>

Try Omarchy runs [Omarchy](https://omarchy.org) in a virtual machine. You can explore its apps, themes, and keyboard-first workflow without repartitioning your drive or leaving Windows. Your Linux files persist between sessions.

<a id="try-it"></a>

## Get started

1. **Download and open [TryOmarchy.exe](https://github.com/omacom/try-omarchy-windows/releases/latest/download/TryOmarchy.exe).** The launcher is about 10 MB and is signed by Brandon South. You can check the signature in the file's **Properties > Digital Signatures** tab.
2. **Choose where to keep Omarchy.** The default is `%LOCALAPPDATA%\TryOmarchy`. Setup may enable Windows Hypervisor Platform and request one restart.
3. **Set up your own account, or pick the quick start.** Your own account is the default. The app then downloads and verifies the graphics runtime and guest image, about 2 GB in total, and starts Omarchy. Omarchy's setup asks for your username and password; the quick start skips that form and signs in as `omarchy` / `omarchy`. Once the desktop opens, press **Super+Space** for the Omarchy menu and **Super+K** to see the keybindings.

See the [user guide](docs/USER-GUIDE.md) for updates, shortcuts, storage, settings, and removal.

## What you can do

- **Use the whole desktop.** Hyprland, Omarchy's apps, themes, menus, notifications, and screensavers run inside the window.
- **Move between Windows and Omarchy.** Share text, images, files, and folders through the clipboard and the optional shared folder. Drop Windows files onto an open Files folder or onto an app, such as a browser upload area.
- **Use your hardware.** Supported graphics drivers can render through VirGL and Venus Vulkan. The launcher falls back to CPU rendering when that path is unavailable.
- **Make it yours.** Choose a display, audio devices, resource profile, and optional fullscreen or direct-launch shortcuts. Windows Hello for guest `sudo` is available for personalized accounts.
- **Keep your work.** The guest disk persists, with backup, restore, reset, and update controls.
- **Take it with you.** When you install Omarchy for real, one command brings your settings, apps, files, and projects over from the trial. See [moving to a real install](docs/MIGRATION.md).

[Watch the Windows demo](https://tryomarchy.com/images/try/windows.mp4) or read the [compatibility guide](docs/COMPATIBILITY.md) for the current hardware and application limits.

## Before you start

- You need a **64-bit Windows 10 or 11 PC** with hardware virtualization enabled. ARM64 Windows PCs are not supported.
- Standard installs use a local **NTFS or ReFS** drive. The guest and downloads live in the folder you choose. First setup downloads the guest image and a portable graphics runtime.
- GPU acceleration depends on the Windows GPU, driver, and bundled runtime. CPU rendering remains available. This is a virtual GPU, not physical GPU passthrough; check [graphics and app limits](docs/COMPATIBILITY.md) for specific workloads.
- Keep important guest files backed up outside the VM. [Backups](docs/BACKUP.md) and [moving an installation](docs/MOVING.md) are documented separately.

## Guides

- [Guide index](docs/README.md): find help for a specific task
- [Using Try Omarchy](docs/USER-GUIDE.md): setup, keys, settings, storage, updates, SSH, and troubleshooting
- [Compatibility](docs/COMPATIBILITY.md): supported hosts, graphics paths, and app limits
- [Windows desktop controls](docs/DESKTOP-CONTROLS.md): display, input, and host integration
- [Guest upgrades](docs/GUEST-UPGRADES.md), [backups](docs/BACKUP.md), and [moving your data](docs/MOVING.md)
- [Changelog](CHANGELOG.md) and [releases](https://github.com/omacom/try-omarchy-windows/releases)
- [Contributing](CONTRIBUTING.md): repository layout, build, and test instructions

If something goes wrong, use **Create diagnostics...** in the tray and [open an issue](https://github.com/omacom/try-omarchy-windows/issues/new/choose). The [user guide](docs/USER-GUIDE.md#reporting-a-problem) explains what it contains.

## Under the hood

Try Omarchy uses QEMU on the Windows Hypervisor Platform, an x86_64 Arch guest image, and the [WINQ-EMU](https://github.com/cmspam/winq-emu) graphics runtime. The Go launcher handles setup, updates, VM supervision, and Windows integration. See the [technical findings](docs/FINDINGS.md), [runtime build](runtime-build/README.md), [guest build](guest-build/README.md), and [release process](docs/RELEASING.md).

To build the launcher with Go, run:

```sh
git clone https://github.com/omacom/try-omarchy-windows
cd try-omarchy-windows/app
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-H windowsgui -s -w" -o TryOmarchy.exe .
```

## Credits and license

Try Omarchy for Windows builds on [Omarchy](https://github.com/basecamp/omarchy), Eduardo's original Try Omarchy app, Jorge Silva's [Windows guest builder](https://github.com/jorge-huxley/try-omarchy-win), [WINQ-EMU](https://github.com/cmspam/winq-emu), and [Chainfire's Windows GPU work](https://github.com/Chainfire/omarchy-windows-hyperv-gpu). Thanks to the [dockur/windows](https://github.com/dockur/windows) project for the Windows development environment.

Scripts and documentation in this repository are [MIT licensed](LICENSE). Omarchy and the guest image carry their own licenses; see the [third-party notices](THIRD_PARTY_NOTICES.md). The app icon uses the [official Omarchy mark](https://omarchy.org/brand/), which remains subject to Omarchy's trademark rights.

Looking for the Mac version? See [Try Omarchy for macOS](https://github.com/omacom/try-omarchy).
