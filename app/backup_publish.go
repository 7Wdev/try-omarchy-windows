package main

import (
	"io"
	"os"
	"path/filepath"
)

func publishBackup(from, to string, link func(string, string) error) error {
	// Keep the no-copy path where hard links are supported. Only unsupported
	// operations fall back; permission, sharing and existing-target errors do not.
	if err := link(from, to); err == nil || !backupHardLinksUnsupported(err) {
		return err
	}
	return copyAndPublishBackup(from, to)
}

func copyAndPublishBackup(from, to string) error {
	source, err := os.Open(from)
	if err != nil {
		return err
	}
	defer source.Close()
	stage, err := os.CreateTemp(filepath.Dir(to), ".try-omarchy-backup-publish-*")
	if err != nil {
		return err
	}
	defer os.Remove(stage.Name())
	defer stage.Close()
	if _, err = io.Copy(stage, setupReader{source}); err != nil {
		return err
	}
	if err = stage.Sync(); err != nil {
		return err
	}
	if err = stage.Close(); err != nil {
		return err
	}
	if err = checkSetupCancelled(); err != nil {
		return err
	}
	// This helper also accepts files: same-volume atomic rename, no replacement,
	// and write-through on Windows. The destination never exposes a partial copy.
	return publishNewDirectory(stage.Name(), to)
}
