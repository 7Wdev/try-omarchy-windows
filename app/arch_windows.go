//go:build windows

package main

import (
	"runtime"
	"unsafe"
)

// IMAGE_FILE_MACHINE_* from winnt.h: what IsWow64Process2 reports for the
// CPU Windows itself runs on, no matter which architecture the asking process
// was built for.
const (
	imageFileMachineAmd64 = 0x8664
	imageFileMachineArm64 = 0xaa64
)

var (
	procGetCurrentProcess = kernel32.NewProc("GetCurrentProcess")
	procIsWow64Process2   = kernel32.NewProc("IsWow64Process2")
)

// nativeMachine returns the IMAGE_FILE_MACHINE_* of the CPU Windows itself is
// running on, or 0 when the answer is unavailable (the API shipped in
// Windows 10 1607; on anything older an x64 process sits on x64 hardware).
// The OS is asked instead of trusting runtime.GOARCH because the launcher
// ships as amd64 and Windows happily runs it on ARM64 PCs under emulation,
// where GOARCH still says amd64 while the native hypervisor DLLs can't load.
func nativeMachine() uint16 {
	if procIsWow64Process2.Find() != nil {
		return 0
	}
	handle, _, _ := procGetCurrentProcess.Call()
	var processMachine, native uint16
	if r, _, _ := procIsWow64Process2.Call(handle,
		uintptr(unsafe.Pointer(&processMachine)), uintptr(unsafe.Pointer(&native))); r == 0 {
		return 0
	}
	return native
}

// hostArchUnsupportedReason returns "" when this machine can run Try Omarchy,
// otherwise the message to show the user. The launcher, the WINQ-EMU runtime
// and the guest image are all x86_64-only, so ARM64 Windows PCs cannot run
// the app - and before this check they got the worst possible tour: an
// emulated x64 process cannot load the native WinHvPlatform.dll, so WHPX
// looked missing, setup helpfully enabled the WHP feature and rebooted, and
// only then blamed firmware virtualization settings (VT-x/SVM) that ARM
// firmware does not even have.
func hostArchUnsupportedReason() string {
	if runtime.GOARCH != "amd64" {
		// A future native ARM64 launcher would still ship the x86_64 guest
		// and runtime today, so it gets the same message.
		return intelAMDOnlyMessage(uiText("arch.arm64"))
	}
	if nativeMachine() == imageFileMachineArm64 {
		return intelAMDOnlyMessage(uiText("arch.arm64_emulated"))
	}
	return ""
}

// intelAMDOnlyMessage is the honest version of the ARM64 story: nothing on
// the machine is broken or misconfigured - the hardware line sits elsewhere.
func intelAMDOnlyMessage(detail string) string {
	return uiTextWith("arch.unsupported", map[string]string{"detail": detail, "url": "https://github.com/omacom/try-omarchy-windows/issues"})
}
