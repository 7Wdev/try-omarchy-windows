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
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if !openClipboard() {
		return nil, false
	}
	data, ok := clipboardGlobalBytes(cfHDrop, 1<<20)
	procCloseClipboard.Call()
	if !ok || len(data) < 22 {
		return nil, false
	}
	offset := int(binary.LittleEndian.Uint32(data))
	if offset < 20 || offset >= len(data)-1 || binary.LittleEndian.Uint32(data[16:20]) != 1 || offset%2 != 0 {
		return nil, false
	}
	var paths []string
	var chars []uint16
	terminated := false
	for pos := offset; pos+1 < len(data); pos += 2 {
		c := binary.LittleEndian.Uint16(data[pos:])
		if c != 0 {
			chars = append(chars, c)
			continue
		}
		if len(chars) == 0 {
			terminated = true
			break
		}
		p := syscall.UTF16ToString(chars)
		if !filepath.IsAbs(p) {
			return nil, false
		}
		paths = append(paths, p)
		chars = nil
		if len(paths) > clipboardTransferLimits.Entries {
			return nil, false
		}
	}
	if !terminated || len(paths) == 0 {
		return nil, false
	}
	return paths, true
}

func clipboardGetFiles() (clipItem, bool) {
	paths, ok := clipboardGetFilePaths()
	if !ok {
		return clipItem{}, false
	}
	data, err := packClipboardFiles(paths)
	if err != nil {
		logf("clipboard files: %v", err)
		infoBox(uiTextWith("tray.transfer.failed", map[string]string{"error": err.Error()}))
		return clipItem{}, false
	}
	return clipItem{Kind: clipFiles, Data: data}, true
}

func clipboardSetFiles(item clipItem) bool {
	cache, err := clipboardFilesCache()
	if err != nil {
		return false
	}
	paths, err := unpackClipboardFiles(item.Data, cache)
	if err != nil {
		logf("clipboard files: %v", err)
		infoBox(uiTextWith("transfer.from_omarchy_failed", map[string]string{"error": err.Error()}))
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
