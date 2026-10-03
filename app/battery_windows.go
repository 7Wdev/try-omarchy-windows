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

func hostBatteryLine() (string, error) {
	var status systemPowerStatus
	ok, _, err := procGetSystemPowerStatus.Call(uintptr(unsafe.Pointer(&status)))
	if ok == 0 {
		return "", fmt.Errorf("GetSystemPowerStatus: %w", err)
	}
	line, err := encodeBatteryLine(status)
	if err != nil {
		return "", err
	}
	details := batteryDetails{}
	if batteryFromWindows(status).Present {
		details = windowsBatteryDetails.get(time.Now(), queryWindowsBatteryDetails)
	} else {
		windowsBatteryDetails.clear()
	}
	return line + encodeBatteryDetailsLine(details), nil
}
