# NVIDIA rendering through an unprivileged guest broker client

The opt-in Linux graphics broker now routes genuine NVIDIA GPU requests from an
ordinary UID/GID 65534 process. Two fresh QEMU guests render both 130×73 OpenGL
shader frames, and a third also reads a real native fence in a late library
finalizer after the client closes its broker connection. All three independently
pass the complete pixel, native command retirement, kernel lease and VM cleanup
audits. This is one graphics process per native worker; it closes no desktop gate.

![Actual frame 2 read back from the NVIDIA GPU by the broker client](images/nvidia-broker-opengl-2026-10-13.png)

Mesa reports `D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)` and OpenGL 4.6.
Each frame checks all 9,490 pixels against the independent triangle oracle,
including background, coverage and alpha; maximum triangle channel error is one
byte. These are bounded shaders, not OpenGL conformance or 4K OpenGL acceptance.

## Implemented route and authority

The guest init runs the broker as root and permanently drops the rendering
process to UID/GID 65534, clears supplementary groups and effective capabilities,
and sets `no_new_privs`. Actual raw opens of the kernel controller and native
virtio port must fail with a permission error. The renderer gets neither FD.

The process-owned shim sends bounded native packets over an authenticated Unix
`SOCK_SEQPACKET` channel. Protocol 2 adds separate POSIX reply status; request
status must remain zero. The broker validates kernel credentials on every packet
and retains the socket's kernel-pinned process handle. It forwards approved GPU
requests through the QEMU virtio port to the existing Windows KMT worker.

The broker derives CPU-view authority only from successful native creation/lock
receipts, including queue ownership, offset, byte count and host generation.
A client cannot choose an arbitrary aperture range. The root controller creates
a restricted view FD and sends it with `SCM_RIGHTS`; fence views remain read-only.
Unknown/repeated grants, wrong generations and native destruction while a view
remains granted fail closed. Explicit release revokes the kernel view before
native unlock/destruction can proceed.

Socket EOF and rejected messages retain the controller and native transport
until the whole client thread group exits. Native queue/backing destruction
still uses the owned Windows worker's retirement and post-VM cleanup contract.
Multiple GPU clients and exported native capability lifetime are separate work:
this route explicitly rejects native FD export/import until the capability store
is integrated. The default direct diagnostic path remains available.

## Actual checks and retained failures

The [immutable capture](evidence/BROKER-ROUTING-2026-10-13.json) includes all three
successful runs, three earlier failures, raw shader rows/native receipts,
source/build hashes and the independent audits. Proprietary images stay local.

| Check | Fresh guest 1 | Fresh guest 2 | Native finalizer guest |
|---|---:|---:|---:|
| Retired native commands | 18 | 18 | 18 |
| Broker native calls | 1,014 | 1,014 | 1,017 |
| Restricted kernel view grants | 162 | 162 | 163 |
| Explicit view revocations | 66 | 66 | 66 |
| Views retained until process exit | 96 | 96 | 97 |
| Late native fence read after channel close | — | — | 57 |
| Guest / native worker / QEMU exit | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |

The retained view counts match the client's exit summary and native physical
mappings released after VM exit. All 581 memory, 561 object-FD and 15 resource-FD
kernel contracts pass; the module unloads with zero leases, CPU bytes and MMIO
accesses. The 256 virtual slots retain the same 96 MiB physical CPU mapping limit.

The first failure stalled at the first received view FD. A separate real
`LD_PRELOAD` reproducer with mock native/kernel peers confirms that the old FD
wrapper re-entered the shim's locked `close()` hook. Its five-second owned-child
watchdog reproduces the deadlock; the fixed shim maps, reads, explicitly releases
and exits normally. Broker-owned FD cleanup now uses the raw close syscall.
The next failures exposed legitimate pending GPU-map statuses and the opaque
private bytes appended to hardware queue replies. Both validations are corrected
and covered by the 36-check view policy test. Failed captures remain failures.

The real Unix/credential tests also retain 14 malformed/replayed/foreign-sender
cases, read-only descriptor transfer, an unauthorized grant and process-exit
lifetime checks. Those portable native/kernel mocks are explicitly labeled and
do not establish GPU rendering. The older general wrapper's exit 1 is retained;
the dedicated broker auditor establishes only the scopes above.

## Reproduction and remaining desktop work

Build the optional guest helper with `WDDM_BRIDGE_TEST_BROKER_ROUTE=1` in
`build_opengl_probe.sh`. Add `--guest-graphics-broker` to the private initramfs
packer alongside the matching guest aperture module/contract and explicit OpenGL
workload. The manifest records the broker and dependency hashes. Omit the option
for the existing direct route. Native FD export diagnostics are not supported
through this first broker route. To audit the capture:

```sh
python3 experimental/windows-native-gpu/verify_broker_routing_capture.py \
  --report /path/to/driver.json --output /fresh/path/audit.json
# Add --finalizer only for the explicitly built native-finalizer workload.
```

WDDM assigns GPU virtual address spaces [per Windows process](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/per-process-gpu-virtual-address-spaces).
Our next design to validate is a coordinator with separate native worker
processes and aggregate VM quotas; separate handle tables alone do not establish
isolation of opaque GPU commands. Cross-process native resource retention,
DRM/GBM/dma-buf imports, compositor synchronization and continuous host display
still need integration. Hyprland, desktop stability, required APIs, NVIDIA
features, concurrent workloads and the ≥93% matched native performance target
remain unverified. The first actual backend-rendered Hyprland session will be
captured, shown to the user and added to PR #2 immediately.
