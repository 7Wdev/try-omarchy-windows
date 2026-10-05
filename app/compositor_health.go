package main

import (
	"sync"
	"time"
)

const (
	compositorBootGrace      = 120 * time.Second
	compositorResumeGrace    = 90 * time.Second
	compositorReconnectGrace = 30 * time.Second
	compositorStallDelay     = 60 * time.Second
)

// A heartbeat proves only that Hyprland processes IPC, not physical rendering.
// Old guests stay unarmed. Failure reports and missing replies both count once
// this boot has sent a heartbeat. Shutdown and host suspension suppress alerts.
type compositorHealth struct {
	mu                                                     sync.Mutex
	armed, connected, ready, suspended, stopping, reported bool
	grace, lastOK, lastTick                                time.Time
}

var guestCompositorHealth compositorHealth

func (h *compositorHealth) boot(now time.Time) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.armed, h.connected, h.ready, h.stopping, h.reported = false, false, false, false, false
	h.grace, h.lastOK, h.lastTick = now.Add(compositorBootGrace), now, now
}
func (h *compositorHealth) userspaceReady() { h.mu.Lock(); h.ready = true; h.mu.Unlock() }
func (h *compositorHealth) connect(now time.Time) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.connected = true
	h.reported = false
	h.extendGrace(now.Add(compositorReconnectGrace))
}
func (h *compositorHealth) disconnect(now time.Time) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.connected = false
	h.extendGrace(now.Add(compositorReconnectGrace))
}
func (h *compositorHealth) heartbeat(now time.Time, ok bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.connected {
		return
	}
	if !h.armed {
		h.lastOK = now
	}
	h.armed = true
	if ok {
		h.lastOK = now
		h.reported = false
	}
}
func (h *compositorHealth) inactive() {
	h.mu.Lock()
	h.armed = false
	h.reported = false
	h.mu.Unlock()
}
func (h *compositorHealth) power(now time.Time, suspended bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.suspended = suspended
	h.extendGrace(now.Add(compositorResumeGrace))
	h.lastTick = now
}
func (h *compositorHealth) stop() { h.mu.Lock(); h.stopping = true; h.mu.Unlock() }
func (h *compositorHealth) extendGrace(until time.Time) {
	if until.After(h.grace) {
		h.grace = until
	}
}
func (h *compositorHealth) stalled(now time.Time, qemuHealthy bool) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	// A delayed ticker is also a resume signal if the platform broadcast was
	// lost. Do not charge that gap to either compositor or QMP silence.
	if now.Sub(h.lastTick) > 5*time.Second {
		h.extendGrace(now.Add(compositorResumeGrace))
	}
	h.lastTick = now
	if !h.armed || !h.ready || h.suspended || h.stopping || h.reported || !qemuHealthy {
		return false
	}
	since := h.lastOK
	if h.grace.After(since) {
		since = h.grace
	}
	if now.Sub(since) < compositorStallDelay {
		return false
	}
	h.reported = true
	return true
}

func (h *compositorHealth) suppressRecovery() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.stopping || h.suspended
}
