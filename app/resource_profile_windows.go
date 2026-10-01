//go:build windows

package main

import (
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

var (
	procGetSystemTimes          = kernel32.NewProc("GetSystemTimes")
	procK32GetProcessMemoryInfo = kernel32.NewProc("K32GetProcessMemoryInfo")
)

func systemCPUTimes() (idle, kernel, user uint64, ok bool) {
	r, _, _ := procGetSystemTimes.Call(uintptr(unsafe.Pointer(&idle)), uintptr(unsafe.Pointer(&kernel)), uintptr(unsafe.Pointer(&user)))
	return idle, kernel, user, r != 0
}

func measureHostResources(sampleCPU bool) hostResources {
	h := hostResources{LogicalCPUs: runtime.NumCPU()}
	// GetSystemTimes is per processor group on hosts with more than 64 CPUs;
	// do not extrapolate one group's load to the whole machine.
	if sampleCPU && h.LogicalCPUs <= 64 {
		i0, k0, u0, ok := systemCPUTimes()
		if ok {
			time.Sleep(750 * time.Millisecond)
			i1, k1, u1, ok := systemCPUTimes()
			if ok {
				h.CPUBusy, h.CPUKnown = cpuBusyFraction(i0, k0, u0, i1, k1, u1)
			}
		}
	}
	h.TotalMiB, h.AvailableMiB = availMemMiB()
	return h
}

// runningGuestMiB is the memory a running Omarchy holds, found through its
// window, or 0 when it is not running. Settings opened from the tray counts
// it as available: the next boot starts after this one has shut down.
func runningGuestMiB() int {
	runningInstanceWindow = 0
	procEnumWindows.Call(runningInstanceCallback, 0)
	hwnd := runningInstanceWindow
	if hwnd == 0 {
		return 0
	}
	var class [64]uint16
	procGetClassNameW.Call(hwnd, uintptr(unsafe.Pointer(&class[0])), uintptr(len(class)))
	if syscall.UTF16ToString(class[:]) != "SDL_app" {
		return 0
	}
	var pid uint32
	procGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&pid)))
	const queryLimitedInformation, vmRead = 0x1000, 0x0010
	process, _, _ := procOpenProcess.Call(queryLimitedInformation|vmRead, 0, uintptr(pid))
	if process == 0 {
		return 0
	}
	defer syscall.CloseHandle(syscall.Handle(process))
	var counters struct {
		cb, pageFaultCount                 uint32
		peakWorkingSetSize, workingSetSize uintptr
		quotaPeakPagedPool, quotaPagedPool uintptr
		quotaPeakNonPaged, quotaNonPaged   uintptr
		pagefileUsage, peakPagefileUsage   uintptr
	}
	counters.cb = uint32(unsafe.Sizeof(counters))
	if ok, _, _ := procK32GetProcessMemoryInfo.Call(process, uintptr(unsafe.Pointer(&counters)), uintptr(counters.cb)); ok == 0 {
		return 0
	}
	return int(counters.workingSetSize >> 20)
}
