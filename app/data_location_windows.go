//go:build windows

package main

import (
	"fmt"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

var (
	procGetDriveTypeW         = syscall.NewLazyDLL("kernel32.dll").NewProc("GetDriveTypeW")
	procGetVolumePathNameW    = syscall.NewLazyDLL("kernel32.dll").NewProc("GetVolumePathNameW")
	procGetVolumeInformationW = syscall.NewLazyDLL("kernel32.dll").NewProc("GetVolumeInformationW")
)

const (
	driveRemovable          = 2
	driveFixed              = 3
	driveRAMDisk            = 6
	fileSupportsSparseFiles = 0x40
)

func supportedLocalDriveType(driveType uintptr) bool {
	return driveType == driveRemovable || driveType == driveFixed || driveType == driveRAMDisk
}

func dataLocationVolumeRoot(path string) (string, error) {
	if strings.HasPrefix(filepath.VolumeName(path), `\\`) {
		return "", uiError(uiText("error.location.network"), nil)
	}
	existing, err := existingDiskPath(path)
	if err != nil {
		return "", err
	}
	pathPtr, err := syscall.UTF16PtrFromString(existing)
	if err != nil {
		return "", err
	}
	var root [32768]uint16
	ok, _, callErr := procGetVolumePathNameW.Call(
		uintptr(unsafe.Pointer(pathPtr)), uintptr(unsafe.Pointer(&root[0])), uintptr(len(root)),
	)
	if ok == 0 {
		return "", callErr
	}
	return syscall.UTF16ToString(root[:]), nil
}

func dataLocationIsLocal(path string) bool {
	root, err := dataLocationVolumeRoot(path)
	if err != nil {
		return false
	}
	rootPtr, err := syscall.UTF16PtrFromString(root)
	if err != nil {
		return false
	}
	driveType, _, _ := procGetDriveTypeW.Call(uintptr(unsafe.Pointer(rootPtr)))
	return supportedLocalDriveType(driveType)
}

func dataLocationSupportsSparseFiles(path string) (bool, error) {
	root, err := dataLocationVolumeRoot(path)
	if err != nil {
		return false, err
	}
	rootPtr, err := syscall.UTF16PtrFromString(root)
	if err != nil {
		return false, err
	}
	var flags uint32
	ok, _, callErr := procGetVolumeInformationW.Call(
		uintptr(unsafe.Pointer(rootPtr)), 0, 0, 0, 0,
		uintptr(unsafe.Pointer(&flags)), 0, 0,
	)
	if ok == 0 {
		return false, callErr
	}
	return flags&fileSupportsSparseFiles != 0, nil
}

func validateStandardDataDrive(path string) error {
	if !dataLocationIsLocal(path) {
		return uiError(uiText("error.location.local_drive"), nil)
	}
	supported, err := dataLocationSupportsSparseFiles(path)
	if err != nil {
		return fmt.Errorf("checking filesystem support: %w", err)
	}
	if !supported {
		return uiError(uiText("error.location.filesystem"), nil)
	}
	return nil
}

// chooseFirstRunDataDirectory asks once, before any payload is downloaded.
// The folder picker selects a drive or parent folder and the launcher keeps
// its files together in a TryOmarchy child directory.
func chooseFirstRunDataDirectory(defaultDir string) (string, bool, error) {
	for {
		answer, err := chooseAction(uiText("location.title"),
			uiTextWith("location.body", map[string]string{"path": defaultDir}),
			uiText("location.use_default"), uiText("location.choose_other"), uiText("location.cancel"),
		)
		if err != nil {
			return "", false, err
		}
		switch answer {
		case 0, 3:
			return "", false, nil
		case 1:
			if err := validateStandardDataDrive(defaultDir); err != nil {
				errorBox(uiTextWith("location.error.default", map[string]string{"error": err.Error()}))
				continue
			}
			return defaultDir, true, nil
		case 2:
			parent, ok := browseForFolder(0, uiText("location.browse"))
			if !ok {
				continue
			}
			selected, err := dataDirectoryForSelection(parent)
			if err != nil {
				errorBox(uiTextWith("location.error.unusable", map[string]string{"error": err.Error()}))
				continue
			}
			if err := validateStandardDataDrive(selected); err != nil {
				errorBox(uiTextWith("location.error.unusable", map[string]string{"error": err.Error()}))
				continue
			}
			selectable, err := standardDataDirectorySelectable(selected)
			if err != nil {
				errorBox(uiTextWith("location.error.inspect", map[string]string{"error": err.Error()}))
				continue
			}
			if !selectable {
				errorBox(uiText("location.error.not_empty"))
				continue
			}
			if err := ensureDataDirectoryWritable(selected); err != nil {
				errorBox(uiTextWith("location.error.write", map[string]string{"error": err.Error()}))
				continue
			}
			available, err := diskFreeBytes(selected)
			if err != nil {
				errorBox(uiTextWith("location.error.free_space", map[string]string{"error": err.Error()}))
				continue
			}
			if msgBox(uiTextWith("location.confirm", map[string]string{"path": selected, "space": formatGiB(available)}), mbYesNo|mbIconQuestion) != idYes {
				continue
			}
			return selected, true, nil
		default:
			return "", false, nil
		}
	}
}
