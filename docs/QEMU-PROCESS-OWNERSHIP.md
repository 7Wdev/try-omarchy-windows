# QEMU process ownership

The native runtime now adopts its QEMU child through a verified Windows process
object before resuming the child. This is groundwork for transferring supervision
to the normal launcher. **Launcher control IPC, activation and normal guest boot
remain unimplemented.** Acceptance counts stay **4/8, 0/4, 3/3**.

## Ownership contract

`OwnedProcess::adoptVerified` checks the candidate's PID and exact creation time,
its relationship to the held, live creator process, and its executable file.
The file comparison uses volume and file identifiers; the expected executable
stays open without write/delete sharing. This follows Microsoft's documented
[process-handle model](https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers),
[creation-time query](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes)
and [file-identity comparison](https://learn.microsoft.com/windows/win32/api/winbase/ns-winbase-file_id_info).

Unknown/all-zero file identities and NUL-bearing image paths are rejected.
Identity checks finish before any job assignment. The caller must already
authenticate its launcher/session: a parent PID is not that authentication.

A successful adoption retains a non-inheritable process handle and assigns the
verified child to a private job with kill-on-close. The direct runtime still
creates QEMU suspended, then adopts it and resumes its original thread. A local
guard reaps that newly created child if identity or job setup fails. Job ownership
uses the documented [Windows job API](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-assignprocesstojobobject).

Stopping an owner before attachment prevents a later attachment. Duplicate
attachment is rejected. Normal and forced stop paths wait for the held process
object to signal exit before reporting it reaped. Native mappings and GPU objects
remain owned until that exit is established. The VM's numeric PID is never used
as a replacement for the retained process object during teardown.

## Verification, October 29, 2026

Strict default and 4K Windows builds run **48 checks** with real hidden helper
children. Controls cover:

- Incorrect PID, missing/stale creation time, wrong creator or executable, an
  exited creator/candidate, unavailable identifiers and NUL-truncated paths.
- Rejection without changing ownership or stopping the live candidate.
- A retained non-inheritable process object after the original handle closes.
- Duplicate/cancelled attachment, natural exit, forced own-child exit and
  repeated stop without changing the recorded result.

These helpers start no QEMU, GPU renderer or desktop window and inject no input.
The creation-time mismatch control does not deliberately force Windows PID reuse.
An earlier local candidate passed the GPU workloads before source review found
the unavailable-ID and NUL-path cases. Its original controls are retained and
excluded from the final ownership acceptance.

Three separate hidden QEMU/WHPX guests exercise the same adoption path with the
existing NVIDIA workloads: allocation-pressure recovery, both original 4K compute
rounds, and both original 3840×2160 shader frames. The guest images, memory guards,
data/pixel oracles and graphics arguments are unchanged. Shader pixels match the
previous captures byte for byte. Host and VM exit naturally; native objects and
aperture mappings retire before VM exit. All 430 launcher sources remain unchanged.

[Sanitized file-audit evidence](evidence/QEMU-PROCESS-OWNERSHIP-2026-10-29.json)
binds frozen/current compiler inputs, executables, test logs, previous control
receipts and the new GPU regressions. Earlier rejected pressure and input evidence
remain retained in their original documents.

Next is an authenticated launcher lifetime/control channel and an explicit
handoff of the actual QEMU identity to the unchanged window/input supervision.
Launcher-parent death recovery, normal session startup, live SDL/input validation,
long browsing sessions and matched-native performance remain required work.
