//go:build windows

package main

import (
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestNativeSettingsResourceProfiles(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires interactive Windows and candidate executable")
	}
	dir := t.TempDir()
	if err := saveSettings(settingsPath(dir), settings{CPUs: 1, MemoryMiB: 1024}); err != nil {
		t.Fatal(err)
	}
	if err := saveResourcePreferences(dir, resourceManual); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(launcher, "-dir", dir, "-settings")
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	defer func() { _ = cmd.Process.Kill() }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
	var window, profile uintptr
	for deadline := time.Now().Add(15 * time.Second); time.Now().Before(deadline); {
		window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
		var owner uint32
		procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
		if owner == uint32(cmd.Process.Pid) {
			profile = findSettingsControl(window, settingsResourceProfileID)
			if profile != 0 {
				break
			}
		}
		time.Sleep(25 * time.Millisecond)
	}
	if profile == 0 {
		t.Fatal("resource profile control did not appear")
	}
	for _, index := range []uintptr{0, 1, 2} {
		procSendMessageW.Call(profile, 0x14e, index, 0) // CB_SETCURSEL
		procSendMessageW.Call(window, wmCommand, settingsResourceProfileID|1<<16, profile)
		for _, id := range []uintptr{settingsMemID, settingsCPUsID} {
			control := findSettingsControl(window, id)
			visible, _, _ := user32.NewProc("IsWindowVisible").Call(control)
			if (visible != 0) != (index == 2) {
				t.Fatalf("profile %d: tuning control %d visibility %d", index, id, visible)
			}
		}
	}
	procSendMessageW.Call(window, wmCommand, settingsSaveID, 0)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("Save did not close Settings")
	}
	saved, err := loadSettings(settingsPath(dir))
	if err != nil || saved.CPUs != 1 || saved.MemoryMiB != 1024 {
		t.Fatalf("Manual values changed after switching profiles: %#v, %v", saved, err)
	}
}
