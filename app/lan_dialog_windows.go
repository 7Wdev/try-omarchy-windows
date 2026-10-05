//go:build windows

package main

import (
	"fmt"
	"syscall"
	"unsafe"
)

type lanForwardChoice struct{ Forward, Adapter string }

type lanDialogState struct {
	window, protocol, address, host, guest uintptr
	adapters                               []lanAdapter
	result                                 lanForwardChoice
	done                                   bool
}

var lanDialog *lanDialogState
var lanDialogRegistered bool
var lanDialogCallback = syscall.NewCallback(lanDialogWindowProc)

func lanDialogWindowProc(hwnd, message, w, l uintptr) uintptr {
	state := lanDialog
	if state == nil {
		r, _, _ := procDefWindowProcW.Call(hwnd, message, w, l)
		return r
	}
	switch message {
	case wmCommand:
		switch w & 0xffff {
		case 1:
			read := func(handle uintptr) string {
				var text [64]uint16
				procGetWindowTextW.Call(handle, uintptr(unsafe.Pointer(&text[0])), 64)
				return syscall.UTF16ToString(text[:])
			}
			protocol, _, _ := procSendMessageW.Call(state.protocol, 0x147, 0, 0)
			index, _, _ := procSendMessageW.Call(state.address, 0x147, 0, 0)
			if index >= uintptr(len(state.adapters)) {
				errorBox(uiText("lan.choose_adapter"))
				return 0
			}
			name := "tcp"
			if protocol == 1 {
				name = "udp"
			}
			value := fmt.Sprintf("%s:%s:%s:%s", name, state.adapters[index].Address, read(state.host), read(state.guest))
			var check forwardList
			if err := check.Set(value); err != nil {
				errorBox(uiTextWith("lan.error.invalid", map[string]string{"error": err.Error()}))
				return 0
			}
			state.result = lanForwardChoice{value, state.adapters[index].Identity}
			procDestroyWindow.Call(hwnd)
			return 0
		case 2:
			procDestroyWindow.Call(hwnd)
			return 0
		}
	case 0x020A: // WM_MOUSEWHEEL: this dialog does not scroll, and must not scroll the disabled Settings page.
		return 0
	case wmClose:
		procDestroyWindow.Call(hwnd)
		return 0
	case wmDestroy:
		state.done = true
		return 0
	}
	r, _, _ := procDefWindowProcW.Call(hwnd, message, w, l)
	return r
}

func chooseLANForward(parent uintptr) (lanForwardChoice, error) {
	adapters, err := availableLANAdapters()
	if err != nil {
		return lanForwardChoice{}, err
	}
	state := &lanDialogState{adapters: adapters}
	lanDialog = state
	defer func() { lanDialog = nil }()
	instance, _, _ := procGetModuleHandleW.Call(0)
	class, _ := syscall.UTF16PtrFromString("TryOmarchyLANForward")
	if !lanDialogRegistered {
		type windowClass struct {
			size, style                   uint32
			callback                      uintptr
			classExtra, windowExtra       int32
			instance, icon, cursor, brush uintptr
			menu, class                   *uint16
			smallIcon                     uintptr
		}
		cursor, _, _ := procLoadCursorW.Call(0, idcArrow)
		wc := windowClass{size: uint32(unsafe.Sizeof(windowClass{})), callback: lanDialogCallback, instance: instance, cursor: cursor, brush: colorBtnface + 1, class: class}
		if result, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); result == 0 {
			return lanForwardChoice{}, err
		}
		lanDialogRegistered = true
	}
	title, _ := syscall.UTF16PtrFromString(uiText("lan.title"))
	style := uintptr(wsCaption | wsSysmenu)
	rect := [4]int32{0, 0, 430, 230}
	procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&rect[0])), style, 0, 0)
	var bounds [4]int32
	procGetWindowRect.Call(parent, uintptr(unsafe.Pointer(&bounds[0])))
	state.window, _, err = procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)), style|wsVisible, uintptr(bounds[0]+24), uintptr(bounds[1]+32), uintptr(rect[2]-rect[0]), uintptr(rect[3]-rect[1]), parent, 0, instance, 0)
	if state.window == 0 {
		return lanForwardChoice{}, err
	}
	procEnableWindow.Call(parent, 0)
	defer procEnableWindow.Call(parent, 1)
	font, _, _ := procGetStockObject.Call(defaultGuiFont)
	var controlErr error
	control := func(class, label string, x, y, width, height int, style, id uintptr) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		text, _ := syscall.UTF16PtrFromString(label)
		h, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(c)), uintptr(unsafe.Pointer(text)), wsVisible|wsChild|style, uintptr(x), uintptr(y), uintptr(width), uintptr(height), state.window, id, instance, 0)
		if h == 0 {
			controlErr = err
		}
		procSendMessageW.Call(h, wmSetfont, font, 1)
		guardWheel(h, class, style)
		return h
	}
	control("STATIC", uiText("lan.adapter"), 16, 16, 398, 22, ssNoprefix, 0)
	state.address = control("COMBOBOX", "", 16, 40, 398, 180, wsTabstop|wsVscroll|3, 4201)
	for _, adapter := range adapters {
		label, _ := syscall.UTF16PtrFromString(adapter.Name + " (" + adapter.Address + ")")
		procSendMessageW.Call(state.address, 0x143, 0, uintptr(unsafe.Pointer(label)))
	}
	procSendMessageW.Call(state.address, 0x14e, 0, 0)
	control("STATIC", uiText("lan.protocol"), 16, 84, 106, 22, ssNoprefix, 0)
	control("STATIC", uiText("lan.windows_port"), 126, 84, 146, 22, ssNoprefix, 0)
	control("STATIC", uiText("lan.omarchy_port"), 276, 84, 138, 22, ssNoprefix, 0)
	state.protocol = control("COMBOBOX", "", 16, 108, 90, 120, wsTabstop|3, 4202)
	for _, value := range []string{"TCP", "UDP"} {
		text, _ := syscall.UTF16PtrFromString(value)
		procSendMessageW.Call(state.protocol, 0x143, 0, uintptr(unsafe.Pointer(text)))
	}
	procSendMessageW.Call(state.protocol, 0x14e, 0, 0)
	state.host = control("EDIT", "8080", 126, 108, 120, 26, wsTabstop|wsBorder|esAutohscroll, 4203)
	state.guest = control("EDIT", "80", 276, 108, 138, 26, wsTabstop|wsBorder|esAutohscroll, 4204)
	control("STATIC", uiText("lan.note"), 16, 148, 398, 28, ssNoprefix, 0)
	addLabel, cancelLabel := uiText("lan.add"), uiText("lan.cancel")
	cancelWidth := int(buttonWidthFor(font, 84, cancelLabel))
	addWidth := int(buttonWidthFor(font, 84, addLabel))
	control("BUTTON", addLabel, 414-cancelWidth-12-addWidth, 188, addWidth, 26, wsTabstop|bsDefpushbutton, 1)
	control("BUTTON", cancelLabel, 414-cancelWidth, 188, cancelWidth, 26, wsTabstop, 2)
	if controlErr != nil {
		procDestroyWindow.Call(state.window)
		return lanForwardChoice{}, controlErr
	}
	procSetFocus.Call(state.address)
	var message msgStruct
	for !state.done {
		result, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&message)), 0, 0, 0)
		if result == 0 || int32(result) == -1 {
			procDestroyWindow.Call(state.window)
			if result == 0 {
				procPostQuitMessage.Call(0)
			}
			break
		}
		if handled, _, _ := procIsDialogMessageW.Call(state.window, uintptr(unsafe.Pointer(&message))); handled != 0 {
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&message)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&message)))
	}
	return state.result, nil
}
