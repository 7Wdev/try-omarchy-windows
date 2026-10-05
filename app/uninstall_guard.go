package main

import (
	"fmt"
	"os"
	"path/filepath"
)

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
	for _, marker := range uninstallMarkers {
		if info, err := os.Lstat(filepath.Join(dir, marker)); err == nil && info.Mode().IsRegular() {
			return nil
		}
	}
	return fmt.Errorf("%s is not a Try Omarchy data folder; nothing was removed", dir)
}
