//go:build windows

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestWindowsAppsUninstallLinkedProfileShowsRecoveryGuidance(t *testing.T) {
	t.Setenv("LOCALAPPDATA", t.TempDir())
	root := t.TempDir()
	realProfile := filepath.Join(root, "real-profile")
	realDir := filepath.Join(realProfile, "TryOmarchy")
	if err := os.MkdirAll(realDir, 0700); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(realDir, provisionModeFilename)
	if err := os.WriteFile(marker, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	junction := filepath.Join(root, "linked-profile")
	if out, err := exec.Command(system32("cmd.exe"), "/c", "mklink", "/J", junction, realProfile).CombinedOutput(); err != nil {
		t.Skipf("junction unavailable: %v %s", err, out)
	}
	dir := filepath.Join(junction, "TryOmarchy")
	resolved, err := prepareMovedLocation(dir, true, true)
	if err != nil {
		t.Fatal(err)
	}
	err = runUninstall(resolved)
	if err == nil || !strings.Contains(err.Error(), junction) || !strings.Contains(err.Error(), "real folder path") || !strings.Contains(err.Error(), "manually") {
		t.Fatalf("Apps uninstall lost junction recovery guidance: %v", err)
	}
	if message := uiTextWith("recovery.error", map[string]string{"error": err.Error()}); !strings.Contains(message, junction) || !strings.Contains(message, "manually") {
		t.Fatalf("Apps uninstall dialog lost guidance: %s", message)
	}
	if data, err := os.ReadFile(marker); err != nil || string(data) != "keep" {
		t.Fatalf("data changed: %q err=%v", data, err)
	}
}

func TestUninstallRefusesJunctionAndLinkedMarkerAncestor(t *testing.T) {
	outside := t.TempDir()
	if err := os.WriteFile(filepath.Join(outside, provisionModeFilename), []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	junction := filepath.Join(root, "junction")
	if out, err := exec.Command(system32("cmd.exe"), "/c", "mklink", "/J", junction, outside).CombinedOutput(); err != nil {
		t.Skipf("junction unavailable: %v %s", err, out)
	}
	if err := removeUninstallDirectory(junction, func(string) error { t.Fatal("attempted junction deletion"); return nil }); err == nil || !strings.Contains(err.Error(), junction) || !strings.Contains(err.Error(), "manually") {
		t.Fatalf("junction root lost refusal or recovery guidance: %v", err)
	}
	docs := t.TempDir()
	if err := os.WriteFile(filepath.Join(outside, "disk.raw"), []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	if out, err := exec.Command(system32("cmd.exe"), "/c", "mklink", "/J", filepath.Join(docs, "vm"), outside).CombinedOutput(); err != nil {
		t.Fatal(err, string(out))
	}
	if err := validateUninstallDirectory(docs); err == nil {
		t.Fatal("accepted marker reached through junction")
	}
	if data, err := os.ReadFile(filepath.Join(outside, "disk.raw")); err != nil || string(data) != "keep" {
		t.Fatal("lost unrelated data")
	}
}

func TestWindowsUninstallFailureKeepsRetryStateAndBlocksSettings(t *testing.T) {
	t.Setenv("LOCALAPPDATA", t.TempDir())
	dir := t.TempDir()
	for _, name := range []string{provisionModeFilename, stableLauncherName, "locked.txt"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("keep"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	locked, err := openBackupDisk(filepath.Join(dir, "locked.txt"))
	if err != nil {
		t.Fatal(err)
	}
	defer locked.Close()
	if err := completeUninstall(dir); err == nil {
		t.Fatal("removed a locked installation")
	}
	if _, err := os.Stat(filepath.Join(dir, stableLauncherName)); err != nil {
		t.Fatal("lost retry launcher", err)
	}
	if err := validateUninstallDirectory(dir); err != nil {
		t.Fatal("lost deletion guard", err)
	}
	if err := checkMovedSettings(dir); err == nil {
		t.Fatal("stale settings could recreate a partly removed installation")
	}
	if err := locked.Close(); err != nil {
		t.Fatal(err)
	}
	if err := removeUninstallDirectory(dir, os.RemoveAll); err != nil {
		t.Fatal(err)
	}
}
