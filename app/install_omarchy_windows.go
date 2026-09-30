//go:build windows

package main

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

const fastStartupKey = `SYSTEM\CurrentControlSet\Control\Session Manager\Power`

func windowsInstallProbes() installProbes {
	return installProbes{
		diskLocked: func(path string) bool {
			// The launcher opens the disk without sharing, so a running
			// Omarchy makes this fail the same way Back up does.
			file, err := openBackupDisk(path)
			if err != nil {
				return true
			}
			file.Close()
			return false
		},
		fastStartup: fastStartupEnabled,
		freeBytes:   platformDiskFreeBytes,
		systemDrive: systemDriveLabel,
	}
}

func readLocalMachineDword(path, name string) (uint32, bool) {
	keyPath, _ := syscall.UTF16PtrFromString(path)
	var key syscall.Handle
	if err := syscall.RegOpenKeyEx(syscall.HKEY_LOCAL_MACHINE, keyPath, 0, syscall.KEY_READ, &key); err != nil {
		return 0, false
	}
	defer syscall.RegCloseKey(key)
	valueName, _ := syscall.UTF16PtrFromString(name)
	var kind, value uint32
	size := uint32(4)
	if err := syscall.RegQueryValueEx(key, valueName, nil, &kind, (*byte)(unsafe.Pointer(&value)), &size); err != nil {
		return 0, false
	}
	if kind != syscall.REG_DWORD || size != 4 {
		return 0, false
	}
	return value, true
}

// fastStartupEnabled reads the Fast Startup switch. It only matters while
// hibernation is available, since Fast Startup is a hibernated kernel session.
func fastStartupEnabled() (on, known bool) {
	value, ok := readLocalMachineDword(fastStartupKey, "HiberbootEnabled")
	if !ok {
		return false, false
	}
	if hibernate, ok := readLocalMachineDword(`SYSTEM\CurrentControlSet\Control\Power`, "HibernateEnabled"); ok && hibernate == 0 {
		return false, true
	}
	return value != 0, true
}

// disableFastStartup runs in the elevated helper started with
// -disable-fast-startup and reports through its exit code.
func disableFastStartup() int {
	keyPath, _ := syscall.UTF16PtrFromString(fastStartupKey)
	var key syscall.Handle
	if err := syscall.RegOpenKeyEx(syscall.HKEY_LOCAL_MACHINE, keyPath, 0, syscall.KEY_SET_VALUE, &key); err != nil {
		logf("disable fast startup: %v", err)
		return 1
	}
	defer syscall.RegCloseKey(key)
	if err := regSetDword(key, "HiberbootEnabled", 0); err != nil {
		logf("disable fast startup: %v", err)
		return 1
	}
	return 0
}

// shellOpen asks Windows to open a file or console, which prompts for
// elevation itself when the target needs it (Disk Management does).
func shellOpen(target string) error {
	file, _ := syscall.UTF16PtrFromString(target)
	info := shellExecuteInfo{lpFile: file, nShow: 1}
	info.cbSize = uint32(unsafe.Sizeof(info))
	if r, _, err := procShellExecuteExW.Call(uintptr(unsafe.Pointer(&info))); r == 0 {
		if errno, ok := err.(syscall.Errno); ok && int(errno) == errorCancelled {
			return nil
		}
		return fmt.Errorf("opening %s: %v", target, err)
	}
	return nil
}

// runInstallOmarchyUI walks through getting ready to install Omarchy next to
// Windows: the checklist first, then the installation steps.
func runInstallOmarchyUI(dir string) error {
	for {
		body, buttons := installChecklist(assessInstallReadiness(dir, windowsInstallProbes()))
		choice, err := chooseAction("Install Omarchy on this PC", body, installButtonLabels(buttons)...)
		if err != nil || choice == 0 {
			return err
		}
		switch buttons[choice-1].action {
		case installFastStartup:
			code, err := runElevated("-disable-fast-startup")
			if err != nil {
				errorBox("Fast Startup could not be turned off:\n\n" + err.Error())
			} else if code == errorCancelled {
				// The user declined the Windows prompt; show the checklist again.
			} else if code != 0 {
				errorBox("Fast Startup could not be turned off. Turn it off in Control Panel > Power Options > Choose what the power buttons do.")
			}
		case installEncryption:
			openWindowsURL("ms-settings:deviceencryption")
		case installExportGuide:
			openWindowsURL(exportGuideURL)
			return nil
		case installNext:
			return runInstallStepsUI()
		case installDone:
			return nil
		}
	}
}

func runInstallStepsUI() error {
	body, buttons := installSteps()
	for {
		choice, err := chooseAction("Install Omarchy on this PC", body, installButtonLabels(buttons)...)
		if err != nil || choice == 0 {
			return err
		}
		switch buttons[choice-1].action {
		case installDiskManagement:
			if err := shellOpen("diskmgmt.msc"); err != nil {
				errorBox(err.Error())
			}
		case installDualBootGuide:
			openWindowsURL(dualBootGuideURL)
		case installMigrationGuide:
			openWindowsURL(migrationGuideURL)
		case installDone:
			return nil
		}
	}
}

func runDisableFastStartupHelper() {
	os.Exit(disableFastStartup())
}
