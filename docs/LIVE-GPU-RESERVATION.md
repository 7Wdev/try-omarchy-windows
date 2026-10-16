# Owned GPU address reservations

The QEMU guest's live NVIDIA Linux runtime now reserves and releases GPU virtual
address ranges through Windows. The 32- and 64-slot diagnostics each created
four reservations (512 MiB, 3 GiB, 2,686,976 bytes and 4 MiB), 38 allocations,
27 direct CPU mappings and three hardware queues. One 4 KiB initialization
command completed at progress fence 17, verified by Windows and the guest.
All four reservations were explicitly released by the guest and all native
objects were reclaimed.

The [physical evidence](evidence/QEMU-LIVE-GPU-RESERVATION-2026-10-09.json)
includes normal, abrupt-exit and disabled controls, source/binary hashes, and
the initial diagnostics that exposed the bridge's capacity limits. These are
partial driver diagnostics. D3D12 initialization still returns `80070057`.
Interactive Omarchy, CUDA, OpenGL, Vulkan, video acceleration and the required
93% native performance remain unverified.

## Reservation and release contract

Microsoft's [reservation interface](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_reservegpuvirtualaddress)
uses an adapter handle, 64 KiB alignment and zero reserved fields. The obsolete
paging-queue reservation mode is excluded. A reservation owns address space;
it does not allocate that quantity of physical memory.

`--driver-reservation` requires GPU-address mappings and an owned QEMU runtime.
The guest sends its typed adapter identity and a bounded descriptor. Windows
retains the native range under a synthetic identity; native adapter handles and
CPU pointers do not cross the protocol. Both ends enforce ownership, alignment,
overflow, returned-address constraints and overlapping-reservation checks.
The diagnostic permits eight ranges, at most 4 GiB each and 16 GiB in total,
within 48-bit GPU address space.

Guest release resolves an exact, whole, owned reservation. Foreign, stale and
partial ranges are rejected before a native call. A failed release retains
ownership and permits retry. Microsoft's [free interface](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtfreegpuvirtualaddress)
also invalidates mapped addresses. The bridge permits fully contained allocation
mappings after tracked hardware queues have retired their submitted commands;
it rejects mappings that only partially overlap. Successful release clears the
corresponding GPU mappings while retaining their allocation handles and CPU
leases. Those allocations can receive new mappings. Failure preserves the old
mapping records.

Disconnect teardown stops and reaps the owned VM, destroys hardware queues,
releases allocation mappings, releases reservations, then closes adapters.
Queue destruction failure retains reservation ownership, with process teardown
as a backstop. The separate `--reservation-eof-test` exits immediately after the
first successful reservation and verifies this ordering without guest unmap
acknowledgements.

## Limits exposed by the live runtime

The first diagnostic reserved 512 MiB but rejected the next 3 GiB request.
After increasing the virtual-address limit, initialization needed a 480-page
GPU-only mapping. That mapping exceeded the old 1 MiB per-mapping bound. GPU-only
mappings now permit 4 MiB, while CPU leases keep their existing 1 MiB slots and
the total GPU mapping budget remains 16 MiB.

The runtime then exhausted 32 vendor allocation objects and requested a maximum
priority allocation. The pool now allows 64 allocations within 128 typed wire
objects. The bridge preserves the documented priority range through
[`D3DDDI_ALLOCATIONPRIORITY_MAXIMUM`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_setallocationpriority)
(`0xc8000000`), including high priority. Reported GPU usage remains guarded at
64 MiB; this is not a hard physical-allocation byte quota.

125 Linux fault scenarios and 18 native CLI rejection scenarios pass. The wire
tests cover malformed reservation input/output, type/parent ownership, stale
identities, separate object and address-space budgets, failed releases,
contained-mapping invalidation, partial-overlap rejection and teardown order.
MSVC and GCC builds pass with warnings treated as errors.

## Next live boundary

After the fourth reservation, the runtime requests a 4 MiB mapping at 1 TiB
with protection value `5` (`Write | Zero`). The current allocation-backed mapping
route rejects it before calling Windows. Microsoft's [protection contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddigpuvirtualaddress_protection_type)
requires a null allocation for zero pages; reads return zero and writes are
discarded. A separate reservation-owned mapping operation needs investigation
and implementation. The present metadata does not record the allocation field
of that request, so its null identity must be verified before forwarding it.
Vendor-private escape operations and packet-based fence signal/wait are also
unfinished. No general command throughput or full desktop rendering has been
measured.
