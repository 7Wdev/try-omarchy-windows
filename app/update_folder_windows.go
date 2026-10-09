//go:build windows

package main

import (
	"strings"
	"sync/atomic"
)

var updateFolderOpen atomic.Bool

func installUpdateFromFolder(cfg trayLaunchConfig, owner uintptr) {
	if !updateFolderOpen.CompareAndSwap(false, true) {
		return
	}
	snapshot := cfg
	go func() {
		defer updateFolderOpen.Store(false)
		pins := pinnedPayloadUpdate{defaultReleaseURL, defaultSumsSHA256, defaultReleaseURL, defaultSumsSHA256}
		if defaultRuntimeReleaseURL != "" {
			pins.RuntimeRelease = defaultRuntimeReleaseURL
		}
		if defaultRuntimeSumsSHA256 != "" {
			pins.RuntimeDigest = defaultRuntimeSumsSHA256
		}
		if pins.Digest != pins.RuntimeDigest {
			infoBox(uiText("update.folder.separate_releases"))
			return
		}
		if msgBox(uiTextWith("update.folder.instructions", map[string]string{
			"version": currentVersion, "files": strings.Join(append([]string{"SHA256SUMS"}, updatePayloadNames()...), "\n"),
			"url": strings.Replace(pins.Release, "/releases/download/", "/releases/tag/", 1),
		}), 1|mbIconInformation) != 1 {
			return
		}
		folder, chosen, err := chooseRecoveryPath(owner, uiText("update.folder.choose"), "", false, true)
		if err == nil && !chosen {
			return
		}
		if err == nil {
			ctx := setupContext()
			err = withUpdateStaging(ctx, func() error {
				if updateAvailable.Load() {
					return nil
				}
				if err := stagePinnedPayloadFolder(ctx, folder, snapshot.dataDir,
					updatePayloadRoot(snapshot.dataDir, "", snapshot.portable), pins); err != nil {
					return err
				}
				updateAvailable.Store(true)
				showTrayNotice(uiText("update.notice.title"), uiTextWith("update.notice.ready", map[string]string{"version": currentVersion}))
				return nil
			})
		}
		if err != nil && setupContext().Err() == nil {
			errorBox(uiTextWith("update.folder.failed", map[string]string{"error": err.Error()}))
		}
	}()
}
