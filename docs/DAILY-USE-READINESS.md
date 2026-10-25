# Daily-use readiness

The NVIDIA backend remains an engineering prototype. A complete 4K GPU desktop
with wallpaper, bar and application windows is verified, including input response,
application replacement, real device-health queries and live resource cleanup.
The latest accepted networked regression lasts 1,800 handoffs over 43.42 seconds. This does not
establish daily-use stability or compatibility with arbitrary applications.

## Next practical acceptance: Chromium

Chromium is already present in the guest image. The new SDL QEMU candidate includes
libslirp and passes CI/local virtio NIC realization checks. An explicitly networked
read-only guest passes DHCP, DNS and certificate-verified public HTTPS, followed by
the complete 4K GPU desktop workload. Preserving the source root's permissions in
the RAM overlay fixes an observed resolver service failure. [Actual frame and
networking evidence](GUEST-BROWSING-PROGRESS.md).

Explicit process and resource-reference agreements now clear the observed
four-process and sixteen-reference startup boundaries. Host, guest broker and
client ioctl tracking use the same authenticated agreement; default behavior and
aggregate memory budgets are preserved. Chromium's latest attempt still times
out during page navigation and reports a scanout/rendering/texturing buffer
allocation failure. The browser sandbox is requested and software fallback is
disabled; independent browser sandbox and WebGL proof have not completed.
Browser GPU rendering and interactive Internet browsing remain unverified.

The browsing test must establish all of the following:

- A normal Wayland Chromium window with its sandbox retained and no software
  GPU fallback, correlated with the actual NVIDIA GPU process and native work.
- Successful DNS, TLS and public HTTPS-page loading through the guest network.
- Correct text, scrolling, resizing, tabs, ordinary input and window switching.
- WebGL/canvas rendering evidence, with software renderers rejected.
- Browser close/reopen and native resource retirement while the desktop remains
  responsive. Video playback and hardware video decoding need separate evidence.

## Remaining product acceptance

Browser success is one application workload. Daily-use readiness also requires a
normal Omarchy account/session and launcher flow, broader application and window
coverage, longer interactive sessions, suspend/reopen behavior where supported,
and reliable failure handling under memory pressure. The private residency
rollback experiment remains excluded after device-loss failures.

The official keyboard/cursor implementation and launcher reserved-key route remain
the required input path. Physical input latency, physical scanout cadence and
matched-native performance must be measured. Backend stage counts (4/8) and
desktop product gates (0/4) are checklists, not percentages of engineering effort.

The full goal still includes broader OpenGL/Vulkan/CUDA/video support, DLSS,
hardware ray tracing and at least 93% matched-native performance. These are not
certified by the current desktop or browser fixtures.
