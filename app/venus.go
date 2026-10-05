package main

import (
	"fmt"
	"strings"
)

// venusVariable forces Venus, the guest's Vulkan path to the host GPU, on or
// off for testing a driver the automatic choice gets wrong.
const venusVariable = "TRYOMARCHY_VENUS"

// venusDecision decides whether the GPU device offers Venus. On NVIDIA hosts
// a Vulkan swapchain (mpv) freezes the guest compositor for good, GTK4 aborts
// while creating its Vulkan device, and an RTX 5080 run hung QEMU (see
// docs/evidence/RESOURCE-PROFILES-INTEL-NVIDIA-2026-09-20.md). Without Venus
// the guest's Vulkan loader finds only lavapipe, which GTK4 skips, so apps use
// OpenGL through virgl. Any NVIDIA adapter counts: a hybrid laptop may render
// QEMU on it. Other vendors and an unreadable driver list keep Venus.
func venusDecision(override, displayDriver string) (bool, string) {
	switch value := strings.ToLower(strings.TrimSpace(override)); value {
	case "on":
		return true, venusVariable + "=on"
	case "off":
		return false, venusVariable + "=off"
	case "":
	default:
		enabled, reason := venusDecision("", displayDriver)
		return enabled, fmt.Sprintf("ignoring %s=%q (use on or off); %s", venusVariable, override, reason)
	}
	if strings.Contains(strings.ToLower(displayDriver), "nvidia") {
		return false, "NVIDIA display driver, Vulkan apps use the guest's software Vulkan or OpenGL (" + venusVariable + "=on forces Venus)"
	}
	return true, "display driver is not NVIDIA"
}
