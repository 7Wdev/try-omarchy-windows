//go:build windows

package main

import (
	"context"
	"time"
)

// One cheap volume query per minute, independent of the QMP liveness loop.
// The monitor belongs to the launcher lifetime, including guest reboots.
func startHostDiskMonitor(dataDir string) func() {
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		var state hostDiskWarningState
		reportedError := false
		ticker := time.NewTicker(time.Minute)
		defer ticker.Stop()
		for {
			if ctx.Err() != nil {
				return
			}
			space, err := platformDiskVolumeSpace(dataDir)
			if ctx.Err() != nil {
				return
			}
			if err != nil {
				if !reportedError {
					logf("host disk space check failed: %v", err)
				}
				reportedError = true
			} else {
				reportedError = false
				showHostDiskWarning(state.observe(space), space.available)
			}
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
			}
		}
	}()
	return cancel
}

func showHostDiskWarning(level hostDiskWarning, available int64) {
	values := map[string]string{"available": formatGiB(available)}
	switch level {
	case hostDiskLow:
		message := uiTextWith("tray.disk.low", values)
		logf("%s", message)
		showTrayNotice(uiText("tray.disk.low_title"), message)
	case hostDiskCritical:
		message := uiTextWith("tray.disk.critical", values)
		logf("%s", message)
		showTrayNotice(uiText("tray.disk.critical_title"), message)
	}
}
