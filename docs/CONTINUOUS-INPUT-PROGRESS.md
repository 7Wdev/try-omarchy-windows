# Continuous display during input checks

The interactive diagnostic no longer holds the Windows picture at frame 1,799
while waiting for application checks. Run 249 presents **2,406 actual GPU frames**
before a failed browser input check, then acknowledges and drains the real final
frame **2,407**. This fixes an observed fixture freeze. It does not establish
smooth daily-use performance or ordinary browser input acceptance.

The live observation shows Chromium moving in front of the terminal after a
normal Windows-to-QEMU click. The owned guest probe independently reports the
expected Chromium window, `document.hasFocus()` and the text field's focus. Two
host keyboard calls return, but the browser observes an empty value and no key
events before its deadline. The scrolling attempt happens after the browser has
closed, so it provides no wheel-routing result. **The input workload remains
rejected.** The official input handlers are unchanged; the missing keyboard
delivery still needs diagnosis.

The failure path supplies ordinary scene damage by moving the owned terminal by
one pixel, then waits for the actual final output acknowledgement. The audit
requires every host/native/SDL frame from 1 through 2,407, no CPU upload, audit
readback only at the two boundaries, and complete source-to-window pixels at
both boundaries. The final capture is a failure-drain frame, not a successful
three-application acceptance image.

The same run completes 5,717 native submissions and 5,887 device-health queries
with zero native, health or compositor failures. Both the host and VM exit
naturally. All tracked native objects and aperture mappings retire before VM
exit. Guest-disk, installed-driver, frozen-source and QEMU guards pass. These
cleanup results preserve the input failure rather than replacing it with a pass.

[Structured retained evidence](evidence/CONTINUOUS-INPUT-FAILURE-2026-10-27.json)
and the [file-only auditor and controller sources](../experimental/windows-native-gpu/interactive-frame-probe/README.md)
record the exact scope. No further desktop automation is needed to replay the
background queue tests or audit the saved captures.

The published queue also rejects a finish request that arrives while an
unauthorized cap frame is in flight and requires end-of-stream acknowledgement.
Those additional controls pass background mock tests and strict compilation;
they have not yet received another live desktop trial. Longer sessions, ordinary
keyboard/wheel routing, normal account/launcher integration, pressure recovery,
physical latency and the matched-native performance target remain open. The
backend and product checklists remain **4/8** and **0/4**.
