package main

import (
	"errors"
	"math"
	"os"
	"path/filepath"
	"testing"
)

func TestPortableFilesystemLimits(t *testing.T) {
	original := queryFilesystemCapability
	t.Cleanup(func() { queryFilesystemCapability = original })
	for _, tc := range []struct {
		name    string
		limit   int64
		allowed bool
	}{
		{"FAT", (4 << 30) - 1, false}, {"fat32", (4 << 30) - 1, false},
		{"exFAT", math.MaxInt64, true}, {"NTFS", math.MaxInt64, true}, {"ReFS", math.MaxInt64, true},
	} {
		if limit := filesystemFileLimit(tc.name); limit != tc.limit {
			t.Fatalf("%s limit=%d", tc.name, limit)
		}
		queryFilesystemCapability = func(string) (filesystemCapability, error) {
			return filesystemCapability{Name: tc.name, MaxFileBytes: tc.limit}, nil
		}
		if err := requirePortableFilesystem("destination"); (err == nil) != tc.allowed {
			t.Fatalf("%s: %v", tc.name, err)
		}
	}
	failure := errors.New("volume unavailable")
	queryFilesystemCapability = func(string) (filesystemCapability, error) { return filesystemCapability{}, failure }
	if err := requirePortableFilesystem("destination"); !errors.Is(err, failure) {
		t.Fatalf("lost volume error: %v", err)
	}
}

func TestPortableStartupFilesystemQueryErrorKeepsLaunching(t *testing.T) {
	original := queryFilesystemCapability
	t.Cleanup(func() { queryFilesystemCapability = original })
	failure := errors.New("GetVolumeInformationW failed")
	queryFilesystemCapability = func(string) (filesystemCapability, error) {
		return filesystemCapability{}, failure
	}
	if err := requirePortableStartupFilesystem("existing-portable"); err != nil {
		t.Fatalf("query error blocked startup: %v", err)
	}
	if err := requirePortableFilesystem("new-portable"); !errors.Is(err, failure) {
		t.Fatalf("creation lost strict query check: %v", err)
	}
}

func TestPortableStartupRefusesFAT32AndAllowsUnknownFilesystem(t *testing.T) {
	original := queryFilesystemCapability
	t.Cleanup(func() { queryFilesystemCapability = original })
	for _, tc := range []struct {
		name    string
		allowed bool
	}{
		{"FAT", false}, {"fat32", false}, {"exFAT", true}, {"NTFS", true}, {"", true}, {"unknown", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			queryFilesystemCapability = func(string) (filesystemCapability, error) {
				return filesystemCapability{Name: tc.name}, nil
			}
			err := requirePortableStartupFilesystem("existing-portable")
			if (err == nil) != tc.allowed {
				t.Fatalf("startup allowed=%v err=%v", tc.allowed, err)
			}
			if !tc.allowed && err.Error() != uiText("error.portable.filesystem") {
				t.Fatalf("missing filesystem guidance: %v", err)
			}
		})
	}
}

func TestPortableFAT32FailsBeforeStagingOrOpeningSource(t *testing.T) {
	original := queryFilesystemCapability
	t.Cleanup(func() { queryFilesystemCapability = original })
	queryFilesystemCapability = func(string) (filesystemCapability, error) {
		return filesystemCapability{MaxFileBytes: (4 << 30) - 1}, nil
	}
	root := t.TempDir()
	source, destination := filepath.Join(root, "source"), filepath.Join(root, "portable")
	if err := createPortableCopyUsingTool(source, destination, "missing-launcher", "missing-tool", nil); err == nil {
		t.Fatal("accepted FAT32 copy")
	}
	if err := stageDirectPortableData(source, destination, installationDisk{}, "missing-tool", nil); err == nil {
		t.Fatal("accepted FAT32 conversion")
	}
	entries, err := os.ReadDir(root)
	if err != nil || len(entries) != 0 {
		t.Fatalf("wrote staging before rejection: %v %v", entries, err)
	}
}

func TestPortableCopyBudgetsFullFilesWithoutSparseSupport(t *testing.T) {
	dir, _ := backupFixture(t)
	zeroFile := filepath.Join(dir, "guest", "zero-payload.bin")
	if err := os.WriteFile(zeroFile, make([]byte, 3*moveBlockSize), 0600); err != nil {
		t.Fatal(err)
	}
	disk, err := openBackupDisk(filepath.Join(dir, "vm", "disk.raw"))
	if err != nil {
		t.Fatal(err)
	}
	defer disk.Close()
	include := func(string) bool { return true }
	_, sparse, err := inventoryMoveFiltered(dir, disk, nil, include)
	if err != nil {
		t.Fatal(err)
	}
	_, full, err := inventoryMoveFiltered(dir, disk, nil, include, true)
	if err != nil {
		t.Fatal(err)
	}
	if full-sparse < 3*moveBlockSize {
		t.Fatalf("non-sparse copy under-budgeted: sparse=%d full=%d", sparse, full)
	}
}
