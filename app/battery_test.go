package main

import (
	"strings"
	"testing"
)

func TestBatteryFromWindows(t *testing.T) {
	for _, tt := range []struct {
		name   string
		status systemPowerStatus
		want   string
	}{
		{"discharging", systemPowerStatus{ACLineStatus: 0, BatteryFlag: 0, BatteryLifePercent: 57, BatteryLifeTime: 8100}, `"present":true,"percentage":57,"state":"discharging","acConnected":false,"timeToEmptySeconds":8100`},
		{"charging", systemPowerStatus{ACLineStatus: 1, BatteryFlag: 8, BatteryLifePercent: 61, BatteryLifeTime: ^uint32(0)}, `"present":true,"percentage":61,"state":"charging","acConnected":true,"timeToEmptySeconds":null`},
		{"desktop", systemPowerStatus{ACLineStatus: 1, BatteryFlag: 128, BatteryLifePercent: 255}, `"present":false,"percentage":null,"state":"unknown","acConnected":true`},
		{"unknown", systemPowerStatus{ACLineStatus: 255, BatteryFlag: 255, BatteryLifePercent: 255}, `"present":false,"percentage":null,"state":"unknown","acConnected":false`},
	} {
		t.Run(tt.name, func(t *testing.T) {
			line, err := encodeBatteryLine(tt.status)
			if err != nil || !strings.HasPrefix(line, "battery {") || !strings.Contains(line, tt.want) || !strings.HasSuffix(line, "\n") {
				t.Fatalf("battery line %q: %v", line, err)
			}
		})
	}
}

func TestBatteryUnknownReadingsPreservePresence(t *testing.T) {
	var cache batteryPresenceCache
	unknown := systemPowerStatus{ACLineStatus: 255, BatteryFlag: 255, BatteryLifePercent: 255, BatteryLifeTime: ^uint32(0)}
	if cache.snapshot(unknown).Present {
		t.Fatal("invented initial presence")
	}
	known := systemPowerStatus{ACLineStatus: 0, BatteryFlag: 0, BatteryLifePercent: 57, BatteryLifeTime: 8100}
	if !cache.snapshot(known).Present {
		t.Fatal("known present")
	}
	got := cache.snapshot(unknown)
	if !got.Present || got.Percentage != nil || got.State != "unknown" || got.TimeToEmptySeconds != nil || got.ACConnected {
		t.Fatalf("unknown: %+v", got)
	}
	known.BatteryLifePercent = 255
	if got := cache.snapshot(known); !got.Present || got.Percentage != nil {
		t.Fatalf("unknown percent: %+v", got)
	}
	absent := systemPowerStatus{ACLineStatus: 1, BatteryFlag: 128, BatteryLifePercent: 255}
	if cache.snapshot(absent).Present || cache.snapshot(unknown).Present {
		t.Fatal("known absence not retained")
	}
	known.BatteryLifePercent = 80
	if got := cache.snapshot(known); !got.Present || *got.Percentage != 80 {
		t.Fatal("recovery", got)
	}
}
