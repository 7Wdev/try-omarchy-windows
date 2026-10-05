package main

import (
	"os"
	"path/filepath"
	"strings"
)

// Called only after acquiring the launcher's single-instance clipboard ports.
// Published copy/received paths may still be in the clipboard or helper windows.
// Only private archives and unfinished stages with no live ticket go.
func cleanClipboardTransferOrphans(cache string) error {
	entries, err := os.ReadDir(cache)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".transfer-in-") && entry.IsDir() {
			if err := os.RemoveAll(filepath.Join(cache, entry.Name())); err != nil {
				return err
			}
		}
		if strings.HasPrefix(entry.Name(), ".transfer-out-") && entry.Type().IsRegular() {
			if err := os.Remove(filepath.Join(cache, entry.Name())); err != nil {
				return err
			}
		}
	}
	return nil
}
