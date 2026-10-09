package main

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

func TestPinnedPayloadFolderTrustAndPublication(t *testing.T) {
	configureSetupCancellation(false)
	for _, fault := range []string{"", "missing", "modified", "wrong-digest", "modified-sums", "cancelled"} {
		t.Run(fault, func(t *testing.T) {
			files := updatePayloadFixture()
			digest := testSHA256(files["SHA256SUMS"])
			folder, dir := t.TempDir(), t.TempDir()
			for _, name := range append([]string{"SHA256SUMS"}, updatePayloadNames()...) {
				if fault == "missing" && name == runtimeZip {
					continue
				}
				data := files[name]
				if fault == "modified" && name == runtimeZip || fault == "modified-sums" && name == "SHA256SUMS" {
					data = []byte("modified")
				}
				if err := os.WriteFile(filepath.Join(folder, name), data, 0600); err != nil {
					t.Fatal(err)
				}
			}
			if fault == "wrong-digest" {
				digest = testSHA256([]byte("different release"))
			}
			pins := pinnedPayloadUpdate{"https://example.test/release", digest, "https://example.test/release", digest}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			if fault == "cancelled" {
				cancel()
			}
			root := updatePayloadRoot(dir, "", false)
			err := stagePinnedPayloadFolder(ctx, folder, dir, root, pins)
			if fault == "" {
				if err != nil {
					t.Fatal(err)
				}
				if err := verifiedPinnedPayloadUpdate(ctx, dir, root, pins); err != nil {
					t.Fatal("offline stage verification:", err)
				}
				if state, _ := readPayloadUpdateState(dir); state != nil {
					t.Fatal("activated before restart")
				}
			} else {
				if err == nil {
					t.Fatal("untrusted folder accepted")
				}
				if _, err := os.Stat(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename)); !os.IsNotExist(err) {
					t.Fatal("failed folder became ready")
				}
				if _, err := os.Stat(root); !os.IsNotExist(err) {
					t.Fatal("failed folder left staged files")
				}
			}
		})
	}
}
