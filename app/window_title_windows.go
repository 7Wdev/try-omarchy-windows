//go:build windows

package main

import (
	"sync"
	"syscall"
	"unsafe"
)

// QEMU's SDL frontend rewrites its window title whenever its keyboard grab or
// run state changes ("QEMU (Try Omarchy-0) - Press Ctrl-Alt-G to exit grab",
// "... [Stopped]"), and the grab changes on every focus change. The title
// enforcer only looks once a second, so that text sat in the title bar for up
// to a second at a time. This WinEvent hook puts the right title back as soon
// as QEMU changes it, and records which display each renamed window shows so
// the enforcer still recognises a window it never saw under QEMU's title.

var (
	procSetWinEventHook = user32.NewProc("SetWinEventHook")
	retitledDisplays    sync.Map // window handle -> guest display index
)

const (
	eventObjectNameChange  = 0x800C
	wineventOutOfContext   = 0x0000
	wineventSkipOwnProcess = 0x0002
	objidWindow            = 0
)

func titleEventCallback(_, _, hwnd, idObject, _, _, _ uintptr) uintptr {
	if hwnd != 0 && int32(idObject) == objidWindow && isQemuDisplayWindow(hwnd, qemuPid.Load()) {
		restoreDisplayTitle(hwnd)
	}
	return 0
}

// restoreDisplayTitle renames a QEMU display window that is showing QEMU's own title.
func restoreDisplayTitle(hwnd uintptr) {
	var buf [maxTitle]uint16
	procGetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), maxTitle)
	index, ok := displayIndexFromTitle(syscall.UTF16ToString(buf[:]))
	if !ok {
		return
	}
	retitledDisplays.Store(hwnd, index)
	value, _ := syscall.UTF16PtrFromString(displayWindowTitle(index))
	procSetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(value)))
}

// recordedDisplayIndex is the display a window showed when the hook renamed it.
func recordedDisplayIndex(hwnd uintptr) (int, bool) {
	value, ok := retitledDisplays.Load(hwnd)
	if !ok {
		return 0, false
	}
	return value.(int), true
}

// installTitleHook must run on a thread that pumps messages: out-of-context
// WinEvent callbacks are delivered while it retrieves them.
func installTitleHook() {
	callback := syscall.NewCallback(titleEventCallback)
	hook, _, _ := procSetWinEventHook.Call(eventObjectNameChange, eventObjectNameChange, 0, callback, 0, 0,
		wineventOutOfContext|wineventSkipOwnProcess)
	if hook == 0 {
		logf("title: WinEvent hook failed - QEMU's own title can show for up to a second")
	}
}
