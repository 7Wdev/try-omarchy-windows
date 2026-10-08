# Adapter queries for the Linux graphics runtime

The Windows worker now supports bounded, chunked adapter query transactions.
A disk-free QEMU/Linux run on the RTX 5090 Laptop GPU completed 32 native
queries: ten public identity/version checks and 22 inputs captured from the
installed NVIDIA Linux runtime. These include an 8192-byte description and
the 50,616-byte UMD initialization query, which cannot fit in one bridge packet.
The same run passed the existing context and shared-RAM GPU copy checks and
clean disconnect. See [QEMU evidence](evidence/QEMU-WDDM-QUERIES-2026-10-08.json).

The [native compatibility diagnostic](evidence/NATIVE-WDDM-QUERIES-2026-10-08.json)
also accepted all 22 successful NVIDIA query inputs through
[D3DKMTQueryAdapterInfo](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtqueryadapterinfo).
These tests do not run a live guest graphics runtime. The shared client accepts
runtime-generated input buffers, but the hardware acceptance used local
captures. No guest GPU command submission, Hyprland rendering or performance
claim follows from adapter query acceptance.

## Wire and lifetime

The host must explicitly enable `--driver-queries`; capability bit 64 advertises
it. The default endpoint rejects all query operations. Context opt-in remains
independent and either flag order is accepted. No native handle or process
pointer appears in a query descriptor.

The five new operations use an existing connection-local adapter ID:

| Operation | Request | Behavior |
| --- | --- | --- |
| `0x2030 BeginAdapterQuery` | Four u32 fields: type, total bytes, two zero reserved fields | Allocate zeroed staging memory; one query per adapter, at most two per session. |
| `0x2031 WriteAdapterQuery` | Range plus input bytes | Ordered chunks of 1..4064 bytes; holes, overlap, retries and writes after execution are rejected. |
| `0x2032 RunAdapterQuery` | Header only | Require the entire input, call the Windows driver once, preserve NTSTATUS and the in/out buffer. |
| `0x2033 ReadAdapterQuery` | Range | Return bounded chunks after execution, including a native failure's in/out buffer and status. |
| `0x2034 EndAdapterQuery` | Header only | Release staging memory; permitted before execution to cancel. |

Buffers are limited to 65,536 bytes each, so staging is at most 128 KiB per
session. A pending query prevents closing its adapter. Disconnect clears query
buffers before native object cleanup. Public query types have exact inline
x64/UTF-16 sizes; opaque UMD data is bounded separately. Pointer-bearing PnP
queries, registry queries and working-set changes are unsupported. The current
type set is 0, 1, 3, 13, 15, 17, 18, 24, 27, 30, 31, 34, 55, 56, 60, 61, 62,
and 66. This is an observed initialization subset, not the full KMT API.

`adapter_query_client.h` implements the complete transaction for the Linux
client. It verifies operation/handle correlation, reserved fields, sizes,
native status consistency and chunk progress, and cancels on a malformed
reply. Callers must serialize the entire transaction on their connection and
disconnect on transport failure. Portable and MSVC tests cover a full
50,616-byte reply, native failure, malformed replies, cancellation, staging
quota, adapter ownership and unsupported layouts. A separate physical
[default-disabled regression](evidence/QEMU-QUERIES-DISABLED-2026-10-08.json)
verified zero native query calls while normal GPU copies still passed.

## Guest ABI adaptations still required

Native adapter queries return Windows data unchanged. The runtime reference
returned `libnvwgf2umx.so` in the UMD filename; the corresponding native query
returned `nvldumdx.dll` in the same driver-store directory. A guest library must
select its compatible, locally supplied Linux UMD rather than loading the
Windows DLL. This project does not redistribute vendor driver binaries.

The WSL adapter type also differs from the physical Windows adapter type.
Microsoft's [pinned WSL source](https://github.com/microsoft/WSL2-Linux-Kernel/blob/d504d40ab83839cb61942a812a99d183c42227b9/drivers/hv/dxgkrnl/dxgvmbus.c#L4276)
sets the documented Paravirtualized bit and adjusts display, post-device,
indirect-display, ACG, timings and compute flags for its guest interface. This
requires an explicit guest API adaptation; it is not a reason to patch
undocumented NVIDIA-private bytes. The native private reply differed from the
WSL reply at 16 byte positions; the diagnostic records difference counts only,
and no private relocation rule is implemented.

## Reproduce

Run `build_umd_context.sh` with the pinned DirectX-Headers and libdxg checkouts
and a fresh capture directory. It now also packs `driver-queries.bin`, selecting
successful NVIDIA inputs using the physical device-ID replies. Keep captures
and the resulting initramfs local. Build the Windows worker and Linux guest
using `build.ps1` and `build_guest.sh`.

Add the local query fixture when building the acceptance initramfs:

```sh
python3 experimental/windows-native-gpu/make_test_initramfs.py \
  --init experimental/windows-native-gpu/build/guest-init \
  --probe experimental/windows-native-gpu/build/guest-probe \
  --query-fixture /local-capture/driver-queries.bin \
  --output /local-build/query-test.cpio
```

Run `test_qemu.py` with the normal QEMU, kernel, bridge and report paths plus
`--driver-queries --expected-driver-queries 22` (use the actual packer count).
Without a private fixture, the opt-in checks five pairs of public identity and
version queries. Context and shared-memory checks can be enabled in the same
run. The ordinary CI-built initramfs contains no captured vendor data.
