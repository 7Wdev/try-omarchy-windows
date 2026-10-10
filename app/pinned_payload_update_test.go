package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestOwnPayloadConvergenceEligibility(t *testing.T) {
	dir := t.TempDir()
	cfg := &config{dir: dir, guestDir: filepath.Join(dir, "guest")}
	pins := pinnedPayloadUpdate{"https://example.test/guest", testSHA256([]byte("guest")), "https://example.test/runtime", testSHA256([]byte("runtime"))}
	writeReceipt := func(root, release, digest string, runtime bool) {
		t.Helper()
		os.MkdirAll(root, 0700)
		var data []byte
		name := installReceiptFilename
		if runtime {
			data, _ = json.Marshal(runtimeReceipt{Schema: 1, Release: release, ManifestSHA256: digest})
			name = runtimeReceiptFilename
		} else {
			data, _ = json.Marshal(installReceipt{Version: installReceiptVersion, Release: release, ManifestSHA256: digest})
		}
		if err := os.WriteFile(filepath.Join(root, name), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	writeReceipt(cfg.guestDir, pins.Release, pins.Digest, false)
	writeReceipt(filepath.Join(dir, "runtime"), pins.RuntimeRelease, testSHA256([]byte("old")), true)
	// Feed disablement must not disable completion of this installed launcher.
	for _, disabled := range []bool{false, true} {
		cfg.desktop.AutomaticUpdatesDisabled = disabled
		if ownPayloadUpdate(cfg, true, false, nil, pins) == nil {
			t.Fatal("runtime-only difference did not converge with disabled=", disabled)
		}
	}
	for _, name := range []string{"release", "sums-sha256", "runtime-release", "runtime-sums-sha256"} {
		if ownPayloadUpdate(cfg, true, false, map[string]bool{name: true}, pins) != nil {
			t.Fatal("overrode explicit", name)
		}
	}
	if ownPayloadUpdate(cfg, true, true, nil, pins) != nil || ownPayloadUpdate(cfg, false, false, nil, pins) != nil {
		t.Fatal("recovery or incomplete installation converged")
	}
	cfg.portable = true
	if ownPayloadUpdate(cfg, true, false, nil, pins) != nil {
		t.Fatal("offline portable installation converged")
	}
	cfg.portable = false
	writeReceipt(filepath.Join(dir, "runtime"), pins.RuntimeRelease, pins.RuntimeDigest, true)
	if ownPayloadUpdate(cfg, true, false, nil, pins) != nil {
		t.Fatal("matching receipts converged again")
	}
	writeReceipt(cfg.guestDir, pins.Release, testSHA256([]byte("old guest")), false)
	if ownPayloadUpdate(cfg, true, false, nil, pins) == nil {
		t.Fatal("guest-only difference did not converge")
	}
	recordFailedUpdate(dir, currentVersion)
	if ownPayloadUpdate(cfg, true, false, nil, pins) != nil {
		t.Fatal("failed payload retried")
	}
}

func TestPinnedPayloadStagesWithoutFeedAndAppliesOffline(t *testing.T) {
	configureSetupCancellation(false)
	for _, splitRuntime := range []bool{false, true} {
		t.Run(map[bool]string{false: "shared", true: "separate-runtime"}[splitRuntime], func(t *testing.T) {
			files := updatePayloadFixture()
			runtimeFiles := files
			if splitRuntime {
				runtimeFiles = map[string][]byte{runtimeZip: []byte("separate runtime")}
				setFixtureSums(runtimeFiles)
			}
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if strings.HasPrefix(filepath.Base(r.URL.Path), "rootfs.") {
					t.Errorf("update requested factory: %s %s", r.Method, r.URL.Path)
					http.Error(w, "factory forbidden", 500)
					return
				}
				if strings.Contains(r.URL.Path, "update.json") {
					t.Error("requested an update feed")
					http.Error(w, "offline", 503)
					return
				}
				source := files
				if strings.HasPrefix(r.URL.Path, "/runtime/") {
					source = runtimeFiles
				}
				data, ok := source[filepath.Base(r.URL.Path)]
				if !ok {
					http.NotFound(w, r)
					return
				}
				http.ServeContent(w, r, "payload", time.Time{}, bytes.NewReader(data))
			}))
			defer server.Close()
			pins := pinnedPayloadUpdate{server.URL + "/guest", testSHA256(files["SHA256SUMS"]), server.URL + "/runtime", testSHA256(runtimeFiles["SHA256SUMS"])}
			dir := t.TempDir()
			root := updatePayloadRoot(dir, "", false)
			if err := stagePinnedPayloadUpdate(context.Background(), server.Client(), dir, root, pins); err != nil {
				t.Fatal(err)
			}
			if state, _ := readPayloadUpdateState(dir); state != nil {
				t.Fatal("background staging activated a transaction")
			}
			server.Close()
			if err := verifiedPinnedPayloadUpdate(context.Background(), dir, root, pins); err != nil {
				t.Fatal("offline next-start verification:", err)
			}
			altered := pins
			altered.RuntimeDigest = testSHA256([]byte("untrusted"))
			if verifiedPinnedPayloadUpdate(context.Background(), dir, root, altered) == nil {
				t.Fatal("local metadata replaced an executable trust root")
			}
			if err := os.WriteFile(filepath.Join(root, pins.RuntimeDigest, runtimeZip), []byte("corrupt"), 0600); err != nil {
				t.Fatal(err)
			}
			if verifiedPinnedPayloadUpdate(context.Background(), dir, root, pins) == nil {
				t.Fatal("corrupt runtime was accepted")
			}
		})
	}
}

func TestPinnedPayloadCancellationNeverPublishesReady(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	dir := t.TempDir()
	digest := testSHA256([]byte("payload"))
	pins := pinnedPayloadUpdate{"http://127.0.0.1:1", digest, "http://127.0.0.1:1", digest}
	if stagePinnedPayloadUpdate(ctx, newDownloadClient(), dir, updatePayloadRoot(dir, "", false), pins) == nil {
		t.Fatal("cancelled stage succeeded")
	}
	if _, err := os.Stat(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename)); !os.IsNotExist(err) {
		t.Fatal("cancelled stage became ready")
	}
}

func TestPinnedPayloadAcceptsMatchingSignedStage(t *testing.T) {
	configureSetupCancellation(false)
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	files := updatePayloadFixture()
	files[stableLauncherName] = []byte("trusted launcher")
	signFixtureUpdate(t, files, currentVersion, private)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(filepath.Base(r.URL.Path), "rootfs.") {
			t.Errorf("update requested factory: %s %s", r.Method, r.URL.Path)
			http.Error(w, "factory forbidden", 500)
			return
		}
		http.ServeContent(w, r, "artifact", time.Time{}, bytes.NewReader(files[filepath.Base(r.URL.Path)]))
	}))
	defer server.Close()
	client := newDownloadClient()
	client.Transport = updateFixtureTransport{base: client.Transport, server: server.URL}
	defer client.CloseIdleConnections()
	dir := t.TempDir()
	root := updatePayloadRoot(dir, "", false)
	manifest, err := stageSignedUpdate(context.Background(), client, server.URL+"/update.json", dir, root, currentVersion, "", public)
	if err != nil || manifest == nil {
		t.Fatal("stage signed same-version payload:", err)
	}
	server.Close()
	pins := pinnedPayloadUpdate{manifest.Release, manifest.ManifestSHA256, manifest.Release, manifest.ManifestSHA256}
	if err := verifiedLauncherPayloadUpdate(context.Background(), dir, root, pins, public); err != nil {
		t.Fatal("matching signed payload was not usable offline:", err)
	}
	pins.RuntimeDigest = testSHA256([]byte("other runtime"))
	if verifiedLauncherPayloadUpdate(context.Background(), dir, root, pins, public) == nil {
		t.Fatal("signed feed overrode the runtime pin")
	}
}
