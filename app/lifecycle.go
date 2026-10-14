package main

import (
	"bufio"
	"io"
	"net"
	"strings"
	"time"
)

// Reclaim is also used by the launcher's local CLI. Only guest lifecycle
// messages can change readiness or reboot state, and those require QEMU.
// An open-url line comes from the -open-url process Windows starts for an
// allowed link: never from QEMU, and only from a process of this Windows user
// in this session. The peer is looked up once and every check applies to
// that one result; a failed lookup refuses both kinds.
func serveLifecycle(c net.Conn, peer func(net.Conn) (uint32, error), fromQEMU, fromSameUser func(uint32) bool, handle func(string, net.Conn)) {
	defer c.Close()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	// Capture ownership before reading: the guest sends one line and closes.
	pid, lookupErr := peer(c)
	known := lookupErr == nil && pid != 0
	trustedPeer := known && fromQEMU(pid)
	// One bounded read; base64 of the longest link fits with room to spare.
	line, err := bufio.NewReader(io.LimitReader(c, 4096)).ReadString('\n')
	if err != nil {
		return
	}
	if strings.HasPrefix(line, "open-url ") {
		if !known || trustedPeer || fromSameUser == nil || !fromSameUser(pid) {
			return
		}
		if _, ok := parseOpenURLLine(line); !ok {
			return
		}
		handle(line[:len(line)-1], c)
		return
	}
	if len(line) > 64 {
		return
	}
	switch line {
	case "ready\n", "reboot\n", "shutdown\n":
		if !trustedPeer {
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
