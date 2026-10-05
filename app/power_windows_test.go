//go:build windows

package main

import (
	"context"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestPowerStartupAndShutdown(t *testing.T) {
	// The production dial must not contact QMP in the launch quiet period.
	oldUp, oldPID := guestUp.Load(), qemuPid.Load()
	t.Cleanup(func() { guestUp.Store(oldUp); qemuPid.Store(oldPID) })
	guestUp.Store(false)
	qemuPid.Store(123)
	p := newGuestPowerState()
	p.handle(pbtApmSuspend)
	guestCompositorHealth.mu.Lock()
	suspended := guestCompositorHealth.suspended
	guestCompositorHealth.mu.Unlock()
	if !suspended {
		t.Fatal("compositor health did not observe host suspension")
	}
	p.handle(pbtApmResumeAutomatic)
	guestCompositorHealth.mu.Lock()
	suspended = guestCompositorHealth.suspended
	grace := guestCompositorHealth.grace
	guestCompositorHealth.mu.Unlock()
	if suspended || !grace.After(time.Now()) {
		t.Fatal("compositor health did not receive resume grace")
	}
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
	socket := filepath.Join(dir, "tools.sock")
	powerSocket := filepath.Join(dir, "power.sock")
	previousDirectory := qmpControlDirectory
	qmpControlDirectory = func() (string, error) { return dir, nil }
	defer func() { qmpControlDirectory = previousDirectory }()
	oldUp, oldPID := guestUp.Load(), qemuPid.Load()
	defer func() { guestUp.Store(oldUp); qemuPid.Store(oldPID) }()
	guestUp.Store(true)
	qemuPid.Store(123)
	cmd := exec.Command(qemu, "-machine", "none", "-nodefaults", "-display", "none", "-S",
		"-qmp", "unix:"+qemuOptionValue(socket)+",server=on,wait=off",
		"-qmp", "unix:"+qemuOptionValue(powerSocket)+",server=on,wait=off")
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
	p := newGuestPowerState()
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
		// Other controls must remain usable while the power monitor is held.
		status("", "paused")
		var state vmRuntimeStatus
		if err := p.client.Call(context.Background(), "query-status", nil, &state); err != nil || state.Status != "paused" {
			t.Fatalf("pause=%+v %v", state, err)
		}
		p.handle(pbtApmResumeAutomatic)
		p.handle(pbtApmResumeSuspend)
		status("", "running")
	}
	// Actual manual changes on the tools monitor relinquish ownership.
	p.handle(pbtApmSuspend)
	status("cont", "running")
	status("stop", "paused")
	p.handle(pbtApmResumeAutomatic)
	status("", "paused")
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
