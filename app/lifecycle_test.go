package main

import (
	"net"
	"testing"
)

func TestLifecycleRefusesForeignPeers(t *testing.T) {
	for _, line := range []string{"ready\n", "reboot\n", "ready extra\n", " ready\n", "ready", "reboot\r\n"} {
		host, peer := net.Pipe()
		handled := make(chan string, 1)
		done := make(chan struct{})
		go func() {
			serveLifecycle(host, func(net.Conn) bool { return false }, func(s string, _ net.Conn) { handled <- s })
			close(done)
		}()
		peer.Write([]byte(line))
		peer.Close()
		<-done
		select {
		case s := <-handled:
			t.Fatalf("foreign %q handled as %q", line, s)
		default:
		}
	}
}
func TestLifecycleAcceptsQEMU(t *testing.T) {
	for _, line := range []string{"ready\n", "reboot\n"} {
		host, peer := net.Pipe()
		handled := make(chan string, 1)
		go serveLifecycle(host, func(net.Conn) bool { return true }, func(s string, _ net.Conn) { handled <- s })
		peer.Write([]byte(line))
		peer.Close()
		if got := <-handled; got+"\n" != line {
			t.Fatal(got)
		}
	}
}
func TestLifecyclePendingPID(t *testing.T) {
	pid := uint32(0)
	if !lifecyclePeerMatches(42, func() uint32 { return pid }, func() bool { pid = 42; return true }) {
		t.Fatal("early QEMU refused")
	}
	if lifecyclePeerMatches(43, func() uint32 { return 42 }, func() bool { t.Fatal("foreign peer waited"); return true }) {
		t.Fatal("foreign accepted")
	}
	if lifecyclePeerMatches(42, func() uint32 { return 0 }, func() bool { return false }) {
		t.Fatal("unpublished peer accepted")
	}
}
