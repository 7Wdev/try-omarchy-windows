//go:build windows

package main

import (
	"path/filepath"
	"testing"
)

func TestWindowsMoveVolumeResolvesByIdentity(t *testing.T) {
	target := filepath.Join(t.TempDir(), "TryOmarchy")
	location, err := windowsMoveVolume(target)
	if err != nil {
		t.Fatal(err)
	}
	if !validMoveVolumeLocation(*location) {
		t.Fatalf("invalid identity: %+v", location)
	}
	resolved, err := windowsLocateMoveVolume(*location)
	if err != nil || !sameWindowsPath(resolved, target) {
		t.Fatalf("resolved=%q err=%v target=%q", resolved, err, target)
	}
	for _, wrong := range []moveVolumeLocation{
		{Volume: `\\?\Volume{00000000-0000-0000-0000-000000000000}\`, Relative: location.Relative},
		{Volume: location.Volume, Relative: `..\outside`},
		{Volume: `C:\`, Relative: location.Relative},
	} {
		if _, err := windowsLocateMoveVolume(wrong); err == nil {
			t.Fatalf("accepted wrong volume %+v", wrong)
		}
	}
}

func TestWindowsFilesystemCapabilityReadsNativeVolume(t *testing.T) {
	capability, err := windowsFilesystemCapability(t.TempDir())
	if err != nil || capability.Name == "" || capability.MaxFileBytes <= 0 {
		t.Fatalf("capability=%+v err=%v", capability, err)
	}
}
