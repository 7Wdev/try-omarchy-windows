package main

import (
	"testing"
	"time"
)

func TestCompositorHealthGraceAndRecovery(t *testing.T) {
	now := time.Unix(1000, 0)
	h := &compositorHealth{}
	h.boot(now)
	h.userspaceReady()
	h.connect(now)
	h.heartbeat(now, true)
	advance := func(seconds int, healthy bool) bool {
		result := false
		for i := 0; i < seconds; i++ {
			now = now.Add(time.Second)
			if h.stalled(now, healthy) {
				result = true
			}
		}
		return result
	}
	if advance(179, true) {
		t.Fatal("alert during boot/stall grace")
	}
	if !advance(1, true) || advance(60, true) {
		t.Fatal("expected one stall alert")
	}
	h.heartbeat(now, true)
	if advance(59, true) || !advance(1, true) {
		t.Fatal("recovered stall not detected")
	}
	h.heartbeat(now, true)
	h.power(now, true)
	if advance(200, true) {
		t.Fatal("suspended guest alerted")
	}
	h.power(now, false)
	if advance(149, true) || !advance(1, true) {
		t.Fatal("resume grace incorrect")
	}
	h.heartbeat(now, true)
	h.disconnect(now)
	if advance(89, true) || !advance(1, true) {
		t.Fatal("disconnect grace incorrect")
	}
	h.connect(now)
	h.heartbeat(now, false)
	if advance(89, true) || !advance(1, true) {
		t.Fatal("reconnect grace incorrect")
	}
	h.stop()
	if advance(300, true) {
		t.Fatal("shutdown alerted")
	}
}
func TestCompositorHealthOldGuestAndQEMUWedge(t *testing.T) {
	now := time.Unix(1000, 0)
	h := &compositorHealth{}
	h.boot(now)
	h.userspaceReady()
	h.connect(now)
	for i := 0; i < 500; i++ {
		now = now.Add(time.Second)
		if h.stalled(now, true) {
			t.Fatal("old guest alerted")
		}
	}
	h.heartbeat(now, false)
	for i := 0; i < 120; i++ {
		now = now.Add(time.Second)
		if h.stalled(now, false) {
			t.Fatal("unhealthy QEMU compositor alert")
		}
	}
	if !h.stalled(now, true) {
		t.Fatal("failure heartbeats not monitored")
	}
	h.boot(now)
	h.userspaceReady()
	h.connect(now)
	for i := 0; i < 500; i++ {
		now = now.Add(time.Second)
		if h.stalled(now, true) {
			t.Fatal("new boot inherited heartbeat capability")
		}
	}
}
func TestCompositorHealthLostResumeBroadcast(t *testing.T) {
	now := time.Unix(1000, 0)
	h := &compositorHealth{}
	h.boot(now)
	h.userspaceReady()
	h.connect(now)
	h.heartbeat(now, true)
	now = now.Add(time.Hour)
	if h.stalled(now, true) {
		t.Fatal("wall clock gap counted as stall")
	}
	for i := 0; i < 149; i++ {
		now = now.Add(time.Second)
		if h.stalled(now, true) {
			t.Fatal("gap grace missing")
		}
	}
	if !h.stalled(now.Add(time.Second), true) {
		t.Fatal("monitor did not resume")
	}
}

func TestCompositorInactiveAndRebootDoNotAlert(t *testing.T) {
	now := time.Unix(1000, 0)
	h := &compositorHealth{}
	h.boot(now)
	h.connect(now)
	h.userspaceReady()
	h.heartbeat(now, true)
	h.inactive()
	for i := 0; i < 300; i++ {
		now = now.Add(time.Second)
		if h.stalled(now, true) {
			t.Fatal("logout reported as stall")
		}
	}
	h.heartbeat(now, true)
	h.stop()
	if !h.suppressRecovery() {
		t.Fatal("shutdown did not suppress recovery")
	}
	h.boot(now)
	if h.suppressRecovery() {
		t.Fatal("new boot inherited shutdown")
	}
}
