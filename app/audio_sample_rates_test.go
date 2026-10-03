package main

import (
	"encoding/binary"
	"testing"
)

func TestAudioSampleRateSelection(t *testing.T) {
	live := []audioEndpointInfo{
		{ID: "speakers", Name: "Renamed speakers", SampleRate: 44100},
		{ID: "usb", Name: "USB", SampleRate: 96000},
		{ID: "unknown", Name: "Unknown format"},
	}
	name, _ := resolveAudioSelection("Old speakers", "speakers", live)
	for _, tc := range []struct {
		name              string
		defaultRate, want int
	}{
		{name, 48000, 44100}, {"USB", 48000, 96000}, {"", 44100, 44100},
		{"Unplugged", 48000, 48000}, {"Unknown format", 44100, 44100},
		{"", 0, 48000}, {"", -1, 48000}, {"", 1000000, 48000},
	} {
		if got := selectedAudioSampleRate(tc.name, live, tc.defaultRate); got != tc.want {
			t.Errorf("%q default %d: got %d, want %d", tc.name, tc.defaultRate, got, tc.want)
		}
	}
	// SDL matches the first duplicate name, even if an ID resolved that name.
	duplicates := append([]audioEndpointInfo{{Name: "USB", SampleRate: 44100}}, live...)
	if got := selectedAudioSampleRate("USB", duplicates, 48000); got != 44100 {
		t.Fatal(got)
	}
}

func TestAudioSampleRatesRespectRuntimeAndMicrophone(t *testing.T) {
	endpoints := mmDeviceList{
		Output:            []audioEndpointInfo{{Name: "Speakers", SampleRate: 44100}},
		Input:             []audioEndpointInfo{{Name: "Mic", SampleRate: 96000}},
		DefaultOutputRate: 48000, DefaultInputRate: 16000,
	}
	p := audioPreferences{Output: "Speakers", Input: "Mic"}
	for _, tc := range []struct {
		selection, disabled bool
		want                audioSampleRates
	}{
		{true, false, audioSampleRates{44100, 96000}},
		{false, false, audioSampleRates{48000, 16000}},
		{true, true, audioSampleRates{44100, 0}},
		{false, true, audioSampleRates{48000, 0}},
	} {
		if got := audioRatesForSelection(p, endpoints, tc.selection, tc.disabled); got != tc.want {
			t.Errorf("selection=%v disabled=%v: got %+v, want %+v", tc.selection, tc.disabled, got, tc.want)
		}
	}
}

func TestAudioSampleRateFromWaveFormat(t *testing.T) {
	for _, size := range []int{18, 40} {
		for _, rate := range []uint32{0, 999, 1000, 44100, 48000, 192000, 999999, 1000000, 0xffffffff} {
			format := make([]byte, size)
			binary.LittleEndian.PutUint32(format[4:8], rate)
			want := 0
			if validAudioSampleRate(int(rate)) {
				want = int(rate)
			}
			if got := audioSampleRateFromWaveFormat(format); got != want {
				t.Errorf("size=%d rate=%d: got %d, want %d", size, rate, got, want)
			}
		}
	}
	if audioSampleRateFromWaveFormat(nil) != 0 || audioSampleRateFromWaveFormat(make([]byte, 17)) != 0 {
		t.Fatal("accepted truncated format")
	}
}

func TestAudioBackendOptionsSampleRates(t *testing.T) {
	for _, tc := range []struct {
		backend  string
		disabled bool
		rates    audioSampleRates
		want     string
	}{
		{"sdl", false, audioSampleRates{44100, 96000}, "sdl,id=snd,out.frequency=44100,in.frequency=96000"},
		{"sdl", true, audioSampleRates{44100, 96000}, "sdl,id=snd,out.frequency=44100,in.voices=0"},
		{"sdl", false, audioSampleRates{-1, 1000000}, "sdl,id=snd,out.frequency=48000,in.frequency=48000"},
		{"dsound", false, audioSampleRates{44100, 96000}, "dsound,id=snd"},
		{"dsound", true, audioSampleRates{44100, 96000}, "dsound,id=snd,in.voices=0"},
		{"none", true, audioSampleRates{44100, 96000}, "none,id=snd"},
	} {
		if got := audioBackendOptions(tc.backend, tc.disabled, tc.rates); got != tc.want {
			t.Errorf("%s disabled=%v: got %q, want %q", tc.backend, tc.disabled, got, tc.want)
		}
	}
}
