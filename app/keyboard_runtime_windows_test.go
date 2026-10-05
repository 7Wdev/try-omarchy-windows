//go:build windows

package main

import (
	"os"
	"os/exec"
	"runtime"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// Opt in on the interactive Windows test desktop. The older observer hook
// consumes synthetic Win events so they cannot open the host Start menu.
func TestKeyboardNativeHookChain(t *testing.T) {
	tool := os.Getenv("TRYOMARCHY_QEMU_HOOK_TEST_BINARY")
	if tool == "" {
		t.Skip("requires an interactive Windows runtime test")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	oldPID, oldExternal := qemuPid.Load(), qemuExternalKeyboardHook.Load()
	oldLeft, oldRight := lwinKey, rwinKey
	defer func() {
		qemuPid.Store(oldPID)
		qemuExternalKeyboardHook.Store(oldExternal)
		lwinKey, rwinKey = oldLeft, oldRight
	}()
	lwinKey = routedKey{qcode: "meta_l"}
	rwinKey = routedKey{qcode: "meta_r"}
	seen := 0
	observer := syscall.NewCallback(func(code, wparam, lparam uintptr) uintptr {
		if int32(code) >= 0 {
			vk := *(*uint32)(unsafe.Pointer(lparam))
			if vk == vkLwin || vk == vkRwin {
				seen++
				return 1
			}
		}
		r, _, _ := procCallNextHookEx.Call(0, code, wparam, lparam)
		return r
	})
	spy, _, err := procSetWindowsHookExW.Call(whKeyboardLL, observer, 0, 0)
	if spy == 0 {
		t.Fatal(err)
	}
	defer procUnhookWindowsHookEx.Call(spy)
	pump := func() {
		var message msgStruct
		for {
			r, _, _ := procPeekMessageW.Call(uintptr(unsafe.Pointer(&message)), 0, 0, 0, pmRemove)
			if r == 0 {
				return
			}
			procTranslateMessage.Call(uintptr(unsafe.Pointer(&message)))
			procDispatchMessageW.Call(uintptr(unsafe.Pointer(&message)))
		}
	}
	cmd := exec.Command(tool, "-machine", "q35,accel=whpx", "-cpu", "host", "-m", "128", "-S", "-nodefaults", "-vga", "none", "-device", "virtio-gpu-pci", "-display", "sdl,gl=off", "-name", "Keyboard hook fixture")
	cmd.Env = keyboardEnvironment(os.Environ(), true)
	var detail diskToolErrors
	cmd.Stderr = &detail
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { cmd.Process.Kill(); cmd.Wait() }()
	var vmWindow uintptr
	find := syscall.NewCallback(func(hwnd, _ uintptr) uintptr {
		var pid uint32
		procGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&pid)))
		visible, _, _ := procIsWindowVisible.Call(hwnd)
		if pid == uint32(cmd.Process.Pid) && visible != 0 {
			vmWindow = hwnd
			return 0
		}
		return 1
	})
	deadline := time.Now().Add(15 * time.Second)
	for vmWindow == 0 && time.Now().Before(deadline) {
		pump()
		procEnumWindows.Call(find, 0)
		time.Sleep(20 * time.Millisecond)
	}
	if vmWindow == 0 {
		t.Fatalf("QEMU window missing: %s", detail.String())
	}
	procSetForegroundWindow.Call(vmWindow)
	for deadline := time.Now().Add(time.Second); time.Now().Before(deadline); {
		pump()
		time.Sleep(10 * time.Millisecond)
	}
	if foregroundPid() != uint32(cmd.Process.Pid) {
		t.Fatal("QEMU did not gain focus for its SDL grab")
	}
	class, _ := syscall.UTF16PtrFromString("STATIC")
	title, _ := syscall.UTF16PtrFromString("Keyboard hook host fixture")
	host, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)), 0x10CF0000, 10, 10, 240, 80, 0, 0, 0, 0)
	if host == 0 {
		t.Fatal(err)
	}
	defer procDestroyWindow.Call(host)
	procSetForegroundWindow.Call(host)
	qemuPid.Store(uint32(cmd.Process.Pid))
	qemuExternalKeyboardHook.Store(true)
	for deadline := time.Now().Add(time.Second); time.Now().Before(deadline); {
		pump()
		time.Sleep(10 * time.Millisecond)
	}
	if foregroundPid() == qemuPid.Load() {
		t.Fatal("host fixture did not take focus")
	}
	launcher, _, err := procSetWindowsHookExW.Call(whKeyboardLL, syscall.NewCallback(hookCallback), 0, 0)
	if launcher == 0 {
		t.Fatal(err)
	}
	defer procUnhookWindowsHookEx.Call(launcher)
	inject := user32.NewProc("keybd_event")
	for _, vk := range []uintptr{vkLwin, vkRwin} {
		inject.Call(vk, vk, 1, 0)
		pump()
		inject.Call(vk, vk, 3, 0)
		pump()
	}
	for deadline := time.Now().Add(time.Second); seen < 4 && time.Now().Before(deadline); {
		pump()
		time.Sleep(10 * time.Millisecond)
	}
	if seen != 4 {
		t.Fatalf("older hook saw %d Win transitions, want 4", seen)
	}
	t.Log("older hook received both Win keys after QEMU grab and host focus transfer")
}
