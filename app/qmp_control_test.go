package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestQMPControlPreservesUnexpectedFiles(t *testing.T) {
	dir, err := os.MkdirTemp(os.TempDir(), "tom-ipc-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(dir)
	previous := qmpControlDirectory
	qmpControlDirectory = func() (string, error) { return dir, nil }
	defer func() { qmpControlDirectory = previous }()
	path, err := qmpControlPath(qmpToolsPort)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("unrelated file"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := prepareQMPControl(); err == nil {
		t.Fatal("replaced a non-socket file")
	}
	got, _ := os.ReadFile(path)
	if string(got) != "unrelated file" {
		t.Fatal("changed unrelated file")
	}
	if _, err := qmpControlPath(12345); err == nil {
		t.Fatal("accepted unknown role")
	}
	qmpControlDirectory = func() (string, error) { return filepath.Join(dir, strings.Repeat("x", 120)), nil }
	if _, err := qmpControlPath(qmpToolsPort); err == nil {
		t.Fatal("accepted an unusable control path")
	}
}

func TestQemuControlArgumentsUsePrivateSockets(t *testing.T) {
	cfg := &config{qmpDir: filepath.Join("private,controls"), memMiB: 2048, cpus: 2, guestDir: "guest", vmDir: "vm", disk: "disk.raw", diskFormat: "raw", audio: "none"}
	args := buildQemuArgs(cfg, "root=/dev/vda")
	count := 0
	for index, arg := range args {
		if arg != "-qmp" {
			continue
		}
		count++
		value := args[index+1]
		if !strings.HasPrefix(value, "unix:private,,controls") || strings.Contains(value, "tcp:") {
			t.Fatalf("guest-accessible control interface: %s", value)
		}
	}
	if count != len(platformQMPControlRoles()) {
		t.Fatalf("expected %d private control channels, got %d", len(platformQMPControlRoles()), count)
	}
}
