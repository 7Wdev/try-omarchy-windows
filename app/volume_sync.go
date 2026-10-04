package main

import (
	"math"
	"time"
)

const volumeSyncEpsilon = 0.001
const volumeSyncDebounce = 40 * time.Millisecond

type endpointVolume struct {
	EndpointID string  `json:"endpointID"`
	Volume     float64 `json:"volume"`
	Muted      bool    `json:"muted"`
}

func (v endpointVolume) valid() bool {
	return v.EndpointID != "" && len(v.EndpointID) <= 4096 &&
		!math.IsNaN(v.Volume) && !math.IsInf(v.Volume, 0) && v.Volume >= 0 && v.Volume <= 1
}

func (v endpointVolume) equal(other endpointVolume) bool {
	return v.EndpointID == other.EndpointID && v.Muted == other.Muted &&
		math.Abs(v.Volume-other.Volume) <= volumeSyncEpsilon
}

type volumeSyncMessage struct {
	Type     string `json:"type"`
	Enabled  bool   `json:"enabled"`
	Origin   string `json:"origin"`
	Sequence uint64 `json:"sequence"`
	endpointVolume
}

type volumeSyncRequest struct {
	Origin   string `json:"origin"`
	Sequence uint64 `json:"sequence"`
	endpointVolume
}

// Owned by the bridge's select loop. Windows always supplies the initial state
// on connect, re-enable and default-device replacement. A request names both
// that endpoint and the observed sequence so delayed guest writes cannot undo
// a newer Windows change. Native setters also verify the current default ID.
type volumeSync struct {
	message volumeSyncMessage
	pending *volumeSyncRequest
}

func (s *volumeSync) observe(value endpointVolume, enabled bool, origin string) bool {
	enabled = enabled && value.valid()
	if s.message.Sequence != 0 && s.message.Enabled == enabled && s.message.endpointVolume.equal(value) {
		return false
	}
	s.pending = nil
	s.message = volumeSyncMessage{
		Type: "volume", Enabled: enabled, Origin: origin,
		Sequence: s.message.Sequence + 1, endpointVolume: value,
	}
	return true
}

func (s *volumeSync) request(request volumeSyncRequest) bool {
	if !s.message.Enabled || !request.valid() || request.Origin != "guest" ||
		request.Sequence != s.message.Sequence || request.EndpointID != s.message.EndpointID {
		return false
	}
	// Keep the newest value in a burst, including a return to the current value.
	s.pending = &request
	return true
}

func (s *volumeSync) acknowledge(value endpointVolume, enabled bool, origin string) {
	if !s.observe(value, enabled, origin) {
		// A failed setter can leave the value unchanged. Still acknowledge the
		// attempt with a new sequence and Windows origin so the guest drops it.
		s.pending = nil
		s.message.Sequence++
		s.message.Origin = origin
	}
}

func (s *volumeSync) take() (endpointVolume, bool) {
	request := s.pending
	s.pending = nil
	if request == nil || !s.message.Enabled || s.message.endpointVolume.equal(request.endpointVolume) {
		return endpointVolume{}, false
	}
	return request.endpointVolume, true
}
