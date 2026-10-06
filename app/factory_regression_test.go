package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

func legacyFactoryFixture(t *testing.T, cfg *config, release, digest string, files map[string][]byte) {
	t.Helper()
	if err := ensureFactory(cfg, release, digest); err != nil {
		t.Fatal(err)
	}
	sums := map[string]string{}
	for _, name := range installedGuestArtifacts {
		sums[name] = testSHA256(files[name])
	}
	if err := writeInstallReceipt(cfg.guestDir, release, digest, installedGuestArtifacts, sums); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(filepath.Join(cfg.guestDir, "SHA256SUMS")); err != nil {
		t.Fatal(err)
	}
}

func TestLegacyFactoryDiskGrowth(t *testing.T) {
	cfg, files, server, _ := onDemandFixture(t, false)
	legacyFactoryFixture(t, cfg, server.URL, testSHA256(files["SHA256SUMS"]), files)
	server.Close()
	sentinel := bytes.Repeat([]byte("older user disk"), 4)
	if err := os.WriteFile(cfg.disk, sentinel, 0600); err != nil {
		t.Fatal(err)
	}
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal("legacy growth blocked:", err)
	}
	disk, err := os.ReadFile(cfg.disk)
	if err != nil {
		t.Fatal(err)
	}
	if len(disk) != 1<<20 || !bytes.Equal(disk[:len(sentinel)], sentinel) {
		t.Fatal("growth changed user contents or size")
	}
}

func TestReceiptRepairPreservesFactoryIdentity(t *testing.T) {
	for _, damage := range []string{"damaged", "missing"} {
		t.Run(damage, func(t *testing.T) {
			cfg, files, server, _ := onDemandFixture(t, true)
			digest := testSHA256(files["SHA256SUMS"])
			// Create a real factory-backed overlay and its identity sidecar.
			if err := prepareDisk(cfg, 1); err != nil {
				t.Fatal(err)
			}
			want, ok := installReceiptArtifactSHA256(cfg.guestDir, "rootfs.ext4")
			if !ok {
				t.Fatal("setup: no rootfs identity")
			}
			before, err := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
			if err != nil {
				t.Fatal(err)
			}
			rootfs := filepath.Join(cfg.guestDir, "rootfs.ext4")
			if damage == "missing" {
				if err := os.Remove(rootfs); err != nil {
					t.Fatal(err)
				}
			} else if err := os.WriteFile(rootfs, []byte("damaged"), 0600); err != nil {
				t.Fatal(err)
			}
			later := time.Now().Add(time.Hour)
			if err := os.Chtimes(filepath.Join(cfg.guestDir, "vmlinuz-linux"), later, later); err != nil {
				t.Fatal(err)
			}
			if err := ensureGuest(cfg, server.URL, digest); err == nil {
				t.Error("accepted missing or damaged backing")
			}
			after, err := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
			if err != nil || !bytes.Equal(before, after) {
				t.Fatal("failed repair changed receipt", err)
			}
			got, ok := installReceiptArtifactSHA256(cfg.guestDir, "rootfs.ext4")
			if !ok || got != want {
				t.Fatal("receipt lost the backing identity")
			}
			if err := os.WriteFile(rootfs, files["rootfs.ext4"], 0600); err != nil {
				t.Fatal(err)
			}
			if err := ensureGuest(cfg, server.URL, digest); err != nil {
				t.Fatal("restored original backing cannot boot:", err)
			}
		})
	}
}

func TestLegacyFactoryOfflineReset(t *testing.T) {
	cfg, files, server, _ := onDemandFixture(t, false)
	digest := testSHA256(files["SHA256SUMS"])
	legacyFactoryFixture(t, cfg, server.URL, digest, files)
	server.Close()
	sentinel := bytes.Repeat([]byte("old user disk"), 4)
	if err := os.WriteFile(cfg.disk, sentinel, 0600); err != nil {
		t.Fatal(err)
	}
	cfg.fresh = true
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal("offline reset with intact local rootfs:", err)
	}
	disk, err := os.ReadFile(cfg.disk)
	if err != nil {
		t.Fatal(err)
	}
	if len(disk) != 1<<20 || !bytes.Equal(disk[:len(files["rootfs.ext4"])], files["rootfs.ext4"]) {
		t.Fatal("reset did not create the selected factory disk")
	}
	retained, err := filepath.Glob(filepath.Join(cfg.vmDir, "before-reset-*", "disk.raw"))
	if err != nil || len(retained) != 1 {
		t.Fatalf("retained disk: %v %v", retained, err)
	}
	old, err := os.ReadFile(retained[0])
	if err != nil || !bytes.Equal(old, sentinel) {
		t.Fatal("reset did not retain user disk", err)
	}
}

func TestPortableBackingReceiptFastPath(t *testing.T) {
	cfg, files, server, _ := onDemandFixture(t, true)
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	rootfs := filepath.Join(cfg.guestDir, "rootfs.ext4")
	info, err := os.Stat(rootfs)
	if err != nil {
		t.Fatal(err)
	}
	// Same size/mtime deliberately demonstrates receipt trust on ordinary launch.
	// Destructive factory reuse must still reject these altered bytes.
	if err := os.WriteFile(rootfs, bytes.Repeat([]byte("x"), len(files["rootfs.ext4"])), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(rootfs, info.ModTime(), info.ModTime()); err != nil {
		t.Fatal(err)
	}
	if err := validateInstalledDiskBacking(cfg); err != nil {
		t.Fatal("unchanged receipt did not use fast path:", err)
	}
	if err := ensureFactory(cfg, server.URL, testSHA256(files["SHA256SUMS"])); err == nil {
		t.Fatal("destructive reuse trusted fast path")
	}
	later := info.ModTime().Add(time.Hour)
	if err := os.Chtimes(rootfs, later, later); err != nil {
		t.Fatal(err)
	}
	if err := validateInstalledDiskBacking(cfg); err == nil {
		t.Fatal("receipt miss did not hash backing")
	}
	if err := os.WriteFile(rootfs, files["rootfs.ext4"], 0600); err != nil {
		t.Fatal(err)
	}
	if err := validateInstalledDiskBacking(cfg); err != nil {
		t.Fatal("valid backing rejected after timestamp change:", err)
	}
}

func TestFactoryUnavailableErrorsKeepLocalCause(t *testing.T) {
	for _, err := range []error{syscall.ENOSPC, &os.PathError{Op: "write", Path: "factory", Err: syscall.ENOSPC}, errInsufficientDiskSpace, errSetupCancelled, context.Canceled, &url.Error{Op: "Get", URL: "https://example.invalid", Err: context.Canceled}, errors.New("checksum mismatch"), &downloadHTTPError{status: 400}} {
		if factoryUnavailable(fmt.Errorf("preparing factory: %w", err)) {
			t.Fatalf("local error classified as network: %v", err)
		}
	}
	for _, err := range []error{&url.Error{Op: "Get", URL: "https://example.invalid", Err: errors.New("unreachable")}, &downloadHTTPError{status: 404}, &downloadUnavailableError{err: io.ErrUnexpectedEOF}} {
		if !factoryUnavailable(fmt.Errorf("preparing factory: %w", err)) {
			t.Fatalf("network error missed: %v", err)
		}
	}
}

func TestPortableTransitionSpaceFailureDoesNotStartPayloadUpdate(t *testing.T) {
	cfg, files, server, _ := onDemandFixture(t, true)
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	beforeDisk, err := os.ReadFile(cfg.disk)
	if err != nil {
		t.Fatal(err)
	}
	beforeReceipt, err := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
	if err != nil {
		t.Fatal(err)
	}
	oldFree := diskFreeBytes
	t.Cleanup(func() { diskFreeBytes = oldFree })
	diskFreeBytes = func(string) (int64, error) { return 0, nil }
	err = preparePortablePayloadTransition(cfg, server.URL, testSHA256([]byte("next payload")))
	if !errors.Is(err, errInsufficientDiskSpace) {
		t.Fatalf("want retryable space failure, got %v", err)
	}
	afterDisk, err := os.ReadFile(cfg.disk)
	if err != nil {
		t.Fatal(err)
	}
	afterReceipt, err := os.ReadFile(filepath.Join(cfg.guestDir, installReceiptFilename))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(beforeDisk, afterDisk) || !bytes.Equal(beforeReceipt, afterReceipt) {
		t.Fatal("failed detachment changed installation")
	}
	if state, err := readPayloadUpdateState(cfg.dir); err != nil || state != nil {
		t.Fatalf("failure started an update: %+v %v", state, err)
	}
	if rolledBack, err := rollbackPendingPayloadUpdates(cfg.dir); err != nil || rolledBack {
		t.Fatal("retryable failure caused rollback", err)
	}
	ready, err := installReceiptMatches(cfg.guestDir, server.URL, testSHA256(files["SHA256SUMS"]), installedGuestArtifacts)
	if err != nil || !ready {
		t.Fatal("previous payload is not ready", err)
	}
}
