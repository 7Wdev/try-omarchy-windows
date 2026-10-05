//go:build windows

package main

import (
	"errors"
	"strings"
	"testing"
)

func acceptShare(path string) (string, error) { return path, nil }

func TestRebootAppliesSavedSettings(t *testing.T) {
	cfg := &config{displays: 1, renderMode: renderAuto, supportsSharing: true, fullscreen: false}
	plan := &bootPlan{explicit: map[string]bool{}, gpuRuntime: true, multiDisplayRuntime: true}
	saved := settings{SchemaVersion: settingsSchemaVersion, Displays: 2, MemoryMiB: 8192, CPUs: 6, Render: renderCPU,
		Share: `C:\Work`, Fullscreen: true}
	notes := applyBootSettings(cfg, plan, saved, resourcePreferences{Profile: resourceManual}, acceptShare)
	if len(notes) != 0 {
		t.Fatalf("unexpected notes: %v", notes)
	}
	if cfg.displays != 2 || cfg.memOverrideMiB != 8192 || cfg.cpuOverride != 6 || cfg.renderMode != renderCPU || !cfg.noGpu {
		t.Fatalf("saved settings not applied: %+v", cfg)
	}
	if cfg.share != `C:\Work` || plan.profile != resourceManual {
		t.Fatalf("share %q profile %q", cfg.share, plan.profile)
	}
	if cfg.fullscreen {
		t.Fatal("fullscreen must stay as launched")
	}
}

func TestRebootKeepsCommandLineChoices(t *testing.T) {
	cfg := &config{displays: 1, memOverrideMiB: 4096, cpuOverride: 2, renderMode: renderCPU, supportsSharing: true}
	plan := &bootPlan{
		explicit:   map[string]bool{"memory": true, "cpus": true, "displays": true, "resource-profile": true, "nogpu": true},
		profile:    resourceBalanced,
		forceCPU:   true,
		gpuRuntime: true, multiDisplayRuntime: true,
	}
	saved := settings{SchemaVersion: settingsSchemaVersion, Displays: 3, MemoryMiB: 8192, CPUs: 6, Render: renderGPU}
	applyBootSettings(cfg, plan, saved, resourcePreferences{Profile: resourceMaximum}, acceptShare)
	if cfg.displays != 1 || cfg.memOverrideMiB != 4096 || cfg.cpuOverride != 2 || cfg.renderMode != renderCPU || plan.profile != resourceBalanced {
		t.Fatalf("command-line choices lost: %+v profile=%s", cfg, plan.profile)
	}
}

func TestRebootRespectsTheLaunchRuntime(t *testing.T) {
	cfg := &config{displays: 1, renderMode: renderCPU, noGpu: true}
	plan := &bootPlan{explicit: map[string]bool{}}
	saved := settings{SchemaVersion: settingsSchemaVersion, Displays: 2, Render: renderGPU, Share: `C:\Work`}
	notes := applyBootSettings(cfg, plan, saved, resourcePreferences{}, acceptShare)
	if cfg.displays != 1 || cfg.renderMode != renderCPU || !cfg.noGpu || cfg.share != "" {
		t.Fatalf("stock QEMU given unsupported choices: %+v", cfg)
	}
	if len(notes) != 3 {
		t.Fatalf("want one note per unavailable choice, got %v", notes)
	}
}

func TestRebootDropsAnInvalidSharedFolder(t *testing.T) {
	cfg := &config{displays: 1, renderMode: renderAuto, supportsSharing: true, share: `C:\Old`}
	plan := &bootPlan{explicit: map[string]bool{}, gpuRuntime: true}
	saved := settings{SchemaVersion: settingsSchemaVersion, Share: `C:\Gone`}
	notes := applyBootSettings(cfg, plan, saved, resourcePreferences{}, func(string) (string, error) {
		return "", errors.New("folder is missing")
	})
	if cfg.share != "" || len(notes) != 1 || !strings.Contains(notes[0], "folder is missing") {
		t.Fatalf("share %q notes %v", cfg.share, notes)
	}
}

func TestRebootKeepsPreviousChoicesForBadSettings(t *testing.T) {
	cfg := &config{displays: 2, memOverrideMiB: 4096, renderMode: renderGPU}
	plan := &bootPlan{explicit: map[string]bool{}, gpuRuntime: true, multiDisplayRuntime: true, profile: resourceManual}
	saved := settings{SchemaVersion: settingsSchemaVersion, Displays: 1, Render: "metal"}
	notes := applyBootSettings(cfg, plan, saved, resourcePreferences{Profile: resourceMaximum}, acceptShare)
	if len(notes) != 1 || cfg.displays != 2 || cfg.memOverrideMiB != 4096 || cfg.renderMode != renderGPU || plan.profile != resourceManual {
		t.Fatalf("bad settings changed the boot: %+v notes=%v", cfg, notes)
	}
}
