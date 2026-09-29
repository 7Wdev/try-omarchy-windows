//go:build windows

package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"runtime"
	"sync"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

type powerPeer struct {
	mu       sync.Mutex
	state    string
	commands []string
	fail     string
	lost     bool
	events   []string
}

func (s *powerPeer) client(t *testing.T) *qmpClient {
	return qmpTestPeer(t, func(conn net.Conn, reader *bufio.Reader) {
		for {
			line, err := reader.ReadBytes('\n')
			if err != nil {
				return
			}
			var request struct{ Execute, ID string }
			if json.Unmarshal(line, &request) != nil {
				return
			}
			s.mu.Lock()
			s.commands = append(s.commands, request.Execute)
			events := s.events
			s.events = nil
			fail := s.fail == request.Execute
			lost := fail && s.lost
			if lost && request.Execute == "stop" {
				s.state = "paused"
			}
			if !fail {
				if request.Execute == "stop" {
					s.state = "paused"
					events = append(events, "STOP")
				}
				if request.Execute == "cont" {
					s.state = "running"
					events = append(events, "RESUME")
				}
			}
			state := s.state
			s.mu.Unlock()
			if lost {
				return
			}
			for _, event := range events {
				fmt.Fprintf(conn, "{\"event\":%q}\n", event)
			}
			if fail {
				fmt.Fprintf(conn, "{\"error\":{\"class\":\"GenericError\",\"desc\":\"injected failure\"},\"id\":%q}\n", request.ID)
			} else if request.Execute == "query-status" {
				fmt.Fprintf(conn, "{\"return\":{\"running\":%t,\"status\":%q},\"id\":%q}\n", state == "running", state, request.ID)
			} else {
				fmt.Fprintf(conn, "{\"return\":{},\"id\":%q}\n", request.ID)
			}
		}
	})
}

func (s *powerPeer) assert(t *testing.T, state string, operations ...string) {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	var actual []string
	for _, command := range s.commands {
		if command != "query-status" {
			actual = append(actual, command)
		}
	}
	if s.state != state || !reflect.DeepEqual(actual, operations) {
		t.Fatalf("state=%s operations=%v; want %s %v", s.state, actual, state, operations)
	}
}

func powerFixture(t *testing.T, peer *powerPeer) *guestPowerState {
	p := &guestPowerState{dial: func(context.Context) (*qmpClient, error) { return peer.client(t), nil }}
	t.Cleanup(p.close)
	return p
}

func TestPowerRepeatedTransitions(t *testing.T) {
	peer := &powerPeer{state: "running"}
	p := powerFixture(t, peer)
	p.handle(pbtApmResumeAutomatic)
	p.handle(pbtApmResumeSuspend)
	peer.assert(t, "running")
	for i := 0; i < 3; i++ {
		p.handle(pbtApmSuspend)
		p.handle(pbtApmSuspend)
		peer.assert(t, "paused", repeatPowerOps(i, true)...)
		p.handle(pbtApmResumeAutomatic)
		p.handle(pbtApmResumeSuspend)
		peer.assert(t, "running", repeatPowerOps(i+1, false)...)
	}
}

func repeatPowerOps(cycles int, paused bool) []string {
	var ops []string
	for i := 0; i < cycles; i++ {
		ops = append(ops, "stop", "cont")
	}
	if paused {
		ops = append(ops, "stop")
	}
	return ops
}

func TestPowerPreservesNonRunningGuests(t *testing.T) {
	for _, state := range []string{"paused", "prelaunch", "inmigrate", "shutdown", "suspended", "internal-error"} {
		t.Run(state, func(t *testing.T) {
			peer := &powerPeer{state: state}
			p := powerFixture(t, peer)
			p.handle(pbtApmSuspend)
			p.handle(pbtApmResumeAutomatic)
			p.handle(pbtApmResumeSuspend)
			peer.assert(t, state)
		})
	}
}

func TestPowerManualChangesWhileAsleep(t *testing.T) {
	for _, state := range []string{"running", "paused"} {
		t.Run(state, func(t *testing.T) {
			peer := &powerPeer{state: "running"}
			p := powerFixture(t, peer)
			p.handle(pbtApmSuspend)
			peer.mu.Lock()
			peer.state = state
			peer.events = []string{"RESUME"}
			if state == "paused" {
				peer.events = append(peer.events, "STOP")
			}
			peer.mu.Unlock()
			p.handle(pbtApmResumeAutomatic)
			p.handle(pbtApmResumeSuspend)
			peer.assert(t, state, "stop")
		})
	}
}

func TestPowerCommandFailures(t *testing.T) {
	for _, tc := range []struct {
		name, command string
		lost          bool
	}{
		{"query rejected", "query-status", false}, {"stop rejected", "stop", false},
		{"query disconnected", "query-status", true}, {"lost stop reply", "stop", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			peer := &powerPeer{state: "running", fail: tc.command, lost: tc.lost}
			p := powerFixture(t, peer)
			p.handle(pbtApmSuspend)
			p.handle(pbtApmResumeAutomatic)
			p.handle(pbtApmResumeSuspend)
			state := "running"
			if tc.command == "stop" && tc.lost {
				state = "paused"
			}
			if tc.command == "stop" {
				peer.assert(t, state, "stop")
			} else {
				peer.assert(t, state)
			}
			if p.client != nil || p.owned {
				t.Fatal("retained failed ownership")
			}
		})
	}
}

func TestPowerResumeFailureAndRetry(t *testing.T) {
	for _, lost := range []bool{false, true} {
		t.Run(fmt.Sprint(lost), func(t *testing.T) {
			peer := &powerPeer{state: "running"}
			p := powerFixture(t, peer)
			p.handle(pbtApmSuspend)
			peer.mu.Lock()
			peer.fail = "cont"
			peer.lost = lost
			peer.mu.Unlock()
			p.handle(pbtApmResumeAutomatic)
			peer.assert(t, "paused", "stop", "cont")
			peer.mu.Lock()
			peer.fail = ""
			peer.mu.Unlock()
			p.handle(pbtApmResumeSuspend)
			if lost {
				peer.assert(t, "paused", "stop", "cont")
			} else {
				peer.assert(t, "running", "stop", "cont", "cont")
			}
		})
	}
}

func TestPowerRuntimeExitAndReplacement(t *testing.T) {
	peer := &powerPeer{state: "running"}
	p := powerFixture(t, peer)
	p.handle(pbtApmSuspend)
	p.client.Close()
	replacement := &powerPeer{state: "paused"}
	p.dial = func(context.Context) (*qmpClient, error) { return replacement.client(t), nil }
	p.handle(pbtApmResumeAutomatic)
	p.handle(pbtApmResumeSuspend)
	replacement.assert(t, "paused")
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	replacement.assert(t, "paused")
}

func TestPowerStartupAndShutdown(t *testing.T) {
	// The production dial must not contact QMP in the launch quiet period.
	oldUp, oldPID := guestUp.Load(), qemuPid.Load()
	t.Cleanup(func() { guestUp.Store(oldUp); qemuPid.Store(oldPID) })
	guestUp.Store(false)
	qemuPid.Store(123)
	p := newGuestPowerState()
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	if p.client != nil || p.owned {
		t.Fatal("contacted early-boot guest")
	}
	peer := &powerPeer{state: "running"}
	p = powerFixture(t, peer)
	p.handle(pbtApmSuspend)
	p.close()
	p.handle(pbtApmResumeAutomatic)
	peer.assert(t, "paused", "stop")
}

func TestPowerUnavailableControls(t *testing.T) {
	p := &guestPowerState{dial: func(context.Context) (*qmpClient, error) { return nil, errors.New("unavailable") }}
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	if p.owned || p.client != nil {
		t.Fatal("claimed unavailable guest")
	}
}

// This is a diskless, displayless Windows QEMU fixture. It proves actual QMP
// run states and event ordering, not host suspend or graphical acceptance.
func TestPowerWindowsQEMURuntime(t *testing.T) {
	qemu := os.Getenv("QEMU_SYSTEM")
	if qemu == "" {
		t.Skip("set QEMU_SYSTEM for isolated Windows runtime evidence")
	}
	dir, err := os.MkdirTemp(os.TempDir(), "tom-power-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(dir)
	socket := filepath.Join(dir, "power.sock")
	cmd := exec.Command(qemu, "-machine", "none", "-nodefaults", "-display", "none", "-S", "-qmp", "unix:"+qemuOptionValue(socket)+",server=on,wait=off")
	configureDiskTool(cmd)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { cmd.Process.Kill(); cmd.Wait() }()
	dial := func(ctx context.Context) (*qmpClient, error) {
		var conn net.Conn
		var err error
		for ctx.Err() == nil {
			conn, err = (&net.Dialer{}).DialContext(ctx, "unix", socket)
			if err == nil {
				break
			}
			time.Sleep(20 * time.Millisecond)
		}
		if err != nil {
			return nil, err
		}
		return newQMPClient(ctx, conn)
	}
	status := func(command, want string) {
		t.Helper()
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		c, err := dial(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer c.Close()
		if command != "" {
			if err := c.Call(ctx, command, nil, nil); err != nil {
				t.Fatal(err)
			}
		}
		var state vmRuntimeStatus
		if err := c.Call(ctx, "query-status", nil, &state); err != nil || state.Status != want {
			t.Fatalf("status=%+v error=%v want=%s", state, err, want)
		}
	}
	p := &guestPowerState{dial: dial}
	defer p.close()
	status("", "prelaunch")
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	status("", "prelaunch")
	status("cont", "running")
	for i := 0; i < 3; i++ {
		p.handle(pbtApmSuspend)
		p.handle(pbtApmSuspend)
		if !p.owned {
			t.Fatal("actual runtime pause not owned")
		}
		var state vmRuntimeStatus
		if err := p.client.Call(context.Background(), "query-status", nil, &state); err != nil || state.Status != "paused" {
			t.Fatalf("pause=%+v %v", state, err)
		}
		p.handle(pbtApmResumeAutomatic)
		p.handle(pbtApmResumeSuspend)
		status("", "running")
	}
	status("stop", "paused")
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	status("", "paused")
}

func TestPowerNotificationLifecycle(t *testing.T) {
	for _, failed := range []bool{false, true} {
		t.Run(fmt.Sprint(failed), func(t *testing.T) {
			calls := 0
			cleanup, err := subscribePowerNotifications(77, func(hwnd uintptr) (uintptr, error) {
				if hwnd != 77 {
					t.Fatalf("receiver %d", hwnd)
				}
				if failed {
					return 0, errors.New("registration rejected")
				}
				return 99, nil
			}, func(handle uintptr) error {
				calls++
				if handle != 99 {
					t.Fatalf("registration handle %d", handle)
				}
				return errors.New("cleanup rejected")
			})
			if (err != nil) != failed {
				t.Fatalf("registration error %v", err)
			}
			cleanup()
			cleanup()
			want := 1
			if failed {
				want = 0
			}
			if calls != want {
				t.Fatalf("cleanup calls %d want %d", calls, want)
			}
		})
	}
}

func TestPowerWindowsNotificationAPI(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	class, _ := syscall.UTF16PtrFromString("STATIC")
	hwnd, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
	if hwnd == 0 {
		t.Fatalf("create hidden receiver: %v", err)
	}
	defer procDestroyWindow.Call(hwnd)
	cleanup, err := registerPowerNotifications(hwnd)
	if err != nil {
		t.Fatal(err)
	}
	cleanup()
	cleanup()
}
