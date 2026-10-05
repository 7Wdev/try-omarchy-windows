package main

import (
	"encoding/json"
	"fmt"
	"sync"
)

type systemPowerStatus struct {
	ACLineStatus        byte
	BatteryFlag         byte
	BatteryLifePercent  byte
	Reserved            byte
	BatteryLifeTime     uint32
	BatteryFullLifeTime uint32
}

type batterySnapshot struct {
	Type               string `json:"type"`
	Present            bool   `json:"present"`
	Percentage         *int   `json:"percentage"`
	State              string `json:"state"`
	ACConnected        bool   `json:"acConnected"`
	TimeToEmptySeconds *int   `json:"timeToEmptySeconds"`
	TimeToFullSeconds  *int   `json:"timeToFullSeconds"`
}

func batteryFromWindows(s systemPowerStatus) batterySnapshot {
	b := batterySnapshot{Type: "state", ACConnected: s.ACLineStatus == 1, State: "unknown"}
	// Unknown flags are not evidence of absence.
	if s.BatteryFlag == 0xff || s.BatteryFlag&0x80 != 0 {
		return b
	}
	b.Present = true
	percent := int(s.BatteryLifePercent)
	if percent <= 100 {
		b.Percentage = &percent
	}
	switch {
	case s.BatteryFlag&0x08 != 0:
		b.State = "charging"
	case b.Percentage != nil && percent == 100 && b.ACConnected:
		b.State = "full"
	case s.ACLineStatus == 0:
		b.State = "discharging"
	case s.ACLineStatus == 1:
		b.State = "not-charging"
	}
	if b.State == "discharging" && s.BatteryLifeTime != ^uint32(0) {
		seconds := int(s.BatteryLifeTime)
		b.TimeToEmptySeconds = &seconds
	}
	// BatteryFullLifeTime is the total lifetime at full charge, not the
	// time until full. Windows does not supply the latter here.
	return b
}

func encodeBatteryLine(s systemPowerStatus) (string, error) {
	return encodeBatterySnapshot(batteryFromWindows(s))
}

func encodeBatterySnapshot(snapshot batterySnapshot) (string, error) {
	// Incomplete Windows readings must leave the guest's last good state alone.
	if snapshot.Present && (snapshot.Percentage == nil || *snapshot.Percentage < 0 || *snapshot.Percentage > 100) {
		return "", nil
	}
	data, err := json.Marshal(snapshot)
	if err != nil {
		return "", fmt.Errorf("encode battery: %w", err)
	}
	return "battery " + string(data) + "\n", nil
}

// Unknown readings reuse the last valid state until Windows reports one again.
type batteryPresenceCache struct {
	mu   sync.Mutex
	last *batterySnapshot
}

func (c *batteryPresenceCache) snapshot(s systemPowerStatus) *batterySnapshot {
	c.mu.Lock()
	defer c.mu.Unlock()
	b := batteryFromWindows(s)
	if s.BatteryFlag == 0xff || (b.Present && b.Percentage == nil) {
		if c.last == nil || (s.BatteryFlag != 0xff && !c.last.Present) {
			return nil
		}
		previous := *c.last
		if s.ACLineStatus <= 1 {
			previous.ACConnected = b.ACConnected
			if previous.Present {
				switch {
				case b.State != "unknown":
					previous.State = b.State
				case s.ACLineStatus == 0:
					previous.State = "discharging"
				case previous.State == "discharging":
					previous.State = "not-charging"
				}
			}
		}
		if s.BatteryFlag != 0xff && b.State != "unknown" && previous.Present {
			previous.State = b.State
		}
		b = previous
	} else if s.ACLineStatus > 1 && c.last != nil {
		b.ACConnected = c.last.ACConnected
	}
	c.last = &b
	result := b
	return &result
}
