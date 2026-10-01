//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

// Starting Try Omarchy again while it runs (the Start menu shortcut, a second
// double-click) brings the running window to the front. It used to stop with
// "Try Omarchy looks like it's already running (port 4450 is in use)".

var (
	procIsIconic            = user32.NewProc("IsIconic")
	runningInstanceWindow   uintptr
	runningInstanceCallback = syscall.NewCallback(runningInstanceProc)
)

func runningInstanceProc(hwnd, _ uintptr) uintptr {
	if visible, _, _ := procIsWindowVisible.Call(hwnd); visible == 0 {
		return 1
	}
	var class [64]uint16
	procGetClassNameW.Call(hwnd, uintptr(unsafe.Pointer(&class[0])), uintptr(len(class)))
	var title [maxTitle]uint16
	procGetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(&title[0])), maxTitle)
	name := syscall.UTF16ToString(class[:])
	if !isRunningInstanceWindow(name, syscall.UTF16ToString(title[:])) {
		return 1
	}
	runningInstanceWindow = hwnd
	if name == "SDL_app" {
		return 0 // the VM window wins over the setup window
	}
	return 1
}

// activateRunningInstance brings a running Try Omarchy to the front and
// reports whether it found one.
func activateRunningInstance() bool {
	runningInstanceWindow = 0
	procEnumWindows.Call(runningInstanceCallback, 0)
	hwnd := runningInstanceWindow
	if hwnd == 0 {
		return false
	}
	if iconic, _, _ := procIsIconic.Call(hwnd); iconic != 0 {
		procShowWindow.Call(hwnd, swRestore)
	}
	procSetForegroundWindow.Call(hwnd)
	return true
}
