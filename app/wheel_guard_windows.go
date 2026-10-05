//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

const (
	wmMousewheel      = 0x020A
	cbGetDroppedState = 0x0157
	wheelGuardID      = 0x7767
)

// One callback for every guarded control: syscall.NewCallback slots are never freed.
var (
	wheelGuardComboCallback  = syscall.NewCallback(wheelGuardComboProc)
	wheelGuardScrollCallback = syscall.NewCallback(wheelGuardScrollProc)
	procDefSubclassProc      = comctl32.NewProc("DefSubclassProc")
)

// A closed drop-down list changes its selection on WM_MOUSEWHEEL, so a page
// scroll over it saved a wrong value. While closed, the wheel goes to the
// parent and the selection stays. An open list keeps the default behavior.
func wheelGuardComboProc(hwnd, message, w, l, id, data uintptr) uintptr {
	if message == wmMousewheel {
		if dropped, _, _ := procSendMessageW.Call(hwnd, cbGetDroppedState, 0, 0); dropped == 0 {
			wheelGuardForward(hwnd, w, l)
			return 0
		}
	}
	result, _, _ := procDefSubclassProc.Call(hwnd, message, w, l)
	return result
}

// A list box or multi-line edit that scrolls keeps the wheel. At the top or
// bottom, or with nothing to scroll, the wheel goes to the parent page.
func wheelGuardScrollProc(hwnd, message, w, l, id, data uintptr) uintptr {
	if message == wmMousewheel && !wheelGuardCanScroll(hwnd, int16(w>>16)) {
		wheelGuardForward(hwnd, w, l)
		return 0
	}
	result, _, _ := procDefSubclassProc.Call(hwnd, message, w, l)
	return result
}

func wheelGuardForward(hwnd, w, l uintptr) {
	if parent, _, _ := user32.NewProc("GetParent").Call(hwnd); parent != 0 {
		procSendMessageW.Call(parent, wmMousewheel, w, l)
	}
}

func wheelGuardCanScroll(hwnd uintptr, delta int16) bool {
	info := settingsScrollInfo{mask: 0x17} // SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS
	info.size = uint32(unsafe.Sizeof(info))
	if ok, _, _ := procGetScrollInfo.Call(hwnd, 1, uintptr(unsafe.Pointer(&info))); ok == 0 {
		return false
	}
	if int64(info.max-info.min)+1 <= int64(info.page) {
		return false
	}
	if delta > 0 {
		return info.pos > info.min
	}
	return int64(info.pos)+int64(info.page) <= int64(info.max)
}

// guardWheel subclasses a control created by the Settings or LAN dialog.
func guardWheel(handle uintptr, class string, style uintptr) {
	if handle == 0 {
		return
	}
	switch class {
	case "COMBOBOX":
		comctl32.NewProc("SetWindowSubclass").Call(handle, wheelGuardComboCallback, wheelGuardID, 0)
	case "LISTBOX", "EDIT":
		if style&wsVscroll != 0 {
			comctl32.NewProc("SetWindowSubclass").Call(handle, wheelGuardScrollCallback, wheelGuardID, 0)
		}
	}
}
