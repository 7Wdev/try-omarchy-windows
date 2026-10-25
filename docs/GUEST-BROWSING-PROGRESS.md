# Guest networking and Chromium progress

The diagnostic Omarchy guest now resolves DNS and loads a public HTTPS page
with certificate verification, then completes the 4K NVIDIA desktop workload.
Chromium acceleration remains unverified: its first run fails during EGL GPU
initialization. This is progress toward browsing, not daily-use acceptance.

The SDL QEMU candidate includes libslirp. Its CI and local checks realize an
actual virtio NIC; the native owner independently checks the NIC's attached
backend through QMP. `--driver-user-network` explicitly enables NAT on the owned
SDL/read-only-root desktop. Default graphics fixtures stay offline, and this
option creates no host port forwards.

An early guest test obtained DHCP and a default route but failed DNS. The
resolver's actual journal showed `200/CHDIR` and directory access failures.
The disposable overlay upper root had been created with private-directory
permissions. Preserving the read-only source root's mode and ownership restores
normal system-service access; the source image stays read-only. The fixture
starts the existing NetworkManager and systemd-resolved services and requires
DNS and ordinary TLS verification before rendering.

## Accepted networked desktop observation

Run 184 completes 1,800 GPU handoffs over 36.75 seconds, 5,816 native submissions
and 5,809 real device-health queries. DNS and certificate-verified HTTPS return
HTTP 200 before the desktop workload. The independent audit retains the prior
pixel, wallpaper, input-response, resize, focus, application-replacement,
ownership and live-cleanup predicates, with exact new runtime identity pins.
Complete first/final source and SDL pixel comparisons have maximum channel error
one. The VM exits naturally and the disk and installed NVIDIA driver guards pass.

![Actual SDL window after the networked 4K GPU desktop run](images/omarchy-network-desktop-2026-10-25.png)

[Observation summaries and stream hashes](evidence/GUEST-NETWORK-DESKTOP-2026-10-25.json)
include the failed prerequisites and browser attempt. Raw streams and private
fixtures remain local; this summary is not a full replay archive. Proprietary
libraries, disks, initrds and compiled binaries are not published.

## Chromium boundary

Run 185 starts the shipped Chromium 153 using Wayland, its sandbox retained,
software GPU fallback disabled and DevTools over owned pipes. The first GPU
process fails EGL initialization and the page-navigation check times out. The
run remains rejected for browser rendering; it shuts down cleanly. GPU-process
environment, broker admission and sandbox compatibility need further diagnosis.
No browser shader success or daily-use readiness is claimed.

The next acceptance requires an actual Chromium window with NVIDIA WebGL shader
pixels, authenticated GPU-process/native-work correlation, the sandbox active,
HTTPS browsing, scrolling, tabs, ordinary input, resizing and close/reopen
cleanup. Normal launcher/account integration, broader applications, longer
sessions, failure recovery and performance measurements remain open. Backend
4/8 and desktop product 0/4 counts stay unchanged.
