//go:build windows

package main

import (
	"fmt"
	"syscall"
	"unsafe"
)

var batterySetupAPI = syscall.NewLazyDLL("setupapi.dll")
var batteryClassGUID = syscall.GUID{Data1: 0x72631e54, Data2: 0x78a4, Data3: 0x11d0, Data4: [8]byte{0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}}

func queryWindowsBatteryDetails() (batteryDetails, error) {
	fail := func(err error) (batteryDetails, error) { return batteryDetails{}, err }
	devices, _, err := batterySetupAPI.NewProc("SetupDiGetClassDevsW").Call(uintptr(unsafe.Pointer(&batteryClassGUID)), 0, 0, 0x12)
	if devices == ^uintptr(0) {
		return fail(err)
	}
	defer batterySetupAPI.NewProc("SetupDiDestroyDeviceInfoList").Call(devices)
	var iface struct {
		Size     uint32
		Class    syscall.GUID
		Flags    uint32
		Reserved uintptr
	}
	iface.Size = uint32(unsafe.Sizeof(iface))
	ok, _, err := batterySetupAPI.NewProc("SetupDiEnumDeviceInterfaces").Call(devices, 0, uintptr(unsafe.Pointer(&batteryClassGUID)), 0, uintptr(unsafe.Pointer(&iface)))
	if ok == 0 {
		return fail(err)
	}
	detailProc := batterySetupAPI.NewProc("SetupDiGetDeviceInterfaceDetailW")
	var size uint32
	detailProc.Call(devices, uintptr(unsafe.Pointer(&iface)), 0, 0, uintptr(unsafe.Pointer(&size)), 0)
	if size < 6 || size > 65536 {
		return fail(fmt.Errorf("invalid battery device path size: %d", size))
	}
	detail := make([]byte, size)
	headerSize := uint32(6)
	if unsafe.Sizeof(uintptr(0)) == 8 {
		headerSize = 8
	}
	*(*uint32)(unsafe.Pointer(&detail[0])) = headerSize
	ok, _, err = detailProc.Call(devices, uintptr(unsafe.Pointer(&iface)), uintptr(unsafe.Pointer(&detail[0])), uintptr(size), 0, 0)
	if ok == 0 {
		return fail(err)
	}
	device, err := syscall.CreateFile((*uint16)(unsafe.Pointer(&detail[4])), syscall.GENERIC_READ|syscall.GENERIC_WRITE, syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		return fail(err)
	}
	defer syscall.CloseHandle(device)
	var timeout, tag, returned uint32
	err = syscall.DeviceIoControl(device, 0x294040, (*byte)(unsafe.Pointer(&timeout)), 4, (*byte)(unsafe.Pointer(&tag)), 4, &returned, nil)
	if err != nil {
		return fail(err)
	}
	if tag == 0 || returned != 4 {
		return fail(fmt.Errorf("battery tag unavailable"))
	}
	query := struct{ Tag, Level, AtRate uint32 }{Tag: tag}
	ioctl := func(level uint32, output *byte, size uint32) error {
		query.Level = level
		return syscall.DeviceIoControl(device, 0x294044, (*byte)(unsafe.Pointer(&query)), 12, output, size, &returned, nil)
	}
	var info batteryInformation
	if err := ioctl(0, (*byte)(unsafe.Pointer(&info)), uint32(unsafe.Sizeof(info))); err != nil {
		return fail(err)
	}
	if returned != uint32(unsafe.Sizeof(info)) {
		return fail(fmt.Errorf("short battery information"))
	}
	readName := func(level uint32) (string, error) {
		var buf [256]uint16
		if err := ioctl(level, (*byte)(unsafe.Pointer(&buf[0])), uint32(unsafe.Sizeof(buf))); err != nil {
			return "", err
		}
		if returned < 2 || returned > uint32(unsafe.Sizeof(buf)) || returned%2 != 0 || buf[returned/2-1] != 0 {
			return "", fmt.Errorf("invalid battery name")
		}
		return syscall.UTF16ToString(buf[:returned/2]), nil
	}
	manufacturer, err := readName(6)
	if err != nil {
		return fail(err)
	}
	model, err := readName(4)
	if err != nil {
		return fail(err)
	}
	return batteryDetailsFromWindows(info, manufacturer, model), nil
}
