//go:build windows

package main

import (
	"fmt"
	"math"
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// One MTA thread owns the enumerator, endpoint and setters. Callbacks only wake
// it, never call COM, write the socket or wait for the bridge. Endpoint changes
// use IMMNotificationClient; volume/mute use IAudioEndpointVolumeCallback.
var (
	volumeIUnknownIID       = mmGUIDValue{Data4: [8]byte{0xc0, 0, 0, 0, 0, 0, 0, 0x46}}
	volumeCallbackIID       = mmGUIDValue{Data1: 0x657804fa, Data2: 0xd6ad, Data3: 0x4496, Data4: [8]byte{0x8a, 0x60, 0x35, 0x27, 0x52, 0xaf, 0x4f, 0x89}}
	volumeDeviceCallbackIID = mmGUIDValue{Data1: 0x7991eec9, Data2: 0x7e89, Data3: 0x4d85, Data4: [8]byte{0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}}
	procVolumeCreateGuid    = ole32MMDevice.NewProc("CoCreateGuid")
	volumeCallbackTable     = [8]uintptr{
		syscall.NewCallback(volumeQueryInterface), syscall.NewCallback(volumeAddRef), syscall.NewCallback(volumeRelease),
		syscall.NewCallback(volumeOnNotify),
	}
	volumeDeviceCallbackTable = [8]uintptr{
		syscall.NewCallback(volumeQueryInterface), syscall.NewCallback(volumeAddRef), syscall.NewCallback(volumeRelease),
		syscall.NewCallback(volumeDeviceStateChanged), syscall.NewCallback(volumeDeviceAdded), syscall.NewCallback(volumeDeviceAdded),
		syscall.NewCallback(volumeDefaultChanged), syscall.NewCallback(volumePropertyChanged),
	}
	volumeCallbacks sync.Map
)

type volumeCOMCallback struct {
	vtable  *[8]uintptr
	iid     mmGUIDValue
	context mmGUIDValue
	wake    chan struct{}
	refs    atomic.Int32
	pin     runtime.Pinner
}

func newVolumeCOMCallback(wake chan struct{}, devices bool, context mmGUIDValue) *volumeCOMCallback {
	c := &volumeCOMCallback{wake: wake, iid: volumeCallbackIID, context: context, vtable: &volumeCallbackTable}
	if devices {
		c.iid, c.vtable = volumeDeviceCallbackIID, &volumeDeviceCallbackTable
	}
	c.refs.Store(1)
	c.pin.Pin(c)
	volumeCallbacks.Store(c, struct{}{})
	return c
}

func volumeQueryInterface(this, iid, out uintptr) uintptr {
	if iid == 0 || out == 0 {
		return 0x80004003
	}
	*(*uintptr)(unsafe.Pointer(out)) = 0
	c := (*volumeCOMCallback)(unsafe.Pointer(this))
	g := *(*mmGUIDValue)(unsafe.Pointer(iid))
	if g != volumeIUnknownIID && g != c.iid {
		return 0x80004002
	}
	volumeAddRef(this)
	*(*uintptr)(unsafe.Pointer(out)) = this
	return 0
}

func volumeAddRef(this uintptr) uintptr {
	return uintptr((*volumeCOMCallback)(unsafe.Pointer(this)).refs.Add(1))
}

func volumeRelease(this uintptr) uintptr {
	c := (*volumeCOMCallback)(unsafe.Pointer(this))
	refs := c.refs.Add(-1)
	if refs == 0 {
		volumeCallbacks.Delete(c)
		c.pin.Unpin()
	}
	return uintptr(refs)
}

func volumeWake(this uintptr) uintptr {
	c := (*volumeCOMCallback)(unsafe.Pointer(this))
	select {
	case c.wake <- struct{}{}:
	default:
	}
	return 0
}

func volumeOnNotify(this, data uintptr) uintptr {
	// AUDIO_VOLUME_NOTIFICATION_DATA begins with the setter's event GUID.
	c := (*volumeCOMCallback)(unsafe.Pointer(this))
	if data != 0 && *(*mmGUIDValue)(unsafe.Pointer(data)) != c.context {
		return volumeWake(this)
	}
	return 0
}

func volumeDefaultChanged(this, flow, role, id uintptr) uintptr {
	if flow == 0 && role == 0 {
		return volumeWake(this)
	} // eRender, eConsole (SDL's role)
	return 0
}
func volumeDeviceStateChanged(this, id, state uintptr) uintptr { return volumeWake(this) }
func volumeDeviceAdded(this, id uintptr) uintptr               { return volumeWake(this) }

// PROPERTYKEY is passed by value. Windows x64 passes this 20-byte struct indirectly.
func volumePropertyChanged(this, id, key uintptr) uintptr { return 0 }

type volumeEndpointResult struct {
	value endpointVolume
	err   error
}
type volumeEndpointCommand struct {
	value    endpointVolume
	expected endpointVolume
	done     chan volumeEndpointResult
}
type windowsVolumeEndpoint struct {
	changes  chan endpointVolume
	commands chan volumeEndpointCommand
	stop     chan struct{}
	done     chan struct{}
}

func startWindowsVolumeEndpoint() (*windowsVolumeEndpoint, endpointVolume, error) {
	b := &windowsVolumeEndpoint{changes: make(chan endpointVolume, 1), commands: make(chan volumeEndpointCommand), stop: make(chan struct{}), done: make(chan struct{})}
	ready := make(chan volumeEndpointResult, 1)
	go b.run(ready)
	r := <-ready
	if r.err != nil {
		<-b.done
		return nil, r.value, r.err
	}
	return b, r.value, nil
}

func (b *windowsVolumeEndpoint) close() { close(b.stop); <-b.done }
func (b *windowsVolumeEndpoint) set(value, expected endpointVolume) (endpointVolume, error) {
	command := volumeEndpointCommand{value, expected, make(chan volumeEndpointResult, 1)}
	b.commands <- command
	r := <-command.done
	return r.value, r.err
}

func (b *windowsVolumeEndpoint) run(ready chan<- volumeEndpointResult) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	defer close(b.done)
	hr, _, _ := procMMCoInitializeEx.Call(0, 0) // COINIT_MULTITHREADED
	if int32(hr) < 0 {
		ready <- volumeEndpointResult{err: fmt.Errorf("volume COM init: 0x%x", hr)}
		return
	}
	defer procMMCoUninitialize.Call()
	var eventContext mmGUIDValue
	if hr, _, _ = procVolumeCreateGuid.Call(uintptr(unsafe.Pointer(&eventContext))); int32(hr) < 0 {
		ready <- volumeEndpointResult{err: fmt.Errorf("volume event context: 0x%x", hr)}
		return
	}
	clsid, _ := mmGUID(mmDeviceEnumeratorCLSID)
	iid, _ := mmGUID(mmDeviceEnumeratorIID)
	var enumerator uintptr
	hr, _, _ = procMMCoCreateInstance.Call(uintptr(unsafe.Pointer(&clsid)), 0, 1, uintptr(unsafe.Pointer(&iid)), uintptr(unsafe.Pointer(&enumerator)))
	if int32(hr) < 0 || enumerator == 0 {
		ready <- volumeEndpointResult{err: fmt.Errorf("volume enumerator: 0x%x", hr)}
		return
	}
	defer mmVCall(enumerator, 2, 0, 0, 0, 0)
	wake := make(chan struct{}, 1)
	deviceCallback := newVolumeCOMCallback(wake, true, eventContext)
	devicePointer := uintptr(unsafe.Pointer(deviceCallback))
	defer volumeRelease(devicePointer)
	if hr = mmVCall(enumerator, 6, devicePointer, 0, 0, 0); int32(hr) < 0 {
		ready <- volumeEndpointResult{err: fmt.Errorf("volume device notification: 0x%x", hr)}
		return
	}
	defer mmVCall(enumerator, 7, devicePointer, 0, 0, 0)
	volumeIID, _ := mmGUID("{5CDF2C82-841E-4546-9722-0CF74078229A}")
	var endpoint uintptr
	var endpointID string
	var callback *volumeCOMCallback
	releaseEndpoint := func() {
		if endpoint != 0 {
			mmVCall(endpoint, 4, uintptr(unsafe.Pointer(callback)), 0, 0, 0)
			mmVCall(endpoint, 2, 0, 0, 0, 0)
			volumeRelease(uintptr(unsafe.Pointer(callback)))
		}
		endpoint, endpointID, callback = 0, "", nil
	}
	defer releaseEndpoint()
	read := func() endpointVolume {
		var device uintptr
		if int32(mmVCall(enumerator, 4, 0, 0, uintptr(unsafe.Pointer(&device)), 0)) < 0 || device == 0 {
			releaseEndpoint()
			return endpointVolume{}
		}
		defer mmVCall(device, 2, 0, 0, 0, 0)
		var idPointer uintptr
		if int32(mmVCall(device, 5, uintptr(unsafe.Pointer(&idPointer)), 0, 0, 0)) < 0 || idPointer == 0 {
			releaseEndpoint()
			return endpointVolume{}
		}
		id := mmWideString(idPointer)
		procMMCoTaskMemFree.Call(idPointer)
		if endpointID != id {
			releaseEndpoint()
			if int32(mmVCall(device, 3, uintptr(unsafe.Pointer(&volumeIID)), 1, 0, uintptr(unsafe.Pointer(&endpoint)))) < 0 || endpoint == 0 {
				endpoint = 0
				return endpointVolume{}
			}
			callback = newVolumeCOMCallback(wake, false, eventContext)
			if int32(mmVCall(endpoint, 3, uintptr(unsafe.Pointer(callback)), 0, 0, 0)) < 0 {
				releaseEndpoint()
				return endpointVolume{}
			}
			endpointID = id
		}
		var level float32
		var mute int32
		if int32(mmVCall(endpoint, 9, uintptr(unsafe.Pointer(&level)), 0, 0, 0)) < 0 || int32(mmVCall(endpoint, 15, uintptr(unsafe.Pointer(&mute)), 0, 0, 0)) < 0 {
			releaseEndpoint()
			return endpointVolume{}
		}
		return endpointVolume{endpointID, float64(level), mute != 0}
	}
	ready <- volumeEndpointResult{value: read()}
	for {
		select {
		case <-b.stop:
			return
		case <-wake:
			value := read()
			select {
			case <-b.changes:
			default:
			}
			b.changes <- value
		case command := <-b.commands:
			current := read() // A queued device notification must never send a write to the old device.
			var err error
			if !command.value.valid() || current.EndpointID != command.value.EndpointID || !current.equal(command.expected) || endpoint == 0 {
				err = fmt.Errorf("Windows playback controls changed")
			} else {
				context := uintptr(unsafe.Pointer(&eventContext))
				// Windows amd64 SyscallN places argument bits in both integer and
				// XMM registers, including this float parameter in argument slot 1.
				if math.Abs(current.Volume-command.value.Volume) > volumeSyncEpsilon {
					hr = mmVCall(endpoint, 7, uintptr(math.Float32bits(float32(command.value.Volume))), context, 0, 0)
					if int32(hr) < 0 {
						err = fmt.Errorf("setting Windows volume: 0x%x", hr)
					}
				}
				if err == nil && current.Muted != command.value.Muted {
					var mute uintptr
					if command.value.Muted {
						mute = 1
					}
					if hr = mmVCall(endpoint, 14, mute, context, 0, 0); int32(hr) < 0 {
						err = fmt.Errorf("setting Windows mute: 0x%x", hr)
					}
				}
			}
			command.done <- volumeEndpointResult{read(), err}
		}
	}
}
