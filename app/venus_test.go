package main

import (
	"strings"
	"testing"
)

func TestVenusDecision(t *testing.T) {
	const nvidia = "Microsoft Basic Display Adapter=10.0.26100.1;NVIDIA GeForce RTX 4070 SUPER=32.0.15.8129"
	const hybrid = "Intel(R) UHD Graphics=31.0.101.5186;NVIDIA GeForce RTX 4060 Laptop GPU=32.0.15.8129"
	const amd = "AMD Radeon(TM) Graphics=31.0.24033.1003"
	for _, c := range []struct {
		override, driver string
		want             bool
	}{
		{"", nvidia, false},
		{"", hybrid, false},
		{"", strings.ToUpper(nvidia), false},
		{"", amd, true},
		{"", "Intel(R) Arc(TM) Graphics=32.0.101.6078", true},
		// An unreadable driver list is unknown, not NVIDIA.
		{"", "", true},
		{"on", nvidia, true},
		{" ON ", nvidia, true},
		{"off", amd, false},
		{"Off", "", false},
		// A typo keeps the automatic choice.
		{"yes", nvidia, false},
		{"1", amd, true},
	} {
		got, reason := venusDecision(c.override, c.driver)
		if got != c.want || reason == "" {
			t.Errorf("venusDecision(%q, %q) = %v %q, want %v", c.override, c.driver, got, reason, c.want)
		}
	}
	if _, reason := venusDecision("yes", amd); !strings.Contains(reason, "ignoring "+venusVariable) {
		t.Fatalf("invalid override is not reported: %q", reason)
	}
}

func TestDisplayDeviceOffersVenusOnlyWhenEnabled(t *testing.T) {
	for _, displays := range []int{1, 2} {
		for _, venus := range []bool{false, true} {
			cfg := &config{displays: displays, useGpu: true, venus: venus, displayWidth: 1920, displayHeight: 1080}
			device := displayDevice(cfg, 1<<30)
			hasVenus := strings.Contains(device, "venus=on") || strings.Contains(device, `"venus":true`)
			if hasVenus != venus {
				t.Fatalf("displays=%d venus=%v: %s", displays, venus, device)
			}
			// virgl maps blob resources through hostmem with or without Venus.
			if !strings.Contains(device, "blob") || !strings.Contains(device, "hostmem") {
				t.Fatalf("displays=%d venus=%v lost blob or hostmem: %s", displays, venus, device)
			}
		}
	}
	if device := displayDevice(&config{displays: 1, venus: true}, 1<<30); strings.Contains(device, "venus") {
		t.Fatalf("CPU rendering offers Venus: %s", device)
	}
}
