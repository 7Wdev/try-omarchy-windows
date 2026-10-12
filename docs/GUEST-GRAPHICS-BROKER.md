# Guest graphics broker integration

The first broker component now authenticates individual Linux client processes
and tracks their kernel process lifetime independently of socket lifetime. Its
real-process contract passes on WSL and inside the QEMU guest, including a
UID 65534 client. The guest subsequently passes the existing full-buffer 4K
resource-FD and OpenGL shader checks. This establishes the broker ingress and
process-exit boundary; GPU calls still use the existing direct connection.

## Implemented boundary

`guest_broker_peer.h` owns an accepted Unix `SOCK_SEQPACKET` connection and its
kernel process handle. The listener enables `SO_PASSCRED` before accepting.
`SO_PEERCRED` must match the configured UID/GID, and every packet must carry
matching `SCM_CREDENTIALS`. Passing an already connected socket to a descendant
does not authorize that descendant. Each process needs its own connection.
Set-ID clients whose real and effective credentials differ are rejected.

The process handle comes directly from `SO_PEERPIDFD`, preserving the socket's
pinned kernel identity. There is no numeric-PID lookup fallback or process-name
matching. A kernel lacking the required socket support is rejected. The
[kernel implementation](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/aa5e9884a2d63aa20fc3396d369382c1ecd16109/net/core/sock.c)
obtains that handle from the socket's peer PID object. The
[Linux Unix-socket documentation](https://man7.org/linux/man-pages/man7/unix.7.html)
describes the per-message credentials and descriptor transfer mechanisms.

Frames have a fixed 32-byte header, a maximum 4096-byte control payload, at most
eight descriptors, and strictly ordered nonzero sequence numbers. Unknown
versions/operations, reserved fields, changed credentials, mismatched lengths,
extra descriptors and truncated messages disconnect the channel. Every received
descriptor is adopted before rejection and closed on failure; close-on-exec is
set when the kernel installs it. Unexpected descriptor-bearing `SCM_PIDFD`
messages are closed and rejected too. The normal listener disables that feature.

Disconnect forbids further requests while preserving the process handle. A
socket EOF, rejected packet or exited process leader does not establish that
all driver finalizers and threads have exited. The contract runs an actual ELF
library finalizer after socket close, and separately verifies a zombie leader
with a live worker. `processExited()` becomes true only after the thread group
exits. Dropping the `Peer` object itself never authorizes GPU resource cleanup.
The [pidfd documentation](https://man7.org/linux/man-pages/man2/pidfd_open.2.html)
describes polling process handles for exit.

This boundary does not authorize a native GPU object, allocation offset or
kernel controller FD. A descriptor received from a client remains untrusted
until the corresponding resource/lease authority validates it.

## Retained checks

The real-process contract checks 13 malformed/replayed/foreign-sender cases,
descriptor counts before/after rejection, two independent peers, a real ELF
finalizer, wrong UID binding, required socket options/type and thread-group
exit. The transferred backing is an owned `memfd`, not a GPU allocation.

The [guest evidence](evidence/BROKER-PEER-2026-10-12.json) retains the full kernel
contract and GPU regression capture, exact source/build hashes and the scoped
audit. The GPU regression still renders directly through the NVIDIA bridge:
both complete 3840×2160 storage rounds, 24 native commands, two ordered GPU
retirements, zero CPU slot rejections and complete guest/kernel/native/VM cleanup.
It does not prove two GPU client processes sharing an imported native resource.

Build the peer contract on Linux:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror -shared -fPIC \
  experimental/windows-native-gpu/guest_broker_finalizer_probe.cpp -o /tmp/broker-finalizer.so
c++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
  experimental/windows-native-gpu/guest_broker_peer_test.cpp -ldl -o /tmp/broker-peer-test
/tmp/broker-peer-test /tmp/broker-finalizer.so
```

`WDDM_BRIDGE_TEST_BROKER_PEER=1` compiles the optional guest helpers with the
OpenGL probe. The private image packer requires both `--guest-broker-contract`
and `--guest-broker-finalizer`; it hashes and includes their dependencies. Init
runs the contract before graphics and rejects a failed contract. Images omit it
unless explicitly selected. CI runs the real process test without a GPU.

## Remaining integration

The next component must route authenticated peers into separate native object
namespaces over the shared VM transport. CPU/GPU memory and pending-work quotas
must remain global, rather than being multiplied by the number of clients.
The existing request/deadline/frame limits are diagnostic limits and need a
separate sustained runtime contract before production use.

A broker-owned capability store must validate imported kernel resource FDs and
retain the native NT reference until the last recipient capability closes.
Producer process exit alone cannot retire an exported shared resource. Restricted
CPU-view FDs must come from validated native mapping replies; clients must never
receive the privileged kernel controller or choose arbitrary aperture offsets.
Per-process view/queue cleanup must wait for kernel process exit and GPU retirement.

After that, the guest needs the graphics device/allocator, formats and compositor
buffer/synchronization integration that Hyprland expects, plus continuous host
presentation. The current native texture FDs are not dma-buf/DRM PRIME. The first
correctly displayed Hyprland session will be captured and added to PR #2
immediately. Desktop stability, required APIs and the ≥93% matched native
performance target remain unverified.
