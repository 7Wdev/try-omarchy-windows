package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func updatePayloadFixture() map[string][]byte {
	files := map[string][]byte{}
	for _, name := range append(updatePayloadNames(), "rootfs.ext4", "rootfs.ext4.zst") {
		files[name] = []byte("verified " + name)
	}
	files["build-spec.json"] = []byte(`{"image":{"architecture":"x86_64"},"runtime":{"storage":{"expandedSizeMiB":1}}}`)
	files["guest-manifest.json"] = []byte(fmt.Sprintf(`{"schemaVersion":1,"artifacts":[{"path":"rootfs.ext4","bytes":%d,"sha256":"%s"},{"path":"rootfs.ext4.zst","bytes":%d,"sha256":"%s"}]}`, len(files["rootfs.ext4"]), testSHA256(files["rootfs.ext4"]), len(files["rootfs.ext4.zst"]), testSHA256(files["rootfs.ext4.zst"])))
	setFixtureSums(files)
	return files
}
func setFixtureSums(files map[string][]byte) {
	var sums strings.Builder
	for name, data := range files {
		if name != "SHA256SUMS" && name != "update.json" && name != "update.json.sig" {
			fmt.Fprintf(&sums, "%s  %s\n", testSHA256(data), name)
		}
	}
	files["SHA256SUMS"] = []byte(sums.String())
}
func signFixtureUpdate(t *testing.T, files map[string][]byte, version string, key ed25519.PrivateKey) {
	t.Helper()
	manifest := updateManifest{Schema: 1, Version: version, Release: transferredReleaseBase + version, ManifestSHA256: testSHA256(files["SHA256SUMS"])}
	manifest.Launcher.Name, manifest.Launcher.SHA256 = stableLauncherName, testSHA256(files[stableLauncherName])
	data, err := json.Marshal(manifest)
	if err != nil {
		t.Fatal(err)
	}
	files["update.json"] = data
	files["update.json.sig"] = []byte(base64.StdEncoding.EncodeToString(ed25519.Sign(key, data)))
}

type updateFixtureTransport struct {
	base   http.RoundTripper
	server string
}

func (t updateFixtureTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	req = req.Clone(req.Context())
	u := *req.URL
	req.URL = &u
	req.URL.Scheme = "http"
	req.URL.Host = strings.TrimPrefix(t.server, "http://")
	req.URL.Path = "/" + filepath.Base(req.URL.Path)
	return t.base.RoundTrip(req)
}

// Exercise the signed channel through the production staging engine, including
// a following release. Native replacement is tested separately on Windows.
func TestBootFirstSignedChannelUpgradeMatrix(t *testing.T) {
	configureSetupCancellation(false)
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	for _, baseline := range []string{"v0.3.0", "v0.4.0", "v0.6.2", "v0.7.1", "v0.8.0", "v0.9.0"} {
		t.Run(baseline, func(t *testing.T) {
			dir := t.TempDir()
			payloadRoot := updatePayloadRoot(dir, "", false)
			files := updatePayloadFixture()
			files[stableLauncherName] = []byte("candidate launcher")
			signFixtureUpdate(t, files, "v0.10.0", private)
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if strings.HasPrefix(filepath.Base(r.URL.Path), "rootfs.") {
					t.Errorf("update requested factory: %s %s", r.Method, r.URL.Path)
					http.Error(w, "factory forbidden", 500)
					return
				}
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
			sentinel := []byte("personal disk untouched")
			disk := filepath.Join(dir, "disk.raw")
			if err := os.WriteFile(disk, sentinel, 0600); err != nil {
				t.Fatal(err)
			}
			for _, version := range []string{"v0.10.0", "v0.11.0"} {
				installed := baseline
				if version == "v0.11.0" {
					installed = "v0.10.0"
					files[stableLauncherName] = []byte("following launcher")
					signFixtureUpdate(t, files, version, private)
				}
				manifest, err := stageSignedUpdate(context.Background(), client, server.URL+"/update.json", dir, payloadRoot, installed, "", public)
				if err != nil || manifest == nil {
					t.Fatalf("stage %s: %v", version, err)
				}
				if state, _ := readLauncherUpdateState(dir); state != nil {
					t.Fatal("background stage activated launcher transaction")
				}
				if state, _ := readPayloadUpdateState(dir); state != nil {
					t.Fatal("background stage activated payload transaction")
				}
				if _, err := verifiedStagedUpdate(context.Background(), dir, payloadRoot, version, public); err != nil {
					t.Fatal(err)
				}
				data, _ := os.ReadFile(disk)
				if !bytes.Equal(data, sentinel) {
					t.Fatal("staging changed personal disk")
				}
				t.Logf("PASS %s -> %s: signature, cache, next-start authentication, disk preservation", installed, version)
			}
			launcher := stagedLauncherPath(dir, "v0.11.0")
			if err := os.WriteFile(launcher, []byte("corrupt"), 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := verifiedStagedUpdate(context.Background(), dir, payloadRoot, "v0.11.0", public); err == nil {
				t.Fatal("accepted corrupt launcher")
			}
			discardStagedUpdate(dir, payloadRoot, "v0.11.0", public)
			if _, err := os.Stat(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename)); !os.IsNotExist(err) {
				t.Fatal("corrupt update stayed ready")
			}
		})
	}
}

func TestUpdatePayloadDownloadResumesAcrossCancellation(t *testing.T) {
	configureSetupCancellation(false)
	files := updatePayloadFixture()
	files["vmlinuz-linux"] = bytes.Repeat([]byte("large transfer"), 200000)
	files["guest-manifest.json"] = []byte(fmt.Sprintf(`{"schemaVersion":1,"artifacts":[{"path":"rootfs.ext4","bytes":%d,"sha256":"%s"},{"path":"rootfs.ext4.zst","bytes":%d,"sha256":"%s"}]}`, len(files["rootfs.ext4"]), testSHA256(files["rootfs.ext4"]), len(files["rootfs.ext4.zst"]), testSHA256(files["rootfs.ext4.zst"])))
	setFixtureSums(files)
	var resumed atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(filepath.Base(r.URL.Path), "rootfs.") {
			t.Errorf("update requested factory: %s %s", r.Method, r.URL.Path)
			http.Error(w, "factory forbidden", 500)
			return
		}
		if filepath.Base(r.URL.Path) == "vmlinuz-linux" && r.Header.Get("Range") != "" {
			resumed.Store(true)
		}
		data := files[strings.TrimPrefix(r.URL.Path, "/")]
		http.ServeContent(w, r, "artifact", time.Time{}, bytes.NewReader(data))
	}))
	defer server.Close()
	ctx, cancel := context.WithCancel(context.Background())
	root := t.TempDir()
	digest := testSHA256(files["SHA256SUMS"])
	err := stageUpdatePayload(ctx, root, server.URL, digest, server.Client(), func(phase string, done, total int64) {
		if phase == downloadPhaseTransfer && total == int64(len(files["vmlinuz-linux"])) && done > 0 {
			cancel()
		}
	})
	if err == nil {
		t.Fatal("cancelled transfer was published")
	}
	part := filepath.Join(root, ".payload-staging-"+digest, "vmlinuz-linux.part")
	info, err := os.Stat(part)
	if err != nil || info.Size() == 0 {
		t.Fatalf("partial transfer lost: %v", err)
	}
	if err := stageUpdatePayload(context.Background(), root, server.URL, digest, server.Client(), nil); err != nil {
		t.Fatal(err)
	}
	if !resumed.Load() {
		t.Fatal("did not request HTTP Range after relaunch")
	}
	payload := filepath.Join(root, digest)
	if err := verifyUpdatePayload(context.Background(), payload, digest); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(payload, "vmlinuz-linux"), []byte("bad"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := stageUpdatePayload(context.Background(), root, server.URL, digest, server.Client(), nil); err == nil {
		t.Fatal("accepted corrupted staged payload")
	}
	if _, err := os.Stat(payload); !os.IsNotExist(err) {
		t.Fatal("corrupted staged payload not discarded")
	}
}

func TestUpdateSpacePreflightAndPruning(t *testing.T) {
	configureSetupCancellation(false)
	oldFree := diskFreeBytes
	diskFreeBytes = func(string) (int64, error) { return 0, nil }
	t.Cleanup(func() { diskFreeBytes = oldFree })
	files := updatePayloadFixture()
	var large atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(filepath.Base(r.URL.Path), "rootfs.") {
			t.Errorf("update requested factory: %s %s", r.Method, r.URL.Path)
			http.Error(w, "factory forbidden", 500)
			return
		}
		if r.Method == http.MethodGet && filepath.Base(r.URL.Path) == "vmlinuz-linux" {
			large.Store(true)
		}
		http.ServeContent(w, r, "artifact", time.Time{}, bytes.NewReader(files[strings.TrimPrefix(r.URL.Path, "/")]))
	}))
	defer server.Close()
	if err := stageUpdatePayload(context.Background(), t.TempDir(), server.URL, testSHA256(files["SHA256SUMS"]), server.Client(), nil); err == nil {
		t.Fatal("low space accepted")
	}
	if large.Load() {
		t.Fatal("large download started before preflight")
	}
	root := t.TempDir()
	active, previous, stale := testSHA256([]byte("active")), testSHA256([]byte("previous")), testSHA256([]byte("stale"))
	for _, name := range []string{active, previous, stale, "personal", ".payload-staging-" + active} {
		if err := os.Mkdir(filepath.Join(root, name), 0700); err != nil {
			t.Fatal(err)
		}
	}
	if err := pruneUpdatePayloads(root, active, previous); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{active, previous, "personal", ".payload-staging-" + active} {
		if _, err := os.Stat(filepath.Join(root, name)); err != nil {
			t.Fatalf("removed retained %s", name)
		}
	}
	if _, err := os.Stat(filepath.Join(root, stale)); !os.IsNotExist(err) {
		t.Fatal("superseded payload retained")
	}
}

func TestQuitBeforeReadyRetriesThenRealFailureRollsBack(t *testing.T) {
	dir := t.TempDir()
	writeVersionFile(t, filepath.Join(dir, "guest"), "new")
	writeVersionFile(t, filepath.Join(dir, "guest.previous"), "old")
	if err := recordPayloadUpdate(dir, "v0.10.0", true, false); err != nil {
		t.Fatal(err)
	}
	for n := 0; n < 5; n++ {
		if err := interruptPendingUpdates(dir); err != nil {
			t.Fatal(err)
		}
		if rolled, err := rollbackPendingPayloadUpdates(dir); err != nil || rolled {
			t.Fatalf("intentional quit %d rolled back: %v", n, err)
		}
		if got := readVersionFile(t, filepath.Join(dir, "guest")); got != "new" {
			t.Fatal("retry changed version")
		}
	}
	// No intentional stop marker: a crash must still roll back once.
	if rolled, err := rollbackPendingPayloadUpdates(dir); err != nil || !rolled {
		t.Fatalf("crash did not roll back: %v", err)
	}
	if got := readVersionFile(t, filepath.Join(dir, "guest")); got != "old" {
		t.Fatal("previous guest not restored")
	}
	if rolled, err := rollbackPendingPayloadUpdates(dir); err != nil || rolled {
		t.Fatal("recovery looped")
	}
	if failedUpdateVersion(dir) != "v0.10.0" {
		t.Fatal("failed version was not suppressed")
	}
}

func TestSystemProxySelection(t *testing.T) {
	for _, tc := range []struct{ target, servers, bypass, want string }{
		{"https://github.com/release", "http=plain:80;https=secure:8080", "", "http://secure:8080"},
		{"https://github.com/release", "proxy:80", "*.github.com;github.com", ""},
		{"http://intranet/release", "proxy:80", "<local>", ""},
		{"https://other.test/release", "proxy:80", "*.github.com", "http://proxy:80"},
		{"https://other.test/release", "http=proxy:80", "", ""},
	} {
		req, _ := http.NewRequest(http.MethodGet, tc.target, nil)
		got, err := systemProxyURL(req.URL, tc.servers, tc.bypass)
		if err != nil {
			t.Fatal(err)
		}
		value := ""
		if got != nil {
			value = got.String()
		}
		if value != tc.want {
			t.Fatalf("%s got %q want %q", tc.target, value, tc.want)
		}
	}
}

func TestUpdateMetadataAndHeadersAreBounded(t *testing.T) {
	configureSetupCancellation(false)
	for _, phase := range []string{"headers", "body"} {
		t.Run(phase, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if strings.HasPrefix(filepath.Base(r.URL.Path), "rootfs.") {
					t.Errorf("update requested factory: %s %s", r.Method, r.URL.Path)
					http.Error(w, "factory forbidden", 500)
					return
				}
				if phase == "body" {
					w.WriteHeader(200)
					w.(http.Flusher).Flush()
				}
				<-r.Context().Done()
			}))
			defer server.Close()
			client := newDownloadClient()
			if client.Timeout != 0 {
				t.Fatal("large transfers have a total timeout")
			}
			transport := client.Transport.(*http.Transport)
			if transport.ResponseHeaderTimeout <= 0 || transport.IdleConnTimeout <= 0 || transport.TLSHandshakeTimeout <= 0 {
				t.Fatal("unbounded transport")
			}
			defer client.CloseIdleConnections()
			ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
			defer cancel()
			started := time.Now()
			if _, err := fetchSmallFileContext(ctx, client, server.URL, 1024); err == nil {
				t.Fatal("stalled metadata was accepted")
			}
			if elapsed := time.Since(started); elapsed > time.Second {
				t.Fatalf("cancellation took %s", elapsed)
			}
		})
	}
}

func TestExplicitRuntimePinKeepsItsOwnTrustRoot(t *testing.T) {
	configureSetupCancellation(false)
	data := []byte(testSHA256([]byte("custom runtime")) + "  " + runtimeZip + "\n")
	sum := testSHA256(data)
	var requested atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(filepath.Base(r.URL.Path), "rootfs.") {
			t.Errorf("update requested factory: %s %s", r.Method, r.URL.Path)
			http.Error(w, "factory forbidden", 500)
			return
		}
		requested.Store(true)
		w.Write(data)
	}))
	defer server.Close()
	cfg := &config{localPayload: true, localPayloadSHA256: testSHA256([]byte("different guest update")), payloadDir: t.TempDir()}
	sums, err := releaseSumsForConfig(cfg, server.Client(), server.URL, sum)
	if err != nil {
		t.Fatal(err)
	}
	if !requested.Load() || sums[runtimeZip] != testSHA256([]byte("custom runtime")) {
		t.Fatal("explicit runtime pin was replaced by another payload cache")
	}
	if payloadIsLocal(cfg, sum) {
		t.Fatal("different trust root selected local cache")
	}
	if !payloadIsLocal(cfg, cfg.localPayloadSHA256) {
		t.Fatal("matching signed update failed to select local cache")
	}
}
