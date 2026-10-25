<p align="center">
  <img src="app/OmarchyIcon.svg" width="88" height="88" alt="Try Omarchy icon">
</p>

<h1 align="center">Try Omarchy for Windows</h1>

**7Wdev development fork:** [build and upstream sync](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/FORK.md) ·
[NVIDIA development PR #2](https://github.com/7Wdev/try-omarchy-windows/pull/2).
The experimental QEMU/WHPX bridge renders the shipped Omarchy wallpaper, bar
and two readable applications on NVIDIA at **3840×2160**, through QEMU SDL.
Normal launcher integration and a stable interactive desktop remain in development.

[Daily-use readiness and the next Chromium browsing checks](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/DAILY-USE-READINESS.md).

![Two-window 4K NVIDIA fixture verified; native health queries supported; backend 4/8; desktop product 0/4](https://raw.githubusercontent.com/7Wdev/try-omarchy-windows/windows-nvgpu-lab/docs/images/gpu-backend-progress-device-state-2026-10-25.svg)

**Native GPU health — October 25, 2026:** the published-runtime build completes
**5,786 real device-health queries and 1,800 GPU handoffs**, including application
close/relaunch, complete source/SDL pixels and live cleanup. Native errors and
fault states are preserved. A separate private rollback experiment crossed the
1 GiB guard and failed; it remains excluded. Progress counts stay unchanged.
[Actual frame, validation and retained failure](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/GPU-DEVICE-EXECUTION-STATE.md).

**Residency recovery investigation — October 24, 2026:** a controlled
single-allocation refusal recovered through 1,800 GPU handoffs, but a
four-allocation repeat caused device loss. That runtime candidate stays
unpublished. The failure is retained with 16 replay checks; private paging-queue
research continues and progress counts stay unchanged.
[Actual frame, rejected candidate and next check](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-RESIDENCY-ROLLBACK-INVESTIGATION.md).

**Verified metadata checkpoint — October 24, 2026:** immutable metadata requests fall
from **54 to 12 across six controlled validations**, while mutable counters
stay fresh. A new **1,800-handoff 4K GPU desktop regression** passes complete
pixels, two application windows, input response, resizing and live cleanup.
Atomic diagnostic receipts fix a real journal-prefix collision. Rendering
speedup and memory-pressure recovery remain unverified; two earlier pressure
failures stay rejected. [Actual frame, retained failures and exact scope](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-METADATA-AND-TRACE-FRAMING.md).

**Earlier two-window checkpoint — October 23, 2026:** two fresh guests each pass
**1,800 GPU handoffs with two application windows**, complete 4K source/SDL
pixels, four focus checkpoints, changing counters and clean live teardown.
Explicit profile 14/v9 opts into a **384 MiB aggregate CPU mapping guard**;
older agreements retain their limits and the GPU usage guard stays 1 GiB.
An earlier **16-reference admission failure remains unresolved**.
[Actual two-window frame, all attempts and limits](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-MULTIAPP-CPU384.md).

**Recovery follow-up:** valid capacity refusals now preserve the broker
connection. Two fresh guests now recover from an explicitly injected
**guest-side, pre-native refusal**, retry the same GPU resource, and pass
**1,800 handoffs** with complete pixels, input/resize response and live cleanup.
Actual native quota-refusal recovery and the intermittent reference-limit cause
remain open. [Actual post-fault frame, all attempts and precise scope](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-INJECTED-ALIAS-GPU-RECOVERY.md).

**Verified pressure checkpoint — October 22, 2026:** two fresh guests exceed **2 GiB of
GPU virtual mappings**, verify 56 GPU copy/evict cycles and retained contents,
then each pass **1,800 desktop handoffs over 35.3 and 34.5 seconds**. Complete
4K source/SDL pixels, rendered input response, lifecycles, resizing and live
cleanup pass. The reported resident-memory and CPU mapping guards are unchanged.
Longer pressure while rendering remains open.
[Actual frame, diagnostic scope and verification limits](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/LIVE-GPU-VA-PRESSURE.md).

**Product priority: preserve the official keyboard/cursor code and QEMU SDL
input path, including the launcher's reserved-key routing.** The GPU relay uses
that frontend's input implementation. Physical-input acceptance and normal
launcher integration remain open. [Verified source map](https://github.com/7Wdev/try-omarchy-windows/blob/windows-nvgpu-lab/docs/UPSTREAM-INPUT-GRAPHICS-INTEGRATION.md).

![Actual two-application NVIDIA SDL backbuffer, metadata/framing run 162 handoff 1800](https://raw.githubusercontent.com/7Wdev/try-omarchy-windows/windows-nvgpu-lab/docs/images/omarchy-metadata-framed-1800-2026-10-24.png)

Actual **2560×1566 SDL pre-swap backbuffer** from a complete 3840×2160 GPU source.
Physical 4K scanout remains unverified. The test uses a disposable UWSM/PAM
account and RAM overlay over a read-only source disk. The released launcher
continues using the upstream graphics runtime.

| Progress measure | Verified status | What remains |
| --- | --- | --- |
| 4K diagnostic scene | **3/3:** shipped wallpaper, Omarchy bar, readable GPU applications | Normal launcher and usable desktop acceptance. |
| Bounded NVIDIA rendering through QEMU SDL | **Two simultaneous-window repeats:** 1,800 handoffs, complete pixels/input response, four focus checkpoints and live cleanup | Intermittent reference failure/recovery, long-run pressure, displayed cadence and official physical input. |
| Backend verification | **4/8 stages demonstrated** | Supported device integration, broader APIs, recovery and performance. |
| Desktop product acceptance | **0/4 gates complete** | Normal account/launcher, continuous multi-window use, reliability and matched-native performance. |

The address-accounting agreement separates owned reservation mappings from
resident memory. The new bounded diagnostic exercises the extended virtual
boundary before the desktop stream, with an independent retained-content
oracle. It adds explicit diagnostic upload/readback and no shader or performance
acceptance. The default residency path and earlier failed controls are retained.

Required targets include broad **OpenGL, Vulkan, CUDA, video acceleration, DLSS,
hardware ray tracing** and **≥93% matched-native performance**, including concurrent
Windows/guest workloads. Each needs actual application or SDK acceptance.
CPU handoff intervals do not establish displayed FPS or native performance.
[Gate definitions and evidence history](docs/GPU-BACKEND-PROGRESS.md) ·
[Driver-update matching](docs/LIVE-SUSTAINED-NVIDIA-DESKTOP.md) ·
[Virtio trace/UART comparison](docs/LIVE-VIRTIO-GRAPHICS-TRACE.md).

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
