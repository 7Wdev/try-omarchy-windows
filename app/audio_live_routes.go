package main

import (
	"encoding/base64"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"time"
)

const audioLiveRoutingPatch = "patches/qemu/0016-live-sdl-audio-routes.patch"

func audioRuntimeSupportsLiveRouting(qemu string) bool {
	return runtimeHasPatch(qemu, audioLiveRoutingPatch)
}

func audioRouteDirectory(dataDir string) string {
	return filepath.Join(dataDir, "vm", "audio-control")
}

// QEMU reads each direction independently. Rename keeps it from ever seeing
// half of a selected name, including when Settings saves during playback.
func writeAudioRoute(dir, direction, name string) error {
	if direction != "output" && direction != "input" {
		return fmt.Errorf("invalid audio route direction")
	}
	if err := (audioPreferences{SchemaVersion: 1, Output: name}).validate(); err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	value := "default"
	if name != "" {
		value = base64.StdEncoding.EncodeToString([]byte(name))
	}
	f, err := os.CreateTemp(dir, ".audio-route-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.WriteString(value + "\n"); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	return renameAudioRoute(f.Name(), filepath.Join(dir, direction), os.Rename, time.Sleep)
}

func publishAudioRoutes(dir string, p audioPreferences, microphoneDisabled bool) error {
	if err := writeAudioRoute(dir, "output", p.Output); err != nil {
		return err
	}
	input := p.Input
	if microphoneDisabled {
		input = ""
	}
	return writeAudioRoute(dir, "input", input)
}

// A separate Settings process can update a running VM. Do not create a route
// directory for an installation that has never started this runtime.
func publishSavedAudioRoutes(dataDir string, p audioPreferences, microphoneDisabled bool) error {
	dir := audioRouteDirectory(dataDir)
	info, err := os.Stat(dir)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("audio control path is not a directory")
	}
	return publishAudioRoutes(dir, p, microphoneDisabled)
}

func renameAudioRoute(from, to string, rename func(string, string) error, sleep func(time.Duration)) error {
	for attempt := 0; ; attempt++ {
		err := rename(from, to)
		// MoveFileEx can report ACCESS_DENIED for an open destination too.
		// Persistent permissions still fail after the same bounded interval.
		if err == nil || runtime.GOOS != "windows" || !retryableWindowsRenameError(err) || attempt == 9 {
			return err
		}
		sleep(20 * time.Millisecond)
	}
}
