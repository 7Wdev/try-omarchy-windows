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

// Exercise actual native control bounds across pages, scrolling and resizing.
// Settings uses a disposable directory and never starts a guest.
func TestNativeSettingsActionsStayVisible(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires interactive Windows and TRYOMARCHY_LAUNCHER_TEST_EXE")
	}
	cmd := exec.Command(launcher, "-dir", t.TempDir(), "-settings")
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = cmd.Process.Kill(); _ = cmd.Wait() }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
	var window uintptr
	for deadline := time.Now().Add(15 * time.Second); time.Now().Before(deadline); {
		window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
		var owner uint32
		if window != 0 {
			procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
		}
		if owner == uint32(cmd.Process.Pid) {
			button := findSettingsControl(window, settingsCancelID)
			if button != 0 {
				break
			}
		}
		window = 0
		time.Sleep(25 * time.Millisecond)
	}
	if window == 0 {
		t.Fatal("Settings did not create its action controls")
	}
	for _, size := range [][2]int32{{520, 420}, {700, 650}, {540, 500}} {
		procSetWindowPos.Call(window, 0, 0, 0, uintptr(size[0]), uintptr(size[1]), 0x0004|0x0002|0x0010)
		for page := 0; page < 5; page++ {
			procSendMessageW.Call(window, wmCommand, settingsPageBase+uintptr(page), 0)
			for _, position := range []uintptr{6, 7} { // SB_TOP and SB_BOTTOM
				procSendMessageW.Call(window, 0x0115, position, 0)
				var client [4]int32
				procGetClientRect.Call(window, uintptr(unsafe.Pointer(&client)))

				viewport := findSettingsControl(window, settingsViewportID)
				var viewportBounds [4]int32
				procGetWindowRect.Call(viewport, uintptr(unsafe.Pointer(&viewportBounds)))
				procScreenToClient.Call(window, uintptr(unsafe.Pointer(&viewportBounds[0])))
				procScreenToClient.Call(window, uintptr(unsafe.Pointer(&viewportBounds[2])))
				for _, c := range []uintptr{settingsFullID, settingsCameraOnID, settingsDisplaysID, settingsAppListID} {
					control := findSettingsControl(window, c)
					parent, _, _ := user32.NewProc("GetParent").Call(control)
					if parent != viewport {
						t.Fatalf("page control %d is outside native clipping viewport", c)
					}
				}
				for _, id := range []uintptr{settingsHelpID, settingsSaveID, settingsCancelID} {
					control := findSettingsControl(window, id)
					var bounds [4]int32
					procGetWindowRect.Call(control, uintptr(unsafe.Pointer(&bounds)))
					procScreenToClient.Call(window, uintptr(unsafe.Pointer(&bounds[0])))
					procScreenToClient.Call(window, uintptr(unsafe.Pointer(&bounds[2])))
					if viewportBounds[3] > bounds[1] {
						t.Fatalf("viewport %v overlaps action %d at %v", viewportBounds, id, bounds)
					}
					if control == 0 || bounds[0] < 0 || bounds[1] < 0 || bounds[2] > client[2] || bounds[3] > client[3] {
						t.Fatalf("size %v page %d scroll %d: action %d bounds %v outside client %v", size, page, position, id, bounds, client)
					}
				}
			}
		}
	}
	procSendMessageW.Call(window, wmCommand, settingsCancelID, 0)
}
