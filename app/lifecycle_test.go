package main

import (
	"errors"
	"net"
	"strings"
	"testing"
	"time"
)

func TestLifecycleRefusesForeignPeers(t *testing.T) {
	for _, line := range []string{"ready\n", "reboot\n", "shutdown\n", "ready extra\n", " ready\n", "ready", "reboot\r\n"} {
		host, peer := net.Pipe()
		handled := make(chan string, 1)
		done := make(chan struct{})
		go func() {
			serveLifecycle(host, func(net.Conn) (uint32, error) { return 7, nil }, func(uint32) bool { return false }, nil, func(s string, _ net.Conn) { handled <- s })
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
		go serveLifecycle(host, func(net.Conn) (uint32, error) { return 7, nil }, func(uint32) bool { return true }, nil, func(s string, _ net.Conn) { handled <- s })
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

func TestLifecycleChecksPeerBeforeReading(t *testing.T) {
	host, peer := net.Pipe()
	checked, done := make(chan struct{}), make(chan struct{})
	go func() {
		serveLifecycle(host, func(net.Conn) (uint32, error) { return 7, nil }, func(uint32) bool { close(checked); return true }, nil, func(string, net.Conn) {})
		close(done)
	}()
	select {
	case <-checked:
	case <-time.After(time.Second):
		t.Fatal("ownership lookup waited for the guest to send and close")
	}
	peer.Write([]byte("ready\n"))
	peer.Close()
	<-done
}

func TestLifecycleOpenURLNeedsTheSameUserAndNotQEMU(t *testing.T) {
	line := openURLLine("zoommtg:join")
	lookupFailed := errors.New("owner table unavailable")
	type peer struct {
		pid       uint32
		lookupErr error
		qemu      bool
		same      bool
		checkable bool
	}
	serve := func(input string, p peer) (string, bool) {
		host, client := net.Pipe()
		handled := make(chan string, 1)
		done := make(chan struct{})
		var same func(uint32) bool
		if p.checkable {
			same = func(pid uint32) bool {
				if pid != p.pid {
					t.Errorf("same-user check got PID %d, looked up %d", pid, p.pid)
				}
				return p.same
			}
		}
		lookups := 0
		go func() {
			serveLifecycle(host, func(net.Conn) (uint32, error) { lookups++; return p.pid, p.lookupErr },
				func(uint32) bool { return p.qemu }, same, func(s string, _ net.Conn) { handled <- s })
			close(done)
		}()
		client.Write([]byte(input))
		client.Close()
		<-done
		if lookups != 1 {
			t.Errorf("peer looked up %d times", lookups)
		}
		select {
		case s := <-handled:
			return s, true
		default:
			return "", false
		}
	}
	ok := peer{pid: 42, same: true, checkable: true}
	if got, handled := serve(line, ok); !handled || got+"\n" != line {
		t.Fatalf("same-user link refused: %q, %v", got, handled)
	}
	for _, refused := range []struct {
		input string
		peer  peer
	}{
		{line, peer{pid: 42, qemu: true, same: true, checkable: true}},              // the guest cannot open Windows-side links
		{line, peer{pid: 42, same: false, checkable: true}},                         // another user or session
		{line, peer{pid: 42, same: true}},                                           // no identity check available
		{line, peer{pid: 42, lookupErr: lookupFailed, same: true, checkable: true}}, // lookup failed
		{line, peer{pid: 0, same: true, checkable: true}},                           // no owner
		{strings.TrimSuffix(line, "\n"), ok},
		{"open-url !!!!\n", ok},
		{"open-url " + strings.Repeat("A", 4096) + "\n", ok},
		{"reclaim" + strings.Repeat(" ", 64) + "\n", ok},
	} {
		if got, handled := serve(refused.input, refused.peer); handled {
			t.Errorf("%.40q %+v handled as %q", refused.input, refused.peer, got)
		}
	}
	if _, handled := serve("ready\n", peer{pid: 42, lookupErr: lookupFailed, qemu: true}); handled {
		t.Fatal("ready accepted after a failed lookup")
	}
	if got, handled := serve("reclaim\n", peer{pid: 42}); !handled || got != "reclaim" {
		t.Fatalf("reclaim no longer accepted: %q, %v", got, handled)
	}
}
