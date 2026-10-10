//go:build windows

package main

import (
	"path/filepath"
	"testing"
)

func TestUpdateReadinessBelongsToInstallation(t *testing.T) {
	previous := updateAvailable.Load()
	defer updateAvailable.Store(previous)
	updateAvailable.Store(true)
	staged, other := t.TempDir(), t.TempDir()
	if err := writeUpdateFile(filepath.Join(launcherUpdateDir(staged), stagedUpdateFilename), []byte(currentVersion)); err != nil {
		t.Fatal(err)
	}
	if !installationUpdateReady(staged) {
		t.Fatal("staged installation not ready")
	}
	if installationUpdateReady(other) {
		t.Fatal("another installation inherited readiness")
	}
}
