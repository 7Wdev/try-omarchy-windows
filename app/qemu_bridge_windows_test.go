//go:build windows

package main

import (
	"net"
	"os"
	"testing"
	"time"
)

func TestAcceptQEMUBridgeServesOnlyQEMUWithinTheLimit(t *testing.T) {
	previous := qemuPid.Load()
	defer qemuPid.Store(previous)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	served := make(chan net.Conn, 4)
	release := make(chan struct{})
	done := make(chan struct{})
	go func() {
		defer close(done)
		acceptQEMUBridge(listener, "test bridge", 1, func(conn net.Conn) {
			served <- conn
			<-release
		})
	}()
	defer func() {
		close(release)
		listener.Close()
		<-done
	}()
	dial := func() net.Conn {
		t.Helper()
		conn, err := net.Dial("tcp", listener.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { conn.Close() })
		return conn
	}
	closedByBridge := func(conn net.Conn) bool {
		conn.SetReadDeadline(time.Now().Add(5 * time.Second))
		_, err := conn.Read(make([]byte, 1))
		return err != nil && !isTimeout(err)
	}

	// The test process is the peer, so it stands in for QEMU only when its
	// PID is the recorded one.
	qemuPid.Store(uint32(os.Getpid()) + 1)
	if stranger := dial(); !closedByBridge(stranger) {
		t.Fatal("a connection from another process was not refused")
	}
	select {
	case <-served:
		t.Fatal("a connection from another process was served")
	default:
	}

	qemuPid.Store(uint32(os.Getpid()))
	dial()
	select {
	case <-served:
	case <-time.After(5 * time.Second):
		t.Fatal("QEMU's connection was not served")
	}
	if extra := dial(); !closedByBridge(extra) {
		t.Fatal("a connection over the limit was not refused")
	}
}

func isTimeout(err error) bool {
	ne, ok := err.(net.Error)
	return ok && ne.Timeout()
}

func TestLifecycleConnectionWaitsForPublishedQEMUPID(t *testing.T) {
	previous := qemuPid.Load()
	defer qemuPid.Store(previous)
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	peer, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer peer.Close()
	conn, err := l.Accept()
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	qemuPid.Store(0)
	result := make(chan bool, 1)
	go func() { result <- lifecycleConnectionFromQEMU(conn) }()
	time.Sleep(20 * time.Millisecond)
	qemuPid.Store(uint32(os.Getpid()))
	if !<-result {
		t.Fatal("early QEMU connection refused")
	}
	qemuPid.Store(uint32(os.Getpid()) + 1)
	if lifecycleConnectionFromQEMU(conn) {
		t.Fatal("foreign connection accepted")
	}
}
