//go:build windows

package main

import (
	"fmt"
	"syscall"
	"unsafe"
)

func init() { queryFilesystemCapability = windowsFilesystemCapability }

func windowsFilesystemCapability(path string) (filesystemCapability, error) {
	// Portable copies can live on a UNC share too. The standard data-location
	// helper deliberately rejects those paths, so query their volume directly.
	existing, err := existingDiskPath(path)
	if err != nil {
		return filesystemCapability{}, err
	}
	ptr, err := syscall.UTF16PtrFromString(existing)
	if err != nil {
		return filesystemCapability{}, err
	}
	var root [32768]uint16
	ok, _, callErr := procGetVolumePathNameW.Call(uintptr(unsafe.Pointer(ptr)), uintptr(unsafe.Pointer(&root[0])), uintptr(len(root)))
	if ok == 0 {
		return filesystemCapability{}, fmt.Errorf("checking portable volume: %w", callErr)
	}
	var name [256]uint16
	var flags uint32
	ok, _, callErr = procGetVolumeInformationW.Call(uintptr(unsafe.Pointer(&root[0])), 0, 0, 0, 0, uintptr(unsafe.Pointer(&flags)),
		uintptr(unsafe.Pointer(&name[0])), uintptr(len(name)))
	if ok == 0 {
		return filesystemCapability{}, fmt.Errorf("checking portable filesystem: %w", callErr)
	}
	fs := syscall.UTF16ToString(name[:])
	return filesystemCapability{Name: fs, MaxFileBytes: filesystemFileLimit(fs), SparseFiles: flags&fileSupportsSparseFiles != 0}, nil
}
