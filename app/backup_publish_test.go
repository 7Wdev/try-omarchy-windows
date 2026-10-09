package main

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestBackupPublishHardLink(t *testing.T) {
	root := t.TempDir()
	from, to := filepath.Join(root, "source"), filepath.Join(root, "backup.zip")
	if err := os.WriteFile(from, []byte("archive"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := publishBackup(from, to, os.Link); err != nil {
		t.Fatal(err)
	}
	source, _ := os.Stat(from)
	target, err := os.Stat(to)
	if err != nil || !os.SameFile(source, target) {
		t.Fatalf("did not retain hard-link path: %v", err)
	}
}

func TestBackupPublishCopy(t *testing.T) {
	for _, existing := range []bool{false, true} {
		t.Run(map[bool]string{false: "new", true: "concurrent backup"}[existing], func(t *testing.T) {
			root := t.TempDir()
			from, to := filepath.Join(root, "source"), filepath.Join(root, "backup.zip")
			contents := bytes.Repeat([]byte("complete archive\x00"), 10000)
			if err := os.WriteFile(from, contents, 0600); err != nil {
				t.Fatal(err)
			}
			if existing {
				if err := os.WriteFile(to, []byte("existing"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			err := copyAndPublishBackup(from, to)
			want := contents
			if existing {
				if err == nil {
					t.Fatal("replaced concurrent backup")
				}
				want = []byte("existing")
			} else if err != nil {
				t.Fatal(err)
			}
			data, err := os.ReadFile(to)
			if err != nil || !bytes.Equal(data, want) {
				t.Fatalf("destination: %v", err)
			}
			data, err = os.ReadFile(from)
			if err != nil || !bytes.Equal(data, contents) {
				t.Fatalf("source changed: %v", err)
			}
			entries, err := os.ReadDir(root)
			if err != nil || len(entries) != 2 {
				t.Fatalf("staging file leaked: %v, %v", entries, err)
			}
			if !existing {
				source, _ := os.Stat(from)
				target, _ := os.Stat(to)
				if os.SameFile(source, target) {
					t.Fatal("copy published a hard link")
				}
			}
		})
	}
}

func TestBackupPublishCopyReadFailure(t *testing.T) {
	root := t.TempDir()
	from, to := filepath.Join(root, "source"), filepath.Join(root, "backup.zip")
	if err := os.Mkdir(from, 0700); err != nil {
		t.Fatal(err)
	}
	if err := copyAndPublishBackup(from, to); err == nil {
		t.Fatal("published unreadable source")
	}
	entries, err := os.ReadDir(root)
	if err != nil || len(entries) != 1 || entries[0].Name() != "source" {
		t.Fatalf("partial destination or staging file leaked: %v, %v", entries, err)
	}
}

func TestBackupPublishDoesNotFallbackOnOtherErrors(t *testing.T) {
	want := os.ErrPermission
	if err := publishBackup("missing", "missing", func(string, string) error { return want }); !errors.Is(err, want) {
		t.Fatalf("error was masked by fallback: %v", err)
	}
}

func TestBackupPublishCopyCancelled(t *testing.T) {
	configureSetupCancellation(false)
	t.Cleanup(func() { configureSetupCancellation(false) })
	root := t.TempDir()
	from, to := filepath.Join(root, "source"), filepath.Join(root, "backup.zip")
	if err := os.WriteFile(from, []byte("archive"), 0600); err != nil {
		t.Fatal(err)
	}
	requestSetupCancel()
	if err := copyAndPublishBackup(from, to); !errors.Is(err, errSetupCancelled) {
		t.Fatalf("ignored cancellation: %v", err)
	}
	entries, err := os.ReadDir(root)
	if err != nil || len(entries) != 1 || entries[0].Name() != "source" {
		t.Fatalf("cancelled copy leaked output: %v, %v", entries, err)
	}
}
