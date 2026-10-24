# Contiguous CPU allocation views

The NVIDIA Linux runtime in our owned QEMU guest now directly accesses a
272-page (1,114,112-byte) Windows allocation view. The bridge maps it across
two adjacent 1 MiB aperture slots and presents one contiguous guest pointer.
The physical 32- and 64-slot runs both created 42 allocations, 31 CPU views,
50 synchronization objects and three hardware queues. One 4 KiB initialization
command retired at progress fence 17. All native resources were reclaimed.

The [hardware evidence](evidence/QEMU-LIVE-CPU-ALLOCATION-SPANS-2026-10-09.json)
includes both normal configurations, guest exit with the spanning view still
owned, and first/last-word write control with native verification and restoration.
These four controls pass. **Full D3D12 initialization has not succeeded**:
the normal runs return device HRESULT `80070057` and never reach copy queue
creation. Interactive Omarchy, the requested APIs and 93% native performance
remain unfinished.

## Mapping and lifetime

The [existing QEMU allocation hub](LIVE-CPU-APERTURE-CAPACITY.md) retains its
1 MiB slot stride and 16/32/64-slot configurations. No QEMU rebuild is needed.
The new host aperture allocator reserves a complete adjacent slot group before
mapping any pages. Each chunk uses the native source address plus its byte
offset, a distinct monotonically increasing generation and its own QMP
acknowledgement. The final chunk maps only the remaining page-aligned bytes.
The tested large view uses a 1 MiB chunk followed by a 64 KiB chunk.

Windows takes `D3DKMTLock2` only after checking the contiguous span is available.
It still requires an exact committed, private, writable native region of the
expected size. The wire reply describes the whole view; guest validation checks
its complete interval against the configured aperture and other active views.
One guest `mmap` covers all its chunks. The guest performs 5,000 volatile loads
from each end; QEMU's allocation MMIO counters stay zero.

Normal unlock unmaps every chunk in reverse order, checking its exact generation.
The whole lease remains owned until all acknowledgements arrive. Partial map or
unmap failure stops and reaps the owned VM before pages can be released. Forged
partial leases, suffixes, old generations and fragmented free space are rejected.
Tests inject failure on the second chunk of both operations and check ownership
at the VM-exit boundary.

CPU views are bounded to 4 MiB each within the existing 16 MiB live CPU-byte
budget. This does not raise the 1 MiB legacy standard-allocation limit, the
64 vendor-allocation quota or 128 total wire objects. The separately bounded
synchronization pool is now 64 objects: the first spanning-view observation
reached its previous 32-object limit. Native fence mappings still have their
own 64-slot capacity.

## Hardware controls

| Control | CPU views / native slot chunks | Result |
| --- | --- | --- |
| Normal, 32 slots | 31 / 32 | All chunk unmaps acknowledged; native initialization command retired; clean teardown |
| Normal, 64 slots | 31 / 32 | Same results and large view; clean teardown |
| Exit with large view owned, 32 slots | 28 / 29 | No guest unmap acknowledgement; native locks, queues and reservations released after owned VM exit |
| Write control, 64 slots | 31 / 32 | Windows checked and restored both words in every view, including the spanning view; clean teardown |

The write control uses repeated references to the same guest pointer and keeps
the native lock until the final release. Native command submission is disabled
for this control, so ioctl 52 is expected to remain unsupported. The normal
submission runs verify 15 allocation retirements while hardware queues remain
alive, four retired GPU Zero-state maps and all paging/residency fences.

MSVC and GCC builds pass with warnings treated as errors. The portable aperture
tests, existing wire contracts, 128 Linux fault scenarios, 33 native CLI rejection
scenarios and initialization-result guard tests pass. CI cannot establish these
hardware results; the public evidence contains metadata and hashes, never the
proprietary runtime image or private driver buffers.

## Initialization milestone and next boundary

The acceptance harness now derives `runtimeInitializationComplete` from real
guest results instead of setting it permanently false. It requires unique
successful device and copy queue results, the genuine NVIDIA UMD still loaded,
the virtio transport with real WSL dxg forwarding disabled, successful probe
exit, normal owned QEMU exit and verified native cleanup. Partial initialization,
duplicate or contradictory results, host-only rendering and synthetic guard
tests cannot satisfy the milestone. Successful initialization must still pass
the enabled resource-lifetime checks. It will not establish desktop acceleration.

After increasing the synchronization limit, the runtime creates 50 objects and
continues to a non-primary allocation whose display-source value is `UINT_MAX`.
The current `validVendorAllocation` preflight accepts only source zero and
rejects this request with `EINVAL` before a native allocation call. This is the
next observed rejection; it has not yet been proven to explain the final
`80070057`. The public [allocation-info contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_allocationinfo2)
associates `VidPnSourceId` with primary surfaces. Its sentinel semantics need
investigation before extending the standalone-allocation route. Vendor-private
escapes, general synchronization, asynchronous queues, rendering and presentation
also remain unfinished.
