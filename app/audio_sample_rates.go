package main

import "encoding/binary"

const fallbackAudioSampleRate = 48000

type audioSampleRates struct {
	Output, Input int
}

func validAudioSampleRate(rate int) bool {
	return rate >= 1000 && rate < 1000000
}

func audioSampleRateOrFallback(rate int) int {
	if validAudioSampleRate(rate) {
		return rate
	}
	return fallbackAudioSampleRate
}

// PKEY_AudioEngine_DeviceFormat contains a WAVEFORMATEX or WAVEFORMATEXTENSIBLE.
func audioSampleRateFromWaveFormat(format []byte) int {
	if len(format) < 18 {
		return 0
	}
	rate := int(binary.LittleEndian.Uint32(format[4:8]))
	if !validAudioSampleRate(rate) {
		return 0
	}
	return rate
}

// Use the first matching name, as SDL does. The launch preferences have already
// resolved stable IDs to current names; a missing name follows Windows default.
func selectedAudioSampleRate(name string, live []audioEndpointInfo, defaultRate int) int {
	if name != "" {
		for _, endpoint := range live {
			if endpoint.Name == name {
				if validAudioSampleRate(endpoint.SampleRate) {
					return endpoint.SampleRate
				}
				break
			}
		}
	}
	return audioSampleRateOrFallback(defaultRate)
}

func audioRatesForSelection(p audioPreferences, endpoints mmDeviceList, selectionEnabled, microphoneDisabled bool) audioSampleRates {
	if !selectionEnabled {
		p = audioPreferences{}
	}
	rates := audioSampleRates{
		Output: selectedAudioSampleRate(p.Output, endpoints.Output, endpoints.DefaultOutputRate),
	}
	if !microphoneDisabled {
		rates.Input = selectedAudioSampleRate(p.Input, endpoints.Input, endpoints.DefaultInputRate)
	}
	return rates
}

func launchAudioSampleRates(p audioPreferences, selectionEnabled, microphoneDisabled bool) audioSampleRates {
	endpoints, err := listAudioEndpoints()
	if err != nil {
		logf("Audio sample rate lookup is unavailable; using Windows defaults or 48000 Hz: %v", err)
	}
	return audioRatesForSelection(p, endpoints, selectionEnabled, microphoneDisabled)
}
