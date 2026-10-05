package main

import (
	"encoding/json"
	"fmt"
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

func TestBatteryUnknownReadingsReuseLastGoodSnapshot(t *testing.T) {
	var cache batteryPresenceCache
	unknown := systemPowerStatus{ACLineStatus: 255, BatteryFlag: 255, BatteryLifePercent: 255, BatteryLifeTime: ^uint32(0)}
	if cache.snapshot(unknown) != nil {
		t.Fatal("initial unknown reading must be skipped")
	}
	known := systemPowerStatus{ACLineStatus: 0, BatteryFlag: 0, BatteryLifePercent: 57, BatteryLifeTime: 8100}
	if !cache.snapshot(known).Present {
		t.Fatal("known present")
	}
	got := cache.snapshot(unknown)
	if !got.Present || *got.Percentage != 57 || got.State != "discharging" || got.ACConnected {
		t.Fatalf("unknown: %+v", got)
	}
	known.BatteryLifePercent = 255
	known.ACLineStatus, known.BatteryFlag = 1, 8
	if got := cache.snapshot(known); !got.Present || *got.Percentage != 57 || !got.ACConnected || got.State != "charging" {
		t.Fatalf("unknown percent: %+v", got)
	}
	unknown.ACLineStatus = 0
	if got := cache.snapshot(unknown); *got.Percentage != 57 || got.ACConnected || got.State != "discharging" {
		t.Fatalf("known AC with unknown flags: %+v", got)
	}
	absent := systemPowerStatus{ACLineStatus: 1, BatteryFlag: 128, BatteryLifePercent: 255}
	if cache.snapshot(absent).Present || cache.snapshot(unknown).Present {
		t.Fatal("known absence not retained")
	}
	if cache.snapshot(known) != nil {
		t.Fatal("new battery without percentage must not reuse removed battery")
	}
	known.BatteryLifePercent = 80
	if got := cache.snapshot(known); !got.Present || *got.Percentage != 80 {
		t.Fatal("recovery", got)
	}
}

func TestBatteryMissingInitialPercentageIsSkipped(t *testing.T) {
	var cache batteryPresenceCache
	status := systemPowerStatus{BatteryLifePercent: 255}
	if cache.snapshot(status) != nil {
		t.Fatal("invented initial percentage")
	}
	if line, err := encodeBatteryLine(status); line != "" || err != nil {
		t.Fatalf("incomplete reading must silently skip: %q, %v", line, err)
	}
}

// These are the guest decoder's percentage rules, applied to the serialized wire state.
func assertGuestBatteryPercentage(t *testing.T, line string) {
	t.Helper()
	if line == "" {
		return
	}
	var state struct {
		Present    bool
		Percentage *int
	}
	if err := json.Unmarshal([]byte(strings.TrimPrefix(line, "battery ")), &state); err != nil {
		t.Fatal(err)
	}
	if state.Present {
		if state.Percentage == nil || *state.Percentage < 0 || *state.Percentage > 100 {
			t.Fatalf("guest rejects percentage: %s", line)
		}
	} else if state.Percentage != nil {
		t.Fatalf("guest rejects absent battery percentage: %s", line)
	}
}

func TestBatterySnapshotsSatisfyGuestPercentageRules(t *testing.T) {
	for _, initial := range []string{"none", "present", "absent"} {
		t.Run(initial, func(t *testing.T) {
			for flag := 0; flag <= 255; flag++ {
				for percent := 0; percent <= 255; percent++ {
					for _, ac := range []byte{0, 1, 255} {
						var cache batteryPresenceCache
						switch initial {
						case "present":
							cache.snapshot(systemPowerStatus{BatteryLifePercent: 57})
						case "absent":
							cache.snapshot(systemPowerStatus{BatteryFlag: 128})
						}
						status := systemPowerStatus{ACLineStatus: ac, BatteryFlag: byte(flag), BatteryLifePercent: byte(percent)}
						line, err := encodeBatteryLine(status)
						if err != nil {
							t.Fatal(err)
						}
						assertGuestBatteryPercentage(t, line)
						if snapshot := cache.snapshot(status); snapshot != nil {
							line, err = encodeBatterySnapshot(*snapshot)
							if err != nil || line == "" {
								t.Fatal(fmt.Sprintf("cached snapshot not usable: %+v, %v", snapshot, err))
							}
							assertGuestBatteryPercentage(t, line)
						}
					}
				}
			}
		})
	}
}
