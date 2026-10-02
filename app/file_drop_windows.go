//go:build windows

package main

import (
	"fmt"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"unsafe"
)

type fileDropWindow struct {
	paths        []string
	list, status uintptr
}

var fileDropWindows sync.Map
var fileDropClassOnce sync.Once
var fileDropClassOK bool
var fileDropCallback = syscall.NewCallback(fileDropWindowProc)
var fileDropListCallback = syscall.NewCallback(fileDropListProc)

func fileDropListProc(hwnd, message, w, l, id, data uintptr) uintptr {
	if message == 0x201 { // WM_LBUTTONDOWN
		parent, _, _ := user32.NewProc("GetParent").Call(hwnd)
		if value, ok := fileDropWindows.Load(parent); ok {
			state := value.(*fileDropWindow)
			index, _, _ := procSendMessageW.Call(hwnd, 0x1a9, 0, l) // LB_ITEMFROMPOINT
			if index>>16 == 0 && int(index&0xffff) < len(state.paths) {
				index &= 0xffff
				procSendMessageW.Call(hwnd, 0x186, index, 0)
				point := [2]int32{int32(int16(l & 0xffff)), int32(int16(l >> 16))}
				user32.NewProc("ClientToScreen").Call(hwnd, uintptr(unsafe.Pointer(&point[0])))
				packed := uintptr(uint64(uint32(point[0])) | uint64(uint32(point[1]))<<32)
				if dragging, _, _ := user32.NewProc("DragDetect").Call(hwnd, packed); dragging != 0 {
					if err := dragHostFiles(parent, []string{state.paths[index]}); err != nil {
						usbSetText(state.status, uiTextWith("drop.error.drag", map[string]string{"error": err.Error()}))
					}
				}
				return 0
			}
		}
	}
	result, _, _ := comctl32.NewProc("DefSubclassProc").Call(hwnd, message, w, l)
	return result
}

// The Windows Shell supplies IDataObject and IDropSource, including the native
// file formats understood by Explorer. COPY is the only advertised operation.
func withHostDropData(paths []string, use func(uintptr) error) error {
	if len(paths) == 0 {
		return fmt.Errorf("select a received file first")
	}
	var parent uintptr
	name, _ := syscall.UTF16PtrFromString(filepath.Dir(paths[0]))
	parse := shell32.NewProc("SHParseDisplayName")
	result, _, _ := parse.Call(uintptr(unsafe.Pointer(name)), 0, uintptr(unsafe.Pointer(&parent)), 0, 0)
	if int32(result) < 0 {
		return fmt.Errorf("cannot open the received folder")
	}
	free := syscall.NewLazyDLL("ole32.dll").NewProc("CoTaskMemFree")
	defer free.Call(parent)
	var absolute, children []uintptr
	defer func() {
		for _, pidl := range absolute {
			free.Call(pidl)
		}
	}()
	for _, path := range paths {
		if filepath.Dir(path) != filepath.Dir(paths[0]) {
			return fmt.Errorf("drag files from one received folder at a time")
		}
		var pidl uintptr
		name, _ := syscall.UTF16PtrFromString(path)
		result, _, _ = parse.Call(uintptr(unsafe.Pointer(name)), 0, uintptr(unsafe.Pointer(&pidl)), 0, 0)
		if int32(result) < 0 {
			return fmt.Errorf("received file is unavailable")
		}
		absolute = append(absolute, pidl)
		child, _, _ := shell32.NewProc("ILFindLastID").Call(pidl)
		children = append(children, child)
	}
	// IID_IDataObject
	iid := [16]byte{0x0e, 0x01, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46}
	var object uintptr
	result, _, _ = shell32.NewProc("SHCreateDataObject").Call(parent, uintptr(len(children)), uintptr(unsafe.Pointer(&children[0])), 0, uintptr(unsafe.Pointer(&iid[0])), uintptr(unsafe.Pointer(&object)))
	if int32(result) < 0 || object == 0 {
		return fmt.Errorf("cannot prepare the file drag")
	}
	defer func() {
		vtable := *(*uintptr)(unsafe.Pointer(object))
		release := *(*uintptr)(unsafe.Pointer(vtable + 2*unsafe.Sizeof(uintptr(0))))
		syscall.SyscallN(release, object)
	}()
	err := use(object)
	runtime.KeepAlive(children)
	return err
}

func dragHostFiles(hwnd uintptr, paths []string) error {
	return withHostDropData(paths, func(object uintptr) error {
		var effect uint32
		result, _, _ := shell32.NewProc("SHDoDragDrop").Call(hwnd, object, 0, 1, uintptr(unsafe.Pointer(&effect)))
		if int32(result) < 0 {
			return fmt.Errorf("Windows could not complete the file drag")
		}
		return nil
	})
}

func fileDropWindowProc(hwnd, message, w, l uintptr) uintptr {
	if value, ok := fileDropWindows.Load(hwnd); ok {
		state := value.(*fileDropWindow)
		switch message {
		case 0x233: // WM_DROPFILES
			query := shell32.NewProc("DragQueryFileW")
			defer shell32.NewProc("DragFinish").Call(w)
			count, _, _ := query.Call(w, 0xffffffff, 0, 0)
			if count == 0 || count > 1000 {
				usbSetText(state.status, uiText("drop.limit"))
				return 0
			}
			var paths []string
			for i := uintptr(0); i < count; i++ {
				length, _, _ := query.Call(w, i, 0, 0)
				if length == 0 || length > 32768 {
					return 0
				}
				data := make([]uint16, length+1)
				query.Call(w, i, uintptr(unsafe.Pointer(&data[0])), uintptr(len(data)))
				paths = append(paths, syscall.UTF16ToString(data))
			}
			if err := sendDroppedFiles(paths); err != nil {
				usbSetText(state.status, uiTextWith("drop.error.send", map[string]string{"error": err.Error()}))
			} else {
				usbSetText(state.status, uiText("drop.sending"))
			}
			return 0
		case wmCommand:
			switch w & 0xffff {
			case 4601:
				if len(state.paths) > 0 {
					cmd := exec.Command("explorer.exe", filepath.Dir(state.paths[0]))
					if cmd.Start() == nil {
						cmd.Process.Release()
					}
				}
			case 4602:
				path, ok, err := chooseRecoveryPath(hwnd, uiText("drop.choose_file"), "", false, false)
				if err == nil && ok {
					err = sendDroppedFiles([]string{path})
				}
				if err != nil {
					usbSetText(state.status, uiTextWith("drop.error.send", map[string]string{"error": err.Error()}))
				}
			case 2:
				procDestroyWindow.Call(hwnd)
			}
			return 0
		case wmClose:
			procDestroyWindow.Call(hwnd)
			return 0
		case wmDestroy:
			fileDropWindows.Delete(hwnd)
			procPostQuitMessage.Call(0)
			return 0
		}
	}
	result, _, _ := procDefWindowProcW.Call(hwnd, message, w, l)
	return result
}

func showFileDropWindow(paths []string) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	ole := syscall.NewLazyDLL("ole32.dll")
	hr, _, _ := ole.NewProc("OleInitialize").Call(0)
	if int32(hr) < 0 {
		logf("cannot initialize native file dragging")
		return
	}
	defer ole.NewProc("OleUninitialize").Call()
	instance, _, _ := procGetModuleHandleW.Call(0)
	class, _ := syscall.UTF16PtrFromString("TryOmarchyFileDrops")
	fileDropClassOnce.Do(func() {
		type wc struct {
			size, style                   uint32
			callback                      uintptr
			classExtra, windowExtra       int32
			instance, icon, cursor, brush uintptr
			menu, class                   *uint16
			smallIcon                     uintptr
		}
		cursor, _, _ := procLoadCursorW.Call(0, idcArrow)
		c := wc{size: uint32(unsafe.Sizeof(wc{})), callback: fileDropCallback, instance: instance, cursor: cursor, brush: colorBtnface + 1, class: class}
		atom, _, _ := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&c)))
		fileDropClassOK = atom != 0
	})
	if !fileDropClassOK {
		return
	}
	title, _ := syscall.UTF16PtrFromString(uiText("drop.title"))
	hwnd, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)), wsCaption|wsSysmenu, 100, 100, 580, 380, 0, 0, instance, 0)
	if hwnd == 0 {
		return
	}
	state := &fileDropWindow{paths: append([]string(nil), paths...)}
	fileDropWindows.Store(hwnd, state)
	font, _, _ := procGetStockObject.Call(defaultGuiFont)
	control := func(class, label string, x, y, width, height int, id, style uintptr) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		text, _ := syscall.UTF16PtrFromString(label)
		h, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(c)), uintptr(unsafe.Pointer(text)), wsChild|wsVisible|style, uintptr(x), uintptr(y), uintptr(width), uintptr(height), hwnd, id, instance, 0)
		procSendMessageW.Call(h, wmSetfont, font, 1)
		return h
	}
	control("STATIC", uiText("drop.intro"), 16, 12, 536, 44, 0, ssNoprefix)
	state.list = control("LISTBOX", "", 16, 64, 536, 190, 4600, wsBorder|wsVscroll|wsTabstop)
	for _, path := range paths {
		text, _ := syscall.UTF16PtrFromString(filepath.Base(path))
		procSendMessageW.Call(state.list, 0x180, 0, uintptr(unsafe.Pointer(text)))
	}
	state.status = control("STATIC", uiText("drop.ready"), 16, 264, 536, 30, 0, ssNoprefix)
	chooseLabel, openLabel, closeLabel := uiText("drop.choose"), uiText("drop.open_received"), uiText("drop.close")
	chooseWidth := int(buttonWidthFor(font, 150, chooseLabel))
	openWidth := int(buttonWidthFor(font, 190, openLabel))
	closeWidth := int(buttonWidthFor(font, 100, closeLabel))
	control("BUTTON", chooseLabel, 16, 302, chooseWidth, 28, 4602, wsTabstop)
	control("BUTTON", openLabel, 16+chooseWidth+10, 302, openWidth, 28, 4601, wsTabstop)
	control("BUTTON", closeLabel, 552-closeWidth, 302, closeWidth, 28, 2, wsTabstop)
	shell32.NewProc("DragAcceptFiles").Call(hwnd, 1)
	comctl32.NewProc("SetWindowSubclass").Call(state.list, fileDropListCallback, 1, 0)
	procShowWindow.Call(hwnd, swShow)
	procSetForegroundWindow.Call(hwnd)
	var message msgStruct
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&message)), 0, 0, 0)
		if r == 0 || int32(r) == -1 {
			break
		}
		if handled, _, _ := procIsDialogMessageW.Call(hwnd, uintptr(unsafe.Pointer(&message))); handled != 0 {
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&message)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&message)))
	}
}
