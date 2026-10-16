# Hardware-only 4K desktop capacity work

**No verified 4K desktop frame has been captured.** The real Omarchy OS creates
a 3840×2160 NVIDIA-backed compositor buffer, but rendering then hits resource
limits. These are failed controls, not a GPU-only desktop success. Quickshell's
independent GPU connection is also still unsupported. The earlier 1280×720
wallpaper/bar appearance used software Qt and does not satisfy this milestone.

The [four retained attempts](evidence/NORMAL-OMARCHY-4K-CAPACITY-CONTROLS-2026-10-16.json)
use the actual read-only Omarchy image and Wayland-enabled D3D12-only Mesa runtime,
with no Qt software override. They retain complete compressed logs, native
records, reports and input/build receipts. Every source-image digest is unchanged.

| Control | Result |
| --- | --- |
| Runs 15/16, version 5 | A shared-resource import would bring GPU-VA mappings to about 279 MiB, over the 256 MiB quota. No audited frame. |
| Run 17, initial unpublished version 6 | The larger map budget passes; reported GPU usage reaches 299,802,624 bytes and exceeds the former 256 MiB guard. Owned VM forced to exit; deferred cleanup is rejected. |
| Run 18, revised version 6 | Both GPU guards pass. A 33,226,752-byte CPU-visible driver allocation exceeds the unchanged 96 MiB CPU-map limit when 91,758,592 bytes are already mapped. Guest abort/deferred cleanup is rejected. |

Runs 17/18 use **identical initramfs bytes**; the revised native worker changes
the reported GPU-usage guard. A complete buffer-commit receipt without audited
native backbuffer bytes is insufficient to claim verified 4K pixels.

The new opt-in version-6 contract is distinct from the preserved version-5 contract:

| Bound | Version 5, high-resolution executable | Version 6 |
| --- | --- | --- |
| GPU virtual mappings | 256 MiB | 512 MiB |
| GPU-reported process usage guard | 256 MiB | 512 MiB |
| CPU mappings | 96 MiB | 96 MiB |
| CPU slots / fence slots | 256 / 256 in this owned experiment | Same |
| Live wire objects / vendor allocations / sync objects | 1,024 / 512 / 192 | Same |
| Pending native submissions / signals | 16 / 16 | Same |
| GPU-VA reservations | 64, with the existing 16 GiB address-space budget | Same |

These are accounting guards; the reported-usage check is not a hard physical
allocation quota. Version 6 requires the expanded GPU-map capability and the
explicit high-resolution lab executable. Default builds reject it. Malformed
negotiation, duplicate negotiation, quota exhaustion and legacy profile rules
remain checked. The normal Windows launcher does not select this experiment.

The default kernel independently retains the 96 MiB CPU-map limit. The separate
version-7 profile now carries a fixed 32-byte capacity receipt: kernel ABI 1,
192 MiB CPU mappings, 256 allocation slots and 256 fence slots, with reserved
fields zero. Its new export is `wddm_bridge_begin_opengl_desktop_4k_cpu_render`.
The kernel needs the immutable load-time `enable_desktop_4k_cpu=1` parameter.
Additive `WDDM_APERTURE_CAPS` discovery leaves the original INFO ABI unchanged;
the authenticated broker independently queries its controller and rejects
missing, malformed or mismatched capacity before forwarding the negotiation.
The Windows owner also requires its actual 256/256-slot runtime and expanded
GPU/CPU capabilities. Versions 1–6 retain their previous budgets and requests.

The [version-7 hardware control](evidence/NORMAL-OMARCHY-4K-CPU-CAPACITY-2026-10-16.json)
passes **1,101 aperture checks**, including the actual 192 MiB allocation boundary
and rejection of one additional page, then initializes the genuine NVIDIA runtime
with that capacity verified. It still produces **zero audited backbuffers**.
The next 33,226,752-byte mapping passes the CPU-byte guard, but needs 32 adjacent
1 MiB aperture slots. At failure, **226 of 256 slots** are occupied, leaving only
30 total free. Native slot preflight rejects the mapping before obtaining its
driver lock (`errno=12`, one slot-quota rejection); Hyprland aborts after 25 retired
GPU commands. The native owner eventually cleans up after normal VM exit, but
the deferred mappings keep this a failed control. No Qt software override is used.

The next capacity work is an explicitly negotiated larger sparse aperture or
safe packing of small views. CPU byte accounting must remain separate from
reserved MMIO slots; moving live mappings or sharing unproven backing aliases
would violate the current lease/lifetime contract.
Independent GPU-client ownership, capability lifetimes and aggregate accounting
still need implementation before Hyprland and Quickshell can both use the GPU.

A [fresh regression with the capacity-aware kernel](evidence/NORMAL-OMARCHY-CPU-HANDSHAKE-REGRESSION-2026-10-16.json)
verifies its unchanged default 96 MiB limit and the preserved version-5 profile:
**300 frames / 948 retired GPU commands**, both native backbuffers accepted by
the strict scene auditor, all 142 fence views, 185 allocation views and 213 slot
maps explicitly released before normal VM exit. The entire source disk digest
is unchanged. Its controlled Foot application remains software-rendered;
this proves NVIDIA compositor regression scope, not an all-GPU desktop.
Seven new evidence tests cover the handshake, real kernel boundary controls,
slot failure, rejection of false desktop success and the passing regression.
All **327 milestone tests**, strict default/high-resolution native builds,
105 default CLI rejections, legacy transport controls and real Unix broker
credential/FD-lifetime checks pass. The earlier failed build/test invocations
remain retained alongside their corrected passing controls.

The [established version-5 regression](evidence/NORMAL-OMARCHY-PROFILE5-REGRESSION-2026-10-16.json)
still passes **300 frames / 948 retired GPU commands** inside the real OS, with
the previous 256 MiB GPU guards and explicit mapping release before VM exit.
Its strict scene auditor verifies both actual native backbuffers. This retains
the working graphics path while the separate 4K profile is developed.

Seven additional evidence tests preserve the failed 4K controls and passing
300-frame regression. All **320 local milestone tests** pass. Strict default and
high-resolution MSVC builds, 105 default CLI rejection cases, 22 version-6
transport controls, 21 legacy controls, default-profile rejection and real Unix
broker credential/FD-lifetime checks pass. No software-rendered shell, upscaled image, successful
Present alone or cleanup after VM exit is counted as the requested GPU-only
4K desktop frame. Desktop acceptance remains **0/4**; the full goal remains open.
