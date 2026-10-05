//go:build windows

package main

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"

	"github.com/klauspost/compress/zstd"
)

// Native state coverage uses small payloads and the legacy marker schema. The
// release acceptance harness separately executes the published launchers.
func TestNativePinnedPayloadConvergesAfterLegacyHop(t *testing.T) {
	configureSetupCancellation(false)
	for _, baseline := range []string{"v0.8.0", "v0.9.0"} {
		t.Run(baseline, func(t *testing.T) {
			dir := t.TempDir()
			cfg := &config{dir: dir, guestDir: filepath.Join(dir, "guest")}
			cfg.desktop.AutomaticUpdatesDisabled = true
			files := updatePayloadFixture()
			encoder, err := zstd.NewWriter(nil)
			if err != nil {
				t.Fatal(err)
			}
			files["rootfs.ext4.zst"] = encoder.EncodeAll(files["rootfs.ext4"], nil)
			encoder.Close()
			var archive bytes.Buffer
			zw := zip.NewWriter(&archive)
			entry, err := zw.Create("bin/qemu-system-x86_64w.exe")
			if err != nil {
				t.Fatal(err)
			}
			entry.Write([]byte("new runtime executable"))
			zw.Close()
			files[runtimeZip] = archive.Bytes()
			files["guest-manifest.json"], _ = json.Marshal(map[string]any{"schemaVersion": 1, "artifacts": []map[string]any{
				{"path": "rootfs.ext4", "bytes": len(files["rootfs.ext4"]), "sha256": testSHA256(files["rootfs.ext4"])},
				{"path": "rootfs.ext4.zst", "bytes": len(files["rootfs.ext4.zst"]), "sha256": testSHA256(files["rootfs.ext4.zst"])},
			}})
			setFixtureSums(files)
			var requests atomic.Int32
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				requests.Add(1)
				data, ok := files[filepath.Base(r.URL.Path)]
				if !ok {
					t.Error("unexpected request", r.URL.Path)
					http.Error(w, "no feed", 503)
					return
				}
				http.ServeContent(w, r, "payload", time.Time{}, bytes.NewReader(data))
			}))
			defer server.Close()
			release := server.URL + "/" + baseline
			digest := testSHA256([]byte(baseline))
			sums := map[string]string{}
			if err := os.MkdirAll(cfg.guestDir, 0700); err != nil {
				t.Fatal(err)
			}
			for _, name := range installedGuestArtifacts {
				data := []byte("old " + name)
				sums[name] = testSHA256(data)
				if err := os.WriteFile(filepath.Join(cfg.guestDir, name), data, 0600); err != nil {
					t.Fatal(err)
				}
			}
			if err := writeInstallReceipt(cfg.guestDir, release, digest, installedGuestArtifacts, sums); err != nil {
				t.Fatal(err)
			}
			runtimeRoot := filepath.Join(dir, "runtime")
			os.MkdirAll(filepath.Join(runtimeRoot, "bin"), 0700)
			os.WriteFile(filepath.Join(runtimeRoot, "bin", "qemu-system-x86_64w.exe"), []byte("old runtime"), 0700)
			if err := writeRuntimeReceipt(runtimeRoot, release, digest, testSHA256([]byte("old archive"))); err != nil {
				t.Fatal(err)
			}
			self, _ := os.Executable()
			launcher, err := os.ReadFile(self)
			if err != nil {
				t.Fatal(err)
			}
			os.MkdirAll(launcherUpdateDir(dir), 0700)
			os.WriteFile(previousLauncherPath(dir), launcher, 0700)
			// Released v0.9 writes no manifestSHA256; this still needs to commit.
			state := &launcherUpdateState{Schema: 1, Version: currentVersion, SHA256: testSHA256(launcher), HasPrevious: true}
			if err := writeLauncherUpdateState(dir, state); err != nil {
				t.Fatal(err)
			}
			capture := func(step string) {
				t.Helper()
				if root := os.Getenv("TRYOMARCHY_HOP_EVIDENCE"); root != "" {
					out := filepath.Join(root, baseline, step)
					for _, name := range []string{updateStateFilename, payloadUpdateStateFilename, "guest/" + installReceiptFilename,
						"runtime/" + runtimeReceiptFilename, "updates/" + stagedUpdateFilename, "updates/" + currentVersion + "/" + pinnedPayloadFilename} {
						data, err := os.ReadFile(filepath.Join(dir, name))
						if os.IsNotExist(err) {
							data = []byte("ABSENT\n")
						} else if err != nil {
							t.Fatal(err)
						}
						path := filepath.Join(out, name)
						os.MkdirAll(filepath.Dir(path), 0700)
						if err := os.WriteFile(path, data, 0600); err != nil {
							t.Fatal(err)
						}
					}
				}
			}
			capture("01-legacy-pending")
			args, _ := encodeRestartArgs(nil)
			if rolled, err := recoverLauncherUpdate(dir, args); err != nil || rolled {
				t.Fatal("first candidate start rolled back", err)
			}
			if err := ensureGuest(cfg, release, digest); err != nil {
				t.Fatal(err)
			}
			if _, err := ensureRuntime(cfg, release, digest); err != nil {
				t.Fatal(err)
			}
			pins := pinnedPayloadUpdate{server.URL + "/" + currentVersion, testSHA256(files["SHA256SUMS"]), server.URL + "/" + currentVersion, testSHA256(files["SHA256SUMS"])}
			own := ownPayloadUpdate(cfg, true, false, nil, pins)
			if own == nil {
				t.Fatal("disabled updates prevented convergence")
			}
			cancel := configureBackgroundUpdates(cfg, server.URL+"/unavailable-feed", false, own)
			defer cancel()
			if requests.Load() != 0 {
				t.Fatal("network before ready")
			}
			capture("02-installed-boot")
			commitLauncherUpdate(dir)
			capture("03-launcher-ready")
			startReadyUpdateCheck()
			root := updatePayloadRoot(dir, "", false)
			deadline := time.Now().Add(10 * time.Second)
			for verifiedPinnedPayloadUpdate(context.Background(), dir, root, pins) != nil {
				if time.Now().After(deadline) {
					t.Fatal("own payload not staged after ready")
				}
				time.Sleep(10 * time.Millisecond)
			}
			cancel()
			capture("04-background-staged")
			if _, installed, _ := installReceiptIdentity(cfg.guestDir); installed != digest || !updateAvailable.Load() {
				t.Fatal("background changed installed payload or failed to offer restart")
			}
			server.Close()
			cfg.payloadDir, cfg.localPayload = root, true
			cfg.localPayloadSHA256, cfg.localRuntimePayloadSHA256 = pins.Digest, pins.RuntimeDigest
			if _, err := ensureRuntime(cfg, pins.RuntimeRelease, pins.RuntimeDigest); err != nil {
				t.Fatal(err)
			}
			if err := ensureGuest(cfg, pins.Release, pins.Digest); err != nil {
				t.Fatal(err)
			}
			capture("05-next-start-applied")
			pending, err := readPayloadUpdateState(dir)
			if err != nil || pending == nil || !pending.GuestPending || !pending.RuntimePending {
				t.Fatal("next start lost rollback transaction", err)
			}
			commitPayloadUpdates(dir)
			pruneCommittedUpdatePayloads(cfg)
			capture("06-payload-ready")
			if pending, err := readPayloadUpdateState(dir); err != nil || pending != nil {
				t.Fatal("ready did not commit payload", err)
			}
			if ownPayloadUpdate(cfg, true, false, nil, pins) != nil {
				t.Fatal("matching launcher staged again")
			}
			t.Log("PASS inherited launcher marker, offline installed boot, ready commit, disabled-feed staging, offline next-start apply, payload-ready commit")
		})
	}
}
