package main

import (
	"errors"
	"strings"
	"testing"
	"time"
)

func TestBatteryPhysicalDetails(t *testing.T) {
	info := batteryInformation{DesignedCapacity: 60000, FullChargedCapacity: 50000, CycleCount: 213, Chemistry: [4]byte{'L', 'I', 'O', 'N'}}
	details := batteryDetailsFromWindows(info, "ACME Corp", "Battery A")
	line := encodeBatteryDetailsLine(details)
	for _, want := range []string{`"energyFullDesignMicroWh":60000000`, `"energyFullMicroWh":50000000`, `"cycleCount":213`, `"manufacturer":"ACME Corp"`, `"modelName":"Battery A"`, `"chemistry":"LION"`} {
		if !strings.Contains(line, want) {
			t.Fatalf("missing %s in %s", want, line)
		}
	}
	if !strings.HasPrefix(line, "battery-details {") || !strings.HasSuffix(line, "\n") {
		t.Fatal(line)
	}
	for _, unknown := range []uint32{0, ^uint32(0), 2147484} {
		info.DesignedCapacity, info.FullChargedCapacity, info.CycleCount = unknown, unknown, 0
		details = batteryDetailsFromWindows(info, "", "")
		if details.EnergyFullDesignMicroWh != nil || details.EnergyFullMicroWh != nil || details.CycleCount != nil {
			t.Fatalf("unknown values included: %+v", details)
		}
	}
	info.DesignedCapacity, info.FullChargedCapacity, info.Capabilities = 60000, 50000, 0x40000000
	details = batteryDetailsFromWindows(info, "", "")
	if details.EnergyFullDesignMicroWh != nil || details.EnergyFullMicroWh != nil {
		t.Fatal("relative capacities have no energy units")
	}
	if got := encodeBatteryDetailsLine(batteryDetails{}); got != "battery-details {}\n" {
		t.Fatal(got)
	}
}

func TestBatteryDetailsText(t *testing.T) {
	for _, text := range []string{"bad\nname", "\xff", "bad\x00name", strings.Repeat("x", 128)} {
		if batteryDetailText(text) != "" {
			t.Fatalf("accepted %q", text)
		}
	}
	if batteryDetailText(" ACME \x00") != "ACME" {
		t.Fatal("name trimming")
	}
}

func TestBatteryDetailsQueriedHourlyAndFailureClears(t *testing.T) {
	var cache batteryDetailsCache
	now := time.Unix(100, 0)
	calls := 0
	query := func() (batteryDetails, error) { calls++; return batteryDetails{Manufacturer: "ACME"}, nil }
	if cache.get(now, query).Manufacturer != "ACME" {
		t.Fatal("first query")
	}
	cache.get(now.Add(time.Minute), query)
	if calls != 1 {
		t.Fatal("queried on state tick")
	}
	result := cache.get(now.Add(time.Hour), func() (batteryDetails, error) {
		calls++
		return batteryDetails{Manufacturer: "stale"}, errors.New("device unavailable")
	})
	if result.Manufacturer != "" || calls != 2 {
		t.Fatal("failure retained details")
	}
}

func TestBatteryDetailsRefreshAfterAbsence(t *testing.T) {
	var cache batteryDetailsCache
	now := time.Unix(100, 0)
	cache.get(now, func() (batteryDetails, error) { return batteryDetails{ModelName: "old"}, nil })
	cache.clear()
	got := cache.get(now.Add(time.Minute), func() (batteryDetails, error) { return batteryDetails{ModelName: "new"}, nil })
	if got.ModelName != "new" {
		t.Fatal("kept details after battery replacement")
	}
}
