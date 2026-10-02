//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

const (
	dtWordBreak = 0x0010
	dtCalcRect  = 0x0400
	dtNoPrefix  = 0x0800
	bsMultiline = 0x2000
)

// measureText returns the size of text drawn in a window's font. With a
// width, lines wrap at words to fit it, as a static control wraps them;
// without one, only the text's own line breaks start new lines.
func measureText(hwnd, font uintptr, text string, width int32) (int32, int32) {
	dc, _, _ := procGetDC.Call(hwnd)
	if dc == 0 {
		return 0, 0
	}
	defer procReleaseDC.Call(hwnd, dc)
	previous, _, _ := procSelectObject.Call(dc, font)
	defer procSelectObject.Call(dc, previous)
	value, err := syscall.UTF16PtrFromString(text)
	if err != nil {
		return 0, 0
	}
	rect := [4]int32{0, 0, width, 0}
	flags := uintptr(dtCalcRect | dtNoPrefix)
	if width > 0 {
		flags |= dtWordBreak
	}
	procDrawTextW.Call(dc, uintptr(unsafe.Pointer(value)), ^uintptr(0), uintptr(unsafe.Pointer(&rect)), flags)
	return rect[2] - rect[0], rect[3] - rect[1]
}

// buttonWidthFor returns a width that fits every label in font, never less
// than minimum, the width the English layout was designed with.
func buttonWidthFor(font uintptr, minimum int32, labels ...string) int32 {
	for _, label := range labels {
		w, _ := measureText(0, font, label, 0)
		minimum = max(minimum, w+24)
	}
	return minimum
}
