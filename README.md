<p align="center">
  <img src="app/OmarchyIcon.svg" width="88" height="88" alt="Try Omarchy icon">
</p>

<h1 align="center">Try Omarchy for Windows</h1>

**7Wdev development fork:** [build and upstream sync](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/FORK.md) ·
[NVIDIA development PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2).
The experimental QEMU/WHPX bridge now captures a real **3840×2160 Omarchy desktop on NVIDIA**:
the shipped wallpaper, Omarchy bar and readable Alacritty application appear together.
Full integration into the Windows launcher and a stable interactive desktop remain in development.

![Real 4K guest GPU frame reaches QEMU SDL; active repeats pass and failures retained; backend 4/8 stages; desktop product 0/4 gates](https://raw.githubusercontent.com/7Wdev/try-omarchy-windows/windows-nvgpu-lab/docs/images/gpu-backend-progress-virtio-trace-2026-10-21.svg)

**Live trace follow-up:** moving the complete graphics diagnostics from the
emulated UART to a dedicated virtio-serial port reduces median steady GPU handoff
intervals from **43–44 ms to 16.6–16.7 ms** in this bounded workload. Two virtio
runs and two UART baselines pass 300 handoffs, full 4K source/SDL pixel audits,
input-response checks and live cleanup. A separate UART input-response failure
remains rejected. Official input and the GPU protocol are unchanged. These CPU
intervals do not establish displayed FPS, physical input latency or 93% native
performance.
[Actual SDL frame, complete traces and comparison limits](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-VIRTIO-GRAPHICS-TRACE.md).

**First SDL display checkpoint — October 21, 2026:** the real guest compositor now
reaches **QEMU SDL on NVIDIA**. Run 102 passes 300 GPU handoffs, full first/last
pixel audits and live GPU-reference cleanup. Both complete 4K SDL source captures
match the guest GPU buffers exactly; the scaled 2560×1566 SDL window differs by
at most one colour value from an independent reference. The official SDL input
implementation is retained, and the user reports moving the cursor.

**Active-workload follow-up:** four fresh 300-frame runs pass complete source
and SDL window pixel audits with live GPU cleanup. Both rejected attempts remain
retained: a corrected Winsock configuration guard and an intermittent all-zero
SDL window capture. Its cause is unconfirmed; the next repeat passes.
The original device-loss failure remains unresolved. **No performance gain is
established** by the transport comparison. Earlier timings include an intentional
50 ms repaint sleep and do not measure desktop FPS.
[Actual animated application, raw repeats, timings and limits](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-ACTIVE-SDL-TRANSPORT.md).

**Failure teardown follow-up:** a controlled frame-64 interruption reproduced a
secondary SDL consumer-loss error. Draining the SDL output before stopping QEMU
now retires all accepted frames and releases native objects while preserving the
original error. A separate fatal control verifies owned-VM exit before host abort.
The corrected bridge also passes a normal 300-frame full-pixel regression.
[Before/after evidence, actual rendering and remaining limits](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-SDL-FAILURE-TEARDOWN.md).

**Latest guest desktop checkpoint — October 20, 2026:** two fresh guests (runs 99–100)
pass **300 actual 4K GPU handoffs** while the same NVIDIA application grows from
1440×960 to 1600×1000 and returns to 1200×800. Complete first/last backbuffers
independently verify the enlarged panel and restored wallpaper. Each also passes
five GPU application launch/close cycles, the QMP input-response diagnostic and
natural shutdown with no mappings or native children deferred to VM exit.

**Product priority: retain the official keyboard/cursor code and QEMU SDL input
path, including the launcher's reserved-key routing.** Forty input-related source,
patch and control files are unchanged from the inspected official commit.
The real guest GPU relay uses that frontend's input implementation; automated
physical-input acceptance and normal launcher integration remain open.
[Verified upstream source map and integration boundary](https://github.com/7Wdev/try-omarchy-windows/blob/c6b554458583d14ea9fc6ef432b75d2a0aacc2a4/docs/UPSTREAM-INPUT-GRAPHICS-INTEGRATION.md).

![Actual QEMU SDL backbuffer showing the NVIDIA Omarchy desktop and application](https://raw.githubusercontent.com/7Wdev/try-omarchy-windows/a803f6b3d1b24ab482ed381cacab68c99fdf61d7/docs/images/omarchy-qemu-sdl-frame-300-2026-10-21.png)

This is run 102, frame 300, converted directly from the actual **2560×1566 SDL
pre-swap backbuffer**. Its GPU source is 3840×2160; physical 4K scanout remains
unverified. The guest runs through UWSM/PAM with a disposable account and RAM
overlay. The source disk is read-only. The released launcher uses the upstream
graphics runtime. [Earlier two-run resize milestone](https://github.com/7Wdev/try-omarchy-windows/blob/c6b554458583d14ea9fc6ef432b75d2a0aacc2a4/docs/LIVE-UWSM-SUSTAINED-RESIZE.md).

| Progress measure | Verified status | What remains |
| --- | --- | --- |
| 4K diagnostic scene | **3/3:** shipped wallpaper, Omarchy bar, readable GPU application | Normal launcher presentation and usable desktop acceptance. |
| Bounded rendering/input response | **Two 300-frame resize runs pass**, plus two separate 300-frame input-response baselines | Long runs, physical input through the official frontend and simultaneous application windows. |
| Real guest GPU display through QEMU SDL | **Four carrier-comparison runs pass 300 handoffs; median virtio intervals 16.6–16.7 ms** | Validate displayed cadence and physical input; resolve retained device-loss, zero-buffer and input-response failures. |
| Backend stages | **4/8 demonstrated; stages 5–6 in progress** | Supported device integration, broader APIs, recovery and performance. |
| Desktop product gates | **0/4 complete; first three in progress** | Normal account/launcher integration, continuous multi-window display, reliability and matched-native performance. |

The **1 GiB reported GPU usage guard is unchanged**. Idle staging reclamation
passes, but the trims still occur after the streams; hardware reallocation of a
trimmed slot during continued rendering remains unverified. Earlier failures
and their original evidence remain retained.

**First 4K desktop frame and guest-to-SDL connection: achieved. Next:** validate
continuous displayed cadence and official physical input with the faster trace
carrier; continue device-loss recovery, resource churn and simultaneous-window
acceptance.
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
