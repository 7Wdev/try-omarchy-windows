//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

var (
	procSetWindowTheme  = syscall.NewLazyDLL("uxtheme.dll").NewProc("SetWindowTheme")
	procSetBkColor      = syscall.NewLazyDLL("gdi32.dll").NewProc("SetBkColor")
	procSelectObject    = syscall.NewLazyDLL("gdi32.dll").NewProc("SelectObject")
	procSaveDC          = syscall.NewLazyDLL("gdi32.dll").NewProc("SaveDC")
	procRestoreDC       = syscall.NewLazyDLL("gdi32.dll").NewProc("RestoreDC")
	procDrawTextW       = user32.NewProc("DrawTextW")
	procDrawFocusRect   = user32.NewProc("DrawFocusRect")
	procFrameRect       = user32.NewProc("FrameRect")
	procGetSysColor     = user32.NewProc("GetSysColor")
	procIsWindowEnabled = user32.NewProc("IsWindowEnabled")
)

const brandPanel = 0x003B2824 // Mac Tokyo Night panel, #24283B.
const brandMuted = 0x00684841 // #414868.

// Native controls keep their roles, names, tab order and command handling.
// Only push buttons are owner-drawn; check/radio state remains Windows-owned.
type brandControl struct {
	handle uintptr
	class  string
	style  uintptr
}

type windowBrand struct {
	background, panel, text, panelText, muted, accent uintptr
	brushes                                           []uintptr
	font, heading                                     uintptr
	primary, activeTab                                uintptr
	controls                                          []brandControl
	highContrast                                      bool
	contentTop                                        int32
	panelControls, sectionControls                    map[uintptr]bool
}

func newWindowBrand() *windowBrand {
	t := &windowBrand{panelControls: map[uintptr]bool{}, sectionControls: map[uintptr]bool{}}
	t.refreshColors()
	makeFont := func(size, weight int, name string) uintptr {
		face, _ := syscall.UTF16PtrFromString(name)
		font, _, _ := procCreateFontW.Call(^uintptr(size-1), 0, 0, 0, uintptr(weight), 0, 0, 0, 0, 0, 0, 5, 0, uintptr(unsafe.Pointer(face)))
		return font
	}
	t.font = makeFont(15, 400, "Segoe UI")
	t.heading = makeFont(25, 700, "Consolas")
	return t
}

func (t *windowBrand) refreshColors() {
	for _, brush := range t.brushes {
		procDeleteObject.Call(brush)
	}
	t.brushes = nil
	var hc struct {
		size, flags uint32
		scheme      *uint16
	}
	hc.size = uint32(unsafe.Sizeof(hc))
	procSystemParametersInfoW.Call(0x42, uintptr(hc.size), uintptr(unsafe.Pointer(&hc)), 0) // SPI_GETHIGHCONTRAST
	t.highContrast = hc.flags&1 != 0
	t.background, t.panel, t.text, t.muted, t.accent = colBg, brandPanel, colText, colDim, colGreen
	t.panelText = t.text
	if t.highContrast {
		t.background, _, _ = procGetSysColor.Call(15)
		t.panel, _, _ = procGetSysColor.Call(5)
		t.text, _, _ = procGetSysColor.Call(18)
		t.panelText, _, _ = procGetSysColor.Call(8)
		t.muted, _, _ = procGetSysColor.Call(17)
		t.accent, _, _ = procGetSysColor.Call(13)
	}
	border := uintptr(brandMuted)
	if t.highContrast {
		border = t.text
	}
	for _, color := range []uintptr{t.background, t.panel, t.accent, border} {
		brush, _, _ := procCreateSolidBrush.Call(color)
		t.brushes = append(t.brushes, brush)
	}
}

func (t *windowBrand) close() {
	for _, h := range append(t.brushes, t.font, t.heading) {
		if h != 0 {
			procDeleteObject.Call(h)
		}
	}
}

func (t *windowBrand) control(handle uintptr, class string, style uintptr) {
	t.controls = append(t.controls, brandControl{handle, class, style})
	procSendMessageW.Call(handle, wmSetfont, t.font, 1)
	t.styleControl(handle, class, style)
}

func (t *windowBrand) styleControl(handle uintptr, class string, style uintptr) {
	if !t.highContrast {
		empty, _ := syscall.UTF16PtrFromString("")
		procSetWindowTheme.Call(handle, uintptr(unsafe.Pointer(empty)), uintptr(unsafe.Pointer(empty)))
	} else {
		procSetWindowTheme.Call(handle, 0, 0)
	}
	if class == "BUTTON" && style&0xf <= bsDefpushbutton {
		current, _, _ := user32.NewProc("GetWindowLongW").Call(handle, ^uintptr(15))
		buttonStyle := style & 0xf
		if !t.highContrast {
			buttonStyle = 0xb
		} // BS_OWNERDRAW
		user32.NewProc("SetWindowLongW").Call(handle, ^uintptr(15), current&^uintptr(0xf)|buttonStyle)
	}
}

func (t *windowBrand) window(handle uintptr) {
	dark := int32(0)
	if !t.highContrast {
		dark = 1
	}
	procDwmSetWindowAttr.Call(handle, 20, uintptr(unsafe.Pointer(&dark)), 4)
}

func (t *windowBrand) handle(hwnd, message, w, l uintptr) (uintptr, bool) {
	switch message {
	case 0x001a, 0x0015: // WM_SETTINGCHANGE / WM_SYSCOLORCHANGE, including live high contrast.
		t.refreshColors()
		t.window(hwnd)
		for _, control := range t.controls {
			t.styleControl(control.handle, control.class, control.style)
		}
		procRedrawWindow.Call(hwnd, 0, 0, 0x185)
		return 0, false
	case 0x0014: // WM_ERASEBKGND
		var rect [4]int32
		procGetClientRect.Call(hwnd, uintptr(unsafe.Pointer(&rect)))
		procFillRect.Call(w, uintptr(unsafe.Pointer(&rect)), t.brushes[0])
		if t.contentTop > 0 {
			rect[0], rect[1], rect[2], rect[3] = 12, t.contentTop-8, rect[2]-12, rect[3]-108
			procFillRect.Call(w, uintptr(unsafe.Pointer(&rect)), t.brushes[1])
		}
		return 1, true
	case 0x0133, 0x0134, 0x0135, wmCtlcolorstatic: // edit/list/button/static
		color := t.text
		if t.sectionControls[l] && !t.highContrast {
			color = colGreen
		}
		enabled, _, _ := procIsWindowEnabled.Call(l)
		if enabled == 0 {
			color = t.muted
		}
		procSetBkMode.Call(w, transparentBkMode)
		background, brush := t.background, t.brushes[0]
		if message == 0x0133 || message == 0x0134 || t.panelControls[l] {
			background, brush = t.panel, t.brushes[1]
			if t.highContrast && enabled != 0 {
				color = t.panelText
			}
		}
		procSetTextColor.Call(w, color)
		procSetBkColor.Call(w, background)
		return brush, true
	case 0x002b: // WM_DRAWITEM
		if l == 0 {
			return 0, false
		}
		item := (*struct {
			kind, id, item, action, state uint32
			handle, dc                    uintptr
			rect                          [4]int32
			data                          uintptr
		})(unsafe.Pointer(l))
		if item.kind != 4 {
			return 0, false
		} // ODT_BUTTON
		dcState, _, _ := procSaveDC.Call(item.dc)
		defer procRestoreDC.Call(item.dc, dcState)
		brush, color := t.brushes[1], t.text
		if item.handle == t.primary {
			brush, color = t.brushes[2], colBg
			if t.highContrast {
				color, _, _ = procGetSysColor.Call(14)
			}
		}
		if item.handle == t.activeTab && !t.highContrast {
			color = colGreen
		}
		if item.state&4 != 0 {
			color = t.muted
		}
		procFillRect.Call(item.dc, uintptr(unsafe.Pointer(&item.rect)), brush)
		border := t.brushes[3]
		if item.state&0x10 != 0 || item.handle == t.activeTab {
			border = t.brushes[2]
		}
		procFrameRect.Call(item.dc, uintptr(unsafe.Pointer(&item.rect)), border)
		procSelectObject.Call(item.dc, t.font)
		procSetTextColor.Call(item.dc, color)
		procSetBkMode.Call(item.dc, transparentBkMode)
		n, _, _ := procSendMessageW.Call(item.handle, wmGettextlength, 0, 0)
		label := make([]uint16, n+1)
		procSendMessageW.Call(item.handle, wmGettext, uintptr(len(label)), uintptr(unsafe.Pointer(&label[0])))
		rect := item.rect
		if item.state&1 != 0 {
			rect[0]++
			rect[1]++
		}
		procDrawTextW.Call(item.dc, uintptr(unsafe.Pointer(&label[0])), n, uintptr(unsafe.Pointer(&rect)), 0x1|0x4|0x20) // center, vertical center, single line
		if item.state&0x10 != 0 {
			rect = item.rect
			rect[0] += 4
			rect[1] += 4
			rect[2] -= 4
			rect[3] -= 4
			procDrawFocusRect.Call(item.dc, uintptr(unsafe.Pointer(&rect)))
		}
		return 1, true
	}
	return 0, false
}
