# Recipient-owned native shared references

The actor coordinator now has a trusted Session API for creating an owned
resource or fence reference in the recipient's namespace. The Windows owner
duplicates its own NT reference; no Windows handle comes from the guest.
This is necessary for Hyprland and Quickshell to share GPU resources, but
the [experimental coordinator](GPU-CLIENT-COORDINATOR.md) now admits independent
producers and implements authenticated shared-FD routing. The actual Omarchy
bar now passes [4K NVIDIA shell-buffer interoperability](LIVE-OMARCHY-4K-GPU-SHELL.md).
Wallpaper, application windows and a complete rendered desktop remain required.

The Session API requires both sharing negotiations, the same native Driver,
the correct resource/fence type, an owned recipient adapter and a verified
physical-adapter match. It enforces existing object/reference quotas, allocates
a new recipient ticket and preserves fence flags. Malformed native replies
cannot publish an alias or close a known producer/recipient reference.
The resource alias also retains the original Linux runtime metadata.

The Windows helper uses [DuplicateHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle)
with unchanged access and inheritance disabled, then checks the resulting
handle flags. Source and recipient own separate references to the same native
object. Direct3D resources/fences use [NT shared handles](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle).
The API is internal to the trusted coordinator and has no new guest opcode;
kernel-authenticated FD lineage must be resolved before a future caller uses it.

Two native probes pass on the physical NVIDIA device (vendor 4318, device 11352):

| Probe | Actual hardware evidence | Scope |
| --- | --- | --- |
| Resource alias | GPU clears a 130×73 RGBA texture; original resource and NT reference close before recipient import. The duplicate NT handle then closes before a retired GPU copy verifies all 9,490 green pixels. No CPU upload; handle is not inheritable. | Host D3D12 resource lifetime. The D3D12 device remains alive. |
| Fence alias | Original NT export closes before imports into a separate KMT device and D3D12. Sixteen GPU queue signals retire. After signal 8, the original KMT sync object and alias NT handle close; the last eight signals still reach recipient CPU/D3D12 fence views. | Host monitored-fence lifetime. The original device/process is not terminated by this probe. |

The source fence is not dereferenced after destruction. Its later printed
source value is a target sentinel; only recipient/D3D12 observations establish
those later retirements. All explicitly owned probe references/devices release.
These are native helper tests, **not two QEMU GPU clients or process-abort proof**.

Separate controls run the actual Session validator against mock native calls.
They verify resource/fence recipient import after donor Session destruction,
physical-adapter/type/negotiation checks, existing quotas and malformed native
replies, including attempts to reuse an already-owned native reference.
The mock controls and native probes retain their separate scopes in the
[immutable evidence](evidence/NATIVE-SHARED-REFERENCE-ALIASES-2026-10-16.json).
Executable/log digests are independently fixed in the milestone tests.

A fresh real-root regression with the new owner still passes **twelve
3840×2160 NVIDIA compositor frames / 93 retired GPU commands**, both complete
backbuffer audits and normal shutdown. All **142 fence views, 181 allocation
views, 269 slot maps and 323 broker views** release before VM exit; reported
usage peaks at 833,302,528 bytes within the explicit 1 GiB guard. The complete
read-only source-image digest is unchanged. This workload uses the existing
single-client path and does not exercise the new alias API. Quickshell remains
absent and final display observation is 2 of 12.

All **357 milestone tests** pass, along with strict default/high-resolution
MSVC builds, strict portable alias controls, 105 default CLI rejections and
seven high-resolution startup guards. CI compiles the native probes; the
retained GPU executions occur locally on NVIDIA hardware.

Next are globally unique kernel capability tickets, authenticated recipient
FD resolution/import, a single coordinated native profile and sharing agreement,
and broker ingress/lifetime handling for independent GPU clients. The current
native global sharing/profile methods reject a second negotiation; forwarding
another Hello is insufficient. Safe cancellation from the earlier pending-wait
abort also remains open. Full Omarchy assets/apps, launcher/UWSM/input, reliable
display, long runs, CUDA/Vulkan/video/NVIDIA features and **≥93% matched-native
performance** remain acceptance work. Counts stay **4/8 stages, 0/4 desktop gates**.
