//go:build windows

package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"runtime"
	"syscall"
	"unsafe"
)

const cfHDrop = 15

var preferredDropEffectFormat = func() uintptr {
	name, _ := syscall.UTF16PtrFromString("Preferred DropEffect")
	id, _, _ := procRegisterClipboardFormatW.Call(uintptr(unsafe.Pointer(name)))
	return id
}()

func clipboardFilesCache() (string, error) {
	dir, err := os.UserCacheDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "TryOmarchy", "Clipboard"), nil
}

func clipboardGetFilePaths() ([]string, bool) {
	paths, status := clipboardReadFilePaths()
	return paths, status == clipboardReady
}

func clipboardReadFilePaths() ([]string, clipboardReadStatus) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if r, _, _ := procIsClipboardFormatAvail.Call(cfHDrop); r == 0 {
		return nil, clipboardUnsupported
	}
	if !openClipboard() {
		return nil, clipboardRetry
	}
	data, status := clipboardGlobalBytesStatus(cfHDrop, maxClipboardPathBytes)
	procCloseClipboard.Call()
	if status == clipboardRejected {
		reportTransferError(uiError(uiText("error.transfer.clipboard_paths_limit"), nil))
	}
	if status != clipboardReady {
		return nil, status
	}
	paths, status := parseClipboardPaths(data, filepath.IsAbs)
	if status == clipboardRejected {
		reportTransferError(uiError(uiText("error.transfer.clipboard_paths_limit"), nil))
	}
	return paths, status
}

func clipboardGetFiles() (clipItem, bool) {
	item, status := clipboardReadFiles()
	return item, status == clipboardReady
}
func clipboardReadFiles() (clipItem, clipboardReadStatus) {
	paths, status := clipboardReadFilePaths()
	if status != clipboardReady {
		return clipItem{}, status
	}
	data, err := packClipboardFiles(paths)
	if err != nil {
		reportTransferError(err)
		return clipItem{}, clipboardRejected
	}
	return clipItem{Kind: clipFiles, Data: data}, clipboardReady
}

func clipboardSetFiles(item clipItem) bool {
	cache, err := clipboardFilesCache()
	if err != nil {
		return false
	}
	paths, err := unpackClipboardFiles(item.Data, cache)
	if err != nil {
		reportTransferError(uiError(uiTextWith("transfer.from_omarchy_failed", map[string]string{"error": err.Error()}), err))
		return false
	}
	keep := false
	defer func() {
		if !keep {
			os.RemoveAll(filepath.Dir(paths[0]))
		}
	}()
	keep = clipboardSetFilePaths(paths)
	return keep
}

func clipboardSetFilePaths(paths []string) bool {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	data := make([]byte, 20)
	binary.LittleEndian.PutUint32(data, 20)
	binary.LittleEndian.PutUint32(data[16:], 1)
	for _, p := range paths {
		u, e := syscall.UTF16FromString(p)
		if e != nil {
			return false
		}
		for _, c := range u {
			data = binary.LittleEndian.AppendUint16(data, c)
		}
	}
	data = append(data, 0, 0)
	h := globalCopy(data)
	if h == 0 {
		return false
	}
	effect := globalCopy([]byte{1, 0, 0, 0}) // DROPEFFECT_COPY, including a source Cut.
	if effect == 0 || preferredDropEffectFormat == 0 {
		procGlobalFree.Call(h)
		if effect != 0 {
			procGlobalFree.Call(effect)
		}
		return false
	}
	effectOwned := false
	defer func() {
		if !effectOwned {
			procGlobalFree.Call(effect)
		}
	}()
	if !openClipboard() {
		procGlobalFree.Call(h)
		return false
	}
	defer procCloseClipboard.Call()
	if r, _, _ := procEmptyClipboard.Call(); r == 0 {
		procGlobalFree.Call(h)
		return false
	}
	if r, _, _ := procSetClipboardData.Call(preferredDropEffectFormat, effect); r == 0 {
		procGlobalFree.Call(h)
		return false
	}
	effectOwned = true
	if r, _, _ := procSetClipboardData.Call(cfHDrop, h); r == 0 {
		procGlobalFree.Call(h)
		return false
	}
	return true
}
