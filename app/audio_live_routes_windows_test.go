//go:build windows

package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

func TestAudioRouteRenameRetriesWindowsLockErrors(t *testing.T) {
	for _, errno := range []syscall.Errno{32, 33, 5, 2} {
		calls, sleeps := 0, 0
		err := renameAudioRoute("from", "to", func(string, string) error {
			calls++
			if calls == 3 {
				return nil
			}
			return &os.LinkError{Op: "rename", Err: errno}
		}, func(delay time.Duration) {
			sleeps++
			if delay != 20*time.Millisecond {
				t.Fatalf("delay=%v", delay)
			}
		})
		if errno == 2 {
			if !errors.Is(err, errno) || calls != 1 || sleeps != 0 {
				t.Fatalf("permanent error: %v calls=%d sleeps=%d", err, calls, sleeps)
			}
		} else if err != nil || calls != 3 || sleeps != 2 {
			t.Fatalf("sharing error: %v calls=%d sleeps=%d", err, calls, sleeps)
		}
	}
	calls := 0
	err := renameAudioRoute("from", "to", func(string, string) error { calls++; return syscall.Errno(32) }, func(time.Duration) {})
	if !errors.Is(err, syscall.Errno(32)) || calls != 10 {
		t.Fatalf("unbounded retry: %v calls=%d", err, calls)
	}
}

func TestAudioRoutePublishWhileTargetIsOpen(t *testing.T) {
	dir := t.TempDir()
	if err := writeAudioRoute(dir, "output", "old"); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "output")
	wide, _ := syscall.UTF16PtrFromString(path)
	// Permit reads/writes but deliberately omit FILE_SHARE_DELETE, as a route
	// reader can do on Windows. Replacing this file must initially fail.
	h, err := syscall.CreateFile(wide, syscall.GENERIC_READ, syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	var closeOnce sync.Once
	closeHandle := func() { closeOnce.Do(func() { syscall.CloseHandle(h) }) }
	defer closeHandle()
	if err := writeAudioRoute(dir, "output", "blocked"); !retryableWindowsRenameError(err) {
		t.Fatalf("locked destination: %v", err)
	}
	entries, _ := os.ReadDir(dir)
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".audio-route-") {
			t.Fatal("temporary route leaked")
		}
	}
	go func() { time.Sleep(50 * time.Millisecond); closeHandle() }()
	if err := writeAudioRoute(dir, "output", "new"); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil || string(data) != "bmV3\n" {
		t.Fatalf("published route: %q %v", data, err)
	}
}
