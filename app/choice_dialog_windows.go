//go:build windows

package main

import (
	"fmt"
	"runtime"
	"strings"
	"syscall"
	"unsafe"
)

// chooseAction uses ordinary, labelled Windows buttons instead of assigning
// unrelated actions to Yes/No. Zero means close/Escape; actions are one-based.
// It runs on its own UI thread and is used before setup or from the About process.
func chooseAction(title, body string, labels ...string) (int, error) {
	return chooseActionWithTextHeight(title, body, 260, labels...)
}

func chooseActionWithTextHeight(title, body string, textHeight int32, labels ...string) (int, error) {
	return chooseActionCancelable(nil, title, body, textHeight, labels...)
}

func chooseActionCancelable(cancel <-chan struct{}, title, body string, textHeight int32, labels ...string) (int, error) {
	type result struct {
		action int
		err    error
	}
	done := make(chan result, 1)
	go func() {
		runtime.LockOSThread()
		// Let Go retire this UI thread on return, including any pending messages.
		brand := newWindowBrand()
		defer brand.close()
		var selected int
		var dialogErr error
		defer func() { done <- result{selected, dialogErr} }()
		instance, _, _ := procGetModuleHandleW.Call(0)
		class, _ := syscall.UTF16PtrFromString("TryOmarchyChoice")
		callback := syscall.NewCallback(func(h, message, w, l uintptr) uintptr {
			if result, handled := brand.handle(h, message, w, l); handled {
				return result
			}
			switch message {
			case wmCommand:
				id := int(w & 0xffff)
				if id >= 3001 && id <= 3000+len(labels) {
					selected = id - 3000
					procDestroyWindow.Call(h)
					return 0
				}
				if id == idCancel {
					procDestroyWindow.Call(h)
					return 0
				}
			case wmClose:
				procDestroyWindow.Call(h)
				return 0
			case wmDestroy:
				procPostQuitMessage.Call(0)
				return 0
			}
			r, _, _ := procDefWindowProcW.Call(h, message, w, l)
			return r
		})
		type windowClass struct {
			size, style                   uint32
			callback                      uintptr
			classExtra, windowExtra       int32
			instance, icon, cursor, brush uintptr
			menu, name                    *uint16
			smallIcon                     uintptr
		}
		icon, _, _ := procLoadIconW.Call(instance, 1)
		cursor, _, _ := procLoadCursorW.Call(0, idcArrow)
		wc := windowClass{size: uint32(unsafe.Sizeof(windowClass{})), callback: callback,
			instance: instance, icon: icon, cursor: cursor, brush: colorBtnface + 1, name: class, smallIcon: icon}
		if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
			dialogErr = fmt.Errorf("register choice window: %w", err)
			return
		}
		defer user32.NewProc("UnregisterClassW").Call(uintptr(unsafe.Pointer(class)), instance)
		style := uintptr(wsCaption | wsSysmenu | 0x02000000)
		work := [4]int32{}
		procSystemParametersInfoW.Call(0x30, 0, uintptr(unsafe.Pointer(&work)), 0)
		frame := [4]int32{}
		procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&frame)), style, 0, 0)
		width := min(int32(520), work[2]-work[0]-32-(frame[2]-frame[0]))
		bodyHeight := min(textHeight, work[3]-work[1]-32-(frame[3]-frame[1])-24-int32(40*len(labels)))
		bodyHeight = max(int32(56), bodyHeight)
		height := bodyHeight + 24 + int32(40*len(labels))
		w, h := width+frame[2]-frame[0], height+frame[3]-frame[1]
		titlePtr, _ := syscall.UTF16PtrFromString(title)
		hwnd, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(titlePtr)),
			style, uintptr(work[0]+(work[2]-work[0]-w)/2), uintptr(work[1]+(work[3]-work[1]-h)/2), uintptr(w), uintptr(h), 0, 0, instance, 0)
		if hwnd == 0 {
			dialogErr = fmt.Errorf("create choice window: %w", err)
			return
		}
		closed := make(chan struct{})
		defer close(closed)
		if cancel != nil {
			go func() {
				select {
				case <-cancel:
					procPostMessageW.Call(hwnd, wmClose, 0, 0)
				case <-closed:
				}
			}()
		}
		brand.window(hwnd)

		add := func(kind, label string, y, height int32, style, id uintptr) uintptr {
			k, _ := syscall.UTF16PtrFromString(kind)
			t, _ := syscall.UTF16PtrFromString(label)
			control, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(k)), uintptr(unsafe.Pointer(t)),
				wsChild|wsVisible|style, 20, uintptr(y), uintptr(width-40), uintptr(height), hwnd, id, instance, 0)
			if control == 0 {
				dialogErr = fmt.Errorf("create choice control: %w", err)
				return 0
			}
			brand.control(control, kind, style)
			return control
		}
		// Read-only multiline text remains selectable and scrollable when a
		// long path, translation, or large system font needs more room.
		add("EDIT", strings.ReplaceAll(body, "\n", "\r\n"), 16, bodyHeight-24,
			esMultiline|esAutovscroll|wsVscroll|wsTabstop|0x0800, 3000)
		var first uintptr
		for i, label := range labels {
			buttonStyle := uintptr(wsTabstop)
			if i == 0 {
				buttonStyle |= bsDefpushbutton
			}
			button := add("BUTTON", label, bodyHeight+int32(i)*40, 32, buttonStyle, uintptr(3001+i))
			if i == 0 {
				first = button
				brand.primary = button
			}
		}
		if dialogErr != nil {
			procDestroyWindow.Call(hwnd)
			return
		}
		// About is spawned with STARTF_USESHOWWINDOW/SW_HIDE to suppress a
		// console. The first ShowWindow would inherit that hidden state. Show
		// the native window explicitly, independent of process startup flags.
		procSetWindowPos.Call(hwnd, 0, 0, 0, 0, 0, swpNoMove|swpNoSize|swpShowWindow|0x0004)
		procSetForegroundWindow.Call(hwnd)
		procSetFocus.Call(first)
		var message msgStruct
		for {
			r, _, err := procGetMessageW.Call(uintptr(unsafe.Pointer(&message)), 0, 0, 0)
			if r == 0 {
				break
			}
			if int32(r) == -1 {
				dialogErr = fmt.Errorf("read choice window message: %w", err)
				procDestroyWindow.Call(hwnd)
				break
			}
			if message.message == wmKeydown && message.wParam == 13 {
				focus, _, _ := procGetFocus.Call()
				id, _, _ := user32.NewProc("GetDlgCtrlID").Call(focus)
				if id >= 3001 && id <= uintptr(3000+len(labels)) {
					procSendMessageW.Call(hwnd, wmCommand, id, 0)
					continue
				}
			}
			if handled, _, _ := procIsDialogMessageW.Call(hwnd, uintptr(unsafe.Pointer(&message))); handled != 0 {
				continue
			}
			procTranslateMessage.Call(uintptr(unsafe.Pointer(&message)))
			procDispatchMessageW.Call(uintptr(unsafe.Pointer(&message)))
		}
	}()
	r := <-done
	return r.action, r.err
}
