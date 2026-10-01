//go:build windows

package main

import (
	"sync"
	"syscall"
	"unsafe"
)

// The launcher follows QEMU's display windows through WinEvents on the shared
// hook thread:
//
//   - Title changes. QEMU's SDL frontend rewrites the title whenever its
//     keyboard grab or run state changes ("QEMU (Try Omarchy-0) - Press
//     Ctrl-Alt-G to exit grab", "... [Stopped]"), and the grab changes on every
//     focus change. The right title goes back at once, and the display each
//     renamed window shows is recorded so the title enforcer still recognises
//     a window it never saw under QEMU's title.
//   - Shown, moved or resized. These wake the title enforcer, so a new window
//     is placed right away instead of up to a second later, and a resize QEMU
//     makes while the guest boots or shuts down is undone before it lingers.
//   - The start and end of a move or resize the user makes with the mouse or
//     the window menu. Those, and maximizing, are the only placements the
//     enforcer remembers.

var (
	procSetWinEventHook = user32.NewProc("SetWinEventHook")
	retitledDisplays    sync.Map // window handle -> guest display index
	draggedWindows      sync.Map // window handle -> the user is moving or resizing it
	userMovedWindows    sync.Map // window handle -> the user moved or resized it
	qemuWindowChanged   = make(chan struct{}, 1)
)

const (
	eventSystemMoveSizeStart  = 0x000A
	eventSystemMoveSizeEnd    = 0x000B
	eventObjectShow           = 0x8002
	eventObjectLocationChange = 0x800B
	eventObjectNameChange     = 0x800C
	wineventOutOfContext      = 0x0000
	wineventSkipOwnProcess    = 0x0002
	objidWindow               = 0
)

func qemuWindowEvent(_, event, hwnd, idObject, _, _, _ uintptr) uintptr {
	if hwnd == 0 || int32(idObject) != objidWindow || !isQemuDisplayWindow(hwnd, qemuPid.Load()) {
		return 0
	}
	switch event {
	case eventObjectNameChange:
		restoreDisplayTitle(hwnd)
	case eventSystemMoveSizeStart:
		draggedWindows.Store(hwnd, true)
	case eventSystemMoveSizeEnd:
		draggedWindows.Delete(hwnd)
		userMovedWindows.Store(hwnd, true)
	}
	select {
	case qemuWindowChanged <- struct{}{}:
	default:
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

// takeUserMoved reports, once, that the user moved or resized the window.
func takeUserMoved(hwnd uintptr) bool {
	_, moved := userMovedWindows.LoadAndDelete(hwnd)
	return moved
}

func beingDragged(hwnd uintptr) bool {
	_, dragging := draggedWindows.Load(hwnd)
	return dragging
}

// installQemuWindowHooks must run on a thread that pumps messages:
// out-of-context WinEvent callbacks are delivered while it retrieves them.
func installQemuWindowHooks() {
	callback := syscall.NewCallback(qemuWindowEvent)
	for _, events := range [][2]uintptr{
		{eventSystemMoveSizeStart, eventSystemMoveSizeEnd},
		{eventObjectShow, eventObjectShow},
		{eventObjectLocationChange, eventObjectNameChange},
	} {
		hook, _, _ := procSetWinEventHook.Call(events[0], events[1], 0, callback, 0, 0,
			wineventOutOfContext|wineventSkipOwnProcess)
		if hook == 0 {
			logf("window events: hook for events %#x-%#x failed; titles and placement are checked once a second", events[0], events[1])
		}
	}
}
