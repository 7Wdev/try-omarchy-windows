//go:build windows

package main

import (
	"net"
	"sync/atomic"
)

// connectionFromQEMU reports whether an accepted loopback connection belongs
// to this launcher's current QEMU. The guest bridges listen on fixed
// localhost ports that any local process, in any Windows session, can reach.
func connectionFromQEMU(conn net.Conn) bool {
	pid, err := loopbackPeerPID(conn)
	return err == nil && pid != 0 && pid == qemuPid.Load()
}

// acceptQEMUBridge serves connections from this launcher's QEMU, at most
// limit at a time, until the listener closes. Other connections are closed
// at once. QEMU's chardevs reconnect every second, so a connection refused
// before the supervisor records the new PID is simply retried.
func acceptQEMUBridge(listener net.Listener, name string, limit int32, serve func(net.Conn)) {
	var active atomic.Int32
	for {
		conn, err := listener.Accept()
		if err != nil {
			logf("%s: accept: %v", name, err)
			return
		}
		if !connectionFromQEMU(conn) {
			logf("%s: refused a connection that is not from Omarchy's QEMU", name)
			conn.Close()
			continue
		}
		if active.Add(1) > limit {
			active.Add(-1)
			logf("%s: refused a connection over the limit of %d", name, limit)
			conn.Close()
			continue
		}
		go func() {
			defer active.Add(-1)
			defer conn.Close()
			serve(conn)
		}()
	}
}
