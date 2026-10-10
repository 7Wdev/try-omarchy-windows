//go:build windows

package main

import (
	"errors"
	"os"
	"path/filepath"
	"syscall"
	"testing"
)

func TestBackupHardLinksUnsupported(t *testing.T) {
	for _, code := range []syscall.Errno{0, 1, 2, 3, 5, 17, 32, 33, 50, 80, 87, 112, 183} {
		want := code == 1 || code == 50
		for _, err := range []error{code, &os.LinkError{Op: "link", Err: code}} {
			if got := backupHardLinksUnsupported(err); got != want {
				t.Fatalf("error %v: got %v, want %v", err, got, want)
			}
		}
	}
	for _, err := range []error{nil, errors.New("Incorrect function.")} {
		if backupHardLinksUnsupported(err) {
			t.Fatalf("accepted %v", err)
		}
	}
}

func TestBackupPublishUnsupportedHardLink(t *testing.T) {
	root := t.TempDir()
	from, to := filepath.Join(root, "source"), filepath.Join(root, "backup.zip")
	if err := os.WriteFile(from, []byte("archive"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, code := range []syscall.Errno{1, 50} {
		err := publishBackup(from, to, func(string, string) error { return &os.LinkError{Op: "link", Err: code} })
		if err != nil {
			t.Fatal(err)
		}
		data, err := os.ReadFile(to)
		if err != nil || string(data) != "archive" {
			t.Fatalf("fallback: %q, %v", data, err)
		}
		if err := os.Remove(to); err != nil {
			t.Fatal(err)
		}
	}
}
