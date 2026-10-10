# Reservation-owned GPU address state

The live NVIDIA Linux runtime in QEMU now maps reservation-owned GPU addresses
to the Windows driver's Zero state. Both 32- and 64-slot hardware diagnostics
accepted three requests: 4 MiB, 1 GiB and 128 KiB. The last request replaced an
existing allocation mapping after tracked hardware commands had retired.
Windows and the guest verified every paging fence. The runtime created
40 allocations, 28 direct CPU mappings, five address reservations and three
hardware queues. One 4 KiB initialization command retired at progress fence 17.

The [hardware evidence](evidence/QEMU-LIVE-GPU-ADDRESS-STATE-2026-10-09.json)
contains normal runs, an abrupt guest exit with state still owned, a disabled
control, and source/binary hashes. All four diagnostics reclaimed their native
objects. These are partial driver diagnostics. D3D12 initialization still fails
(`8007000e` in the enabled runs); interactive Omarchy, CUDA, OpenGL, Vulkan,
video acceleration and the required 93% native performance remain unverified.

## Native page-table operations

The preceding [reservation investigation](LIVE-GPU-RESERVATION.md) exposed a
`Write | Zero` mapping. A subsequent observation confirmed that its allocation
handle is null. Microsoft's [mapping contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_mapgpuvirtualaddress)
requires null allocation handles for Zero and NoAccess, permits replacement
inside a previously reserved or mapped range, and requires the paging fence
to retire before GPU access. A nonzero base address takes precedence over
the minimum and maximum address hints.

The new `--driver-gpu-state` opt-in requires the owned reservation route. The
guest sends a reservation identity and the paging-queue identity. Both protocol
ends verify the adapter relationship, containment, page alignment, 48-bit
address bounds, zero reserved fields and supported protection bits. The guest
requires a null allocation. Windows calls `D3DKMTMapGpuVirtualAddress` with a null allocation and the
original protection value. Native pointers and KMT handles stay on the host.

The [protection contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddigpuvirtualaddress_protection_type)
defines Zero as reads returning zero and writes being discarded, and NoAccess
as invalid pages. The code handles both states; live physical acceptance in
this milestone covers **Zero only**. Shader read/write behavior and NoAccess
fault behavior have not been measured. GPU page tables perform the access;
the bridge does not intercept GPU reads or synthesize backing allocations.

## Ownership and limits

Address-only state can cover up to 4 GiB per request within the existing
16 GiB reservation budget. It does not consume the separate 16 MiB budget
for allocation-backed GPU mappings. A bounded shadow map tracks at most
32 disjoint state intervals. Replacement splits intervals, adjacent intervals
with the same owner and protection merge, and successful allocation mapping
removes overlapping shadow state. Plans are prepared before native operations;
failed operations preserve the previous ownership records.

Replacing allocation mappings requires the same device, complete coverage of
each affected mapping and retirement of all tracked hardware commands. Partial
allocation replacement and replacement of the legacy standard-allocation route
are rejected. Successful replacement clears GPU mapping records and their
budget while retaining allocation handles, residency and CPU leases. Those
allocations can be mapped again. Shadow metadata is committed only after a
valid native reply; the host retains native ownership if validation or fence
retirement fails and stops the owned VM before teardown.

Whole-reservation release removes its state only after the native release
acknowledgement. Disconnect teardown stops and reaps QEMU, destroys hardware
queues, releases allocations, releases reservations and closes adapters.
`--gpu-state-eof-test` exits immediately after the first retired state mapping
and confirms that its native reservation survives until the owned VM exits.

MSVC and GCC builds pass with warnings treated as errors. Wire tests cover
input/output validation, type and parent ownership, foreign ranges, failed
operations, full versus partial allocation replacement, remapping and interval
split/merge/budget behavior. All 128 Linux fault scenarios and 26 native CLI
rejection scenarios pass.

## Remaining live boundaries

After these mappings, the runtime requests a CPU lock for a 272-page
(1,114,112-byte) allocation mapping, larger than the current 1 MiB aperture
slot limit. The host bridge's preflight rejects it before `D3DKMTLock2`; the
guest receives `ENOMEM`. The enabled runs return `8007000e`. Packet-based synchronization
signal/wait and vendor-private escape operations are also unfinished; ioctl 30
appears during cleanup. The diagnostics do not prove general rendering,
asynchronous submission throughput, reset recovery or desktop stability.
