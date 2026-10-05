//go:build windows

package main

import (
	"fmt"
	"syscall"
	"time"
	"unsafe"
)

var procGetSystemPowerStatus = syscall.NewLazyDLL("kernel32.dll").NewProc("GetSystemPowerStatus")

var windowsBatteryDetails batteryDetailsCache
var windowsBatteryPresence batteryPresenceCache

func hostBatteryLine() (string, error) {
	var status systemPowerStatus
	ok, _, err := procGetSystemPowerStatus.Call(uintptr(unsafe.Pointer(&status)))
	if ok == 0 {
		return "", fmt.Errorf("GetSystemPowerStatus: %w", err)
	}
	snapshot := windowsBatteryPresence.snapshot(status)
	if snapshot == nil {
		return "", nil
	}
	line, err := encodeBatterySnapshot(*snapshot)
	if err != nil {
		return "", err
	}
	details := batteryDetails{}
	if snapshot.Present {
		details = windowsBatteryDetails.get(time.Now(), queryWindowsBatteryDetails)
	} else if status.BatteryFlag != 0xff {
		windowsBatteryDetails.clear()
	}
	return line + encodeBatteryDetailsLine(details), nil
}
