package main

import (
	"encoding/json"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

const batteryCapacityRelative = 0x40000000

// Windows capacities are mWh unless BATTERY_CAPACITY_RELATIVE is set.
type batteryInformation struct {
	Capabilities        uint32
	Technology          byte
	Reserved            [3]byte
	Chemistry           [4]byte
	DesignedCapacity    uint32
	FullChargedCapacity uint32
	DefaultAlert1       uint32
	DefaultAlert2       uint32
	CriticalBias        uint32
	CycleCount          uint32
}

type batteryDetails struct {
	EnergyFullDesignMicroWh *int64 `json:"energyFullDesignMicroWh,omitempty"`
	EnergyFullMicroWh       *int64 `json:"energyFullMicroWh,omitempty"`
	CycleCount              *int64 `json:"cycleCount,omitempty"`
	Manufacturer            string `json:"manufacturer,omitempty"`
	ModelName               string `json:"modelName,omitempty"`
	Chemistry               string `json:"chemistry,omitempty"`
}

func batteryDetailsFromWindows(info batteryInformation, manufacturer, model string) batteryDetails {
	positive := func(value uint32, multiplier int64) *int64 {
		if value == 0 || value == ^uint32(0) || int64(value)*multiplier > 2147483647 {
			return nil
		}
		n := int64(value) * multiplier
		return &n
	}
	d := batteryDetails{CycleCount: positive(info.CycleCount, 1), Manufacturer: batteryDetailText(manufacturer), ModelName: batteryDetailText(model), Chemistry: batteryDetailText(string(info.Chemistry[:]))}
	if info.Capabilities&batteryCapacityRelative == 0 {
		d.EnergyFullDesignMicroWh = positive(info.DesignedCapacity, 1000)
		d.EnergyFullMicroWh = positive(info.FullChargedCapacity, 1000)
	}
	return d
}

func batteryDetailText(value string) string {
	value = strings.TrimSpace(strings.TrimRight(value, "\x00"))
	// Bound the protocol and omit control characters that cannot be displayed.
	if !utf8.ValidString(value) || len(value) > 127 || strings.ContainsFunc(value, func(r rune) bool { return r < 32 || r == 127 }) {
		return ""
	}
	return value
}

func encodeBatteryDetailsLine(details batteryDetails) string {
	data, _ := json.Marshal(details)
	return "battery-details " + string(data) + "\n"
}

type batteryDetailsCache struct {
	mu      sync.Mutex
	next    time.Time
	details batteryDetails
}

func (c *batteryDetailsCache) get(now time.Time, query func() (batteryDetails, error)) batteryDetails {
	c.mu.Lock()
	defer c.mu.Unlock()
	if !now.Before(c.next) {
		details, err := query()
		c.details = batteryDetails{}
		if err == nil {
			c.details = details
		}
		c.next = now.Add(time.Hour)
	}
	return c.details
}

func (c *batteryDetailsCache) clear() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.next = time.Time{}
	c.details = batteryDetails{}
}
