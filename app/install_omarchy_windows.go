//go:build windows

package main

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const fastStartupKey = `SYSTEM\CurrentControlSet\Control\Session Manager\Power`

var propsys = syscall.NewLazyDLL(system32("propsys.dll"))

func diskLocked(path string) bool {
	// The launcher opens the disk without sharing, so a running Omarchy
	// makes this fail the same way Back up does.
	file, err := openBackupDisk(path)
	if err != nil {
		return true
	}
	file.Close()
	return false
}

func windowsInstallProbes() installProbes {
	return installProbes{
		diskLocked:  diskLocked,
		fastStartup: fastStartupEnabled,
		bitLocker: func(drive string) bitLockerState {
			value, ok := bitLockerProtection(drive)
			if !ok {
				return bitLockerUnknown
			}
			return bitLockerFromShell(value)
		},
		freeBytes:   platformDiskFreeBytes,
		systemDrive: systemDriveLabel,
	}
}

// bitLockerProtection reads a drive's BitLocker status the way Explorer does
// for its drive icons, through the shell property system. Unlike
// manage-bde or the BitLocker WMI class it needs no administrator rights.
func bitLockerProtection(drive string) (int32, bool) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	hr, _, _ := ole32.NewProc("CoInitializeEx").Call(0, 2)
	if uint32(hr) != 0x80010106 { // An existing COM apartment is also usable.
		if int32(hr) < 0 {
			return 0, false
		}
		defer ole32.NewProc("CoUninitialize").Call()
	}
	name, _ := syscall.UTF16PtrFromString("System.Volume.BitLockerProtection")
	var key propertyKey
	if hr, _, _ := propsys.NewProc("PSGetPropertyKeyFromName").Call(uintptr(unsafe.Pointer(name)), uintptr(unsafe.Pointer(&key))); int32(hr) < 0 {
		return 0, false
	}
	path, err := syscall.UTF16PtrFromString(drive + `\`)
	if err != nil {
		return 0, false
	}
	iidShellItem2 := comGUID{0x7e9fb0d3, 0x919f, 0x4307, [8]byte{0xab, 0x2e, 0x9b, 0x18, 0x60, 0x31, 0x0c, 0x93}}
	var item uintptr
	if hr, _, _ := shell32.NewProc("SHCreateItemFromParsingName").Call(uintptr(unsafe.Pointer(path)), 0, uintptr(unsafe.Pointer(&iidShellItem2)), uintptr(unsafe.Pointer(&item))); int32(hr) < 0 {
		return 0, false
	}
	defer recoveryCOMCall(item, 2)
	var value int32
	if hr := recoveryCOMCall(item, 16, uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&value))); int32(hr) < 0 { // IShellItem2::GetInt32
		return 0, false
	}
	return value, true
}

// openBitLockerSettings opens the page that turns BitLocker off: Device
// encryption in Settings on Windows Home, the BitLocker page in Control Panel
// on the editions that have one.
func openBitLockerSettings() {
	var key syscall.Handle
	path, _ := syscall.UTF16PtrFromString(`SOFTWARE\Microsoft\Windows NT\CurrentVersion`)
	if syscall.RegOpenKeyEx(syscall.HKEY_LOCAL_MACHINE, path, 0, syscall.KEY_READ, &key) == nil {
		edition := registryString(key, "EditionID")
		syscall.RegCloseKey(key)
		if edition != "" && !strings.HasPrefix(edition, "Core") {
			cmd := exec.Command(system32("control.exe"), "/name", "Microsoft.BitLockerDriveEncryption")
			if cmd.Start() == nil {
				cmd.Process.Release()
				return
			}
		}
	}
	openWindowsURL("ms-settings:deviceencryption")
}

// requestOmarchyShutdown asks the running Omarchy to shut down the way the
// window's close button does, then waits a little for it to finish.
func requestOmarchyShutdown(dir string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	c, err := dialQMPControl(ctx, qmpToolsPort)
	if err != nil {
		return err
	}
	err = c.Call(ctx, "system_powerdown", nil, nil)
	c.Close()
	if err != nil {
		return err
	}
	disk := filepath.Join(dir, "vm", "disk.raw")
	for deadline := time.Now().Add(15 * time.Second); time.Now().Before(deadline) && diskLocked(disk); {
		time.Sleep(500 * time.Millisecond)
	}
	return nil
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
// Windows. Each pass shows only what is left to do, and the install steps
// once nothing is.
func runInstallOmarchyUI(dir string) error {
	shutdownRequested := false
	for {
		r := assessInstallReadiness(dir, windowsInstallProbes())
		r.ShutdownRequested = shutdownRequested
		body, buttons := installPage(r)
		choice, err := chooseAction("Install Omarchy on this PC", body, installButtonLabels(buttons)...)
		if err != nil || choice == 0 {
			return err
		}
		switch buttons[choice-1].action {
		case installShutDown:
			if err := requestOmarchyShutdown(dir); err != nil {
				errorBox("Omarchy did not respond. Shut it down from its menu instead.\n\n" + err.Error())
			} else {
				shutdownRequested = true
			}
		case installFastStartup:
			code, err := runElevated("-disable-fast-startup")
			if err != nil {
				errorBox("Fast Startup could not be turned off:\n\n" + err.Error())
			} else if code == errorCancelled {
				// The user declined the Windows prompt; show the list again.
			} else if code != 0 {
				errorBox("Fast Startup could not be turned off. Turn it off in Control Panel > Power Options > Choose what the power buttons do.")
			}
		case installBitLocker:
			openBitLockerSettings()
		case installExportGuide:
			openWindowsURL(exportGuideURL)
			return nil
		case installDiskManagement:
			if err := shellOpen("diskmgmt.msc"); err != nil {
				errorBox(err.Error())
			}
		case installGuide:
			openWindowsURL(dualBootGuideURL)
		case installDone:
			return nil
		}
	}
}

func runDisableFastStartupHelper() {
	os.Exit(disableFastStartup())
}
