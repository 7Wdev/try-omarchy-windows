package main

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPortablePayloadVersionsPreserveRollback(t *testing.T) {
	configureSetupCancellation(false)
	root := t.TempDir()
	old := []byte("old manifest")
	if err := os.WriteFile(filepath.Join(root, "SHA256SUMS"), old, 0600); err != nil {
		t.Fatal(err)
	}
	artifacts := map[string][]byte{}
	for _, name := range append([]string{runtimeZip, "rootfs.ext4", "rootfs.ext4.zst"}, downloadedGuestArtifacts...) {
		artifacts[name] = []byte("new " + name)
	}
	artifacts["guest-manifest.json"] = []byte(fmt.Sprintf(`{"schemaVersion":1,"artifacts":[{"path":"rootfs.ext4","bytes":%d,"sha256":"%s"},{"path":"rootfs.ext4.zst","bytes":%d,"sha256":"%s"}]}`, len(artifacts["rootfs.ext4"]), testSHA256(artifacts["rootfs.ext4"]), len(artifacts["rootfs.ext4.zst"]), testSHA256(artifacts["rootfs.ext4.zst"])))
	var manifest strings.Builder
	for name, data := range artifacts {
		fmt.Fprintf(&manifest, "%s  %s\n", testSHA256(data), name)
	}
	artifacts["SHA256SUMS"] = []byte(manifest.String())
	digest := testSHA256(artifacts["SHA256SUMS"])
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		data, ok := artifacts[strings.TrimPrefix(r.URL.Path, "/")]
		if !ok {
			http.NotFound(w, r)
			return
		}
		w.Write(data)
	}))
	defer server.Close()
	if err := stagePortablePayload(root, server.URL, digest, server.Client(), nil); err != nil {
		t.Fatal(err)
	}
	if portablePayloadDirectory(root, digest) != filepath.Join(root, digest) {
		t.Fatal("new version not selected")
	}
	if portablePayloadDirectory(root, testSHA256(old)) != root {
		t.Fatal("old version no longer selects its payload")
	}
	got, _ := os.ReadFile(filepath.Join(root, "SHA256SUMS"))
	if string(got) != string(old) {
		t.Fatal("overwrote old payload")
	}
	if err := stagePortablePayload(root, server.URL, digest, server.Client(), nil); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, digest, runtimeZip), []byte("corrupt"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := stagePortablePayload(root, server.URL, digest, server.Client(), nil); err == nil {
		t.Fatal("accepted corrupted staged runtime")
	}
}

func TestPortableLauncherUpdateTarget(t *testing.T) {
	root := t.TempDir()
	got, err := launcherUpdateTarget(filepath.Join(root, "data"), true)
	if err != nil || got != filepath.Join(root, stableLauncherName) {
		t.Fatalf("portable target: %s %v", got, err)
	}
	if _, err := launcherUpdateTarget(filepath.Join(root, "unrelated"), true); err == nil {
		t.Fatal("accepted unrelated parent")
	}
	got, err = launcherUpdateTarget(root, false)
	if err != nil || got != filepath.Join(root, stableLauncherName) {
		t.Fatal("changed standard update target")
	}
}

func TestPortablePayloadCancellationKeepsPreviousVersion(t *testing.T) {
	configureSetupCancellation(false)
	t.Cleanup(func() { configureSetupCancellation(false) })
	root := t.TempDir()
	old := []byte("previous payload")
	if err := os.WriteFile(filepath.Join(root, "SHA256SUMS"), old, 0600); err != nil {
		t.Fatal(err)
	}
	data := []byte(testSHA256([]byte("image")) + "  rootfs.ext4\n")
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Write(data) }))
	defer server.Close()
	err := stagePortablePayload(root, server.URL, testSHA256(data), server.Client(), func(string, int64, int64) { requestSetupCancel() })
	if err == nil {
		t.Fatal("cancelled payload was published")
	}
	entries, err := os.ReadDir(root)
	if err != nil || len(entries) != 2 || entries[1].Name() != "SHA256SUMS" || !strings.HasPrefix(entries[0].Name(), ".payload-staging-") {
		t.Fatalf("unexpected staged files: %v %v", entries, err)
	}
	got, err := os.ReadFile(filepath.Join(root, "SHA256SUMS"))
	if err != nil || string(got) != string(old) {
		t.Fatal("lost previous payload")
	}
}
