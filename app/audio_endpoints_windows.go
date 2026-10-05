//go:build windows

package main

import (
	"fmt"
	"runtime"
	"syscall"
	"unsafe"
)

var (
	ole32MMDevice          = syscall.NewLazyDLL("ole32.dll")
	procMMCLSIDFromString  = ole32MMDevice.NewProc("CLSIDFromString")
	procMMCoInitializeEx   = ole32MMDevice.NewProc("CoInitializeEx")
	procMMCoUninitialize   = ole32MMDevice.NewProc("CoUninitialize")
	procMMCoCreateInstance = ole32MMDevice.NewProc("CoCreateInstance")
	procMMCoTaskMemFree    = ole32MMDevice.NewProc("CoTaskMemFree")
	procMMPropVariantClear = ole32MMDevice.NewProc("PropVariantClear")
)

// Core Audio endpoint IDs ({0.0.0.00000000}.{guid}) are stable across renames
// and reboots. Friendly names are what SDL matches, so both are returned and
// the launcher resolves ID to name at each start.
const (
	mmDeviceEnumeratorCLSID = "{BCDE0395-E52F-467C-8E3D-C4579291692E}"
	mmDeviceEnumeratorIID   = "{A95664D2-9614-4F35-A746-DE8DB63617E6}"
	pkeyAudioDeviceFormat   = "{F19F064D-082C-4E27-BC73-6882A1BB8E4C}"
	pkeyDeviceFriendlyName  = "{A45C254E-DF1C-4EFD-8020-67D146A850E0},14"
)

// These layouts match GUID, PROPERTYKEY and PROPVARIANT on 64-bit Windows.
// Named fields give the COM buffers the alignment that byte arrays alone do
// not guarantee.
type mmGUIDValue struct {
	Data1 uint32
	Data2 uint16
	Data3 uint16
	Data4 [8]byte
}

type mmPropertyKey struct {
	FormatID mmGUIDValue
	ID       uint32
}

type mmPropVariant struct {
	Type     uint16
	Reserved [3]uint16
	Value    uintptr
	Extra    uintptr
}

func listAudioEndpoints() (mmDeviceList, error) {
	type result struct {
		list mmDeviceList
		err  error
	}
	done := make(chan result, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		list, err := enumerateMMAudioEndpoints()
		done <- result{list, err}
	}()
	r := <-done
	return r.list, r.err
}

func mmGUID(text string) (g mmGUIDValue, err error) {
	wide, err := syscall.UTF16PtrFromString(text)
	if err != nil {
		return g, err
	}
	hr, _, _ := procMMCLSIDFromString.Call(
		uintptr(unsafe.Pointer(wide)), uintptr(unsafe.Pointer(&g)))
	if hr != 0 {
		return g, fmt.Errorf("bad guid %s", text)
	}
	return g, nil
}

func mmWideString(p uintptr) string {
	if p == 0 {
		return ""
	}
	var text []uint16
	for offset := uintptr(0); offset < 8192; offset += 2 {
		u := *(*uint16)(unsafe.Pointer(p + offset))
		if u == 0 {
			break
		}
		text = append(text, u)
	}
	return syscall.UTF16ToString(text)
}

func mmVCall(obj uintptr, slot int, a1, a2, a3, a4 uintptr) uintptr {
	if obj == 0 {
		return 0xC0000102 // E_POINTER-style failure
	}
	vtable := *(*uintptr)(unsafe.Pointer(obj))
	fn := *(*uintptr)(unsafe.Pointer(vtable + uintptr(slot)*unsafe.Sizeof(uintptr(0))))
	r, _, _ := syscall.SyscallN(fn, obj, a1, a2, a3, a4)
	return r
}

func enumerateMMAudioEndpoints() (mmDeviceList, error) {
	var list mmDeviceList
	hr, _, _ := procMMCoInitializeEx.Call(0, 2)
	if int32(hr) < 0 && uint32(hr) != 0x80010106 { // RPC_E_CHANGED_MODE
		return list, fmt.Errorf("COM init failed: 0x%x", hr)
	}
	if uint32(hr) != 0x80010106 {
		defer procMMCoUninitialize.Call()
	}
	clsid, err := mmGUID(mmDeviceEnumeratorCLSID)
	if err != nil {
		return list, err
	}
	iid, err := mmGUID(mmDeviceEnumeratorIID)
	if err != nil {
		return list, err
	}
	var enumerator uintptr
	hr, _, _ = procMMCoCreateInstance.Call(
		uintptr(unsafe.Pointer(&clsid)), 0, 1,
		uintptr(unsafe.Pointer(&iid)), uintptr(unsafe.Pointer(&enumerator)))
	if int32(hr) < 0 || enumerator == 0 {
		return list, fmt.Errorf("audio endpoint enumeration is unavailable")
	}
	defer mmVCall(enumerator, 2, 0, 0, 0, 0)
	pkey, err := mmGUID(pkeyDeviceFriendlyName[:38])
	if err != nil {
		return list, err
	}
	var pid uint32
	fmt.Sscanf(pkeyDeviceFriendlyName[39:], "%d", &pid)
	pkeyValue := mmPropertyKey{FormatID: pkey, ID: pid}
	for direction := 0; direction < 2; direction++ {
		// SDL WASAPI uses eConsole (0), independently for render and capture.
		var defaultDevice uintptr
		if int32(mmVCall(enumerator, 4, uintptr(direction), 0, uintptr(unsafe.Pointer(&defaultDevice)), 0)) >= 0 && defaultDevice != 0 {
			rate := mmAudioSampleRate(defaultDevice)
			if direction == 0 {
				list.DefaultOutputRate = rate
			} else {
				list.DefaultInputRate = rate
			}
			mmVCall(defaultDevice, 2, 0, 0, 0, 0)
		}
	}
	for direction, out := range []*[]audioEndpointInfo{&list.Output, &list.Input} {
		var collection uintptr
		// IMMDeviceEnumerator::EnumAudioEndpoints(eRender=0, eCapture=1, DEVICE_STATE_ACTIVE=1)
		hr = mmVCall(enumerator, 3, uintptr(direction), 1,
			uintptr(unsafe.Pointer(&collection)), 0)
		if int32(hr) < 0 || collection == 0 {
			continue
		}
		var count uint32
		mmVCall(collection, 3, uintptr(unsafe.Pointer(&count)), 0, 0, 0)
		if count > 64 {
			count = 64
		}
		*out = append(*out, collectMMAudioEndpoints(count, func(i uint32) (audioEndpointInfo, error) {
			var device uintptr
			if int32(mmVCall(collection, 4, uintptr(i), uintptr(unsafe.Pointer(&device)), 0, 0)) < 0 || device == 0 {
				return audioEndpointInfo{}, fmt.Errorf("audio endpoint is unavailable")
			}
			defer mmVCall(device, 2, 0, 0, 0, 0)
			var idPtr uintptr
			if int32(mmVCall(device, 5, uintptr(unsafe.Pointer(&idPtr)), 0, 0, 0)) < 0 || idPtr == 0 {
				return audioEndpointInfo{}, fmt.Errorf("audio endpoint ID is unavailable")
			}
			id := mmWideString(idPtr)
			procMMCoTaskMemFree.Call(idPtr)
			name, err := mmFriendlyName(device, &pkeyValue)
			if err != nil {
				return audioEndpointInfo{}, err
			}
			return audioEndpointInfo{ID: id, Name: name, SampleRate: mmAudioSampleRate(device)}, nil
		})...)

		mmVCall(collection, 2, 0, 0, 0, 0)
	}
	return list, nil
}

func collectMMAudioEndpoints(count uint32, read func(uint32) (audioEndpointInfo, error)) []audioEndpointInfo {
	var endpoints []audioEndpointInfo
	for i := uint32(0); i < count; i++ {
		endpoint, err := read(i)
		if err != nil {
			logf("audio: skipping unavailable endpoint: %v", err)
			continue
		}
		if endpoint.ID != "" {
			endpoints = append(endpoints, endpoint)
		}
	}
	return endpoints
}

func mmFriendlyName(device uintptr, pkey *mmPropertyKey) (string, error) {
	var store uintptr
	hr := mmVCall(device, 4, 0, uintptr(unsafe.Pointer(&store)), 0, 0)
	if int32(hr) < 0 || store == 0 {
		return "", fmt.Errorf("opening audio endpoint properties failed: 0x%x", hr)
	}
	defer mmVCall(store, 2, 0, 0, 0, 0)
	var value mmPropVariant
	hr = mmVCall(store, 5, uintptr(unsafe.Pointer(pkey)),
		uintptr(unsafe.Pointer(&value)), 0, 0)
	if int32(hr) < 0 {
		return "", fmt.Errorf("reading audio endpoint name failed: 0x%x", hr)
	}
	defer procMMPropVariantClear.Call(uintptr(unsafe.Pointer(&value)))
	// VT_LPWSTR = 31
	if value.Type != 31 {
		return "", fmt.Errorf("audio endpoint name has unexpected type %d", value.Type)
	}
	text := mmWideString(value.Value)
	if text == "" {
		return "", fmt.Errorf("audio endpoint name is empty")
	}
	return text, nil
}

// Read the shared-mode format without activating a client or opening a stream.
// Failure only leaves the rate unknown; endpoint selection must keep working.
func mmAudioSampleRate(device uintptr) int {
	formatID, err := mmGUID(pkeyAudioDeviceFormat)
	if err != nil {
		return 0
	}
	key := mmPropertyKey{FormatID: formatID, ID: 0}
	var store uintptr
	if int32(mmVCall(device, 4, 0, uintptr(unsafe.Pointer(&store)), 0, 0)) < 0 || store == 0 {
		return 0
	}
	defer mmVCall(store, 2, 0, 0, 0, 0)
	var value mmPropVariant
	if int32(mmVCall(store, 5, uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&value)), 0, 0)) < 0 {
		return 0
	}
	defer procMMPropVariantClear.Call(uintptr(unsafe.Pointer(&value)))
	// VT_BLOB: cbSize is the first DWORD of the union, pBlobData the next pointer.
	if value.Type != 65 || uint32(value.Value) < 18 || value.Extra == 0 {
		return 0
	}
	format := unsafe.Slice((*byte)(unsafe.Pointer(value.Extra)), 18)
	return audioSampleRateFromWaveFormat(format)
}
