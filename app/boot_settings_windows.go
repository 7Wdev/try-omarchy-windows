//go:build windows

package main

import (
	"fmt"
	"sync/atomic"
	"time"
)

// bootPlan holds what every guest boot is built from. Under -no-reboot a guest
// reboot exits QEMU and the supervisor starts it again, so each relaunch
// re-reads the saved settings: the Settings window promises that Resources,
// Graphics, displays and the shared folder apply at the next boot.
type bootPlan struct {
	explicit map[string]bool
	// baseCmdline is the kernel command line without the per-boot words:
	// shared folder name, locale and console size.
	baseCmdline                  string
	profile                      string
	forceCPU                     bool
	timeZone, keyboard, language string
	home                         string
	// The launch picked one runtime and keeps it. Stock QEMU renders only on
	// the CPU and drives one display; only the bundled runtime drives several.
	gpuRuntime, multiDisplayRuntime bool
}

// bootShare is the folder shared with the running boot, for the tray.
var bootShare atomic.Pointer[string]

func currentShare() string {
	if share := bootShare.Load(); share != nil {
		return *share
	}
	return ""
}

// applyBootSettings copies the saved choices that take effect at a guest
// reboot into cfg and returns the ones this launch cannot apply. Fullscreen
// stays as launched: the window watcher keeps the launch's mode. Forwards
// follow forward_live.go instead.
func applyBootSettings(cfg *config, plan *bootPlan, saved settings, resources resourcePreferences, validateShare func(string) (string, error)) []string {
	next := *cfg
	var forwards forwardList
	var sshKey string
	if err := applySettings(&next, saved, plan.explicit, &forwards, &sshKey); err != nil {
		return []string{"saved settings not applied: " + err.Error()}
	}
	var notes []string
	if !plan.explicit["resource-profile"] {
		if err := validateResourceProfile(resources.Profile); err != nil {
			notes = append(notes, "resource profile not applied: "+err.Error())
		} else {
			plan.profile = resources.Profile
		}
	}
	cfg.memOverrideMiB, cfg.cpuOverride = next.memOverrideMiB, next.cpuOverride

	mode := next.renderMode
	if plan.forceCPU {
		mode = renderCPU
	}
	if mode != renderCPU && !plan.gpuRuntime {
		if mode != cfg.renderMode {
			notes = append(notes, fmt.Sprintf("render %s needs the bundled runtime; it applies when Try Omarchy starts again", mode))
		}
		mode = cfg.renderMode
	}
	cfg.renderMode = mode
	cfg.noGpu = mode == renderCPU

	if next.displays > 1 && !plan.multiDisplayRuntime {
		if next.displays != cfg.displays {
			notes = append(notes, fmt.Sprintf("%d displays need the bundled runtime; they apply when Try Omarchy starts again", next.displays))
		}
	} else {
		cfg.displays = next.displays
	}

	share := next.share
	if share != "" && !cfg.supportsSharing {
		notes = append(notes, "shared folder disabled for this boot: selected QEMU has no virtio-9p")
		share = ""
	} else if share != "" {
		validated, err := validateShare(share)
		if err != nil {
			notes = append(notes, fmt.Sprintf("shared folder disabled for this boot: %v", err))
			share = ""
		} else {
			share = validated
		}
	}
	cfg.share = share
	return notes
}

// reloadBootSettings runs before a relaunch after a guest reboot. A damaged
// settings file keeps the previous boot's choices rather than stopping a
// running session; the next launch reports it.
func reloadBootSettings(cfg *config, plan *bootPlan) {
	if saved, err := loadSettings(settingsPath(cfg.dir)); err != nil {
		logf("reboot: keeping the previous settings: %v", err)
	} else {
		resources, err := loadResourcePreferences(cfg.dir)
		if err != nil {
			logf("reboot: keeping the previous resource profile: %v", err)
			resources.Profile = plan.profile
		}
		validate := func(path string) (string, error) { return validateWindowsSharedFolder(path, cfg.dir, plan.home) }
		for _, note := range applyBootSettings(cfg, plan, saved, resources, validate) {
			logf("reboot: %s", note)
		}
	}
	cfg.useGpu = false
	if plan.gpuRuntime {
		probe, err := loadRenderProbe(cfg.dir)
		if err != nil {
			logf("ignoring %s: %v", renderProbeFilename, err)
		}
		var reason string
		cfg.useGpu, reason = startWithGPU(cfg.renderMode, probe, cfg.runtimeID, cfg.displayDriver, time.Now())
		if reason != "" {
			logf("rendering: %s", reason)
		}
	}
	// Startup fallbacks recover one failed start, so a clean reboot tries the
	// preferred audio again; memory is planned afresh by planBootResources. A
	// host that refused nested virtualization keeps refusing, so
	// kernel-irqchip=off stays.
	cfg.audio = "sdl"
	logf("reboot: applied saved settings - render=%s displays=%d share=%t", cfg.renderMode, cfg.displays, cfg.share != "")
}

// planBootResources sizes the guest for the next boot and logs it.
func planBootResources(cfg *config, plan *bootPlan) error {
	profile := effectiveResourceProfile(plan.profile, cfg.cpuOverride, cfg.memOverrideMiB)
	host := measureHostResources(profile == resourceMaximum)
	allocation, err := planGuestResources(profile, host, cfg.useGpu, cfg.cpuOverride, cfg.memOverrideMiB,
		plan.explicit["cpus"], plan.explicit["memory"])
	if err != nil {
		return err
	}
	cfg.cpus, cfg.memMiB, cfg.hostTotalMiB = allocation.CPUs, allocation.MemoryMiB, host.TotalMiB
	logf("resources: profile=%s, %d of %d logical processors, %d MiB guest RAM; Windows available=%d MiB, CPU sample known=%t busy=%.1f%%",
		profile, cfg.cpus, host.LogicalCPUs, cfg.memMiB, host.AvailableMiB, host.CPUKnown, host.CPUBusy*100)
	return nil
}

// bootCmdline finishes the kernel command line for one boot: the shared
// folder's name, the Windows locale and the console sized to the window.
func bootCmdline(cfg *config, plan *bootPlan) string {
	cmdline := plan.baseCmdline + shareCmdline(cfg.share)
	zone, layout, variant, locale := hostLocale(plan.timeZone, plan.keyboard, plan.language)
	if words := hostLocaleCmdline(zone, layout, variant, locale); words != "" {
		cmdline += words
		logf("guest follows Windows locale:%s", words)
	}
	// Launch-UX contract (NOTES.md): guest console sized to the window it will
	// actually get, so the picture fills it from the first frame.
	conW, conH := screenSize(cfg.fullscreen)
	if cfg.fullscreen {
		conW, conH = fullscreenTargetSize(cfg.fullscreenDisplay)
	}
	if !cfg.fullscreen {
		if p := rememberedWindow(cfg.dir); p != nil && !p.Maximized {
			conW, conH = p.consoleSize()
		}
	}
	cfg.displayWidth, cfg.displayHeight = conW, conH
	share := cfg.share
	bootShare.Store(&share)
	return cmdline + fmt.Sprintf(" video=%dx%d", conW, conH)
}
