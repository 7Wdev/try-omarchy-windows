package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const uninstallStateName = "uninstall-state.json"

type uninstallState struct {
	Version   int    `json:"version"`
	Directory string `json:"directory"`
}

func saveUninstallState(dir string) error {
	path := filepath.Join(dir, uninstallStateName)
	if err := validateMovePath(path); err != nil {
		return err
	}
	data, err := json.Marshal(uninstallState{Version: 1, Directory: dir})
	if err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".uninstall-state-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err := f.Write(data); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return publishMoveFile(f.Name(), path)
}

func validUninstallState(dir string) bool {
	return validUninstallStateAt(dir, dir)
}

func validUninstallStateAt(dir, expected string) bool {
	path := filepath.Join(dir, uninstallStateName)
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() || info.Size() > 64<<10 || validateMovePath(path) != nil {
		return false
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	var state uninstallState
	return json.Unmarshal(data, &state) == nil && state.Version == 1 && filepath.IsAbs(state.Directory) && filepath.Clean(state.Directory) == state.Directory && pathsEqual(state.Directory, expected)
}

// uninstallMarkers are files that only a Try Omarchy data folder holds. The
// Apps & features entry is written after the guest and disk are complete, so
// a real installation has several of them.
var uninstallMarkers = []string{
	filepath.Join("vm", "disk.raw"),
	filepath.Join("guest", installReceiptFilename),
	filepath.Join("runtime", runtimeReceiptFilename),
	provisionModeFilename,
	networkIdentityFilename,
}

// validateUninstallDirectory refuses to delete a folder that is not a Try
// Omarchy data folder. Uninstall removes the whole folder, and -dir is
// taken from the command line, so a mistyped or crafted path must not
// delete a drive or someone's documents.
func validateUninstallDirectory(dir string) error {
	if !filepath.IsAbs(dir) || filepath.Clean(dir) != dir {
		return fmt.Errorf("%s is not a clean absolute path; nothing was removed", dir)
	}
	if filepath.Dir(dir) == dir {
		return fmt.Errorf("%s is a drive root; nothing was removed", dir)
	}
	info, err := os.Lstat(dir)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("%s is not a folder; nothing was removed", dir)
	}
	if err := validateMovePath(dir); err != nil {
		return err
	}
	for _, marker := range uninstallMarkers {
		path := filepath.Join(dir, marker)
		if info, err := os.Lstat(path); err == nil && info.Mode().IsRegular() && validateMovePath(path) == nil {
			return nil
		}
	}
	if validUninstallState(dir) {
		return nil
	}
	return fmt.Errorf("%s is not a Try Omarchy data folder; nothing was removed", dir)
}

// Keep a removal receipt until the directory is gone. A partial deletion may
// have removed every original marker; the next attempt must still be guarded.
func removeUninstallDirectory(dir string, remove func(string) error) error {
	if err := validateUninstallDirectory(dir); err != nil {
		return err
	}
	if err := saveUninstallState(dir); err != nil {
		return err
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.Name() == uninstallStateName || entry.Name() == stableLauncherName {
			continue
		}
		if err := validateUninstallDirectory(dir); err != nil {
			return err
		}
		if err := remove(filepath.Join(dir, entry.Name())); err != nil {
			return err
		}
	}
	if err := validateUninstallDirectory(dir); err != nil {
		return err
	}
	if err := remove(filepath.Join(dir, stableLauncherName)); err != nil {
		return err
	}
	if err := validateUninstallDirectory(dir); err != nil {
		return err
	}
	if err := os.Remove(filepath.Join(dir, uninstallStateName)); err != nil {
		return err
	}
	if err := os.Remove(dir); err != nil {
		if receiptErr := saveUninstallState(dir); receiptErr != nil {
			return fmt.Errorf("removing the folder: %v; preserving retry state: %w", err, receiptErr)
		}
		return err
	}
	return nil
}

// Called only after guarded removal succeeds. The pointer proves which moved
// installation owned the bootstrap. Portable host state and unknown files stay.
func cleanupUninstallBootstrap(defaultDir, dir string) error {
	pointed, found, err := loadDataLocationPointer(defaultDir)
	if err != nil {
		return err
	}
	if !found || !pathsEqual(pointed, dir) {
		return nil
	}
	if err := validateMovePath(defaultDir); err != nil {
		return err
	}
	diagnostics := filepath.Join(defaultDir, "diagnostics")
	if err := validateMovePath(diagnostics); err != nil {
		return err
	}
	entries, err := os.ReadDir(diagnostics)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	for _, entry := range entries {
		name := entry.Name()
		stamp := strings.TrimSuffix(strings.TrimPrefix(name, "try-omarchy-diagnostics-"), ".zip")
		if _, err := time.Parse("20060102-150405.000000000", stamp); err != nil {
			continue
		}
		if name != "try-omarchy-diagnostics-"+stamp+".zip" {
			continue
		}
		path := filepath.Join(diagnostics, name)
		info, err := os.Lstat(path)
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			continue
		}
		if err := validateMovePath(path); err != nil {
			return err
		}
		if err := os.Remove(path); err != nil {
			return err
		}
	}
	if entries, err := os.ReadDir(diagnostics); err == nil && len(entries) == 0 {
		if err := os.Remove(diagnostics); err != nil {
			return err
		}
	}
	if err := os.Remove(dataLocationPointerPath(defaultDir)); err != nil {
		return err
	}
	entries, err = os.ReadDir(defaultDir)
	if err != nil {
		return err
	}
	if len(entries) == 0 {
		return os.Remove(defaultDir)
	}
	return nil
}
