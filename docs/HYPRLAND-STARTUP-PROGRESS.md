# Actual Hyprland startup: capacity integration

Updated 2026-10-15. **No verified Hyprland frame yet.** The desktop acceptance
count remains 0/4. There is no reliable first-frame ETA while graphics
initialization and Windows presentation remain incomplete.

**Superseded by the [compositor-buffer development checkpoint](LIVE-HYPRLAND-COMPOSITOR-BUFFERS.md).**
The expanded mapping path now clears initialization and Hyprland commits native
output buffers. Pixel verification, Windows presentation and a finalizer crash
remain open. The original capacity failures below are retained unchanged.

The private diagnostic now boots the actual Hyprland 0.56.2 binary
(`efb50993780079460b0cbed1363e2166a2de1d9f`) in an ordinary-user, disk-free
QEMU/WHPX guest. An Aquamarine 0.15.1 prototype connects its allocator to the
experimental render node and GBM backend. The real compositor reaches Mesa's
D3D12 initialization and submits native NVIDIA work. The prototype currently
uses Aquamarine's internal headless output path; Windows presentation is still
unimplemented for this compositor. This is startup evidence, not a desktop demo.

## Observed failures and fixes

The retained [startup records](evidence/HYPRLAND-STARTUP-LIMITS-2026-10-15.json)
include failed runs and cleanup, with hashes of the local full captures,
private binaries and available working-tree source files.

| Run | Actual result |
| --- | --- |
| 3 | The old OpenGL fence profile reaches its 96-sync-object limit. EGL initialization fails. Six native commands retire; host and VM exit normally. |
| 4 | Explicit compositor profile 3 clears that limit, then exhausts the separate 128 QEMU fence mappings while creating a hardware queue. Four commands retire. The bridge fails and forces the owned VM to stop before releasing its mappings. |
| 5 | Adding queue/paging preflight capacity checks makes the same exhaustion return an error. Hyprland still fails EGL initialization; five commands retire, the bridge and VM exit with code 0, and no forced VM stop is needed. All mappings are released. |

Runs 4/5 share the same private guest image. Its startup hook still prints the
old `boundedProfile=2` label; the actual shim and Windows negotiation receipts
both show version 3. The next image fixes that label. No failed run is counted
as a successful compositor session.

## Explicit initialization capacity

`wddm_bridge_begin_opengl_compositor()` negotiates profile 3 before any device
is opened. Unknown versions, changed profile fields, missing capabilities,
late negotiation and repeated negotiation are rejected. Existing profiles
retain their limits.

| Bound | Profile 2 | Compositor initialization profile 3 |
| --- | ---: | ---: |
| Owned protocol objects | 384 | 512 |
| Synchronization objects, including imports | 96 | 192 |
| Vendor allocations, including imports | 192 | 256 |
| Hardware queues | 16 | 16 |
| Lifetime submitted commands / hardware signals | 64 | 256 |
| Pending commands / hardware signals | 16 | 16 |
| Protocol requests | 10,000 | 50,000 |
| GPU address reservations | 16 | 16 |

The diagnostic host additionally accepts `--driver-fence-slots 256`. This
requires the matching QEMU build, guest kernel module, broker and shim.
Without that option, the established 64/128-slot selection is unchanged.
The explicit 256-fence guest layout allows 512 combined memory leases; older
layouts still allow 256. CPU mapped-byte limits, GPU memory limits and
reservation-byte limits are unchanged. The existing optional 256 CPU slots
provide space for the compositor's simultaneous mappings.

These are bounded initialization limits. They do not establish sustained
rendering, recovery, 4K desktop operation or the ≥93% native-performance target.
The new 256-fence configuration still needs an end-to-end guest run with the
new QEMU artifact.

## Validation and next work

Local checks pass: Windows default and diagnostic builds with `/W4 /WX`;
102 default-host CLI rejection cases; four diagnostic-host startup gates;
Linux wire tests including synchronization create/import exhaustion and reuse;
1,636 shared-fence protocol checks; QEMU fence command bounds; 21 transport
cases for each of the three OpenGL profiles and one disabled case per profile;
the existing Linux ioctl fault suite; and all 211 retained milestone tests on
Windows. The matching 7.2.8 guest kernel aperture module compiles. The portable
broker view-policy test passes; its privileged RPC subtest was skipped in this
local non-root invocation. Hosted CI and the new QEMU build are pending.

Next: test actual startup with the expanded mappings, finish multi-device
buffer ownership and compositor output integration, then connect frames to
the Windows presenter. The first correctly displayed backend-rendered
Hyprland frame will be captured and added to PR #2 immediately. Input,
applications, sustained rendering, recovery and performance require their own
acceptance afterward.
