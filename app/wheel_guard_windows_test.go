//go:build windows

package main

import (
	"runtime"
	"syscall"
	"testing"
	"unsafe"
)

func TestWheelGuardKeepsClosedDropdownSelection(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	instance, _, _ := procGetModuleHandleW.Call(0)
	create := func(class string, style, parent uintptr) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		h, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(c)), 0, style, 0, 0, 200, 120, parent, 0, instance, 0)
		if h == 0 {
			t.Fatalf("CreateWindowExW(%s) failed", class)
		}
		return h
	}
	parent := create("STATIC", 0, 0)
	defer procDestroyWindow.Call(parent)
	for _, guarded := range []bool{false, true} {
		combo := create("COMBOBOX", wsChild|wsVisible|0x0003|wsVscroll, parent)
		for _, item := range []string{"a", "b", "c"} {
			text, _ := syscall.UTF16PtrFromString(item)
			procSendMessageW.Call(combo, 0x143, 0, uintptr(unsafe.Pointer(text)))
		}
		procSendMessageW.Call(combo, 0x14e, 1, 0)
		if guarded {
			guardWheel(combo, "COMBOBOX", 0x0003|wsVscroll)
		}
		// One notch toward the user (negative delta).
		delta := int16(-120)
		procSendMessageW.Call(combo, wmMousewheel, uintptr(uint16(delta))<<16, 0)
		index, _, _ := procSendMessageW.Call(combo, 0x147, 0, 0)
		if guarded && index != 1 {
			t.Fatalf("guarded dropdown changed selection to %d on wheel", index)
		}
		t.Logf("guarded=%v selection after wheel=%d", guarded, index)
		procDestroyWindow.Call(combo)
	}
}
