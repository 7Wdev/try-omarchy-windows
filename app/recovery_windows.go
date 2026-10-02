//go:build windows

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

func chooseBackupDestination() (string, bool, error) {
	for {
		name, ok, err := chooseRecoveryPath(0, uiText("recovery.backup.choose"), "Omarchy-"+time.Now().Format("2006-01-02-150405")+".zip", true, false)
		if err != nil || !ok {
			return "", ok, err
		}
		if _, err = os.Lstat(name); os.IsNotExist(err) {
			return name, true, nil
		} else if err != nil {
			return "", false, err
		}
		infoBox(uiText("recovery.backup.exists"))
	}
}

func beginRecoveryProgress(status string) {
	ui := getUI()
	ui.cancelMessage.Store(uiText("recovery.cancel"))
	ui.setProgress(0, 0)
	ui.setStatus("%s", status)
}

func runRecoveryUI(dir, action string) error {
	configureSetupCancellation(false)
	switch action {
	case "portable-create":
		parent, ok, err := chooseRecoveryPath(0, uiText("recovery.portable.choose"), "", false, true)
		if err != nil || !ok {
			return err
		}
		self, err := os.Executable()
		if err != nil {
			return err
		}
		destination := filepath.Join(parent, "OmarchyPortable-"+time.Now().Format("20060102-150405"))
		beginRecoveryProgress(uiText("recovery.portable.status"))
		err = createPortableCopy(dir, destination, self, recoveryProgress(recoveryCopying))
		uiDone()
		if err != nil {
			return err
		}
		infoBox(uiTextWith("recovery.portable.done", map[string]string{"path": destination}))
	case "snapshots":
		return runCheckpointUI(dir)
	case "move", "move-cleanup":
		return runMoveUI(dir, action == "move-cleanup")
	case "backup":
		name, ok, err := chooseBackupDestination()
		if err != nil || !ok {
			return err
		}
		beginRecoveryProgress(uiText("recovery.backup.status"))
		defer uiDone()
		if err = writeVMBackupProgress(dir, name, recoveryProgress(recoveryBackingUp)); err != nil {
			return err
		}
		uiDone()
		infoBox(uiTextWith("recovery.backup.done", map[string]string{"path": name}))
	case "restore":
		source, ok, err := chooseRecoveryPath(0, uiText("recovery.restore.choose_backup"), "", false, false)
		if err != nil || !ok {
			return err
		}
		parent, ok, err := chooseRecoveryPath(0, uiText("recovery.restore.choose_folder"), "", false, true)
		if err != nil || !ok {
			return err
		}
		destination := filepath.Join(parent, "OmarchyRestored-"+time.Now().Format("2006-01-02-150405"))
		if msgBox(uiTextWith("recovery.restore.confirm", map[string]string{"path": destination}), mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
			return nil
		}
		beginRecoveryProgress(uiText("recovery.restore.status"))
		defer uiDone()
		if err = restoreVMBackupProgress(source, destination, recoveryProgress(recoveryRestoring)); err != nil {
			return err
		}
		uiDone()
		if err := createRestoredLaunchers(destination); err != nil {
			infoBox(uiTextWith("recovery.restore.done_no_shortcuts", map[string]string{"path": destination, "error": err.Error()}))
		} else {
			// The restored copy has its shortcuts; the first launch must not
			// offer them again.
			if err := recordShortcutOffer(destination); err != nil {
				logf("could not record the shortcut offer for %s: %v", destination, err)
			}
			infoBox(uiTextWith("recovery.restore.done", map[string]string{"path": destination}))
		}
	case "reset":
		return resetFromSettings(dir)
	case "uninstall":
		return runUninstall(dir)
	case "install-omarchy":
		return runInstallOmarchyUI(dir)
	default:
		return fmt.Errorf("unknown recovery action")
	}
	return nil
}

// Backup failure or cancellation must never fall through into reset.
func confirmResetBackup(dir string) (bool, error) {
	choice := msgBox(uiText("recovery.reset.confirm_backup"), 3|mbIconQuestion|0x200)
	if choice != idYes && choice != idNo {
		return false, nil
	}
	if choice == idYes {
		name, ok, err := chooseBackupDestination()
		if err != nil || !ok {
			return false, err
		}
		beginRecoveryProgress(uiText("recovery.reset.backup_status"))
		if err = writeVMBackupProgress(dir, name, recoveryProgress(recoveryBackingUp)); err != nil {
			return false, err
		}
	}
	if msgBox(uiText("recovery.reset.confirm"), mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
		return false, nil
	}
	return !setupCancelled(), checkSetupCancelled()
}

func resetFromSettings(dir string) error {
	if !completeInstallExists(dir, "disk.raw") {
		return uiError(uiText("error.reset.no_install"), nil)
	}
	proceed, err := confirmResetBackup(dir)
	if err != nil || !proceed {
		return err
	}
	data, err := os.ReadFile(filepath.Join(dir, "guest", "build-spec.json"))
	if err != nil {
		return err
	}
	var spec buildSpec
	if err = json.Unmarshal(data, &spec); err != nil {
		return err
	}
	storage, err := loadStorageSettings(dir)
	if err != nil {
		return err
	}
	cfg := &config{dir: dir, guestDir: filepath.Join(dir, "guest"), vmDir: filepath.Join(dir, "vm"), disk: filepath.Join(dir, "vm", "disk.raw"), diskFormat: "raw", diskGiB: storage.DiskGiB}
	beginRecoveryProgress(uiText("recovery.reset.status"))
	old, err := resetStandardDisk(cfg, spec.Runtime.Storage.ExpandedSizeMiB)
	if err != nil {
		return err
	}
	uiDone()
	infoBox(uiTextWith("recovery.reset.done", map[string]string{"path": old}))
	return nil
}

func reportRecoveryResult(err error) {
	uiDone()
	if err != nil && !errors.Is(err, errSetupCancelled) {
		errorBox(uiTextWith("recovery.error", map[string]string{"error": err.Error()}))
	}
}

// Place launchers beside the restored data, never over the original desktop or
// Start-menu links. The explicit -dir argument keeps both installations separate.
func createRestoredLaunchers(dir string) error {
	target, err := stableLauncherPath(dir)
	if err != nil {
		return err
	}
	prefs, err := loadLaunchPreferences(dir)
	if err != nil {
		return err
	}
	// A backup can be restored beside its original installation. Keep direct
	// launch preferences, but require an explicit choice before that new copy
	// starts at every Windows sign-in too.
	if prefs.LaunchAtSignIn {
		prefs.LaunchAtSignIn = false
		if err := saveLaunchPreferences(dir, prefs); err != nil {
			return err
		}
	}
	for _, item := range []struct{ name, arguments string }{
		{"Start Omarchy.lnk", launchShortcutArguments(dir, prefs.StartAutomatically)},
		{"Settings.lnk", settingsShortcutArguments(dir)},
	} {
		path := filepath.Join(dir, item.name)
		if _, err := os.Lstat(path); err == nil {
			return fmt.Errorf("shortcut already exists: %s", path)
		} else if !os.IsNotExist(err) {
			return err
		}
		if err := writeShellLink(path, target, item.arguments, dir); err != nil {
			return err
		}
	}
	return recordShortcutOffer(dir)
}

// recoveryStep names what a recovery progress line says it is doing.
type recoveryStep int

const (
	recoveryBackingUp recoveryStep = iota
	recoveryRestoring
	recoveryCopying
	recoveryMoving
)

func (s recoveryStep) status(file string) string {
	values := map[string]string{"file": file}
	switch s {
	case recoveryRestoring:
		return uiTextWith("recovery.progress.restoring", values)
	case recoveryCopying:
		return uiTextWith("recovery.progress.copying", values)
	case recoveryMoving:
		return uiTextWith("recovery.progress.moving", values)
	default:
		return uiTextWith("recovery.progress.backing_up", values)
	}
}

func recoveryProgress(step recoveryStep) backupProgress {
	last := ""
	return func(current, total int64, name string) {
		ui := getUI()
		if name != last {
			if isProgressPhase(name) {
				ui.setStatus("%s", name)
			} else {
				ui.setStatus("%s", step.status(name))
			}
			last = name
		}
		ui.setProgress(current, total)
	}
}

// A retained portable disk still uses its relative guest backing path. Keep
// that layout and provide a portable launcher in the enclosing recovery bundle.
func createRollbackRecoveryLaunchers(dir, sourceInstallation string) error {
	disk, err := inspectInstallationDisk(dir)
	if err != nil {
		return err
	}
	if disk.Format == "raw" {
		return createRestoredLaunchers(dir)
	}
	self, err := os.Executable()
	if err != nil {
		return err
	}
	root := filepath.Dir(dir)
	if err := copyLauncher(self, filepath.Join(root, stableLauncherName), replaceLauncher); err != nil {
		return err
	}
	arguments, err := preparePortableRecoveryPayload(dir, filepath.Join(filepath.Dir(sourceInstallation), "payload"))
	if err != nil {
		return err
	}
	for _, name := range []string{"Start Omarchy.cmd", "Settings.cmd"} {
		args := append([]string(nil), arguments...)
		if name == "Settings.cmd" {
			args = append(args, "-settings")
		}
		command, err := portableRecoveryCommand(args)
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(root, name), []byte(command), 0600); err != nil {
			return err
		}
	}
	return nil
}
