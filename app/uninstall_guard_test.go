package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestValidateUninstallDirectoryNeedsADataFolder(t *testing.T) {
	for _, marker := range uninstallMarkers {
		dir := t.TempDir()
		path := filepath.Join(dir, marker)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
		if err := validateUninstallDirectory(dir); err != nil {
			t.Errorf("%s: %v", marker, err)
		}
	}

	// A folder of documents, even one with the same subfolder names and a
	// downloaded launcher, is not an installation.
	docs := t.TempDir()
	for _, name := range []string{"vm", "guest", "runtime", filepath.Join("vm", "disk.raw", "inner")} {
		if err := os.MkdirAll(filepath.Join(docs, name), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	for _, name := range []string{stableLauncherName, "settings.json", "notes.txt"} {
		if err := os.WriteFile(filepath.Join(docs, name), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if err := validateUninstallDirectory(docs); err == nil {
		t.Fatal("accepted a folder that is not a data folder")
	}

	for _, dir := range []string{
		"",
		"relative",
		docs + string(filepath.Separator) + ".." + string(filepath.Separator) + filepath.Base(docs),
		filepath.Join(docs, "missing"),
		filepath.Join(docs, "notes.txt"),
		filepath.VolumeName(docs) + string(filepath.Separator),
	} {
		if err := validateUninstallDirectory(dir); err == nil {
			t.Errorf("accepted %q", dir)
		}
	}
}
