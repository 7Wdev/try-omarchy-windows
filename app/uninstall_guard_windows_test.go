//go:build windows

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

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
	if err := removeUninstallDirectory(junction, func(string) error { t.Fatal("attempted junction deletion"); return nil }); err == nil {
		t.Fatal("accepted junction root")
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
