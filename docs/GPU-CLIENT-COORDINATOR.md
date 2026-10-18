# Experimental GPU-client coordinator

The Windows owner and guest root broker now admit independent GPU clients in
the owned, read-only Omarchy diagnostic. This requires explicit
`--driver-coordinator` opt-in. The normal launcher keeps its existing backend.
Counts remain **4/8 verification stages and 0/4 desktop acceptance gates**.

Real runs identify Hyprland, Xwayland, the desktop portal and Quickshell as
separate authenticated clients. The [isolated two-client fixture](LIVE-OMARCHY-4K-GPU-SHELL.md)
now renders the actual Omarchy bar at 3840x2160 through Quickshell's
**D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU), Mesa 26.0.8** OpenGL renderer.
Wallpaper and application windows remain missing. The first capture is
incompletely populated; later captures show verified bar glyphs. This does not
establish the full desktop or close a desktop acceptance gate.

## Protocol and ownership

One native Driver initializes OpenGL and resource/fence sharing once. Each
Session validates its own negotiation against that immutable agreement.
Only the compositor can present. Repeated negotiation cannot reset global
budgets or another client's handles.

The root supervisor supplies the compositor PID through an inherited root
pipe. Ordinary children lose its writer before dropping privileges. The broker
pins peers with kernel pidfds, checks packet credentials and stamps actors
itself. Clients cannot select actor IDs or send coordinator control packets.
Metadata-only connections allocate no native actor. Completed connection slots
are reused; draining native actors remain charged until safe release.

The original 16-byte driver header and all 4,096 inner bytes stay unchanged.
Actor calls have a separate 40-byte envelope. Coordinator protocol version 4
uses a 32-byte control header, exact 80-byte initialization and 176-byte reply.
Malformed framing poisons the transport; control and actor sequences are
separately monotonic and serialized on the one controller connection.

Global kernel capability tickets retain producer/local-ticket/kind/cookie
lineage. FD inspection validates without minting aliases. Resolution creates
an independently owned recipient NT reference after actual FD, device and
physical-GPU checks. Final kernel FD release drives reaping; EOF cannot free
memory which another process or pending GPU command may still use.

## Explicit aggregate admission

| Resource | Coordinator aggregate limit |
| --- | --- |
| Native actors, including retained actors | 4 |
| Hardware queues / sync objects | 32 / 384 |
| GPU address reservations | 256 ranges / 64 GiB virtual address space |
| CPU mapping bytes / virtual slots | 192 MiB / 512 |
| Mapped GPU aliases / reported GPU-usage guard | 1 GiB each |
| Physical fence aperture | 256 pages; insufficient for the four-client run |
| Kernel view leases | 768 with the separate immutable module option |

Per-client OpenGL profiles 1-10 retain their contracts, including 16 queues,
192 sync objects, 64 reservations and 16 GiB virtual address space in v10.
Virtual reservations do not allocate that amount of VRAM. CPU and mapped or
reported GPU byte budgets are shared, not multiplied. Reported usage remains
a guard, not a hard physical VRAM quota.

A separate kernel discovery ioctl proves the 768-view option; INFO/CAPS stay
unchanged. The module option requires the existing CPU/sparse options and exact
512-CPU/256-fence hubs. Defaults retain their 256/512-view limits. The root
controller queries the actual kernel before initializing the native group.

## Retained failures and current scope

Hardware exposed successive limits: the old 60-second service window,
aggregate hardware queues, the 512-view kernel count, a 3 GiB request crossing
the 16 GiB address-space guard, and finally the 256-page fence aperture.
The four-client workload remains failed. The isolated first-frame fixture
disables Xwayland and portals only in its disposable RAM overlay and selects
no software renderer; it does not establish those disabled services work.

Cleanup now proves VM exit and executes each phase across every actor before
advancing: queues, other objects, reservations, adapters. A sibling queue
previously prevented reservation release permanently. Failed releases remain
charged and reported. General GPU abort cancellation is still unfinished.

The coordinator accepts valid, same-device borrowed paging and queue-progress
fences for GPU waits, including synchronization-only contexts. Shim and Session
validate ownership; the native owner submits real WDDM waits and reads real
fence words. Pending waits protect the owning queues from destruction. Actual
runs retire 36/33 borrowed waits without CPU-invented completion values.

A separate default-off render-node option admits up to 256 live opaque GEM
imports for the aperture exporter's authenticated native DMA-BUFs. This supplies
Hyprland's real PRIME preflight without guest-memory/DMA attachment or CPU maps.
The kernel holds buffer references through handle and file lifetime. Actual
guest controls pass 980 checks before rendering. Cross-process resource aliases
and the visible Omarchy bar now pass on physical NVIDIA hardware.

Strict GCC/MSVC controls cover cached profiles, malformed capacities,
independent namespaces, presentation ownership, draining actors, fatal partial
initialization, phased cleanup and legacy/foreign-device paging rejection.
Real Unix FD tests cover duplicate lifetime, producer exit, colliding tickets,
forged cookies, caching and metadata-only inspection. Native calls in these
controls are mocks, explicitly labeled in their output.

Normal launcher/UWSM, input, reliable display, repeatability, long runs,
CUDA/Vulkan/video support and matched-native performance remain open.
