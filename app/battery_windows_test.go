//go:build windows

package main

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestWindowsBatterySnapshot(t *testing.T) {
	line, err := hostBatteryLine()
	if err != nil {
		t.Fatal(err)
	}
	if line == "" {
		t.Skip("Windows battery percentage is temporarily unavailable")
	}
	if !strings.HasPrefix(line, "battery ") {
		t.Fatalf("unexpected power message: %q", line)
	}
	stateLine, detailsLine, ok := strings.Cut(strings.TrimSuffix(line, "\n"), "\n")
	if !ok || !strings.HasPrefix(detailsLine, "battery-details ") {
		t.Fatalf("missing physical details message: %q", line)
	}
	var details batteryDetails
	if err := json.Unmarshal([]byte(strings.TrimPrefix(detailsLine, "battery-details ")), &details); err != nil {
		t.Fatal(err)
	}
	var snapshot batterySnapshot
	if err := json.Unmarshal([]byte(strings.TrimSpace(strings.TrimPrefix(stateLine, "battery "))), &snapshot); err != nil {
		t.Fatal(err)
	}
	if snapshot.Type != "state" || snapshot.Present && (snapshot.Percentage == nil || *snapshot.Percentage < 0 || *snapshot.Percentage > 100) {
		t.Fatalf("invalid Windows power state: %+v", snapshot)
	}
	percentage := -1
	if snapshot.Percentage != nil {
		percentage = *snapshot.Percentage
	}
	t.Logf("Windows host power: present=%t percentage=%d state=%s AC=%t", snapshot.Present, percentage, snapshot.State, snapshot.ACConnected)
}
