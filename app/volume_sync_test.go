package main

import "testing"

func TestVolumeSyncLoopPrevention(t *testing.T) {
	var sync volumeSync
	v := endpointVolume{"speakers", 0.5, false}
	sync.observe(v, true, "windows")
	if sync.observe(endpointVolume{"speakers", 0.5005, false}, true, "windows") {
		t.Fatal("epsilon echo published")
	}
	request := volumeSyncRequest{"guest", sync.message.Sequence, v}
	if !sync.request(request) {
		t.Fatal("valid echo rejected")
	}
	if _, apply := sync.take(); apply {
		t.Fatal("echo reapplied to Windows")
	}
	request.Volume = 0.7
	sync.request(request)
	request.Volume = 0.8
	sync.request(request)
	if got, apply := sync.take(); !apply || got.Volume != 0.8 {
		t.Fatalf("burst did not retain last change: %+v %v", got, apply)
	}
	sync.observe(request.endpointVolume, true, "guest")
	if sync.request(request) {
		t.Fatal("delayed request overwrote acknowledged state")
	}
	request.Sequence = sync.message.Sequence
	request.Origin = "windows"
	if sync.request(request) {
		t.Fatal("host origin accepted as guest write")
	}
}

func TestVolumeSyncMute(t *testing.T) {
	var sync volumeSync
	sync.observe(endpointVolume{"speakers", 0.63, false}, true, "windows")
	if !sync.request(volumeSyncRequest{"guest", sync.message.Sequence, endpointVolume{"speakers", 0.63, true}}) {
		t.Fatal("mute rejected")
	}
	v, apply := sync.take()
	if !apply || !v.Muted || v.Volume != 0.63 {
		t.Fatalf("mute discarded volume: %+v", v)
	}
	sync.observe(v, true, "guest")
	v.Muted = false
	if !sync.observe(v, true, "windows") || sync.message.Muted {
		t.Fatal("Windows unmute not published")
	}
}

func TestVolumeSyncDeviceSwitch(t *testing.T) {
	var sync volumeSync
	sync.observe(endpointVolume{"speakers", 0.6, false}, true, "windows")
	old := volumeSyncRequest{"guest", sync.message.Sequence, endpointVolume{"speakers", 0.9, true}}
	sync.request(old)
	sync.observe(endpointVolume{"headphones", 0.2, false}, true, "windows")
	if _, apply := sync.take(); apply || sync.request(old) {
		t.Fatal("old-device request survived replacement")
	}
	if sync.message.EndpointID != "headphones" || sync.message.Volume != 0.2 || sync.message.Muted {
		t.Fatalf("new endpoint state lost: %+v", sync.message)
	}
	if !sync.observe(endpointVolume{}, true, "windows") || sync.message.Enabled {
		t.Fatal("no-default-device state stayed enabled")
	}
}

func TestVolumeSyncToggleOff(t *testing.T) {
	var sync volumeSync
	v := endpointVolume{"speakers", 0.5, false}
	sync.observe(v, true, "windows")
	r := volumeSyncRequest{"guest", sync.message.Sequence, endpointVolume{"speakers", 0.8, true}}
	sync.request(r)
	sync.observe(v, false, "windows")
	if _, apply := sync.take(); apply || sync.request(r) {
		t.Fatal("off allowed a pending or new write")
	}
	v.Volume = 0.3
	sync.observe(v, true, "windows")
	if sync.message.Volume != 0.3 || !sync.message.Enabled || sync.request(r) {
		t.Fatal("re-enable did not start from current Windows state")
	}
}

func TestVolumeSyncWindowsChangeCancelsGuestBurst(t *testing.T) {
	var sync volumeSync
	sync.observe(endpointVolume{"speakers", 0.5, false}, true, "windows")
	r := volumeSyncRequest{"guest", sync.message.Sequence, endpointVolume{"speakers", 0.8, true}}
	sync.request(r)
	sync.observe(endpointVolume{"speakers", 0.3, false}, true, "windows")
	if _, apply := sync.take(); apply || sync.request(r) {
		t.Fatal("guest burst overwrote a newer Windows change")
	}
}
