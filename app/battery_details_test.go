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

func TestBatteryDetailsQueriedHourlyAndFailureRetries(t *testing.T) {
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
	if result.Manufacturer != "ACME" || calls != 2 {
		t.Fatal("failure lost details")
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

func TestBatteryDetailsRetryBackoffIsBounded(t *testing.T) {
	var cache batteryDetailsCache
	now := time.Unix(100, 0)
	cache.get(now, func() (batteryDetails, error) { return batteryDetails{ModelName: "Main"}, nil })
	now = now.Add(time.Hour)
	calls := 0
	fail := func() (batteryDetails, error) { calls++; return batteryDetails{}, errors.New("driver waking") }
	for i, want := range []time.Duration{time.Minute, 2 * time.Minute, 4 * time.Minute, 5 * time.Minute, 5 * time.Minute} {
		if cache.get(now, fail).ModelName != "Main" {
			t.Fatal("lost valid details")
		}
		if got := cache.next.Sub(now); got != want {
			t.Fatalf("retry %d: %s", i, got)
		}
		cache.get(now.Add(want/2), fail)
		if calls != i+1 {
			t.Fatal("retry too early")
		}
		now = cache.next
	}
	cache.get(now, func() (batteryDetails, error) { return batteryDetails{ModelName: "Recovered"}, nil })
	if cache.retry != 0 || cache.next.Sub(now) != time.Hour {
		t.Fatal("successful retry not reset")
	}
	cache.clear()
	if cache.retry != 0 {
		t.Fatal("backoff survived absence")
	}
}

func TestEnumerateAndAggregateSystemBatteries(t *testing.T) {
	first := batteryInformation{Capabilities: batterySystemBattery, DesignedCapacity: 60000, FullChargedCapacity: 50000, CycleCount: 213, Chemistry: [4]byte{'L', 'I', 'O', 'N'}}
	second := first
	second.DesignedCapacity, second.FullChargedCapacity, second.CycleCount = 30000, 24000, 17
	calls := 0
	got, err := enumerateBatteryDetails(func(index uint32) (batteryInformation, batteryDetails, bool, error) {
		calls++
		switch index {
		case 0:
			return first, batteryDetailsFromWindows(first, "ACME", "Main pack"), false, nil
		case 1:
			return batteryInformation{}, batteryDetails{ModelName: "UPS"}, false, nil
		case 2:
			return second, batteryDetailsFromWindows(second, "Other", "Detachable pack"), false, nil
		default:
			return batteryInformation{}, batteryDetails{}, true, nil
		}
	})
	if err != nil || calls != 4 || got.EnergyFullDesignMicroWh == nil || *got.EnergyFullDesignMicroWh != 90000000 || got.EnergyFullMicroWh == nil || *got.EnergyFullMicroWh != 74000000 {
		t.Fatalf("aggregate: %+v %v calls=%d", got, err, calls)
	}
	if got.ModelName != "" || got.Manufacturer != "" || got.CycleCount != nil || got.Chemistry != "LION" {
		t.Fatalf("ambiguous details: %+v", got)
	}
	a := batteryDetailsFromWindows(first, "ACME", "Main")
	for _, info := range []batteryInformation{
		{Capabilities: batterySystemBattery | batteryCapacityRelative, DesignedCapacity: 100, FullChargedCapacity: 80},
		{Capabilities: batterySystemBattery, DesignedCapacity: ^uint32(0), FullChargedCapacity: 0},
	} {
		d := aggregateBatteryDetails([]batteryDetails{a, batteryDetailsFromWindows(info, "", "")})
		if d.EnergyFullDesignMicroWh != nil || d.EnergyFullMicroWh != nil || d.Chemistry != "" {
			t.Fatal("partial totals", d)
		}
	}
	v := int64(1500000000)
	if aggregateBatteryDetails([]batteryDetails{{EnergyFullMicroWh: &v}, {EnergyFullMicroWh: &v}}).EnergyFullMicroWh != nil {
		t.Fatal("guest integer overflow")
	}
	if got := aggregateBatteryDetails([]batteryDetails{a}); got.ModelName != "Main" || *got.CycleCount != 213 {
		t.Fatal("single pack details", got)
	}
	_, err = enumerateBatteryDetails(func(index uint32) (batteryInformation, batteryDetails, bool, error) {
		if index == 0 {
			return first, a, false, nil
		}
		return batteryInformation{}, batteryDetails{}, false, errors.New("second pack unavailable")
	})
	if err == nil {
		t.Fatal("accepted incomplete enumeration")
	}
}
