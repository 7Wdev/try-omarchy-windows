package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func portableRecoveryFixture(t *testing.T) (string, string) {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "retained", "data")
	payload := filepath.Join(t.TempDir(), "payload")
	for _, part := range []string{"guest", "runtime/bin", "vm"} {
		if err := os.MkdirAll(filepath.Join(dir, part), 0700); err != nil {
			t.Fatal(err)
		}
	}
	hashes := map[string]string{}
	var manifest strings.Builder
	for _, name := range installedGuestArtifacts {
		data := []byte("original " + name)
		if err := os.WriteFile(filepath.Join(dir, "guest", name), data, 0600); err != nil {
			t.Fatal(err)
		}
		hashes[name] = testSHA256(data)
		manifest.WriteString(hashes[name] + "  " + name + "\n")
	}
	guestData := []byte(manifest.String())
	guestPin := testSHA256(guestData)
	if err := writeInstallReceipt(filepath.Join(dir, "guest"), "https://example.invalid/original-guest", guestPin, installedGuestArtifacts, hashes); err != nil {
		t.Fatal(err)
	}
	runtimeData := []byte(testSHA256([]byte("runtime archive")) + "  " + runtimeZip + "\n")
	runtimePin := testSHA256(runtimeData)
	if err := os.WriteFile(filepath.Join(dir, "runtime/bin/qemu-system-x86_64w.exe"), []byte("retained runtime"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := writeRuntimeReceipt(filepath.Join(dir, "runtime"), "https://example.invalid/original-runtime", runtimePin, testSHA256([]byte("runtime archive"))); err != nil {
		t.Fatal(err)
	}
	for pin, data := range map[string][]byte{guestPin: guestData, runtimePin: runtimeData} {
		folder := filepath.Join(payload, pin)
		if err := os.MkdirAll(folder, 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(folder, "SHA256SUMS"), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	return dir, payload
}

func TestPortableRecoveryPreservesOriginalPayloadIdentities(t *testing.T) {
	dir, source := portableRecoveryFixture(t)
	args, err := preparePortableRecoveryPayload(dir, source)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(args[:2], " ") != "-portable -no-update" {
		t.Fatal(args)
	}
	options := map[string]string{}
	for i := 2; i < len(args); i += 2 {
		options[args[i]] = args[i+1]
	}
	if options["-release"] != "https://example.invalid/original-guest" || options["-runtime-release"] != "https://example.invalid/original-runtime" {
		t.Fatal("recovery changed versions", args)
	}
	cfg := &config{portable: true, payloadDir: filepath.Join(filepath.Dir(dir), "payload")}
	for _, prefix := range []string{"", "runtime-"} {
		release, pin := options["-"+prefix+"release"], options["-"+prefix+"sums-sha256"]
		sums, err := releaseSumsForConfig(cfg, nil, release, pin)
		if err != nil {
			t.Fatal("offline startup cannot read manifest", err)
		}
		if prefix == "runtime-" {
			if !runtimeReceiptMatches(filepath.Join(dir, "runtime"), release, pin, sums[runtimeZip]) {
				t.Fatal("runtime would be replaced during recovery")
			}
		} else if ready, err := installReceiptMatches(filepath.Join(dir, "guest"), release, pin, installedGuestArtifacts); err != nil || !ready {
			t.Fatal("guest would be replaced during recovery", err)
		}
	}
	if _, err := preparePortableRecoveryPayload(dir, source); err != nil {
		t.Fatal("retry failed", err)
	}
}

func TestPortableRecoveryRejectsChangedManifest(t *testing.T) {
	dir, source := portableRecoveryFixture(t)
	_, pin, _ := installReceiptIdentity(filepath.Join(dir, "guest"))
	if err := os.WriteFile(filepath.Join(source, pin, "SHA256SUMS"), []byte("changed"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := preparePortableRecoveryPayload(dir, source); err == nil {
		t.Fatal("unauthenticated recovery manifest accepted")
	}
	if _, err := os.Stat(filepath.Join(dir, "guest/rootfs.ext4")); err != nil {
		t.Fatal("retained data was removed")
	}
}

func TestPortableRecoveryCommandProtectsBatchArguments(t *testing.T) {
	command, err := portableRecoveryCommand([]string{"-portable", "-release", "https://example.invalid/a%20b?x=1&y=!value!"})
	if err != nil || !strings.Contains(command, "setlocal DisableDelayedExpansion") || !strings.Contains(command, `"https://example.invalid/a%%20b?x=1&y=!value!"`) {
		t.Fatal(command, err)
	}
	for _, bad := range []string{"bad\" & command", "bad\ncommand", "bad\rcommand"} {
		if _, err := portableRecoveryCommand([]string{bad}); err == nil {
			t.Fatal("accepted batch command", bad)
		}
	}
}

func TestPortableRecoveryUsesRestoredSeparateManifestsWithoutCache(t *testing.T) {
	dir, source := portableRecoveryFixture(t)
	for _, part := range []string{"guest", "runtime"} {
		var pin string
		if part == "guest" {
			_, pin, _ = installReceiptIdentity(filepath.Join(dir, part))
		} else {
			_, pin, _ = runtimeReceiptIdentity(filepath.Join(dir, part))
		}
		data, err := os.ReadFile(filepath.Join(source, pin, "SHA256SUMS"))
		if err != nil {
			t.Fatal(err)
		}
		os.WriteFile(filepath.Join(dir, part, "SHA256SUMS"), data, 0600)
	}
	os.RemoveAll(source)
	if _, err := preparePortableRecoveryPayload(dir, source); err != nil {
		t.Fatal("restored metadata did not support offline recovery", err)
	}
}
