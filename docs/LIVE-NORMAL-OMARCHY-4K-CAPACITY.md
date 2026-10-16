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

The kernel independently enforces the 96 MiB CPU-map limit. The next capacity
change needs matching kernel and host support, explicit profile selection and
real failure/cleanup tests. Raising a host constant alone would be insufficient.
Independent GPU-client ownership, capability lifetimes and aggregate accounting
still need implementation before Hyprland and Quickshell can both use the GPU.

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
