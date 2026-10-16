# Independent GPU-client coordinator: actor routing foundation

The actual 4K compositor now has [clean memory admission and shutdown](LIVE-NORMAL-OMARCHY-4K-PRESENTATION-BUDGET.md).
Quickshell still cannot initialize its GPU context: the authenticated broker
currently accepts one producer and closes its listener. Supporting another
producer requires owned native sessions and shared capability lifetimes.
The [recipient-owned native alias API](GPU-SHARED-REFERENCE-ALIASES.md) now
passes Session controls and separate NVIDIA NT-reference lifetime probes;
authenticated cross-client FD routing is still pending.

`driver_actor_router.h` adds a coordinator building block around the existing
real `driver_bridge::Session` validator. A trusted coordinator registers up to
four distinct sessions and receives monotonic actor IDs that are never reused.
The new, separate 40-byte envelope carries actor, sequence, operation and exact
inner-packet extent. The existing 16-byte driver header remains unchanged, and
all 4,096 inner bytes remain available. Malformed/replayed framing poisons the
coordinator stream before native dispatch; an unknown retired actor is rejected.

Each actor retains its own wire handles, negotiated session state and request
counter. A numeric handle collision resolves within the selected session;
nested foreign parents cannot reach the native Driver. Exhausting one actor's
request budget does not reset it through Hello or consume another's budget.
The shared native Driver remains responsible for aggregate physical resources.
This router does **not** establish that current GPU budgets fit two renderers.

Disconnect marks an actor draining and refuses further client calls. It retains
the session and its live objects and still charges its actor slot. Empty close
can detach it; disconnect cannot destroy GPU memory that the VM may still map.
The router borrows sessions: its enclosing owner must retain them and the
Driver, prove VM exit when abort cleanup requires it, and only then destroy
nonempty sessions. The portable control checks this ordering using an explicit
mock VM-stop state; it is not evidence of actual native abort cancellation.

Strict GCC and MSVC controls exercise two actual Session validators against a
mock Driver, colliding IDs, foreign parent rejection, continued use by the live
actor after another disconnects, retained resources, bounded actor slots,
non-reused IDs, independent 10,000-call budgets and malformed/replayed envelopes.
These controls are wired into the native build and portable CI.

**Runtime integration remains disabled.** The Windows owner does not accept
this envelope yet; the broker still admits one GPU producer. No second NVIDIA
client, Quickshell GPU frame, shared-FD interoperability or full desktop is
claimed by these controls. The existing 4K hardware evidence remains pinned to
its original executable/source digests.

The next implementation must:

1. Register native actors from authenticated broker peers/pidfds. The broker
   must stamp the actor itself; a client-provided actor ID grants no authority.
2. Coordinate one exact GPU/profile agreement and aggregate native resource
   admission. The current native `beginOpenGl` rejects a second global session;
   forwarding another Hello/BeginOpenGl cannot reset the first actor's state.
3. Allocate globally unique kernel capability tickets and retain their
   actor/local-ticket/native-reference lineage. Current per-session wire IDs
   collide, so separate ObjectStores cannot safely resolve foreign FDs alone.
   Import must create an owned alias in the recipient's session after resolving
   the actual capability FD, with its full kind/cookie/generation checks.
4. Keep producer resources alive while recipient buffer/sync FDs exist, and
   reap them only after kernel-confirmed final release. Handle abnormal producer
   exit and pending GPU work without invented fence retirement or premature
   backing-memory release. The earlier version-8 abort control remains open.
5. Verify both Hyprland and Quickshell on NVIDIA at 3840×2160, then normal
   launcher/UWSM, input/display, long-run stability, application APIs and matched
   performance. Desktop acceptance remains **0/4**.
