package main

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/klauspost/compress/zstd"
)

func factoryPayloadFixture(t *testing.T, rootfs []byte) map[string][]byte {
	t.Helper()
	files := updatePayloadFixture()
	encoder, err := zstd.NewWriter(nil)
	if err != nil {
		t.Fatal(err)
	}
	defer encoder.Close()
	files["rootfs.ext4"] = rootfs
	files["rootfs.ext4.zst"] = encoder.EncodeAll(rootfs, nil)
	files["build-spec.json"] = []byte(`{"image":{"architecture":"x86_64"},"runtime":{"storage":{"expandedSizeMiB":1}}}`)
	files["guest-manifest.json"] = []byte(fmt.Sprintf(`{"schemaVersion":1,"artifacts":[{"path":"rootfs.ext4","bytes":%d,"sha256":"%s"},{"path":"rootfs.ext4.zst","bytes":%d,"sha256":"%s"}]}`, len(rootfs), testSHA256(rootfs), len(files["rootfs.ext4.zst"]), testSHA256(files["rootfs.ext4.zst"])))
	setFixtureSums(files)
	return files
}

func installFactoryFixture(t *testing.T, guest string, rootfs []byte) string {
	t.Helper()
	files := factoryPayloadFixture(t, rootfs)
	os.MkdirAll(guest, 0700)
	sums := map[string]string{}
	for _, name := range append(append([]string{}, preparedBootArtifacts...), "rootfs.ext4") {
		if err := os.WriteFile(filepath.Join(guest, name), files[name], 0600); err != nil {
			t.Fatal(err)
		}
		sums[name] = testSHA256(files[name])
	}
	digest := testSHA256(files["SHA256SUMS"])
	if err := writeInstallReceipt(guest, "https://example.invalid/v0.0.9-preview", digest, append(append([]string{}, preparedBootArtifacts...), "rootfs.ext4"), sums); err != nil {
		t.Fatal(err)
	}
	return testSHA256(rootfs)
}

func onDemandFixture(t *testing.T, portable bool) (*config, map[string][]byte, *httptest.Server, *atomic.Int64) {
	return onDemandFixtureFiles(t, portable, factoryPayloadFixture(t, []byte("exact pinned factory")))
}
func onDemandFixtureFiles(t *testing.T, portable bool, files map[string][]byte) (*config, map[string][]byte, *httptest.Server, *atomic.Int64) {
	t.Helper()
	configureSetupCancellation(false)
	t.Cleanup(func() { configureSetupCancellation(false) })
	requests := &atomic.Int64{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		name := filepath.Base(r.URL.Path)
		if strings.HasPrefix(name, "rootfs.") {
			requests.Add(1)
		}
		data, ok := files[name]
		if !ok {
			http.NotFound(w, r)
			return
		}
		http.ServeContent(w, r, name, time.Time{}, bytes.NewReader(data))
	}))
	t.Cleanup(server.Close)
	dir := t.TempDir()
	cfg := &config{dir: dir, guestDir: filepath.Join(dir, "guest"), vmDir: filepath.Join(dir, "vm"), disk: filepath.Join(dir, "vm", "disk.raw"), diskFormat: "raw", portable: portable}
	os.MkdirAll(cfg.vmDir, 0700)
	if portable {
		cfg.payloadDir = t.TempDir()
		if err := stageUpdatePayload(context.Background(), cfg.payloadDir, server.URL, testSHA256(files["SHA256SUMS"]), server.Client(), nil); err != nil {
			t.Fatal(err)
		}
		cfg.disk = filepath.Join(cfg.vmDir, "disk.qcow2")
		cfg.diskFormat = "qcow2"
	}
	if err := ensureGuestFiles(cfg, server.URL, testSHA256(files["SHA256SUMS"])); err != nil {
		t.Fatal(err)
	}
	if requests.Load() != 0 {
		t.Fatal("boot preparation requested factory", requests.Load())
	}
	return cfg, files, server, requests
}

func TestFactoryOnDemandCreationAndResetRequests(t *testing.T) {
	for _, portable := range []bool{false, true} {
		t.Run(fmt.Sprint(portable), func(t *testing.T) {
			cfg, files, server, requests := onDemandFixture(t, portable)
			receipt, _ := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
			var state installReceipt
			json.Unmarshal(receipt, &state)
			if _, ok := state.Files["rootfs.ext4"]; ok {
				t.Fatal("boot receipt invented a factory")
			}
			if err := prepareDisk(cfg, 1); err != nil {
				t.Fatal(err)
			}
			if requests.Load() == 0 {
				t.Fatal("creation did not acquire factory")
			}
			rootfs, _ := os.ReadFile(filepath.Join(cfg.guestDir, "rootfs.ext4"))
			if !bytes.Equal(rootfs, files["rootfs.ext4"]) {
				t.Fatal("wrong factory")
			}
			before := requests.Load()

			if err := ensureGuest(cfg, server.URL, testSHA256(files["SHA256SUMS"])); err != nil {
				t.Fatal("offline boot", err)
			}
			if requests.Load() != before {
				t.Fatal("reuse requested factory")
			}
			if portable {
				os.Remove(cfg.disk)
				independentFixture(t, cfg.disk, 1<<20)
			}
			os.Remove(filepath.Join(cfg.guestDir, "rootfs.ext4"))
			cfg.fresh = true
			if err := prepareDisk(cfg, 1); err != nil {
				t.Fatal("reset", err)
			}
			if requests.Load() <= before {
				t.Fatal("reset did not acquire missing factory")
			}
		})
	}
}

func TestFactoryAcquisitionFailuresKeepWorkingInstallation(t *testing.T) {
	for _, mode := range []string{"offline", "404", "bad-archive", "bad-rootfs", "cancelled", "low-space", "bad-metadata"} {
		t.Run(mode, func(t *testing.T) {
			fixture := factoryPayloadFixture(t, []byte("exact pinned factory"))
			if mode == "bad-rootfs" {
				encoder, _ := zstd.NewWriter(nil)
				fixture["rootfs.ext4.zst"] = encoder.EncodeAll([]byte("wrong unpacked bytes"), nil)
				encoder.Close()
				fixture["guest-manifest.json"] = []byte(fmt.Sprintf(`{"schemaVersion":1,"artifacts":[{"path":"rootfs.ext4","bytes":%d,"sha256":"%s"},{"path":"rootfs.ext4.zst","bytes":%d,"sha256":"%s"}]}`, len(fixture["rootfs.ext4"]), testSHA256(fixture["rootfs.ext4"]), len(fixture["rootfs.ext4.zst"]), testSHA256(fixture["rootfs.ext4.zst"])))
				setFixtureSums(fixture)
			}
			cfg, files, server, _ := onDemandFixtureFiles(t, false, fixture)
			sentinel := bytes.Repeat([]byte("user files"), 16)
			os.WriteFile(cfg.disk, sentinel, 0600)
			marker := filepath.Join(launcherUpdateDir(cfg.dir), stagedUpdateFilename)
			writeUpdateFile(marker, []byte("v0.10.1"))
			before, _ := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
			boot, _ := os.ReadFile(filepath.Join(cfg.guestDir, "vmlinuz-linux"))
			oldFree := diskFreeBytes
			t.Cleanup(func() { diskFreeBytes = oldFree })
			switch mode {
			case "offline":
				server.Close()
			case "404":
				delete(files, "rootfs.ext4.zst")
			case "bad-archive":
				files["rootfs.ext4.zst"] = []byte("corrupt")
			case "cancelled":
				requestSetupCancel()
			case "low-space":
				diskFreeBytes = func(string) (int64, error) { return 0, nil }
			case "bad-metadata":
				os.WriteFile(filepath.Join(cfg.guestDir, "guest-manifest.json"), []byte("damaged"), 0600)
				files["guest-manifest.json"] = []byte("also damaged")
			}
			release, digest, _ := installReceiptIdentity(cfg.guestDir)
			err := ensureFactory(cfg, release, digest)
			if err == nil {
				t.Fatal("failure succeeded")
			}
			hint := uiTextWith("error.factory.unavailable", map[string]string{"version": factoryReleaseLabel(release)})
			wantHint := mode == "offline" || mode == "404"
			if strings.Contains(err.Error(), hint) != wantHint {
				t.Fatalf("network hint=%v, error: %v", wantHint, err)
			}
			stages, err := filepath.Glob(filepath.Join(cfg.dir, ".factory-*"))
			if err != nil || len(stages) != 0 {
				t.Fatalf("failed acquisition left stages: %v %v", stages, err)
			}
			disk, _ := os.ReadFile(cfg.disk)
			after, _ := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
			afterBoot, _ := os.ReadFile(filepath.Join(cfg.guestDir, "vmlinuz-linux"))
			ready, _ := os.ReadFile(marker)
			if !bytes.Equal(disk, sentinel) || !bytes.Equal(before, after) || !bytes.Equal(boot, afterBoot) || string(ready) != "v0.10.1" {
				t.Fatal("acquisition failure changed existing state")
			}
			if _, err := os.Stat(filepath.Join(cfg.guestDir, "rootfs.ext4")); !os.IsNotExist(err) {
				t.Fatal("failed factory was published")
			}
		})
	}
}

func TestBootOnlyUpdateAndRollbackRoundTrip(t *testing.T) {
	cfg, files, server, requests := onDemandFixture(t, false)
	disk := bytes.Repeat([]byte("user sentinel"), 100)
	disk = append(disk, make([]byte, (1<<20)-len(disk))...)
	os.WriteFile(cfg.disk, disk, 0600)
	oldReceipt, _ := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
	files["vmlinuz-linux"] = []byte("new kernel")
	setFixtureSums(files)
	if err := ensureGuest(cfg, server.URL, testSHA256(files["SHA256SUMS"])); err != nil {
		t.Fatal(err)
	}
	if requests.Load() != 0 {
		t.Fatal("update fetched rootfs", requests.Load())
	}
	state, _ := readPayloadUpdateState(cfg.dir)
	if state == nil {
		t.Fatal("no rollback journal")
	}
	restored, err := rollbackPendingPayloadUpdates(cfg.dir)
	if err != nil || !restored {
		t.Fatal("rollback", restored, err)
	}
	after, _ := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
	afterDisk, _ := os.ReadFile(cfg.disk)
	if !bytes.Equal(oldReceipt, after) || !bytes.Equal(disk, afterDisk) {
		t.Fatal("rollback lost boot identity or user disk")
	}
	server.Close()
	release, digest, _ := installReceiptIdentity(cfg.guestDir)
	if err := ensureGuest(cfg, release, digest); err != nil {
		t.Fatal("offline restored boot", err)
	}
}

func TestTemplateFreeBackupAndCheckpointRoundTrip(t *testing.T) {
	cfg, _, server, _ := onDemandFixture(t, false)
	os.WriteFile(cfg.disk, bytes.Repeat([]byte("disk"), 1024), 0600)
	server.Close()
	archive := filepath.Join(t.TempDir(), "backup.zip")
	if err := writeVMBackup(cfg.dir, archive); err != nil {
		t.Fatal(err)
	}
	restored := filepath.Join(t.TempDir(), "restored")
	if err := restoreVMBackup(archive, restored); err != nil {
		t.Fatal(err)
	}
	if !completeInstallExists(restored, "disk.raw") {
		t.Fatal("template-free restore incomplete")
	}
	release, digest, _ := installReceiptIdentity(filepath.Join(restored, "guest"))
	restoredCfg := &config{dir: restored, guestDir: filepath.Join(restored, "guest"), disk: filepath.Join(restored, "vm", "disk.raw")}
	if err := ensureGuest(restoredCfg, release, digest); err != nil {
		t.Fatal("restore needed network", err)
	}
	original, _ := os.ReadFile(cfg.disk)
	copied, _ := os.ReadFile(restoredCfg.disk)
	if !bytes.Equal(original, copied) {
		t.Fatal("restore disk changed")
	}
	store := checkpointStore{installation: cfg.dir}
	if _, err := store.Create("template-free checkpoint", nil); err != nil {
		t.Fatal(err)
	}
}

func TestBootPayloadCachesNeverFetchFactory(t *testing.T) {
	configureSetupCancellation(false)
	files := factoryPayloadFixture(t, []byte("factory"))
	var requests atomic.Int64
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		name := filepath.Base(r.URL.Path)
		if strings.HasPrefix(name, "rootfs.") {
			requests.Add(1)
			http.Error(w, "factory forbidden", 500)
			return
		}
		http.ServeContent(w, r, name, time.Time{}, bytes.NewReader(files[name]))
	}))
	defer server.Close()
	root := t.TempDir()
	digest := testSHA256(files["SHA256SUMS"])
	for _, full := range []bool{false, true} {
		if full {
			os.WriteFile(filepath.Join(root, digest, "rootfs.ext4.zst"), files["rootfs.ext4.zst"], 0600)
		}
		if err := stageUpdatePayload(context.Background(), root, server.URL, digest, server.Client(), nil); err != nil {
			t.Fatal(err)
		}
	}
	if requests.Load() != 0 {
		t.Fatal("cache fetched factory", requests.Load())
	}
}

func independentFixture(t *testing.T, path string, size int64) {
	t.Helper()
	if err := createQcow2Overlay(path, "../guest/rootfs.ext4", size); err != nil {
		t.Fatal(err)
	}
	file, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	zeros := make([]byte, 12)
	binary.BigEndian.PutUint64(zeros, 0)
	if _, err := file.WriteAt(zeros, 8); err != nil {
		t.Fatal(err)
	}
}

func TestFactoryOfflineVerifiedReset(t *testing.T) {
	for _, portable := range []bool{false, true} {
		t.Run(fmt.Sprint(portable), func(t *testing.T) {
			cfg, _, server, requests := onDemandFixture(t, portable)
			if err := prepareDisk(cfg, 1); err != nil {
				t.Fatal(err)
			}
			before := requests.Load()
			server.Close()
			cfg.fresh = true
			if err := prepareDisk(cfg, 1); err != nil {
				t.Fatal("verified offline reset", err)
			}
			if requests.Load() != before {
				t.Fatal("offline reset fetched factory")
			}
		})
	}
}

func TestFactoryCancelledTransferPreservesDiskAndReceipt(t *testing.T) {
	cfg, files, server, _ := onDemandFixture(t, false)
	server.Config.Handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if filepath.Base(r.URL.Path) == "rootfs.ext4.zst" {
			requestSetupCancel()
		}
		w.Write(files[filepath.Base(r.URL.Path)])
	})
	os.WriteFile(cfg.disk, []byte("saved disk"), 0600)
	before, _ := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
	release, digest, _ := installReceiptIdentity(cfg.guestDir)
	if err := ensureFactory(cfg, release, digest); err == nil {
		t.Fatal("cancelled transfer succeeded")
	}
	disk, _ := os.ReadFile(cfg.disk)
	after, _ := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
	if string(disk) != "saved disk" || !bytes.Equal(before, after) {
		t.Fatal("cancelled transfer changed installation")
	}
}

func TestDamagedExistingDataDisablesWholeSetupCleanup(t *testing.T) {
	for _, name := range []string{"vm/disk.raw", "vm/disk.qcow2", "vm/before-reset-saved/disk.raw", "guest.previous/sentinel", "checkpoints/sentinel"} {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			file := filepath.Join(dir, filepath.FromSlash(name))
			os.MkdirAll(filepath.Dir(file), 0700)
			os.WriteFile(file, []byte("personal recovery data"), 0600)
			if err := cleanupCancelledSetup(dir, filepath.Join(dir, "TryOmarchy.exe"), true); err != nil {
				t.Fatal(err)
			}
			data, _ := os.ReadFile(file)
			if string(data) != "personal recovery data" {
				t.Fatal("cancel deleted existing data")
			}
		})
	}
}

func TestTemplateFreeArchiveRejectsInvalidRecoveryMetadata(t *testing.T) {
	for _, mode := range []string{"unknown-version", "v1-missing-template", "tampered-sums", "tampered-metadata", "truncated-disk", "absent-template-receipt"} {
		t.Run(mode, func(t *testing.T) {
			cfg, _, server, _ := onDemandFixture(t, false)
			server.Close()
			os.WriteFile(cfg.disk, []byte("complete writable disk"), 0600)
			archive := filepath.Join(t.TempDir(), "good.zip")
			if err := writeVMBackup(cfg.dir, archive); err != nil {
				t.Fatal(err)
			}
			z, err := zip.OpenReader(archive)
			if err != nil {
				t.Fatal(err)
			}
			defer z.Close()
			var manifest backupManifest
			entries := map[string][]byte{}
			for _, f := range z.File {
				r, err := f.Open()
				if err != nil {
					t.Fatal(err)
				}
				data, err := io.ReadAll(r)
				r.Close()
				if err != nil {
					t.Fatal(err)
				}
				if f.Name == backupManifestName {
					json.Unmarshal(data, &manifest)
				} else {
					entries[f.Name] = data
				}
			}
			switch mode {
			case "unknown-version":
				manifest.Version = 99
			case "v1-missing-template":
				manifest.Version = 1
			case "tampered-sums":
				entries["guest/SHA256SUMS"] = []byte("tampered")
			case "tampered-metadata":
				entries["guest/guest-manifest.json"] = []byte(`{"schemaVersion":1}`)
			case "truncated-disk":
				entries["vm/disk.raw"] = []byte("short")
			case "absent-template-receipt":
				var receipt installReceipt
				json.Unmarshal(entries["guest/"+installReceiptFilename], &receipt)
				receipt.Files["rootfs.ext4"] = verifiedArtifact{SHA256: testSHA256([]byte("missing")), Size: 7}
				entries["guest/"+installReceiptFilename], _ = json.Marshal(receipt)
			}
			if mode != "truncated-disk" {
				for i := range manifest.Files {
					data := entries[manifest.Files[i].Name]
					manifest.Files[i].Size = int64(len(data))
					manifest.Files[i].SHA256 = testSHA256(data)
				}
			}
			var out bytes.Buffer
			writer := zip.NewWriter(&out)
			for name, data := range entries {
				w, _ := writer.Create(name)
				w.Write(data)
			}
			w, _ := writer.Create(backupManifestName)
			json.NewEncoder(w).Encode(manifest)
			writer.Close()
			reader, err := zip.NewReader(bytes.NewReader(out.Bytes()), int64(out.Len()))
			if err != nil {
				t.Fatal(err)
			}
			destination := filepath.Join(t.TempDir(), "restore")
			if err := restoreVMBackupReader(reader, destination, nil); err == nil {
				t.Fatal("invalid archive restored")
			}
			if _, err := os.Stat(destination); !os.IsNotExist(err) {
				t.Fatal("invalid restore published")
			}
		})
	}
}

func TestTemplateFreePortableBackupExportAndCheckpoint(t *testing.T) {
	tool, err := exec.LookPath("qemu-img")
	if err != nil {
		t.Skip("qemu-img unavailable")
	}
	cfg, _, server, _ := onDemandFixture(t, false)
	server.Close()
	toolBytes, err := os.ReadFile(tool)
	if err != nil {
		t.Fatal(err)
	}
	runtimeTool := filepath.Join(cfg.dir, "runtime", "bin", filepath.Base(tool))
	os.MkdirAll(filepath.Dir(runtimeTool), 0700)
	if err := os.WriteFile(runtimeTool, toolBytes, 0700); err != nil {
		t.Fatal(err)
	}
	original := bytes.Repeat([]byte("complete user disk"), 128)
	os.WriteFile(cfg.disk, original, 0600)
	if err := makeRestoredDiskPortable(cfg.dir, tool, nil); err != nil {
		t.Fatal(err)
	}
	cfg.portable = true
	cfg.disk = filepath.Join(cfg.vmDir, "disk.qcow2")
	output := filepath.Join(t.TempDir(), "exported")
	if err := stagePortableData(cfg.dir, output, tool, nil); err != nil {
		t.Fatal(err)
	}
	inventory, err := inspectInstallationDisk(output)
	if err != nil || inventory.Backing != "" {
		t.Fatal("export dependencies", inventory, err)
	}
	archive := filepath.Join(t.TempDir(), "portable.zip")
	if err := writeVMBackup(cfg.dir, archive); err != nil {
		t.Fatal(err)
	}
	restored := filepath.Join(t.TempDir(), "restored")
	if err := restoreVMBackup(archive, restored); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(filepath.Join(restored, "vm", "disk.raw"))
	if !bytes.HasPrefix(got, original) {
		t.Fatal("portable backup lost disk contents")
	}
	if err := makeRestoredDiskPortable(restored, tool, nil); err != nil {
		t.Fatal(err)
	}
	store := checkpointStore{installation: cfg.dir}
	entry, err := store.Create("Template-free portable", nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.rollbackUsingTool(entry.ID, tool, nil); err != nil {
		t.Fatal("checkpoint rollback", err)
	}
	disk, err := inspectInstallationDisk(cfg.dir)
	if err != nil || disk.Backing != "" {
		t.Fatal("rollback reattached factory", disk, err)
	}
}

func TestSmallDiskGrowthWithoutTemplateUsesAuthenticatedOriginalMetadata(t *testing.T) {
	for _, damage := range []bool{false, true} {
		t.Run(fmt.Sprint(damage), func(t *testing.T) {
			cfg, _, server, requests := onDemandFixture(t, false)
			server.Close()
			original := bytes.Repeat([]byte("older user disk"), 10)
			os.WriteFile(cfg.disk, original, 0600)
			if damage {
				os.WriteFile(filepath.Join(cfg.guestDir, "guest-manifest.json"), []byte("changed metadata"), 0600)
			}
			err := prepareDisk(cfg, 1)
			after, _ := os.ReadFile(cfg.disk)
			if damage {
				if err == nil || !bytes.Equal(original, after) {
					t.Fatal("damaged metadata authorized disk mutation", err)
				}
			} else {
				if err != nil || len(after) != 1<<20 || !bytes.HasPrefix(after, original) {
					t.Fatal("growth lost original data", err)
				}
			}
			if requests.Load() != 0 {
				t.Fatal("growth acquired factory")
			}
		})
	}
}

func TestPortableBootOnlyPublicationDetachesOriginalBacking(t *testing.T) {
	tool, err := exec.LookPath("qemu-img")
	if err != nil {
		t.Skip("qemu-img unavailable")
	}
	for _, badBacking := range []bool{false, true} {
		t.Run(fmt.Sprint(badBacking), func(t *testing.T) {
			cfg, files, server, requests := onDemandFixture(t, true)
			if err := prepareDisk(cfg, 1); err != nil {
				t.Fatal(err)
			}
			toolBytes, err := os.ReadFile(tool)
			if err != nil {
				t.Fatal(err)
			}
			installedTool := filepath.Join(cfg.dir, "runtime", "bin", filepath.Base(tool))
			os.MkdirAll(filepath.Dir(installedTool), 0700)
			os.WriteFile(installedTool, toolBytes, 0700)
			originalDisk, _ := os.ReadFile(cfg.disk)
			originalFactory, _ := os.ReadFile(filepath.Join(cfg.guestDir, "rootfs.ext4"))
			originalReceipt, _ := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
			if badBacking {
				os.WriteFile(filepath.Join(cfg.guestDir, "rootfs.ext4"), []byte("corrupted original backing"), 0600)
			}
			files["vmlinuz-linux"] = []byte("new portable kernel")
			setFixtureSums(files)
			nextDigest := testSHA256(files["SHA256SUMS"])
			requests.Store(0)
			if err := stageUpdatePayload(context.Background(), cfg.payloadDir, server.URL, nextDigest, server.Client(), nil); err != nil {
				t.Fatal(err)
			}
			server.Close()
			err = ensureGuest(cfg, server.URL, nextDigest)
			if requests.Load() != 0 {
				t.Fatal("portable update fetched a new factory")
			}
			if badBacking {
				afterDisk, _ := os.ReadFile(cfg.disk)
				afterReceipt, _ := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
				if err == nil || !bytes.Equal(originalDisk, afterDisk) || !bytes.Equal(originalReceipt, afterReceipt) {
					t.Fatal("wrong backing allowed publication", err)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			disk, err := inspectInstallationDisk(cfg.dir)
			if err != nil || disk.Backing != "" {
				t.Fatal("boot-only guest published over a dependent disk", disk, err)
			}
			oldFactory, _ := os.ReadFile(filepath.Join(cfg.dir, "guest.previous", "rootfs.ext4"))
			if !bytes.Equal(oldFactory, originalFactory) {
				t.Fatal("changed original backing")
			}
			if _, err := os.Stat(filepath.Join(cfg.guestDir, "rootfs.ext4")); !os.IsNotExist(err) {
				t.Fatal("update published a factory")
			}
			raw, cleanup, err := materializeInstallationDiskWithTool(t.TempDir(), disk, tool)
			if err != nil {
				t.Fatal(err)
			}
			defer cleanup()
			contents, _ := os.ReadFile(raw)
			if !bytes.HasPrefix(contents, originalFactory) {
				t.Fatal("detachment changed logical data")
			}
			rolledBack, err := rollbackPendingPayloadUpdates(cfg.dir)
			if err != nil || !rolledBack {
				t.Fatal("portable component rollback", err)
			}
			disk, err = inspectInstallationDisk(cfg.dir)
			if err != nil || disk.Backing != "" {
				t.Fatal("rollback reattached old factory", disk, err)
			}
		})
	}
}
