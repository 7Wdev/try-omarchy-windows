package main

import (
	"archive/zip"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

// A checkpoint retains the full bootable state, not just a disk whose external
// kernel or backing image may disappear during an update. The archive layout
// is versioned independently from the catalog so future storage backends do
// not change snapshot identities or the UI contract.
type vmCheckpoint struct {
	Problem       string    `json:"-"`
	Version       int       `json:"version"`
	ID            string    `json:"id"`
	Name          string    `json:"name"`
	Created       time.Time `json:"created"`
	Architecture  string    `json:"architecture"`
	ArchiveBytes  int64     `json:"archiveBytes"`
	ArchiveSHA256 string    `json:"archiveSHA256"`
}

type checkpointStore struct{ installation string }

func validCheckpointID(id string) bool {
	if len(id) != 32 || strings.ToLower(id) != id {
		return false
	}
	_, err := hex.DecodeString(id)
	return err == nil
}

func validCheckpointName(name string) bool {
	return name != "" && name == strings.TrimSpace(name) && utf8.ValidString(name) &&
		utf8.RuneCountInString(name) <= 80 && !strings.ContainsFunc(name, unicode.IsControl)
}

func (s checkpointStore) path() string { return filepath.Join(s.installation, "checkpoints") }

func (s checkpointStore) open(create bool) (*os.Root, error) {
	if err := validateMovePath(s.path()); err != nil {
		return nil, err
	}
	if create {
		if err := os.MkdirAll(s.path(), 0700); err != nil {
			return nil, err
		}
	}
	return os.OpenRoot(s.path())
}

func readCheckpoint(root *os.Root, id string) (vmCheckpoint, error) {
	var entry vmCheckpoint
	if !validCheckpointID(id) {
		return entry, fmt.Errorf("invalid snapshot identity")
	}
	for _, name := range []string{id, id + "/snapshot.json", id + "/vm.zip"} {
		info, err := root.Lstat(name)
		if err != nil {
			return entry, err
		}
		if err := rejectMoveLink(filepath.Join(root.Name(), name), info); err != nil {
			return entry, err
		}
		if name == id+"/snapshot.json" && info.Size() > 4096 {
			return entry, fmt.Errorf("snapshot metadata is too large")
		}
		if name == id && !info.IsDir() || name != id && !info.Mode().IsRegular() {
			return entry, fmt.Errorf("invalid snapshot file")
		}
	}
	f, err := root.Open(id + "/snapshot.json")
	if err != nil {
		return entry, err
	}
	defer f.Close()
	dec := json.NewDecoder(io.LimitReader(f, 4097))
	dec.DisallowUnknownFields()
	if err = dec.Decode(&entry); err != nil {
		return entry, err
	}
	if dec.Decode(new(any)) != io.EOF || entry.Version != 1 || entry.ID != id || !validCheckpointName(entry.Name) || entry.Created.IsZero() || !validSHA256(entry.ArchiveSHA256) || entry.ArchiveBytes <= 0 || entry.ArchiveBytes > backupMaxBytes {
		return entry, fmt.Errorf("invalid snapshot metadata")
	}
	if entry.Architecture != "x86_64" && entry.Architecture != "aarch64" {
		return entry, fmt.Errorf("unknown snapshot architecture")
	}
	info, err := root.Stat(id + "/vm.zip")
	if err != nil || info.Size() != entry.ArchiveBytes {
		return entry, fmt.Errorf("snapshot archive size changed")
	}
	return entry, nil
}

func (s checkpointStore) List() ([]vmCheckpoint, error) {
	root, err := s.open(false)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	defer root.Close()
	dir, err := root.Open(".")
	if err != nil {
		return nil, err
	}
	defer dir.Close()
	files, err := dir.ReadDir(4097)
	if err != nil && err != io.EOF {
		return nil, err
	}
	if len(files) > 4096 {
		return nil, fmt.Errorf("snapshot store has too many entries")
	}
	var entries []vmCheckpoint
	for _, file := range files {
		if !validCheckpointID(file.Name()) {
			continue
		} // Staging is never advertised as complete.
		entry, err := readCheckpoint(root, file.Name())
		if err != nil {
			entry = vmCheckpoint{ID: file.Name(), Name: uiText("snapshots.damaged"), Problem: err.Error()}
		}
		entries = append(entries, entry)
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Created.After(entries[j].Created) })
	return entries, nil
}

func (s checkpointStore) Create(name string, report backupProgress) (vmCheckpoint, error) {
	var entry vmCheckpoint
	name = strings.TrimSpace(name)
	if !validCheckpointName(name) {
		return entry, uiError(uiText("error.snapshots.name_length"), nil)
	}
	root, err := s.open(true)
	if err != nil {
		return entry, err
	}
	defer root.Close()
	guard, err := lockMoveStore(moveStore{dir: s.path()})
	if err != nil {
		return entry, err
	}
	defer guard.Close()
	if err := s.cleanupInterrupted(root); err != nil {
		return entry, err
	}
	entries, err := s.List()
	if err != nil {
		return entry, err
	}
	if len(entries) >= 1000 {
		return entry, uiError(uiText("error.snapshots.limit"), nil)
	}
	var token [16]byte
	if _, err := rand.Read(token[:]); err != nil {
		return entry, err
	}
	id := hex.EncodeToString(token[:])
	stage := ".pending-" + id
	if err := root.Mkdir(stage, 0700); err != nil {
		return entry, err
	}
	defer root.RemoveAll(stage)
	archive := filepath.Join(s.path(), stage, "vm.zip")
	if err := writeVMArchive(s.installation, archive, report, true); err != nil {
		return entry, err
	}
	z, err := zip.OpenReader(archive)
	if err != nil {
		return entry, err
	}
	_, files, err := readVMBackup(z)
	if err != nil {
		z.Close()
		return entry, err
	}
	specFile := files["guest/build-spec.json"]
	if specFile.UncompressedSize64 > 1<<20 {
		z.Close()
		return entry, fmt.Errorf("guest specification is too large")
	}
	specReader, err := specFile.Open()
	if err != nil {
		z.Close()
		return entry, err
	}
	var spec struct {
		Image struct {
			Architecture string `json:"architecture"`
		} `json:"image"`
	}
	err = json.NewDecoder(io.LimitReader(specReader, 1<<20)).Decode(&spec)
	specReader.Close()
	z.Close()
	if err != nil || (spec.Image.Architecture != "x86_64" && spec.Image.Architecture != "aarch64") {
		return entry, fmt.Errorf("cannot identify guest architecture")
	}
	f, err := root.Open(stage + "/vm.zip")
	if err != nil {
		return entry, err
	}
	h := sha256.New()
	size, err := io.Copy(h, setupReader{f})
	f.Close()
	if err != nil {
		return entry, err
	}
	entry = vmCheckpoint{Version: 1, ID: id, Name: name, Created: time.Now().UTC(), Architecture: spec.Image.Architecture, ArchiveBytes: size, ArchiveSHA256: hex.EncodeToString(h.Sum(nil))}
	meta, err := root.OpenFile(stage+"/snapshot.json", os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return entry, err
	}
	err = json.NewEncoder(meta).Encode(entry)
	if err == nil {
		err = meta.Sync()
	}
	closeErr := meta.Close()
	if err != nil {
		return entry, err
	}
	if closeErr != nil {
		return entry, closeErr
	}
	if err := checkSetupCancelled(); err != nil {
		return entry, err
	}
	if err := root.Rename(stage, id); err != nil {
		return entry, err
	}
	return entry, nil
}

// Restore creates an independently bootable copy. It never replaces the only
// current disk, and verification finishes before the destination is published.
func (s checkpointStore) Restore(id, destination string, report backupProgress) error {
	sourcePath, err := filepath.Abs(s.installation)
	if err != nil {
		return err
	}
	destination, err = filepath.Abs(destination)
	if err != nil {
		return err
	}
	if err := validateMovePath(destination); err != nil {
		return err
	}
	if pathsOverlap(sourcePath, destination) {
		return uiError(uiText("error.snapshots.restore_folder"), nil)
	}
	return s.restoreVerified(id, destination, report)
}

// Private destinations are used only by the journaled active rollback.
func (s checkpointStore) restoreVerified(id, destination string, report backupProgress) error {
	root, err := s.open(false)
	if err != nil {
		return err
	}
	defer root.Close()
	guard, err := lockMoveStore(moveStore{dir: s.path()})
	if err != nil {
		return err
	}
	defer guard.Close()
	entry, err := readCheckpoint(root, id)
	if err != nil {
		return err
	}
	f, err := root.Open(id + "/vm.zip")
	if err != nil {
		return err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, setupReader{f}); err != nil {
		return err
	}
	if hex.EncodeToString(h.Sum(nil)) != entry.ArchiveSHA256 {
		return fmt.Errorf("snapshot checksum mismatch; the current installation was not changed")
	}
	z, err := zip.NewReader(f, entry.ArchiveBytes)
	if err != nil {
		return err
	}
	return restoreVMBackupReader(z, destination, report)
}

func (s checkpointStore) Delete(id string) error {
	root, err := s.open(false)
	if err != nil {
		return err
	}
	defer root.Close()
	guard, err := lockMoveStore(moveStore{dir: s.path()})
	if err != nil {
		return err
	}
	defer guard.Close()
	if !validCheckpointID(id) {
		return fmt.Errorf("invalid snapshot identity")
	}
	if err := validateMovePath(filepath.Join(s.path(), id)); err != nil {
		return err
	}
	dir, err := root.Open(id)
	if err != nil {
		return err
	}
	files, err := dir.ReadDir(-1)
	dir.Close()
	if err != nil {
		return err
	}
	if len(files) > 2 {
		return fmt.Errorf("snapshot contains other files; keep them before removing it")
	}
	for _, file := range files {
		if file.Name() != "snapshot.json" && file.Name() != "vm.zip" {
			return fmt.Errorf("snapshot contains an unknown file")
		}
		info, err := file.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("snapshot contains an unexpected directory or link")
		}
		if err := rejectMoveLink(filepath.Join(s.path(), id, file.Name()), info); err != nil {
			return err
		}
	}
	tombstone := ".deleting-" + id
	if _, err := root.Lstat(tombstone); !os.IsNotExist(err) {
		return fmt.Errorf("an earlier snapshot deletion needs cleanup")
	}
	if err := root.Rename(id, tombstone); err != nil {
		return err
	}
	return root.RemoveAll(tombstone)
}
