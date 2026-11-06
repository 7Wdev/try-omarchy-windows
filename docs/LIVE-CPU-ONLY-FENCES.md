# CPU-only monitored fences

The live NVIDIA Linux runtime now creates Windows monitored fences with
`NoGPUAccess` through QEMU. Both expanded-aperture tests created two such fences,
eight GPU-addressed monitored fences and eight mutexes. Initialization advanced
to 27 allocations and 24 direct CPU mappings, then stopped at the unsupported
GPU virtual-address reservation operation (`80004001`). One native 4 KiB command
completed at progress fence 10 in Windows and the guest.

The [physical evidence](evidence/QEMU-LIVE-CPU-ONLY-FENCES-2026-10-09.json) records
three accepted diagnostics with source and binary hashes. Full D3D12
initialization, interactive Omarchy, CUDA, OpenGL, Vulkan, video acceleration and
the required 93% native performance remain unverified.

## Fence contract

Microsoft documents that [`NoGPUAccess`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_synchronizationobject_flags)
removes the fence's GPU virtual-address mapping, keeps its counter 64 bits wide
and permits packet-based signal/wait operations. The bridge preserves bit 7
(`128`) only for monitored fences. Mutex flags remain zero; shared, unknown and
combined flags are rejected. The native driver must return exactly the requested
flags, an owned synchronization identity, CPU storage and a zero GPU address for
this mode. Ordinary monitored fences still require a valid nonzero GPU address.

The independent synchronization quota is now 32 objects, bounded by the existing
64 total wire objects and 64 read-only QEMU fence slots. CPU counters use the same
owned page leases as other fences. Guest mappings remain read-only. Direct reads
bypass MMIO, and native destruction requires acknowledged unmap or confirmed VM
exit. The change supports creation, direct reads and destruction; packet-based
signal/wait remains unfinished.

## Verification and next boundary

The 32- and 64-slot cases each created 18 synchronization objects, 27 allocations,
three hardware queues and 24 CPU mappings. Eight mappings used CPU aperture
slots beyond the legacy limit. Both acknowledged all 15 fence unmaps and all
24 CPU unmaps, reclaimed five allocations with queues alive and released every
native object without cleanup failures.

The explicit `--sync-eof-test` control exits after mapping the first
`NoGPUAccess` fence. It retained 17 synchronization objects, three hardware queues
and 19 CPU locks until normal owned VM exit. Native cleanup then released them;
the test requires zero guest unmap acknowledgements. The initial run used a
20-allocation minimum and correctly failed that expectation; the repeated fresh
report uses the observed 19-allocation exit stage and passes all ownership and
cleanup checks. Both reports remain recorded in the evidence.

MSVC and GCC checks passed with warnings treated as errors. All 101 Linux fault
cases and 12 native invalid-option/control checks passed. Protocol tests cover
zero GPU addresses for this fence mode, rejection of an unexpected GPU address,
invalid flag combinations, mutex restrictions, failure cleanup and the 32-object
quota.

The next live request reserves 512 MiB of GPU address space on the owned adapter,
between 64 MiB and 1 TiB, with zero reserved fields. Microsoft's
[reservation structure](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_reservegpuvirtualaddress)
defines the adapter-based operation and its 64 KiB alignment requirements.
Reservation and release ownership must be implemented before forwarding it.
No native reservation is issued by this revision.
