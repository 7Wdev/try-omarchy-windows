//go:build windows

package main

import (
	"bytes"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestNativeInstallWalkthroughDuringActiveLifecycle(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires interactive Windows and candidate executable")
	}
	listener, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", lifecyclePort))
	if err != nil {
		t.Skip("requires an otherwise idle Try Omarchy lifecycle port")
	}
	defer listener.Close()
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "vm"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "vm", "disk.raw"), []byte("test disk"), 0600); err != nil {
		t.Fatal(err)
	}
	// An inspection must leave a running owner's recovery journal alone.
	journal := filepath.Join(dir, checkpointRollbackFile)
	pending := []byte("pending recovery belongs to the running launcher")
	if err := os.WriteFile(journal, pending, 0600); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(launcher, "-dir", dir, "-recovery", "install-omarchy", "-no-update")
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = cmd.Process.Kill(); _ = cmd.Wait() }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchyChoice")
	title, _ := syscall.UTF16PtrFromString("Install Omarchy on this PC")
	for deadline := time.Now().Add(10 * time.Second); time.Now().Before(deadline); {
		window, _, _ := user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)))
		var owner uint32
		if window != 0 {
			procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
		}
		if owner == uint32(cmd.Process.Pid) {
			got, err := os.ReadFile(journal)
			if err != nil || !bytes.Equal(got, pending) {
				t.Fatalf("walkthrough changed pending recovery: %q, %v", got, err)
			}
			return
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Fatal("install walkthrough did not open while the lifecycle port was occupied")
}
