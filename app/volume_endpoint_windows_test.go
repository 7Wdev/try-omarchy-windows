//go:build windows

package main

import (
	"testing"
	"unsafe"
)

func TestVolumeEndpointCallbacks(t *testing.T) {
	wake := make(chan struct{}, 1)
	c := newVolumeCOMCallback(wake, false)
	p := uintptr(unsafe.Pointer(c))
	defer volumeRelease(p)
	var out uintptr
	if hr := volumeQueryInterface(p, uintptr(unsafe.Pointer(&volumeCallbackIID)), uintptr(unsafe.Pointer(&out))); hr != 0 || out != p {
		t.Fatalf("callback interface: %x %x", hr, out)
	}
	volumeRelease(out)
	volumeOnNotify(p, uintptr(unsafe.Pointer(&volumeEventContext)))
	select {
	case <-wake:
		t.Fatal("own setter echoed")
	default:
	}
	volumeOnNotify(p, uintptr(unsafe.Pointer(&mmGUIDValue{})))
	select {
	case <-wake:
	default:
		t.Fatal("external volume/mute did not wake")
	}
	volumeDefaultChanged(p, 1, 0, 0)
	volumeDefaultChanged(p, 0, 2, 0)
	select {
	case <-wake:
		t.Fatal("capture/communications role changed playback")
	default:
	}
	volumeDefaultChanged(p, 0, 0, 0)
	select {
	case <-wake:
	default:
		t.Fatal("playback default did not wake")
	}
	for i := 0; i < 10; i++ {
		volumeWake(p)
	} // A callback burst must never block COM.
	if len(wake) != 1 {
		t.Fatal("callback notifications were not coalesced")
	}
}

func TestAudioBridgeVolumeProtocol(t *testing.T) {
	for _, line := range []string{
		`{"type":"get-volume"}`,
		`{"type":"set-volume","endpointID":"speakers","volume":0.5,"muted":true,"sequence":1,"origin":"guest"}`,
	} {
		if _, err := parseAudioBridgeRequest([]byte(line)); err != nil {
			t.Fatal(err)
		}
	}
	for _, line := range []string{
		`{"type":"set-volume","endpointID":"speakers","volume":0.5,"sequence":1,"origin":"guest"}`,
		`{"type":"set-volume","endpointID":"speakers","volume":1.1,"muted":false,"sequence":1,"origin":"guest"}`,
		`{"type":"set-volume","endpointID":"speakers","volume":0.5,"muted":false,"sequence":1,"origin":"windows"}`,
		`{"type":"get-catalog","volume":0.5}`,
		`{"type":"get-volume","endpointID":"speakers"}`,
		`{"type":"get-volume"} {}`,
	} {
		if _, err := parseAudioBridgeRequest([]byte(line)); err == nil {
			t.Fatalf("accepted %s", line)
		}
	}
}
