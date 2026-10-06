//go:build windows

package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

func TestMoveStreamsAbsent(t *testing.T) {
	for _, code := range []syscall.Errno{1, 38, 87, 0, 2, 3, 5, 32, 33, 50} {
		want := code == 1 || code == 38 || code == 87
		for _, err := range []error{code, fmt.Errorf("stream query: %w", code)} {
			if got := moveStreamsAbsent(err); got != want {
				t.Errorf("error %v: absent=%v, want %v", err, got, want)
			}
		}
	}
	if moveStreamsAbsent(nil) || moveStreamsAbsent(errors.New("The parameter is incorrect.")) {
		t.Fatal("accepted an error without a supported Windows error code")
	}
}

func TestWindowsMoveRetriesTemporaryLock(t *testing.T) {
	for _, code := range []syscall.Errno{5, 32, 33, 3} {
		calls, pauses := 0, 0
		err := publishWindowsMoveWith("from", "to", func(string, string) error {
			calls++
			return code
		}, func(time.Duration) { pauses++ })
		wantCalls := 15
		if code == 3 {
			wantCalls = 1
		}
		if !errors.Is(err, code) || calls != wantCalls || pauses != wantCalls-1 {
			t.Fatalf("error %d: calls=%d pauses=%d err=%v", code, calls, pauses, err)
		}
	}
}

func TestWindowsMoveSucceedsAfterNativeHandleCloses(t *testing.T) {
	source := filepath.Join(t.TempDir(), "source")
	target := source + "-published"
	if err := os.WriteFile(source, []byte("preserve bytes"), 0600); err != nil {
		t.Fatal(err)
	}
	f, err := openBackupDisk(source)
	if err != nil {
		t.Fatal(err)
	}
	closed := make(chan struct{})
	go func() { time.Sleep(100 * time.Millisecond); f.Close(); close(closed) }()
	err = publishWindowsMove(source, target)
	<-closed
	if err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(target)
	if err != nil || string(data) != "preserve bytes" {
		t.Fatalf("published data: %q (%v)", data, err)
	}
}

func TestMoveRejectsAlternateStreamsBeforeCopyAndCleanup(t *testing.T) {
	s, source, destination := moveFixture(t)
	if err := os.MkdirAll(filepath.Dir(destination), 0700); err != nil {
		t.Fatal(err)
	}
	stream := filepath.Join(source, "settings.json") + ":extra"
	if err := os.WriteFile(stream, []byte("preserve this stream"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.prepare(source, destination, nil); err == nil {
		t.Fatal("silently dropped an alternate stream")
	}
	if err := os.Remove(stream); err != nil {
		t.Fatal(err)
	}
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	if err := s.markBooted(destination); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(stream, []byte("added after move"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := s.cleanup(destination); err == nil {
		t.Fatal("deleted a new alternate stream")
	}
	if _, err := os.Stat(filepath.Join(source, "vm", "disk.raw")); err != nil {
		t.Fatal("removed original disk despite failed stream check")
	}
}

func TestMoveUninstallRetiresOnlyMatchingRedirects(t *testing.T) {
	t.Setenv("LOCALAPPDATA", t.TempDir())
	s := hostMoveStore()
	a, b, c, d := filepath.Join(t.TempDir(), "a"), filepath.Join(t.TempDir(), "b"), filepath.Join(t.TempDir(), "c"), filepath.Join(t.TempDir(), "d")
	state := moveState{Version: 1, Redirects: map[string]string{a: b, c: d}}
	if err := s.save(state); err != nil {
		t.Fatal(err)
	}
	if err := forgetMovedInstallation(b); err != nil {
		t.Fatal(err)
	}
	state, err := s.load()
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := state.Redirects[a]; ok {
		t.Fatal("uninstalled destination still redirects fresh launches")
	}
	if state.Redirects[c] != d {
		t.Fatal("removed another installation's redirect")
	}
}

func TestMoveCopiesFilePastSpaceCheckThreshold(t *testing.T) {
	s, source, destination := moveFixture(t)
	data := bytes.Repeat([]byte{0x5a}, 33<<20)
	name := "large-file.bin"
	if err := os.WriteFile(filepath.Join(source, name), data, 0600); err != nil {
		t.Fatal(err)
	}
	prepareFixtureMove(t, s, source, destination)
	if err := s.recover(func(*installationMove) error { return nil }); err != nil {
		t.Fatal(err)
	}
	for _, root := range []string{source, destination} {
		got, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got, data) {
			t.Fatalf("file changed at %s", root)
		}
	}
}
