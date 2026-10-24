# Native GPU views through process exit

On 2026-10-12, a deterministic guest test reproduced a segmentation fault in a
late ELF library finalizer. The bridge's `atexit` handler had already unmapped
the finalizer's original native monitored-fence CPU view. Keeping process-owned
views alive until kernel process teardown fixes this test in three fresh
QEMU/WHPX guests on the RTX 5090 Laptop GPU. Two further fresh guests preserve
the genuine Mesa OpenGL timeline FD and future GPU queue-wait results.

This establishes a process-exit mapping-lifetime fix. It does not establish
general desktop stability, DRM/GBM or dma-buf support, a Hyprland session, API
conformance or near-native performance. The roadmap remains **4/8 verified
stages and 0/4 completed desktop acceptance gates**.

## Failure and fix

The old exit hook explicitly destroyed the bridge before `_dl_fini` ran driver
DSO finalizers. It destroyed mapping controllers, unmapped CPU views and closed
client placeholder FDs. A finalizer retaining a view could fault on its next
load. Closing a client FD early also allowed descriptor reuse before a library
closed its original descriptor.

The bridge now stops forwarding under its normal request mutex, closes the
transport, and retains its bounded process-owned state, mappings and client FDs
through library finalization. Hooks recheck the stopped flag under that same
mutex, so a request racing the stop cannot enter the transport afterward.
Existing client FDs can still be closed by their owner. Kernel process teardown
closes remaining descriptors and destroys the address space; the host continues
to wait for its owned VM to exit before releasing native backing objects.
Explicit object destruction and CPU unlock during normal execution still
release their mappings promptly.

The optional diagnostic DSO creates a real native `NoGPUAccess` monitored
fence initialized to 57, retains its original CPU mapping, and reads it in an
ELF finalizer after the bridge exit hook. It makes no new ioctl or mapping in
that finalizer. Both the old and fixed guests use the same test DSO, OpenGL
probe, initializer, Mesa runtime, kernel and Windows bridge; the interposer is
the changed component.

| Capture | Result |
| --- | --- |
| `guest-teardown/run-11`, old interposer | Shader pixels pass, fence arms at 57, bridge stops, finalizer begins, then SIGSEGV (11), guest exit 128. No finalizer completion. Native/VM cleanup passes. This is the expected failing control. |
| `guest-teardown/run-12` through `run-14`, fixed interposer | Each finalizer reads 57 after bridge stop; normal guest exit 0. All shader pixels and native/kernel/VM cleanup pass independently. |
| `mesa-fence-fd/run-12` and `run-13`, fixed interposer | Genuine GL semaphore import, GPU signals, a future GPU wait and independent queue release still pass. Each run audits both shader frames, 19 retired commands, 83 fence leases and 104 allocation leases. |

Each fixed finalizer run retains 4 paging-fence views, 3 hardware-queue fence
views, 44 synchronization-fence views and 46 CPU allocation views at process
exit. The auditor correlates the 51 fence and 46 allocation views with native
post-VM mapping retirement; all live mappings and native objects end at zero.
Each run independently audits 18 retired commands and all 18,980 pixels across
two 130×73 shader frames, including background, coverage and alpha. Maximum
triangle RGB error is one channel unit. These are bounded OpenGL diagnostics,
not 4K desktop tests.

## Evidence and regression checks

[Retained evidence](evidence/EXIT-LIFETIME-2026-10-12.json) includes all six
captures with independently repeatable audits, 35 normalized source hashes,
artifact/image hashes and raw local capture hashes. The fixed interposer in
both guest images is byte-identical to the one used in the final Linux fault
suite. Private images contain installed NVIDIA files and remain local.

A portable regression also reproduces the early client-FD close on the old
bridge (exit 90 / `EBADF`). The fixed bridge preserves the FD until the late
finalizer, rejects new device opens, ioctls and profile requests, and allows the
original FD to close. Its fake worker receives only the initial negotiation;
this test makes no GPU calls.

Validation passes 204 Linux ioctl fault cases, 21 transport cases for each
OpenGL profile plus both disabled-profile checks, 120 milestone tests and 12
private-image packaging tests. The new capture-corruption tests reject missing,
duplicated or reordered lifetime receipts, wrong signals, mismatched retirement
counts, altered pixels, failed native cleanup and forced VM termination. CI
also compiles the finalizer DSO and explicit probe mode without running a GPU
workload on its host.

```sh
python3 experimental/windows-native-gpu/verify_exit_lifetime_capture.py \
  --report /path/to/driver.json --output /path/to/new-exit-audit.json
# Only for the deliberately failing old-interposer control:
python3 experimental/windows-native-gpu/verify_exit_lifetime_capture.py \
  --expected-old-mapping-failure --report /path/to/old/driver.json \
  --output /path/to/new-control-audit.json
python3 -m unittest discover -s experimental/windows-native-gpu -p 'test_exit_lifetime_milestone.py'
```

The matching `driver.log` and `driver.host.log` must accompany the report.
The ordinary success auditor still rejects the crashing control. Its independent
pixel/cleanup audit does not change its failed process outcome. The older general
runtime verifier returns 1 for these extra operations; dedicated auditors verify
only their documented scopes.

Build the optional DSO with `WDDM_BRIDGE_TEST_EXIT_LIFETIME=1` and the pinned
`WDDM_BRIDGE_DXG_HEADERS` / `WDDM_BRIDGE_DIRECTX_HEADERS` source directories when
calling `build_opengl_probe.sh`. Include the generated `exit-lifetime-probe.so`
using the packer's explicit `--exit-lifetime-probe` option. Regular probes do not
enable this test; the packer rejects it outside the OpenGL workload.

## Findings still open

The earlier [legacy exit-128 failure](LIVE-GPU-MESA-FENCE-FD.md) did not record
its terminating signal. The deterministic test confirms a real lifetime bug
and its fix, but does not conclusively attribute that historical failure.
Six traced legacy runs and one untraced run completed before constructing the
deterministic reproducer; they did not establish broad stability.

A separate unchanged V1 legacy image hit its 256 aggregate-object quota during
the FD workload (`guest-teardown/run-8`, guest exit 1). That rejection and its
raw capture hashes are retained separately. It is not a finalizer fault or a
successful workload, and supporting the legacy workload reliably still needs
capacity/lifetime work rather than suppressing quota checks.

The next desktop work remains a usable guest graphics device path,
compositor-compatible buffers and synchronization, and the first correctly
displayed Hyprland frame. Continuous display, Vulkan, CUDA, video, long runs,
recovery and ≥93% native performance all remain open.
