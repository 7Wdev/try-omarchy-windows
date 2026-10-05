package main

import (
	"sync/atomic"
	"time"
)

// HostPowerTransition lets guest-health checks distinguish a host power
// transition from a guest freeze. Running is published after QMP reconciliation;
// allow a short grace after Since for the guest agent and graphics to settle.
type HostPowerTransition struct {
	Phase string // suspended, resuming, recovery, running
	Since time.Time
}

var hostPowerTransition atomic.Pointer[HostPowerTransition]
var guestRuntimeGeneration atomic.Uint64

func HostPowerState() HostPowerTransition {
	if state := hostPowerTransition.Load(); state != nil {
		return *state
	}
	return HostPowerTransition{Phase: "running"}
}

func setHostPowerTransition(phase string) {
	old := hostPowerTransition.Load()
	if old != nil && old.Phase == phase {
		return
	}
	hostPowerTransition.Store(&HostPowerTransition{Phase: phase, Since: time.Now()})
}
