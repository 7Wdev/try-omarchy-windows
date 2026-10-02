package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// createPortableCopy publishes a complete independent installation only after
// its disk conversion and content verification succeed. The source is retained.
func createPortableCopy(dir, destination, launcher string, report backupProgress) error {
	tool := "qemu-img"
	if runtime.GOOS == "windows" {
		tool += ".exe"
	}
	tool = filepath.Join(dir, "runtime", "bin", tool)
	return createPortableCopyUsingTool(dir, destination, launcher, tool, report)
}

func createPortableCopyUsingTool(dir, destination, launcher, tool string, report backupProgress) error {
	destination, err := filepath.Abs(destination)
	if err != nil {
		return err
	}
	dir, err = filepath.Abs(dir)
	if err != nil {
		return err
	}
	if pathsOverlap(dir, destination) {
		return uiError(uiText("error.portable.destination"), nil)
	}
	if err := validateMovePath(destination); err != nil {
		return err
	}
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		return uiError(uiText("error.portable.new_folder"), nil)
	}
	sums, err := parseVerifiedSums(defaultSums, defaultSumsSHA256)
	if err != nil {
		return err
	}
	ready, err := installReceiptMatches(filepath.Join(dir, "guest"), defaultReleaseURL, defaultSumsSHA256, installedGuestArtifacts)
	if err != nil {
		return err
	}
	if !ready {
		return uiError(uiText("error.portable.update_first"), nil)
	}
	if !runtimeReceiptMatches(filepath.Join(dir, "runtime"), defaultReleaseURL, defaultSumsSHA256, sums[runtimeZip]) {
		return uiError(uiText("error.portable.runtime"), nil)
	}
	stage, err := os.MkdirTemp(filepath.Dir(destination), ".try-omarchy-portable-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	bundle := filepath.Join(stage, "bundle")
	if err := os.Mkdir(bundle, 0700); err != nil {
		return err
	}
	data := filepath.Join(bundle, "data")
	if err := stagePortableData(dir, data, tool, report); err != nil {
		return err
	}
	if err := copyLauncher(launcher, filepath.Join(bundle, stableLauncherName), os.Rename); err != nil {
		return err
	}
	payload := filepath.Join(bundle, "payload")
	if err := os.Mkdir(payload, 0700); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(payload, "SHA256SUMS"), defaultSums, 0600); err != nil {
		return err
	}
	for name, flags := range map[string]string{"Start Omarchy.cmd": "-portable", "Settings.cmd": "-portable -settings"} {
		if err := os.WriteFile(filepath.Join(bundle, name), []byte("@echo off\r\n\"%~dp0TryOmarchy.exe\" "+flags+" %*\r\n"), 0600); err != nil {
			return err
		}
	}
	if err := checkSetupCancelled(); err != nil {
		return err
	}
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		return fmt.Errorf("portable destination appeared during copying")
	}
	return renamePortableFileWithRetry(bundle, destination)
}

func makeRestoredDiskPortable(data, tool string, report backupProgress) error {
	raw := filepath.Join(data, "vm", "disk.raw")
	info, err := os.Stat(raw)
	if err != nil {
		return err
	}
	if err := requireDiskSpace(data, info.Size()+diskSpaceReserve); err != nil {
		return err
	}
	overlay := filepath.Join(data, "vm", "disk.qcow2")
	if report != nil {
		report(0, info.Size(), progressPhase(uiText("progress.creating_portable_disk")))
	}
	cmd := exec.CommandContext(setupContext(), tool, "convert", "-f", "raw", "-O", "qcow2", raw, overlay)
	configureDiskTool(cmd)
	var detail diskToolErrors
	cmd.Stderr = &detail
	if err := cmd.Run(); err != nil {
		if setupCancelled() {
			return errSetupCancelled
		}
		return fmt.Errorf("creating portable disk: %w: %s", err, detail.String())
	}
	// Compare logical contents before discarding the staging-only raw copy.
	cmd = exec.CommandContext(setupContext(), tool, "compare", "-f", "raw", "-F", "qcow2", raw, overlay)
	configureDiskTool(cmd)
	if err := cmd.Run(); err != nil {
		if setupCancelled() {
			return errSetupCancelled
		}
		return fmt.Errorf("verifying portable disk: %w", err)
	}
	f, err := os.OpenFile(overlay, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	err = f.Sync()
	f.Close()
	if err != nil {
		return err
	}
	if err := os.Remove(raw); err != nil {
		return err
	}
	if _, err := inspectInstallationDisk(data); err != nil {
		return err
	}
	return nil
}
