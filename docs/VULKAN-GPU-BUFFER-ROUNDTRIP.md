# Typed GPU buffer export/import and 4K regressions — October 28, 2026

Four fresh QEMU/WHPX guests using the opt-in Windows buffer metadata path each
pass two **64 KiB GPU data roundtrips**, checking every one of **16,384 words per
round**. These include descriptor zero, descriptor exhaustion with a failed-import
rollback and retry, and two normal runs. A separate actual GPU corruption is
rejected at the final word. The original **3840×2160 compute and shader/texture
workloads** also pass their complete word/pixel oracles with the candidate.

This remains an engineering prototype. Counts stay **backend 4/8, desktop product
0/4, diagnostic scene 3/3**. These diagnostics disable display, host input and
source disks so development can continue in the background. They do not establish
daily-use desktop stability or Vulkan window presentation.

## Windows metadata and guest ownership

The disabled-option baseline passes its first data round, then fails its second
creator allocation: native `OpenSharedHandle` returns `E_NOINTERFACE`. That partial
run remains rejected. The enabled path creates a Windows D3D12 prototype from a
public buffer description, extracts its opaque Windows runtime metadata through
KMT, and retires every temporary resource/device/adapter/NT handle before using
that metadata for the actual guest allocation. Private layouts are not decoded.

The tested Windows runtime metadata is **280 bytes**; the original Linux runtime
metadata is **264 bytes**. They remain separate during allocation, information
queries and FD import. Windows receives its native metadata; the guest receives
its original bytes. A successful native D3D12 import must expose the declared
buffer width, flags and complete buffer description before ownership is published.
A rejected, unsubmitted typed buffer is synchronously destroyed.

The new operation has its own queried capability tuple and one-shot declaration,
bound to the caller's thread and original owned-device/adapter snapshot. Existing
texture descriptions, opcode values and capability tuples retain their contracts.
The ordinary GPU-VA validator remains unchanged. A separate validator forwards
NVIDIA's exact opaque `0x10000009` mapping value only for internally proven
successful resource opens; ownership, address bounds and real paging fences remain
required. Microsoft documents [driver protection](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/driver-protection)
as vendor-specific information.

[Dozen patch 0007](../experimental/windows-native-gpu/mesa/0007-linux-dozen-native-gpu-buffers.patch)
places the declaration around the real dedicated external-buffer
`CreateCommittedResource` call, cancelling it on success or failure. Standard
Vulkan applications use this ICD path; the GPU probe does not call the declaration
API itself. Both `DZN_WDDM_NATIVE_GPU_BUFFERS=true` and
`WDDM_BRIDGE_NATIVE_GPU_BUFFERS=1` are explicit candidate opt-ins. The hook recognizes
Dozen's GPU-only CUSTOM heap properties as well as DEFAULT heaps. Its public
[buffer layout constraints](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc)
remain checked. Missing support or an incompatible eligible description fails closed.

## Actual GPU payload and descriptor controls

GPU `vkCmdFillBuffer` writes sixteen distinct stripes into device-local storage
that is not host-visible. After an external ownership release and a retired fence,
the application exports an FD and frees the creator's buffer and memory. A new
Vulkan allocation imports that FD. External ownership acquisition, a GPU copy to
coherent readback memory, a host barrier and a retired fence precede the complete
integer comparison. The application never uploads the source payload.

| Round | Verified words | Actual and independent FNV-1a |
| --- | --- | --- |
| 0 | 16,384 | `a39e4f675023a325` |
| 1 | 16,384 | `62b2ec7741bea325` |

[Dozen patch 0006](../experimental/windows-native-gpu/mesa/0006-linux-dozen-import-memory-fds.patch)
uses a temporary CLOEXEC duplicate for HANDLE-based import, including caller FD
zero. Success consumes the caller FD; failure closes only the duplicate. The
pressure workload exhausts only its own guest-process FD limit, verifies
`VK_ERROR_OUT_OF_HOST_MEMORY`, a retained caller FD and a null output, restores
the limit and successfully retries that FD. Both rounds then pass actual GPU data
checks. The [Vulkan import ownership contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryFdInfoKHR.html)
remains authoritative.

A separate asynchronous-wait adapter normalizes an actual `eventfd()` result of
zero to a nonzero descriptor. It preserves the counter and descriptor flags and
avoids confusing a valid Linux FD with Windows' null HANDLE sentinel. The default
shim profile retains its old behavior. Actual preload tests cover both profiles;
no GPU fence is written by this adapter.

| Corrected-profile run | Result |
| --- | --- |
| 7, option disabled | First GPU round passes; second creator allocation fails. Partial only, rejected. |
| 8 and 12 | Enabled path: both complete GPU data rounds pass in each fresh guest. |
| 9 | FD zero: both complete GPU data rounds pass; the earlier wait hang does not recur in this workload. |
| 10 | Two real FD-pressure failures retain caller ownership; both restored-limit retries and GPU data rounds pass. |
| 11 | Actual GPU-written zero at word **16,383** is rejected against **2,353,232,594**. |

Each complete positive run retires **10 native submissions, 12 hardware-queue
signals, six device-health queries and six kernel notifications**. The original
strict broker predicate passes with zero retained references. Native/kernel
mappings and tracked objects retire before natural host/VM exit. The tests retain
**256 CPU slots, 128 fence slots, a 96 MiB CPU mapping guard and a 256 MiB reported
GPU-usage guard**. Template allocations also undergo reported-usage checks.

## Retained failures and 4K regression

Earlier data-only successes did not establish complete FD ownership. Original
runs 2, 3 and 6 retain two Linux FD references until producer exit; the corruption
run retains one. Their strict broker checks remain false. Patch 0006 clears that
ownership problem in the subsequent fixed-ICD runs and current candidate; no
historical predicate or log is rewritten.

Original run 1 refuses an imported mapping and retains four reservations until VM
exit. Original run 4 fails earlier at native creator import. A later descriptor-zero
run imports successfully but hangs on a GPU wait and needs an owned VM stop with
exit 90; seven reservations and other mappings/objects survive until that stop.
The first eventfd-adapter FD-zero repeat verifies one full data round, then hits
another creator-import failure. None of these controls counts as stable recovery.

The first two 4K regression attempts with the new broker also fail: it was compiled
with the small diagnostic profile and rejects the larger CPU lock receipt before
rendering. Both failures and their deferred cleanup remain retained. Rebuilding
the same broker source for the existing 4K profile fixes this packaging mismatch
without changing the target profile's byte guards. The corrected candidate then
passes both original 4K compute rounds, checking **8,294,400 words per round**, and
both original 4K shader frames with complete independent pixel checks. Shader
RGBA captures are byte-identical to the earlier verified frames. These are
windowless regressions, not a new Omarchy integration or physical-scanout result.

## Verification and remaining scope

- [GPU workload](../experimental/windows-native-gpu/vulkan_memory_roundtrip_workload.h),
  [isolated probe builder](../experimental/windows-native-gpu/build_vulkan_memory_roundtrip_probe.py)
  and [independent integer reference](../experimental/windows-native-gpu/vulkan_memory_roundtrip_reference.py).
- [File-only hardware audit](../experimental/windows-native-gpu/verify_vulkan_typed_buffers.py),
  [receipt checks](../experimental/windows-native-gpu/vulkan_typed_buffer_evidence.py)
  and [sanitized evidence](evidence/VULKAN-TYPED-BUFFERS-2026-10-28.json).
- Thirteen synthetic negative evidence tests, strict Windows default/4K builds,
  guest default/async builds, real OS eventfd/preload controls, exact patched-ICD
  helper controls and broker credential/descriptor-lifetime tests. These CPU
  checks do not establish GPU rendering by themselves.

Actual external-buffer hardware coverage is **64 KiB, plain transfer usage,
resource flags zero**, on the recorded RTX 5090 Laptop GPU / driver 621.01. The
64 MiB/UAV capability bounds have CPU contract coverage, not broader hardware
compatibility coverage. Imported images, cross-process Vulkan imports, Vulkan
rasterization/windows and sustained application use remain open. Metadata template
creation cost is unmeasured; ordinary budget refusal still needs a recoverable
allocation-error path. General crash/pressure recovery is not certified.

Official launcher, keyboard and mouse source remain unchanged. Normal launcher
integration, official Windows input responsiveness, longer desktop/browsing
sessions, broader applications, at least 93% matched-native performance and
CUDA/video/DLSS/ray-tracing requirements remain unverified. Runtime images,
proprietary files and raw GPU captures remain private.
