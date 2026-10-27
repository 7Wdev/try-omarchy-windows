# Daily-use readiness

The NVIDIA backend remains an engineering prototype. A complete 4K GPU desktop
with wallpaper, bar and application windows is verified, including input response,
application replacement, real device-health queries and live resource cleanup.
The latest public-source networked regression completes 1,800 handoffs with an
explicit 4 GiB application-memory agreement. A separate default-off regression
retains positive eviction/reuse coverage; the larger pool produces zero evictions
in this workload. [Actual frame and exact limits](APPLICATION-MEMORY.md). This does not
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
older aggregate memory budgets are preserved. A new explicit memory agreement
clears the old 1 GiB reported-usage boundary. A private follow-up now displays a
readable public HTTPS page and NVIDIA shader with GPU compositing enabled and
all 17 observed GPU threads sandboxed. A later private run now completes 1,800
handoffs, complete first/final 4K source-to-SDL pixels, terminal typing/cursor
response, terminal resize, focus checkpoints and browser close/reopen with live
GPU retirement. Both browser sessions retain the shader/compositing/sandbox
checks. The fixture waits for browser readiness before starting the existing
30-second terminal input timer; official input code is unchanged. Browser
scrolling/typing/tabs, browser resize, continuous browsing and longer sessions
remain unaccepted. [Actual final frame and exact limits](GPU-BROWSER-LIFECYCLE.md).

A further 1,800-handoff private trial passes controlled browser wheel, text and
tab checks in both sessions through owned CDP pipes, with the GPU process,
compositing and shader preserved. Complete final pixels and live cleanup pass.
These direct browser checks do not certify ordinary SDL/OS browser input or
physical latency. [Actual interaction frame and precise limits](GPU-BROWSER-INTERACTIONS.md).

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
An observation-only browser repeat using the published host crosses the existing
GPU-usage guard and requires an owned VM stop. Native cleanup succeeds after
exit, but live recovery remains unverified. A new owner-only pre-native admission
pilot avoids that forced stop in one repeat: the guest receives ENOMEM and the
VM exits naturally with live cleanup. Chromium restarts its GPU process and
navigation still fails. The pilot uses mapping-size estimates with uncovered
cases; it is not a complete pressure-recovery solution.
[Policy and retained hardware observations](MAPPED-RESIDENCY-ADMISSION.md).

The official keyboard/cursor implementation and launcher reserved-key route remain
the required input path. Physical input latency, physical scanout cadence and
matched-native performance must be measured. Backend stage counts (4/8) and
desktop product gates (0/4) are checklists, not percentages of engineering effort.

The full goal still includes broader OpenGL/Vulkan/CUDA/video support, DLSS,
hardware ray tracing and at least 93% matched-native performance. These are not
certified by the current desktop or browser fixtures.
