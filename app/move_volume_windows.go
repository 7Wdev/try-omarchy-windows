//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

var (
	procGetVolumeNameForVolumeMountPointW = kernel32.NewProc("GetVolumeNameForVolumeMountPointW")
	procGetVolumePathNamesForVolumeNameW  = kernel32.NewProc("GetVolumePathNamesForVolumeNameW")
)

func init() {
	identifyMoveVolume = windowsMoveVolume
	locateMoveVolume = windowsLocateMoveVolume
	retireMoveLock = func(guard *os.File) error {
		var remove byte = 1
		ok, _, err := kernel32.NewProc("SetFileInformationByHandle").Call(guard.Fd(), 4,
			uintptr(unsafe.Pointer(&remove)), unsafe.Sizeof(remove)) // FileDispositionInfo
		if ok == 0 {
			return err
		}
		return nil
	}
}

func windowsMoveVolume(path string) (*moveVolumeLocation, error) {
	root, err := dataLocationVolumeRoot(path)
	if err != nil {
		return nil, err
	}
	ptr, err := syscall.UTF16PtrFromString(root)
	if err != nil {
		return nil, err
	}
	var volume [64]uint16
	ok, _, callErr := procGetVolumeNameForVolumeMountPointW.Call(uintptr(unsafe.Pointer(ptr)),
		uintptr(unsafe.Pointer(&volume[0])), uintptr(len(volume)))
	if ok == 0 {
		return nil, fmt.Errorf("recording moved volume identity: %w", callErr)
	}
	rel, err := filepath.Rel(root, path)
	if err != nil {
		return nil, err
	}
	location := &moveVolumeLocation{Volume: syscall.UTF16ToString(volume[:]), Relative: rel}
	if !validMoveVolumeLocation(*location) {
		return nil, fmt.Errorf("invalid moved volume identity")
	}
	return location, nil
}

func windowsLocateMoveVolume(location moveVolumeLocation) (string, error) {
	if !validMoveVolumeLocation(location) {
		return "", fmt.Errorf("invalid moved volume identity")
	}
	ptr, err := syscall.UTF16PtrFromString(location.Volume)
	if err != nil {
		return "", err
	}
	var paths [32768]uint16
	var length uint32
	ok, _, callErr := procGetVolumePathNamesForVolumeNameW.Call(uintptr(unsafe.Pointer(ptr)),
		uintptr(unsafe.Pointer(&paths[0])), uintptr(len(paths)), uintptr(unsafe.Pointer(&length)))
	if ok == 0 {
		return "", fmt.Errorf("the moved volume is unavailable; reconnect its drive: %w", callErr)
	}
	for start := 0; start < len(paths) && paths[start] != 0; {
		end := start
		for end < len(paths) && paths[end] != 0 {
			end++
		}
		root := syscall.UTF16ToString(paths[start:end])
		start = end + 1
		target := filepath.Join(root, location.Relative)
		actual, err := windowsMoveVolume(target)
		if err == nil && strings.EqualFold(actual.Volume, location.Volume) && pathsEqual(actual.Relative, location.Relative) {
			return target, nil
		}
	}
	return "", fmt.Errorf("the moved volume has no available mount path; reconnect its drive")
}
