//go:build windows

package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestNativeStableRestartHelper(t *testing.T) {
	marker := os.Getenv("TRYOMARCHY_STABLE_MARKER")
	if marker == "" {
		return
	}
	self, err := os.Executable()
	if err != nil {
		os.Exit(2)
	}
	data, err := json.Marshal(struct {
		PID  int
		Path string
	}{os.Getpid(), self})
	if err != nil {
		os.Exit(3)
	}
	if err := os.WriteFile(marker, data, 0600); err != nil {
		os.Exit(4)
	}
	os.Exit(0)
}

func TestNativeStableRollbackHelper(t *testing.T) {
	dir := os.Getenv("TRYOMARCHY_STABLE_ROLLBACK_DIR")
	if dir == "" {
		return
	}
	args, err := encodeRestartArgs([]string{"-test.run=^TestNativeStableRestartHelper$"})
	if err != nil {
		os.Exit(2)
	}
	if err = applyLauncherUpdate(dir, 2147483647, args, true); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(3)
	}
	os.Exit(0)
}

// The signed feed uses a throwaway test key and native test executables. This
// verifies publication/rollback mechanics, not a public stable release or guest boot.
func TestNativeSignedStableUpdateAndRollback(t *testing.T) {
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	candidate, err := os.ReadFile(self)
	if err != nil {
		t.Fatal(err)
	}
	for _, version := range []string{"v1.0.0", "v1.0.1"} {
		t.Run(version, func(t *testing.T) {
			dir := t.TempDir()
			target := filepath.Join(dir, stableLauncherName)
			// A PE overlay gives the previous executable a distinct hash while keeping
			// it runnable, so rollback must restore the previous bytes, not the candidate.
			previous := append(append([]byte(nil), candidate...), []byte("previous launcher fixture")...)
			if err := os.WriteFile(target, previous, 0700); err != nil {
				t.Fatal(err)
			}
			sentinel := []byte(`{"schemaVersion":1,"cameraDisabled":true}`)
			if err := os.WriteFile(filepath.Join(dir, desktopPreferencesFilename), sentinel, 0600); err != nil {
				t.Fatal(err)
			}
			data := validUpdateJSON(version)
			var metadata updateManifest
			if err := json.Unmarshal(data, &metadata); err != nil {
				t.Fatal(err)
			}
			metadata.Launcher.SHA256 = testSHA256(candidate)
			data, err = json.Marshal(metadata)
			if err != nil {
				t.Fatal(err)
			}
			server, key := signedUpdateServer(t, data, false)
			defer server.Close()
			manifest, err := fetchUpdateManifest(server.Client(), server.URL+"/update.json", key)
			if err != nil {
				t.Fatal(err)
			}
			state := &launcherUpdateState{Schema: updateStateVersion, Version: manifest.Version, SHA256: manifest.Launcher.SHA256}
			if err := writeLauncherUpdateState(dir, state); err != nil {
				t.Fatal(err)
			}
			marker := filepath.Join(dir, "started.txt")
			t.Setenv("TRYOMARCHY_STABLE_MARKER", marker)
			args, err := encodeRestartArgs([]string{"-test.run=^TestNativeStableRestartHelper$"})
			if err != nil {
				t.Fatal(err)
			}
			if err := applyLauncherUpdate(dir, 2147483647, args, false); err != nil {
				t.Fatal(err)
			}
			waitStarted := func() {
				t.Helper()
				deadline := time.Now().Add(10 * time.Second)
				for {
					data, err := os.ReadFile(marker)
					if err == nil {
						var started struct {
							PID  int
							Path string
						}
						if err := json.Unmarshal(data, &started); err != nil {
							t.Fatal(err)
						}
						if !pathsEqual(started.Path, target) || started.PID <= 0 {
							t.Fatalf("wrong restart: %s", data)
						}
						// Production passes the running launcher's PID. Wait for
						// this helper too before exercising replacement/rollback.
						waitForProcess(started.PID)
						return
					}
					if time.Now().After(deadline) {
						t.Fatal("launcher did not restart")
					}
					time.Sleep(25 * time.Millisecond)
				}
			}
			waitStarted()
			if ok, err := verifyFileSHA256(target, testSHA256(candidate), nil); err != nil || !ok {
				t.Fatal("candidate was not installed", err)
			}
			old, err := os.ReadFile(previousLauncherPath(dir))
			if err != nil || !bytes.Equal(old, previous) {
				t.Fatal("previous executable was not retained", err)
			}
			state, err = readLauncherUpdateState(dir)
			if err != nil || state == nil || !state.HasPrevious || state.Version != version {
				t.Fatal("rollback marker missing", err)
			}
			state.Started = true
			if err := writeLauncherUpdateState(dir, state); err != nil {
				t.Fatal(err)
			}
			if err := os.Remove(marker); err != nil {
				t.Fatal(err)
			}
			t.Setenv("TRYOMARCHY_STABLE_ROLLBACK_DIR", dir)
			cmd := exec.Command(previousLauncherPath(dir), "-test.run=^TestNativeStableRollbackHelper$")
			if output, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("rollback: %v: %s", err, output)
			}
			waitStarted()
			actual, err := os.ReadFile(target)
			if err != nil || !bytes.Equal(actual, previous) {
				t.Fatal("rollback did not restore original bytes", err)
			}
			state, err = readLauncherUpdateState(dir)
			if err != nil || state != nil {
				t.Fatal("rollback marker not cleared", err)
			}
			actual, err = os.ReadFile(filepath.Join(dir, desktopPreferencesFilename))
			if err != nil || !bytes.Equal(actual, sentinel) {
				t.Fatal("update or rollback changed preferences", err)
			}
		})
	}
}

func TestNativeBootFirstChannelAppliesOnlyOnNextStart(t *testing.T) {
	configureSetupCancellation(false)
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	launcher, err := os.ReadFile(self)
	if err != nil {
		t.Fatal(err)
	}
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	target := filepath.Join(dir, stableLauncherName)
	previous := append(append([]byte(nil), launcher...), []byte("baseline")...)
	if err := os.WriteFile(target, previous, 0700); err != nil {
		t.Fatal(err)
	}
	files := updatePayloadFixture()
	files[stableLauncherName] = launcher
	signFixtureUpdate(t, files, "v1.0.0", private)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		data, ok := files[strings.TrimPrefix(r.URL.Path, "/")]
		if !ok {
			http.NotFound(w, r)
			return
		}
		http.ServeContent(w, r, "artifact", time.Time{}, bytes.NewReader(data))
	}))
	defer server.Close()
	client := newDownloadClient()
	client.Transport = updateFixtureTransport{base: client.Transport, server: server.URL}
	defer client.CloseIdleConnections()
	marker := filepath.Join(dir, "started.txt")
	t.Setenv("TRYOMARCHY_STABLE_MARKER", marker)
	args, err := encodeRestartArgs([]string{"-test.run=^TestNativeStableRestartHelper$"})
	if err != nil {
		t.Fatal(err)
	}
	for _, version := range []string{"v1.0.0", "v1.0.1"} {
		installed := "v0.9.0"
		if version == "v1.0.1" {
			installed = "v1.0.0"
			files[stableLauncherName] = append(append([]byte(nil), launcher...), []byte("following")...)
			signFixtureUpdate(t, files, version, private)
		}
		payloadRoot := updatePayloadRoot(dir, "", false)
		manifest, err := stageSignedUpdate(context.Background(), client, server.URL+"/update.json", dir, payloadRoot, installed, "", public)
		if err != nil || manifest == nil {
			t.Fatalf("stage: %v", err)
		}
		actual, err := os.ReadFile(target)
		if err != nil || !bytes.Equal(actual, previous) {
			t.Fatal("running launcher replaced during background download")
		}
		if _, err := os.Stat(marker); !os.IsNotExist(err) {
			t.Fatal("staging started another launcher")
		}
		if _, err := verifiedStagedUpdate(context.Background(), dir, payloadRoot, version, public); err != nil {
			t.Fatal(err)
		}
		// Production next-start passes this manifest to the stopped-parent helper.
		// Test executables have the same helper code but no guest or public version pin.
		state := &launcherUpdateState{Schema: updateStateVersion, Version: version, SHA256: manifest.Launcher.SHA256}
		if err := writeLauncherUpdateState(dir, state); err != nil {
			t.Fatal(err)
		}
		t.Setenv("TRYOMARCHY_BOOT_FIRST_DIR", dir)
		t.Setenv("TRYOMARCHY_BOOT_FIRST_ARGS", args)
		cmd := exec.Command(stagedLauncherPath(dir, version), "-test.run=^TestNativeBootFirstApplyHelper$")
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("apply staged launcher: %v: %s", err, output)
		}
		deadline := time.Now().Add(10 * time.Second)
		for {
			data, err := os.ReadFile(marker)
			if err == nil {
				var started struct {
					PID  int
					Path string
				}
				if err := json.Unmarshal(data, &started); err != nil {
					t.Fatal(err)
				}
				if !pathsEqual(started.Path, target) {
					t.Fatal("wrong executable restarted")
				}
				waitForProcess(started.PID)
				break
			}
			if time.Now().After(deadline) {
				t.Fatal("updated launcher did not start")
			}
			time.Sleep(25 * time.Millisecond)
		}
		if ok, err := verifyFileSHA256(target, manifest.Launcher.SHA256, nil); err != nil || !ok {
			t.Fatal("replacement mismatch", err)
		}
		if actual, err := os.ReadFile(previousLauncherPath(dir)); err != nil || !bytes.Equal(actual, previous) {
			t.Fatal("rollback executable changed", err)
		}
		// Only readiness removes the transaction marker.
		if state, err := readLauncherUpdateState(dir); err != nil || state == nil || !state.HasPrevious {
			t.Fatal("first-ready transaction missing", err)
		}
		if err := clearLauncherUpdateMarker(dir); err != nil {
			t.Fatal(err)
		}
		if err := os.Remove(marker); err != nil {
			t.Fatal(err)
		}
		previous = files[stableLauncherName]
		t.Logf("PASS %s signed stage, unchanged running launcher, next-start replacement and retained rollback", version)
	}
}

func TestNativeInstalledPayloadStartsOfflineWithoutRequests(t *testing.T) {
	configureSetupCancellation(false)
	dir := t.TempDir()
	guest := filepath.Join(dir, "guest")
	if err := os.MkdirAll(guest, 0700); err != nil {
		t.Fatal(err)
	}
	sums := map[string]string{}
	for _, name := range installedGuestArtifacts {
		data := []byte("installed " + name)
		sums[name] = testSHA256(data)
		if err := os.WriteFile(filepath.Join(guest, name), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t.Error("offline boot made a request")
		http.Error(w, "offline", 503)
	}))
	release := server.URL + "/v0.9.0"
	digest := testSHA256([]byte("trusted manifest"))
	if err := writeInstallReceipt(guest, release, digest, installedGuestArtifacts, sums); err != nil {
		t.Fatal(err)
	}
	runtimeRoot := filepath.Join(dir, "runtime")
	if err := os.MkdirAll(filepath.Join(runtimeRoot, "bin"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(runtimeRoot, "bin", "qemu-system-x86_64w.exe"), []byte("installed qemu"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := writeRuntimeReceipt(runtimeRoot, release, digest, testSHA256([]byte("archive"))); err != nil {
		t.Fatal(err)
	}
	server.Close()
	vm := filepath.Join(dir, "vm")
	os.MkdirAll(vm, 0700)
	disk := filepath.Join(vm, "disk.raw")
	os.WriteFile(disk, []byte("existing user disk"), 0600)
	cfg := &config{dir: dir, guestDir: guest, vmDir: vm, disk: disk}
	started := time.Now()
	if err := ensureGuest(cfg, release, digest); err != nil {
		t.Fatal(err)
	}
	if _, err := ensureRuntime(cfg, release, digest); err != nil {
		t.Fatal(err)
	}
	if elapsed := time.Since(started); elapsed > time.Second {
		t.Fatalf("offline installed boot preparation took %s", elapsed)
	}
	t.Logf("offline installed boot preparation: %s; zero update requests", time.Since(started))
}

func TestNativeBootFirstApplyHelper(t *testing.T) {
	dir := os.Getenv("TRYOMARCHY_BOOT_FIRST_DIR")
	if dir == "" {
		return
	}
	if err := applyLauncherUpdate(dir, 2147483647, os.Getenv("TRYOMARCHY_BOOT_FIRST_ARGS"), false); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	os.Exit(0)
}

func TestNativeLauncherIntentionalStopsRetryUntilReady(t *testing.T) {
	dir := t.TempDir()
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	previous, err := os.ReadFile(self)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(launcherUpdateDir(dir), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(previousLauncherPath(dir), previous, 0700); err != nil {
		t.Fatal(err)
	}
	state := &launcherUpdateState{Schema: updateStateVersion, Version: currentVersion, SHA256: testSHA256(previous), Started: true, HasPrevious: true}
	if err := writeLauncherUpdateState(dir, state); err != nil {
		t.Fatal(err)
	}
	args, _ := encodeRestartArgs(nil)
	for n := 0; n < 5; n++ {
		if err := interruptPendingUpdates(dir); err != nil {
			t.Fatal(err)
		}
		if rolled, err := recoverLauncherUpdate(dir, args); err != nil || rolled {
			t.Fatalf("intentional stop %d rolled back: %v", n, err)
		}
	}
	if state, err := readLauncherUpdateState(dir); err != nil || state == nil || !state.Started || state.Interrupted {
		t.Fatal("retry transaction lost", err)
	}
	commitLauncherUpdate(dir)
	if state, err := readLauncherUpdateState(dir); err != nil || state != nil {
		t.Fatal("ready did not commit", err)
	}
	if _, err := os.Stat(previousLauncherPath(dir)); err != nil {
		t.Fatal("ready discarded rollback launcher", err)
	}
}
