# Normal Omarchy image boot prerequisite

Updated 2026-10-16. Two fresh QEMU/WHPX guests boot the real Omarchy 4.0.4
root image, start its systemd 262 and power off normally. The source block
device is read-only, guest writes stay in a 512 MiB RAM overlay, and the full
7,516,192,768-byte source image has the same SHA-256 before and after every run.
This is a prerequisite for moving the NVIDIA backend into the normal OS;
**it does not start or verify the normal graphical session**.

## Why this step is needed

The existing 300-frame NVIDIA test runs Hyprland and Foot in a disk-free
diagnostic initramfs. The real image seeds Lua-based Hyprland defaults from
`/etc/skel`, starts a UWSM session, and launches Quickshell through
`omarchy-launch-shell`. Its home directory is unprovisioned in the inspected
factory image. Normal user setup, session services and a separate Quickshell
GPU client need integration with the broker and renderer.

`normal_root_guest_init.c` is a separate guest-only PID 1 bootstrap. It requires
the root block device to report read-only, loads the matching overlay module,
mounts ext4 read-only with journal recovery disabled, and adds a bounded tmpfs
upper layer. It replaces only the overlay's fstab with an empty persistent-mount
list, injects a boot-check service and switches to the image's real `/sbin/init`.
The service verifies the root type and block policy, reports the actual OS and
systemd versions, and requests normal poweroff. Every change is temporary.

The host harness uses separate read-only QEMU file/raw nodes and an owned
Windows process job. It exposes no network or display and kills only that job
on failure. This CPU-only boot check is separate from the existing NVIDIA
runtime owner; joining those paths is unfinished work.

## Retained results

| Run | Result | Guest systemd receipt | Shutdown | Source image |
| --- | --- | --- | --- | --- |
| 1 | Real-image boot passes | Omarchy 4.0.4 / systemd 262 | Normal poweroff | Complete hash unchanged |
| 2 | Fresh repeat with the public packager passes | Independent systemd boot ID | Normal poweroff | Complete hash unchanged |
| 3 | Missing root disk rejected before root switch | No OS readiness receipt | Bootstrap powers off normally | Unattached; complete hash unchanged |

[Immutable evidence](evidence/NORMAL-OMARCHY-ROOT-BOOT-DEVELOPMENT-2026-10-16.json)
has SHA-256 `330f408ddeadb7e465e79f2680b1ea5b7e4d65a00ab96c5da1c9c4933c5a998d`.
It retains the actual consoles, QEMU arguments, before/after input hashes,
kernel/initramfs/module identities, distinct boot IDs and the failed control.
The initial local packager lacked the `zstd` command; Python 3.14's decoder
resolved that build-only failure before any VM ran. The source disk was never
mounted or changed on the host.

Twelve milestone tests retain these outcomes and reject modified consoles,
changed/foreign images, writable or malformed block nodes, wrong source paths,
forced exits, missing readiness and false graphics claims. Packager controls
check the archive layout, input architecture, module release and byte bounds.
The Wayland scene provenance test now compares retained compiler inputs with
their build receipts, allowing future source improvements without rewriting
historical captures.

All 297 local milestone tests pass. The bootstrap compiles as strict static C11
with warnings as errors. Rebuilding with the current public packager produces
the exact initramfs digest used by successful run 2.

## Build the bootstrap

Compile without executing it on the build host:

```sh
cc -static -std=c11 -O2 -Wall -Wextra -Werror \
  experimental/windows-native-gpu/normal_root_guest_init.c -o /path/to/init
python3 experimental/windows-native-gpu/make_normal_root_initramfs.py \
  --init /path/to/init --overlay-module /path/to/overlay.ko \
  --kernel-release 7.2.8-arch1-2 --output /path/to/private-normal-root.cpio.gz
```

The kernel and uncompressed overlay module must match. This archive contains
only the bootstrap, overlay module and initial console node; it contains no
root image or NVIDIA runtime. The local root image and compiled artifacts stay
private. The tested QEMU command and read-only node policy are retained in the
evidence; normal launcher integration is still pending.

The next work is to connect the NVIDIA broker/renderer to this OS, provision
the real user/session and support independent GPU clients for Hyprland and
Quickshell. The [actual NVIDIA scene](LIVE-HYPRLAND-WAYLAND-SCENE.md) remains
the latest graphics result. Full Omarchy rendering, input/resize, reliable
display, recovery, long runs, 4K and matched-native performance remain open.
The checklist stays **4/8 verification stages and 0/4 desktop gates**.
