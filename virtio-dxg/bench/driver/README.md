# Driver-level measurements

Tools for what one GL operation costs on the guest's graphics stack (GL →
Mesa's d3d12 driver → D3D12 → NVIDIA's user-mode driver → virtio-dxg).

- `passbench.c`: GTK 4's pattern for a CSS drop shadow, N times per frame
  (two new 96x96 textures and framebuffers, two clears, four draws, then
  everything deleted), with the CPU time per frame and per shadow and the
  GPU time (time-elapsed query). Surfaceless EGL:
  `gcc -O2 passbench.c -lEGL -lGL; ./a.out 50 300 0` (`REUSE=1` keeps the
  textures, isolating creation).
- `d3d12bench.cpp`: the D3D12 calls Mesa makes per offscreen pass (views,
  barriers, render-target binding, clears, descriptor copies) timed one by
  one, built the same way for native Windows (MSYS2:
  `g++ -O2 d3d12bench.cpp -ld3d12 -ldxgi`) and for Linux guests or WSL2
  (`g++ -O2 -I$DXH/include -I$DXH/include/wsl/stubs -I$DXH/include/directx
  d3d12bench.cpp $DXH/src/dxguids.cpp -ldl`, `DXH` = DirectX-Headers).
- `d3d12count_gen.py`: generates an `LD_PRELOAD` library counting every
  `ID3D12Device` and `ID3D12GraphicsCommandList` call of a process by vtable
  slot (Mesa creates its device through `D3D12GetInterface`, which it
  wraps), plus repeats of the same pipeline state, root signature, vertex
  buffer, viewport and scissor. `kill -USR1` writes the counts since the
  previous signal.

## Findings (RTX 5090 Laptop GPU, driver 621.24)

Per call, the same GPU and driver version:

| D3D12 call | Windows (native) | This VM | WSL2 |
| --- | --- | --- | --- |
| CreateRenderTargetView | ~300 ns | ~320 ns | ~350 ns |
| CreateShaderResourceView | 110-150 ns | ~300 ns | ~300 ns |
| ResourceBarrier | 140-190 ns | ~525 ns | ~540 ns |
| OMSetRenderTargets | 70-120 ns | ~460 ns | ~470 ns |
| ClearRenderTargetView | 180-270 ns | ~1,080 ns | ~1,130 ns |
| Descriptor writes and copies | ~27 ns | ~27 ns | ~30 ns |

NVIDIA's Linux (WSL) driver takes 3-6x longer per command-list call than
its Windows build; WSL2 measures the same as this VM, so it is the
driver, not the virtualization. Calls saved are time saved: a GTK shadow
pass costs ~35 us of CPU, against ~4.5 us of GPU time.

What GTK 4's renderer sends per draw while Files scrolls (`d3d12count`):
about 8 D3D12 calls, every draw with a new pipeline state, vertex buffer
and root constants, 85% with a new root signature (GTK changes shader and
vertex layout from one draw to the next), 3.6 descriptor-table bindings,
and per frame about 120 new textures (two per icon shadow).

Texture creation. A texture the recycling cache cannot supply (a new size,
as for items partly visible while scrolling) was a committed resource:
kernel allocation, GPU virtual address, residency, destruction, each a host
round trip, sometimes for milliseconds. `passbench 50 600 0 2` (two new
sizes per frame, as Files): frame p50 2.5-2.65 ms, p90 5-14 ms, p99 19-36
ms committed; p50 1.9 ms, p90 2.3 ms, p99 3.1-3.3 ms with textures placed
in pooled heaps. Files (touchpad-style scrolling, windowed VM): 118-131 fps
before, 130-148 fps after, no corrupted icons in 444.

Measure in a VM with a window (`DISPLAY_OPT="sdl,gl=on"`). Headless, the
compositor's round trip took 2-4 ms instead of 0.7-1 ms, and the GPU stayed
at P4 (~1.4 GHz) even when fully busy, where a windowed VM reaches P0.

### Files context menus

In a headless 1080p/240 Hz session, the second through fourth openings of
the same Files menu each created five D3D12 graphics pipelines. Creation
occupied 59.2, 57.7 and 70.3 ms of the longest guest output gaps (66.7,
66.3 and 76.2 ms). No GLSL compilation or NIR linking was observed during
those three openings. These are guest output timings for one session.

An experimental D3D12 pipeline library in commit `70bc2b7` prevented Files
from opening on the tested RTX 5090 Laptop GPU. A second launch captured a
guest SIGABRT through NVIDIA's userspace driver at the Mesa call site for
`StorePipeline`.

The replacement experiment uses `VDXG_LIVE_PSO=1` and keeps up to 64 live
graphics pipeline objects per Mesa screen. The default is off. Performance,
reference lifetimes under stress and rendering correctness require validation
on this implementation before enabling it for desktop sessions.

On build `fdb576d`, Files opened and completed all four context-menu cycles
with the live cache both enabled and disabled. Each session scrolled the same
3,000-entry folder for 15 seconds in a headless 1920×1080 guest configured
for 240 Hz, with four vCPUs and 6 GiB of RAM.

| Guest output measurement | Cache off | Cache on |
| --- | ---: | ---: |
| Scrolling updates per second | 53.8 | 54.3 |
| 99th-percentile scrolling interval | 55.95 ms | 49.77 ms |
| Longest gap during the first menu opening | 112.47 ms | 111.92 ms |
| Longest gap on each subsequent opening, in order | 62.42, 75.17, 74.98 ms | 71.14, 54.10, 49.48 ms |

Menu gaps include the interval from the guest input marker to the first
output update. The median of the three repeated-opening gaps fell from
74.98 to 54.10 ms, though the second opening was slower with reuse.
One run per setting leaves the magnitude uncertain. These whole-output
timestamps cannot identify when menu pixels first appeared or establish
physical display pacing. Cache hit counts and time in pipeline creation
during each opening would distinguish actual reuse from session variability.

### Files grid scrolling

A separate cache-off profile of the same 3,000-entry folder produced 59.5
guest output updates/s, a 37.75 ms p99 interval and an 87.44 ms worst
interval. Over 15.03 seconds Nautilus's main thread ran for 12.40 seconds
and spent 0.27 seconds runnable waiting for CPU. The 2,354 userspace CPU
samples were resolved against the guest GTK build
`6338771fcb62702311e60fa571df2d5040edd640`.

One repeated stack (34 samples at the same instruction) went from the scroll
adjustment through grid anchor tracking, item realization and grid splitting
to `gtk_list_base_get_border_spacing` and `gtk_css_node_do_ensure_style`.
At least 214 samples across the 80 most frequent call paths passed through
the same recursive GTK CSS validation site. These counts identify CPU
work, not elapsed GPU time or the cost removable by a single optimization.
Investigate repeated border-spacing reads during one grid layout operation
and which style changes invalidate visible items while scrolling. Any reuse
must preserve theme changes, hover, selection, icon sizing and animations.

Two experimental source patches under `bench/desktop` target that profile:

- `gtk-grid-split-spacing.patch` stores both spacing values beside the
  allocated column geometry. Splitting an existing tile uses its allocation's
  spacing. Before a nonempty allocation it still reads the style. A CPU
  fixture compiled both split functions from the matching GTK source.
  Across 7,080 synthetic splits, item counts, rectangles and returned tile
  positions matched the geometry reference, with zero style reads per
  post-allocation split. The reference used the spacing of the original
  allocation, including cases where the current style had since changed.
- `gtk-css-skip-valid-children.patch` checks a child's invalid flag before
  adding ancestor hashes to the selector filter and descending into the child.
  This mirrors the recursive function's existing early return. Only source
  applicability has been checked for this patch.

Neither candidate has a measured desktop speedup. Live reallocation, theme
transitions, icon positions and selection during scrolling still need a
matched guest build and comparison.
