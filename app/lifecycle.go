package main

import (
	"bufio"
	"io"
	"net"
	"time"
)

// Reclaim is also used by the launcher's local CLI. Only guest lifecycle
// messages can change readiness or reboot state, and those require QEMU.
func serveLifecycle(c net.Conn, fromQEMU func(net.Conn) bool, handle func(string, net.Conn)) {
	defer c.Close()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	line, err := bufio.NewReader(io.LimitReader(c, 64)).ReadString('\n')
	if err != nil {
		return
	}
	switch line {
	case "ready\n", "reboot\n":
		if !fromQEMU(c) {
			return
		}
	case "reclaim\n":
	default:
		return
	}
	handle(line[:len(line)-1], c)
}

// The user-net lifecycle socket is not a reconnecting chardev. Hold an early
// connection briefly until the supervisor publishes the PID, never trusting a
// pending PID unless it becomes the current QEMU's exact PID.
func lifecyclePeerMatches(peer uint32, current func() uint32, wait func() bool) bool {
	if peer == 0 {
		return false
	}
	for {
		pid := current()
		if pid != 0 {
			return pid == peer
		}
		if !wait() {
			return false
		}
	}
}
