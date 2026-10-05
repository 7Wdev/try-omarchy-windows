package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const moveStateName = "move-state.json"
const moveBlockSize = 64 << 10

type moveFile struct {
	Name      string `json:"name"`
	Size      int64  `json:"size"`
	ModTime   int64  `json:"modTime"`
	SHA256    string `json:"sha256"`
	Directory bool   `json:"directory,omitempty"`
}

type installationMove struct {
	ID          string              `json:"id"`
	Source      string              `json:"source"`
	Destination string              `json:"destination"`
	Stage       string              `json:"stage"`
	Phase       string              `json:"phase"`
	Default     bool                `json:"default"`
	Files       []moveFile          `json:"files"`
	Booted      bool                `json:"booted,omitempty"`
	Volume      *moveVolumeLocation `json:"volume,omitempty"`
}

type moveVolumeLocation struct {
	Volume   string `json:"volume"`
	Relative string `json:"relative"`
}

var identifyMoveVolume = func(string) (*moveVolumeLocation, error) { return nil, nil }
var locateMoveVolume = func(location moveVolumeLocation) (string, error) {
	return "", fmt.Errorf("the moved volume %s is unavailable", location.Volume)
}

var retireMoveLock = func(guard *os.File) error { return os.Remove(guard.Name()) }

// Host-owned state stays outside both installations. Redirects also cover
// explicit -dir launches, so retained guest disks cannot silently diverge.
type moveState struct {
	Version   int                           `json:"version"`
	Redirects map[string]string             `json:"redirects"`
	Pending   *installationMove             `json:"pending,omitempty"`
	Retained  *installationMove             `json:"retained,omitempty"`
	Locations map[string]moveVolumeLocation `json:"locations,omitempty"`
}

type moveStore struct{ dir, defaultDir string }

func (s moveStore) load() (moveState, error) {
	state := moveState{Version: 1, Redirects: map[string]string{}}
	f, err := os.Open(filepath.Join(s.dir, moveStateName))
	if os.IsNotExist(err) {
		return state, nil
	}
	if err != nil {
		return state, err
	}
	defer f.Close()
	dec := json.NewDecoder(io.LimitReader(f, 16<<20))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&state); err != nil {
		return state, fmt.Errorf("reading move history: %w", err)
	}
	if err := dec.Decode(new(any)); err != io.EOF {
		return state, fmt.Errorf("invalid trailing move history")
	}
	if state.Version != 1 {
		return state, fmt.Errorf("unsupported move history version")
	}
	if state.Redirects == nil {
		state.Redirects = map[string]string{}
	}
	for target, location := range state.Locations {
		if !filepath.IsAbs(target) || !validMoveVolumeLocation(location) {
			return state, fmt.Errorf("invalid moved volume location")
		}
	}
	for _, m := range []*installationMove{state.Pending, state.Retained} {
		if m == nil {
			continue
		}
		if m.Volume != nil && !validMoveVolumeLocation(*m.Volume) {
			return state, fmt.Errorf("invalid move volume identity")
		}
		if len(m.ID) != 32 || !filepath.IsAbs(m.Source) || !filepath.IsAbs(m.Destination) ||
			pathsOverlap(m.Source, m.Destination) ||
			m.Stage != filepath.Join(filepath.Dir(m.Destination), ".TryOmarchy-move-"+m.ID) {
			return state, fmt.Errorf("invalid move history paths")
		}
		if _, err := hex.DecodeString(m.ID); err != nil {
			return state, fmt.Errorf("invalid move identity")
		}
		if m.Phase != "copying" && m.Phase != "verified" && m.Phase != "active" && m.Phase != "activating" && m.Phase != "cleaning" {
			return state, fmt.Errorf("invalid move phase")
		}
		for _, entry := range m.Files {
			if !safeMoveName(entry.Name) || entry.Size < 0 || (!entry.Directory && !validSHA256(entry.SHA256)) {
				return state, fmt.Errorf("invalid move inventory")
			}
		}
	}
	return state, nil
}

func (s moveStore) save(state moveState) error {
	data, err := json.MarshalIndent(state, "", "  ")
	if err != nil {
		return err
	}
	if len(data) >= 16<<20 {
		return fmt.Errorf("move inventory is too large")
	}
	if err := os.MkdirAll(s.dir, 0700); err != nil {
		return err
	}
	f, err := os.CreateTemp(s.dir, ".move-state-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err := f.Write(data); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return publishMoveFile(f.Name(), filepath.Join(s.dir, moveStateName))
}

func pathsOverlap(a, b string) bool {
	within := func(parent, child string) bool {
		if pathsEqual(parent, child) {
			return true
		}
		r, err := filepath.Rel(strings.ToLower(parent), strings.ToLower(child))
		return err == nil && r != ".." && !strings.HasPrefix(r, ".."+string(filepath.Separator))
	}
	return within(a, b) || within(b, a)
}

func safeMoveName(name string) bool {
	return name != "." && name != "" && !filepath.IsAbs(name) && filepath.Clean(name) == name &&
		name != ".." && !strings.HasPrefix(name, ".."+string(filepath.Separator)) && !strings.Contains(name, ":")
}

// Reject links in every existing path component, including Windows junctions.
// Only the path itself gets the additional-stream check: ancestor directories
// are outside this installation and may legitimately carry unrelated streams
// (e.g. clipboard/tooling data on the user's profile folder).
func validateMovePath(path string) error {
	first := true
	for current := filepath.Clean(path); ; current = filepath.Dir(current) {
		info, err := os.Lstat(current)
		if err != nil && !os.IsNotExist(err) {
			return err
		}
		if err == nil {
			if first {
				if err := rejectMoveLink(current, info); err != nil {
					return err
				}
			} else if err := rejectAncestorLink(current, info); err != nil {
				return err
			}
		}
		first = false
		if filepath.Dir(current) == current {
			return nil
		}
	}
}

func resolveMovedDirectory(state moveState, dir string, removing ...bool) (string, error) {
	target := dir
	redirected := false
	for source, target := range state.Redirects {
		if pathsEqual(dir, source) {
			return resolveMoveTarget(state, target, removing...)
		}
	}
	for known := range state.Locations {
		if pathsEqual(known, dir) {
			target, redirected = known, true
			break
		}
	}
	if redirected {
		return resolveMoveTarget(state, target, removing...)
	}
	return target, nil
}

func validMoveVolumeLocation(location moveVolumeLocation) bool {
	if !strings.HasPrefix(location.Volume, `\\?\Volume{`) || !strings.HasSuffix(location.Volume, `}\`) ||
		len(location.Volume) != 49 || !safeMoveName(location.Relative) {
		return false
	}
	guid := location.Volume[11:47]
	if guid[8] != '-' || guid[13] != '-' || guid[18] != '-' || guid[23] != '-' {
		return false
	}
	_, err := hex.DecodeString(strings.ReplaceAll(guid, "-", ""))
	return err == nil
}

func resolveMoveTarget(state moveState, target string, removing ...bool) (string, error) {
	if location, ok := state.Locations[target]; ok {
		if !validMoveVolumeLocation(location) {
			return "", fmt.Errorf("invalid moved volume location")
		}
		resolved, err := locateMoveVolume(location)
		if err != nil {
			return "", err
		}
		target = resolved
	}
	if !filepath.IsAbs(target) {
		return "", fmt.Errorf("invalid moved location")
	}
	if err := validateMovePath(target); err != nil {
		return "", err
	}
	if len(removing) > 0 && removing[0] && validUninstallState(target) {
		return target, nil
	}
	info, err := os.Lstat(filepath.Join(target, "vm", "disk.raw"))
	if err != nil || !info.Mode().IsRegular() {
		return "", fmt.Errorf("the moved installation at %s is unavailable; reconnect its drive", target)
	}
	if err := validateMovePath(filepath.Join(target, "vm", "disk.raw")); err != nil {
		return "", err
	}
	return target, nil
}

// Repair only the selected installation. An unrelated disconnected drive must
// not block this launch, and a failed entry-point repair remains retryable.
func (s moveStore) relocate(state moveState, dir string, activate func(*installationMove) error, removing ...bool) (moveState, error) {
	old := dir
	for source, target := range state.Redirects {
		if pathsEqual(dir, source) {
			old = target
			break
		}
	}
	for known := range state.Locations {
		if pathsEqual(old, known) {
			old = known
			break
		}
	}
	location, ok := state.Locations[old]
	if !ok {
		return state, nil
	}
	target, err := resolveMoveTarget(state, old, removing...)
	if err != nil {
		return state, err
	}
	if pathsEqual(old, target) {
		return state, nil
	}
	pointed, found, err := loadDataLocationPointer(s.defaultDir)
	if err != nil {
		return state, err
	}
	if err := activate(&installationMove{Source: old, Destination: target}); err != nil {
		return state, err
	}
	if found && pathsEqual(pointed, old) {
		if err := saveDataLocationPointer(s.defaultDir, target); err != nil {
			return state, err
		}
	}
	for source, destination := range state.Redirects {
		if pathsEqual(destination, old) {
			state.Redirects[source] = target
		}
	}
	// Old Apps entries and shortcuts with an explicit -dir also recover.
	state.Redirects[old] = target
	delete(state.Locations, old)
	state.Locations[target] = location
	if state.Retained != nil && pathsEqual(state.Retained.Destination, old) {
		state.Retained.Destination = target
		state.Retained.Stage = filepath.Join(filepath.Dir(target), ".TryOmarchy-move-"+state.Retained.ID)
	}
	return state, s.save(state)
}

// Upgrade reachable histories without guessing a volume from a drive letter
// after it has already changed. Disconnected legacy moves still fail safely.
func (s moveStore) rememberVolume(state moveState, dir string, removing ...bool) (moveState, error) {
	for source, target := range state.Redirects {
		if !pathsEqual(source, dir) && !pathsEqual(target, dir) {
			continue
		}
		if _, found := state.Locations[target]; found {
			return state, nil
		}
		if _, err := resolveMoveTarget(state, target, removing...); err != nil {
			return state, err
		}
		location, err := identifyMoveVolume(target)
		if err != nil {
			return state, err
		}
		if location == nil {
			return state, nil
		}
		if state.Locations == nil {
			state.Locations = map[string]moveVolumeLocation{}
		}
		state.Locations[target] = *location
		if state.Retained != nil && pathsEqual(state.Retained.Destination, target) {
			state.Retained.Volume = location
		}
		return state, s.save(state)
	}
	return state, nil
}

func (s moveStore) relocatePending(state moveState) error {
	m := state.Pending
	if m == nil || m.Volume == nil {
		return nil
	}
	target, err := locateMoveVolume(*m.Volume)
	if err != nil {
		return err
	}
	if pathsEqual(target, m.Destination) {
		return nil
	}
	if !filepath.IsAbs(target) || pathsOverlap(m.Source, target) || pathsOverlap(s.dir, target) {
		return fmt.Errorf("invalid relocated move path")
	}
	stage := filepath.Join(filepath.Dir(target), ".TryOmarchy-move-"+m.ID)
	if err := validateMovePath(stage); err != nil {
		return err
	}
	if validUninstallStateAt(stage, m.Stage) {
		if err := saveUninstallState(stage); err != nil {
			return err
		}
	}
	m.Destination, m.Stage = target, stage
	return s.save(state)
}

func moveSourceFile(root string, entry moveFile, disk *os.File) (*os.File, bool, error) {
	if entry.Name == filepath.Join("vm", "disk.raw") {
		_, err := disk.Seek(0, io.SeekStart)
		return disk, false, err
	}
	f, err := openBackupDisk(filepath.Join(root, entry.Name))
	return f, true, err
}

func moveExcluded(name string) bool {
	return name == dataLocationPointerName || name == "portable-host" || strings.HasPrefix(name, "portable-host"+string(filepath.Separator))
}

// Count nonzero 64 KiB regions to budget conservatively for both NTFS and ReFS
// allocation units. This avoids charging a mostly empty sparse disk's capacity.
func inventoryMove(source string, disk *os.File, report backupProgress) ([]moveFile, int64, error) {
	return inventoryMoveFiltered(source, disk, report, func(name string) bool { return !moveExcluded(name) })
}

func inventoryMoveFiltered(source string, disk *os.File, report backupProgress, include func(string) bool, fullFiles ...bool) ([]moveFile, int64, error) {
	var files []moveFile
	var required int64 = diskSpaceReserve
	err := filepath.WalkDir(source, func(path string, d os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if path == source {
			return nil
		}
		name, err := filepath.Rel(source, path)
		if err != nil {
			return err
		}
		if !include(name) {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		if err := rejectMoveLink(path, info); err != nil {
			return err
		}
		if !safeMoveName(name) || (!info.IsDir() && !info.Mode().IsRegular()) {
			return fmt.Errorf("unsupported file: %s", name)
		}
		if len(files) >= backupMaxFiles {
			return fmt.Errorf("too many files to move")
		}
		entry := moveFile{Name: name, Size: info.Size(), ModTime: info.ModTime().UnixNano(), Directory: info.IsDir()}
		if !entry.Directory {
			fullAllocation := len(fullFiles) > 0 && fullFiles[0] && name != filepath.Join("vm", "disk.raw")
			if fullAllocation {
				if entry.Size > backupMaxBytes {
					return fmt.Errorf("installation exceeds supported size")
				}
				required += (entry.Size + moveBlockSize - 1) / moveBlockSize * moveBlockSize
				if required > backupMaxBytes {
					return fmt.Errorf("installation exceeds supported size")
				}
			}
			f, closeFile, err := moveSourceFile(source, entry, disk)
			if err != nil {
				return err
			}
			h := sha256.New()
			buf := make([]byte, moveBlockSize)
			var size int64
			for {
				if err := checkSetupCancelled(); err != nil {
					if closeFile {
						f.Close()
					}
					return err
				}
				n, readErr := f.Read(buf)
				if n > 0 {
					size += int64(n)
					h.Write(buf[:n])
					if !fullAllocation && !zeroBytes(buf[:n]) {
						required += moveBlockSize
					}
					if required > backupMaxBytes {
						if closeFile {
							f.Close()
						}
						return fmt.Errorf("installation exceeds supported size")
					}
					if report != nil {
						report(size, entry.Size, name)
					}
				}
				if readErr != nil {
					if closeFile {
						f.Close()
					}
					if readErr != io.EOF {
						return readErr
					}
					break
				}
			}
			if size != entry.Size {
				return fmt.Errorf("%s changed while preparing the move", name)
			}
			entry.SHA256 = hex.EncodeToString(h.Sum(nil))
		}
		required += moveBlockSize
		files = append(files, entry)
		return nil
	})
	return files, required, err
}

func copyMoveFile(source, target string, entry moveFile, disk *os.File, report backupProgress) error {
	in, closeFile, err := moveSourceFile(source, entry, disk)
	if err != nil {
		return err
	}
	if closeFile {
		defer in.Close()
	}
	out, err := os.OpenFile(target, os.O_CREATE|os.O_EXCL|os.O_RDWR, 0600)
	if err != nil {
		return err
	}
	defer out.Close()
	if err := setSparse(out); err != nil {
		return err
	}
	h := sha256.New()
	buf := make([]byte, moveBlockSize)
	var copied int64
	for {
		if err := checkSetupCancelled(); err != nil {
			return err
		}
		n, readErr := in.Read(buf)
		if n > 0 {
			h.Write(buf[:n])
			for start := 0; start < n; start += 4096 {
				block := buf[start:min(start+4096, n)]
				if zeroBytes(block) {
					_, err = out.Seek(int64(len(block)), io.SeekCurrent)
				} else {
					_, err = out.Write(block)
				}
				if err != nil {
					return err
				}
			}
			copied += int64(n)
			if copied > entry.Size {
				return fmt.Errorf("%s grew during the move", entry.Name)
			}
			if copied%(32<<20) < int64(n) {
				free, err := diskFreeBytes(filepath.Dir(target))
				if err != nil {
					return err
				}
				if free < diskSpaceReserve {
					return errInsufficientDiskSpace
				}
			}
			if report != nil {
				report(copied, entry.Size, entry.Name)
			}
		}
		if readErr != nil {
			if readErr != io.EOF {
				return readErr
			}
			break
		}
	}
	if copied != entry.Size || hex.EncodeToString(h.Sum(nil)) != entry.SHA256 {
		return fmt.Errorf("%s changed during the move", entry.Name)
	}
	if err := out.Truncate(entry.Size); err != nil {
		return err
	}
	if err := out.Sync(); err != nil {
		return err
	}
	if _, err := out.Seek(0, io.SeekStart); err != nil {
		return err
	}
	h.Reset()
	if _, err := io.Copy(h, setupReader{out}); err != nil {
		return err
	}
	if hex.EncodeToString(h.Sum(nil)) != entry.SHA256 {
		return fmt.Errorf("verification failed for %s", entry.Name)
	}
	if err := out.Close(); err != nil {
		return err
	}
	t := time.Unix(0, entry.ModTime)
	return os.Chtimes(target, t, t)
}

func (s moveStore) prepare(source, destination string, report backupProgress) (*installationMove, error) {
	state, err := s.load()
	if err != nil {
		return nil, err
	}
	if state.Pending != nil || state.Retained != nil {
		return nil, fmt.Errorf("finish the previous move and remove its retained copy before moving again")
	}
	source, err = filepath.Abs(source)
	if err != nil {
		return nil, err
	}
	destination, err = filepath.Abs(destination)
	if err != nil {
		return nil, err
	}
	if pathsOverlap(source, destination) || pathsOverlap(s.dir, source) || pathsOverlap(s.dir, destination) {
		return nil, uiError(uiText("error.move.destination"), nil)
	}
	if err := validateMovePath(source); err != nil {
		return nil, err
	}
	if err := validateUninstallDirectory(source); err != nil {
		return nil, err
	}
	if err := validateMovePath(destination); err != nil {
		return nil, err
	}
	selectedDefault, hasDefault, err := loadDataLocationPointer(s.defaultDir)
	if err != nil {
		return nil, err
	}
	ownsDefault := (hasDefault && pathsEqual(selectedDefault, source)) || (!hasDefault && pathsEqual(source, s.defaultDir))
	if pathsEqual(destination, s.defaultDir) && !ownsDefault {
		return nil, uiError(uiText("error.move.default_taken"), nil)
	}
	if err := s.checkDestination(destination); err != nil {
		return nil, err
	}
	for _, name := range []string{updateStateFilename, payloadUpdateStateFilename} {
		if _, err := os.Lstat(filepath.Join(source, name)); !os.IsNotExist(err) {
			return nil, uiError(uiText("error.move.pending_update"), nil)
		}
	}
	disk, err := openBackupDisk(filepath.Join(source, "vm", "disk.raw"))
	if err != nil {
		return nil, uiError(uiTextWith("error.move.close_first", map[string]string{"error": err.Error()}), err)
	}
	defer disk.Close()
	files, required, err := inventoryMove(source, disk, report)
	if err != nil {
		return nil, err
	}
	free, err := diskFreeBytes(filepath.Dir(destination))
	if err != nil {
		return nil, err
	}
	if free < required {
		return nil, fmt.Errorf("%w: need %s, available %s", errInsufficientDiskSpace, formatGiB(required), formatGiB(free))
	}
	var id [16]byte
	if _, err := rand.Read(id[:]); err != nil {
		return nil, err
	}
	m := &installationMove{ID: hex.EncodeToString(id[:]), Source: source, Destination: destination, Phase: "copying", Files: files}
	m.Volume, err = identifyMoveVolume(destination)
	if err != nil {
		return nil, err
	}
	m.Stage = filepath.Join(filepath.Dir(destination), ".TryOmarchy-move-"+m.ID)
	selected, found, err := loadDataLocationPointer(s.defaultDir)
	if err != nil {
		return nil, err
	}
	m.Default = (found && pathsEqual(selected, source)) || (!found && pathsEqual(source, s.defaultDir))
	state.Pending = m
	if err := s.save(state); err != nil {
		return nil, err
	}
	if err := os.Mkdir(m.Stage, 0700); err != nil {
		return nil, err
	}
	if err := saveUninstallState(m.Stage); err != nil {
		return nil, err
	}
	for _, entry := range files {
		target := filepath.Join(m.Stage, entry.Name)
		if entry.Directory {
			err = os.MkdirAll(target, 0700)
		} else {
			if err = os.MkdirAll(filepath.Dir(target), 0700); err == nil {
				err = copyMoveFile(source, target, entry, disk, report)
			}
		}
		if err != nil {
			// Leave the durable copying record for recovery. Only staging is
			// eligible for removal; source files have not been changed.
			return nil, err
		}
	}
	if err := checkSetupCancelled(); err != nil {
		return nil, err
	}
	m.Phase = "verified"
	if err := s.save(state); err != nil {
		return nil, err
	}
	return m, nil
}

func (s moveStore) checkDestination(destination string) error {
	entries, err := os.ReadDir(destination)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	// Moving back to Local AppData may replace the bootstrap-only directory.
	if pathsEqual(destination, s.defaultDir) {
		for _, entry := range entries {
			if entry.Name() != dataLocationPointerName && entry.Name() != "portable-host" {
				return fmt.Errorf("destination is not empty: %s", destination)
			}
		}
		return nil
	}
	if len(entries) != 0 {
		return fmt.Errorf("destination is not empty: %s", destination)
	}
	return nil
}

// recover is called under the host mutation lock and lifecycle ownership.
// Once verified, activation is retried forward, never back to a stale disk.
func (s moveStore) recover(activate func(*installationMove) error) error {
	state, err := s.load()
	if err != nil {
		return err
	}
	m := state.Pending
	if m == nil {
		return nil
	}
	if err := validateMovePath(m.Stage); err != nil {
		return err
	}
	if err := validateMovePath(m.Destination); err != nil {
		return err
	}
	if m.Phase == "copying" {
		if err := removeMoveInventory(m.Stage, m.Files, false); err != nil {
			return fmt.Errorf("removing interrupted move staging: %w", err)
		}
		state.Pending = nil
		return s.save(state)
	}
	if _, err := os.Stat(m.Stage); err == nil {
		if err := s.checkDestination(m.Destination); err != nil {
			return err
		}
		if pathsEqual(m.Destination, s.defaultDir) {
			host := filepath.Join(s.defaultDir, "portable-host")
			if _, err := os.Lstat(host); err == nil {
				if err := validateMovePath(host); err != nil {
					return err
				}
				if err := publishMoveDirectory(host, filepath.Join(m.Stage, "portable-host")); err != nil {
					return err
				}
			} else if !os.IsNotExist(err) {
				return err
			}
			if err := os.Remove(dataLocationPointerPath(s.defaultDir)); err != nil && !os.IsNotExist(err) {
				return err
			}
		}
		if err := os.Remove(m.Destination); err != nil && !os.IsNotExist(err) {
			return err
		}
		if err := publishMoveDirectory(m.Stage, m.Destination); err != nil {
			return err
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	// A crash may have happened after rename. Verify identity and contents
	// before redirecting anything; no guest can have started before activation.
	if m.Phase != "activating" {
		if err := verifyMoveInventory(m.Destination, m.Files); err != nil {
			return err
		}
		m.Phase = "activating"
		if err := s.save(state); err != nil {
			return err
		}
	}
	if err := activate(m); err != nil {
		return err
	}
	// The private staging receipt does not authorize removal of the active copy.
	if err := os.Remove(filepath.Join(m.Destination, uninstallStateName)); err != nil && !os.IsNotExist(err) {
		return err
	}
	if m.Default {
		if pathsEqual(m.Destination, s.defaultDir) {
			if err := os.Remove(dataLocationPointerPath(s.defaultDir)); err != nil && !os.IsNotExist(err) {
				return err
			}
		} else if err := saveDataLocationPointer(s.defaultDir, m.Destination); err != nil {
			return err
		}
	}
	for source, target := range state.Redirects {
		if pathsEqual(target, m.Source) {
			state.Redirects[source] = m.Destination
		}
		if pathsEqual(source, m.Destination) {
			delete(state.Redirects, source)
		}
	}
	state.Redirects[m.Source] = m.Destination
	delete(state.Locations, m.Source)
	if m.Volume != nil {
		if state.Locations == nil {
			state.Locations = map[string]moveVolumeLocation{}
		}
		state.Locations[m.Destination] = *m.Volume
	}
	m.Phase = "active"
	state.Pending = nil
	state.Retained = m
	return s.save(state)
}

func verifyMoveInventory(root string, files []moveFile) error {
	return verifyMoveFiles(root, files, false)
}

func verifyMoveFiles(root string, files []moveFile, allowMissing bool, lockedDisk ...*os.File) error {
	for _, entry := range files {
		path := filepath.Join(root, entry.Name)
		if err := validateMovePath(path); err != nil {
			return err
		}
		info, err := os.Lstat(path)
		if allowMissing && os.IsNotExist(err) {
			continue
		}
		if err != nil {
			return err
		}
		if entry.Directory {
			if !info.IsDir() {
				return fmt.Errorf("expected directory: %s", path)
			}
			continue
		}
		if !info.Mode().IsRegular() || info.Size() != entry.Size {
			return fmt.Errorf("moved file changed: %s", path)
		}
		var f *os.File
		closeFile := true
		if len(lockedDisk) > 0 && lockedDisk[0] != nil && entry.Name == filepath.Join("vm", "disk.raw") {
			f = lockedDisk[0]
			closeFile = false
			_, err = f.Seek(0, io.SeekStart)
		} else {
			f, err = os.Open(path)
		}
		if err != nil {
			return err
		}
		h := sha256.New()
		_, err = io.Copy(h, f)
		if closeFile {
			f.Close()
		}
		if err != nil {
			return err
		}
		if hex.EncodeToString(h.Sum(nil)) != entry.SHA256 {
			return fmt.Errorf("moved file failed verification: %s", path)
		}
	}
	return nil
}

// Delete only inventoried paths, and never follow links. Unexpected files keep
// their directory nonempty and stop cleanup rather than being recursively lost.
func removeMoveInventory(root string, files []moveFile, verify bool, lockedDisk ...*os.File) error {
	if err := validateMovePath(root); err != nil {
		return err
	}
	if _, err := os.Lstat(root); os.IsNotExist(err) {
		return nil
	}
	if err := validateUninstallDirectory(root); err != nil {
		return err
	}
	if verify {
		if err := verifyMoveFiles(root, files, true, lockedDisk...); err != nil {
			return err
		}
		if err := saveUninstallState(root); err != nil {
			return err
		}
	}
	for _, entry := range files {
		if !safeMoveName(entry.Name) {
			return fmt.Errorf("invalid move cleanup path")
		}
	}
	for i := len(files) - 1; i >= 0; i-- {
		path := filepath.Join(root, files[i].Name)
		if err := validateMovePath(path); err != nil {
			return err
		}
		if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
			return err
		}
		if files[i].Name == filepath.Join("vm", "disk.raw") && len(lockedDisk) > 0 && lockedDisk[0] != nil {
			// Windows completes DeleteFile only after the last handle closes.
			// Release after deletion succeeds, before removing the vm directory.
			if err := lockedDisk[0].Close(); err != nil {
				return err
			}
		}
	}
	if validUninstallState(root) {
		if err := validateUninstallDirectory(root); err != nil {
			return err
		}
		if err := os.Remove(filepath.Join(root, uninstallStateName)); err != nil {
			return err
		}
	}
	if err := os.Remove(root); err != nil && !os.IsNotExist(err) {
		entries, readErr := os.ReadDir(root)
		if readErr != nil {
			return err
		}
		for _, entry := range entries {
			if !moveExcluded(entry.Name()) {
				if receiptErr := saveUninstallState(root); receiptErr != nil {
					return receiptErr
				}
				return err
			}
		}
	}
	return nil
}

func (s moveStore) checkUninstall(dir string) (moveState, error) {
	state, err := s.load()
	if err != nil {
		return state, err
	}
	if state.Pending != nil {
		return state, uiError(uiText("error.move.finish_before_uninstall"), nil)
	}
	if state.Retained != nil && pathsEqual(state.Retained.Destination, dir) {
		return state, uiError(uiText("error.move.cleanup_before_uninstall"), nil)
	}
	return state, nil
}

func (s moveStore) forgetInstallation(dir string) error {
	state, err := s.checkUninstall(dir)
	if err != nil {
		return err
	}
	for source, target := range state.Redirects {
		if pathsEqual(target, dir) {
			delete(state.Redirects, source)
		}
	}
	delete(state.Locations, dir)
	return s.save(state)
}

func (s moveStore) pruneEmpty() error {
	if err := validateMovePath(s.dir); err != nil {
		return err
	}
	guard, err := lockMoveStore(s)
	if err != nil {
		return err
	}
	defer guard.Close()
	state, err := s.load()
	if err != nil {
		return err
	}
	if len(state.Redirects) != 0 || len(state.Locations) != 0 || state.Pending != nil || state.Retained != nil {
		return nil
	}
	if err := validateMovePath(s.dir); err != nil {
		return err
	}
	if err := os.Remove(filepath.Join(s.dir, moveStateName)); err != nil && !os.IsNotExist(err) {
		return err
	}
	// Never remove arbitrary files or another operation's live lock.
	if err := retireMoveLock(guard); err != nil {
		return err
	}
	if err := guard.Close(); err != nil {
		return err
	}
	if entries, err := os.ReadDir(s.dir); err != nil {
		return err
	} else if len(entries) != 0 {
		return nil
	}
	return os.Remove(s.dir)
}

func (s moveStore) markBooted(dir string) error {
	state, err := s.load()
	if err != nil {
		return err
	}
	if state.Retained == nil || !pathsEqual(state.Retained.Destination, dir) || state.Retained.Booted {
		return nil
	}
	state.Retained.Booted = true
	return s.save(state)
}

func (s moveStore) cleanup(dir string) error {
	state, err := s.load()
	if err != nil {
		return err
	}
	m := state.Retained
	if m == nil || !pathsEqual(m.Destination, dir) || !m.Booted {
		return uiError(uiText("error.move.start_first"), nil)
	}
	if state.Pending != nil {
		return errors.New("finish the pending move first")
	}
	disk, err := openMoveCleanupDisk(filepath.Join(m.Source, "vm", "disk.raw"))
	if err != nil && !(m.Phase == "cleaning" && os.IsNotExist(err)) {
		return uiError(uiTextWith("error.move.close_original", map[string]string{"error": err.Error()}), err)
	}
	if disk != nil {
		defer disk.Close()
	}
	if m.Phase != "cleaning" {
		if err := verifyMoveFiles(m.Source, m.Files, false, disk); err != nil {
			return err
		}
		m.Phase = "cleaning"
		if err := s.save(state); err != nil {
			return err
		}
	}
	if err := removeMoveInventory(m.Source, m.Files, true, disk); err != nil {
		return err
	}
	state.Retained = nil
	return s.save(state)
}
