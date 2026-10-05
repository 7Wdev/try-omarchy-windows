//go:build windows

package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

func hostMoveStore() moveStore {
	return moveStore{dir: filepath.Join(os.Getenv("LOCALAPPDATA"), defaultDataDirectoryName+"-host"), defaultDir: defaultDataDirectory()}
}

func lockMoveStore(s moveStore) (*os.File, error) {
	if err := validateMovePath(s.dir); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(s.dir, 0700); err != nil {
		return nil, err
	}
	path := filepath.Join(s.dir, "mutation.lock")
	ptr, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	const deleteAccess = 0x00010000
	h, err := syscall.CreateFile(ptr, syscall.GENERIC_READ|syscall.GENERIC_WRITE|deleteAccess, 0, nil, syscall.OPEN_ALWAYS, syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		return nil, fmt.Errorf("another settings or move operation is in progress; try again when it finishes: %w", err)
	}
	return os.NewFile(uintptr(h), path), nil
}

func rejectAncestorLink(path string, info os.FileInfo) error {
	ptr, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return err
	}
	attributes, err := syscall.GetFileAttributes(ptr)
	if err != nil {
		return err
	}
	if info.Mode()&os.ModeSymlink != 0 || attributes&fileAttributeReparsePoint != 0 {
		return fmt.Errorf("linked paths cannot be moved: %s", path)
	}
	return nil
}

func rejectMoveLink(path string, info os.FileInfo) error {
	if err := rejectAncestorLink(path, info); err != nil {
		return err
	}
	return rejectMoveStreams(path)
}

func publishMoveFile(from, to string) error      { return publishWindowsMove(from, to) }
func publishMoveDirectory(from, to string) error { return publishWindowsMove(from, to) }

// Scanners can briefly deny a rename after a verified file has been closed.
// Retry only Windows sharing/locking/access errors, with a fixed upper bound.
// Publication also serves rollback recovery: it must remain usable after setup
// cancellation so the journal can restore a bootable installation.
func publishWindowsMove(from, to string) error {
	return publishWindowsMoveWith(from, to, replaceLauncher, time.Sleep)
}

func publishWindowsMoveWith(from, to string, rename func(string, string) error, pause func(time.Duration)) error {
	var err error
	for attempt := 0; attempt < 15; attempt++ {
		err = rename(from, to)
		if err == nil {
			return nil
		}
		if !errors.Is(err, syscall.Errno(5)) && !errors.Is(err, syscall.Errno(32)) && !errors.Is(err, syscall.Errno(33)) {
			break
		}
		if attempt < 14 {
			pause(200 * time.Millisecond)
		}
	}
	return fmt.Errorf("publishing %s as %s: %w", from, to, err)
}

// Resolve before first-run selection or any maintenance action, including
// explicit -dir and update helpers. Settings can read but cannot recover a move
// because its process deliberately does not own the lifecycle listener.
func prepareMovedLocation(dir string, recover bool, removing ...bool) (string, error) {
	s := hostMoveStore()
	lock, err := lockMoveStore(s)
	if err != nil {
		return "", err
	}
	defer lock.Close()
	state, err := s.load()
	if err != nil {
		return "", err
	}
	if state.Pending != nil {
		if !recover {
			return "", uiError(uiText("error.move.needs_recovery"), nil)
		}
		if err := s.relocatePending(state); err != nil {
			return "", err
		}
		if err := s.recover(activateMovedInstallation); err != nil {
			return "", err
		}
		state, err = s.load()
		if err != nil {
			return "", err
		}
	}
	state, err = s.rememberVolume(state, dir, removing...)
	if err != nil {
		return "", err
	}
	state, err = s.relocate(state, dir, activateMovedInstallation, removing...)
	if err != nil {
		return "", err
	}
	return resolveMovedDirectory(state, dir, removing...)
}

// Called while holding the host mutation lock. Saves from older Settings
// windows must not recreate files at a source path after its move commits.
func checkMovedSettings(dir string) error {
	if validUninstallState(dir) {
		return uiError(uiText("error.uninstall.incomplete"), nil)
	}
	state, err := hostMoveStore().load()
	if err != nil {
		return err
	}
	if state.Pending != nil {
		return uiError(uiText("error.move.finish_before_save"), nil)
	}
	resolved, err := resolveMovedDirectory(state, dir)
	if err != nil {
		return err
	}
	if !pathsEqual(resolved, dir) {
		return uiError(uiTextWith("error.move.reopen_settings", map[string]string{"path": resolved}), nil)
	}
	return nil
}

func activateMovedInstallation(m *installationMove) error {
	target := filepath.Join(m.Destination, stableLauncherName)
	if _, err := os.Stat(target); err != nil {
		return err
	}
	// Update only links owned by this install. Repeating this step after a
	// failure accepts links already pointing to the destination.
	paths, err := launcherShortcutPaths()
	if err != nil {
		return err
	}
	paths = append(paths, filepath.Join(m.Destination, "Start Omarchy.lnk"), filepath.Join(m.Destination, "Settings.lnk"))
	prefs, err := loadLaunchPreferences(m.Destination)
	if err != nil {
		return err
	}
	if err := changeOwnedShortcuts(paths, []string{filepath.Join(m.Source, stableLauncherName), target}, func(path, args string) error {
		newArgs := launchShortcutArguments(m.Destination, prefs.StartAutomatically)
		for _, arg := range strings.Fields(args) {
			if arg == "-settings" {
				newArgs = settingsShortcutArguments(m.Destination)
				break
			}
		}
		return writeShellLink(path, target, newArgs, m.Destination)
	}); err != nil {
		return err
	}
	if err := syncSignInShortcut(target, m.Destination, prefs.LaunchAtSignIn); err != nil {
		return err
	}
	if err := syncSignInShortcut(filepath.Join(m.Source, stableLauncherName), m.Source, false); err != nil {
		return err
	}
	if err := registerUninstallEntry(target, m.Destination); err != nil {
		return err
	}
	return unregisterUninstallEntry(m.Source)
}

func runMoveUI(dir string, cleanup bool) error {
	s := hostMoveStore()
	lock, err := lockMoveStore(s)
	if err != nil {
		return err
	}
	defer lock.Close()
	if err := s.recover(activateMovedInstallation); err != nil {
		return err
	}
	state, err := s.load()
	if err != nil {
		return err
	}
	if cleanup {
		m := state.Retained
		if m == nil || !pathsEqual(m.Destination, dir) || !m.Booted {
			return uiError(uiText("error.move.start_first"), nil)
		}
		self, err := os.Executable()
		if err != nil {
			return err
		}
		if pathsOverlap(m.Source, self) {
			cmd := exec.Command(filepath.Join(dir, stableLauncherName), "-dir", dir, "-recovery", "move-cleanup", "-update-wait-pid", strconv.Itoa(os.Getpid()))
			if err := cmd.Start(); err != nil {
				return fmt.Errorf("starting cleanup from the new location: %w", err)
			}
			return nil
		}
		// An orphaned QEMU must not be using either disk while cleanup runs.
		disk, err := openBackupDisk(filepath.Join(dir, "vm", "disk.raw"))
		if err != nil {
			return uiError(uiTextWith("error.move.close_cleanup", map[string]string{"error": err.Error()}), err)
		}
		defer disk.Close()
		if msgBox(uiTextWith("move.cleanup.confirm", map[string]string{"original": m.Source, "moved": m.Destination}), mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
			return nil
		}
		beginRecoveryProgress(uiText("move.cleanup.status"))
		defer uiDone()
		getUI().finishOnly.Store(true)
		if err := s.cleanup(dir); err != nil {
			return err
		}
		infoBox(uiText("move.cleanup.done"))
		return nil
	}
	parent, ok := browseForFolder(0, uiText("move.choose"))
	if !ok {
		return nil
	}
	destination, err := dataDirectoryForSelection(parent)
	if err != nil {
		return err
	}
	if err := validateStandardDataDrive(destination); err != nil {
		return err
	}
	if msgBox(uiTextWith("move.confirm", map[string]string{"from": dir, "to": destination}), mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
		return nil
	}
	// The retained stable launcher must understand move redirects too.
	if _, err := stableLauncherPath(dir); err != nil {
		return err
	}
	beginRecoveryProgress(uiText("move.status.checking"))
	defer uiDone()
	m, err := s.prepare(dir, destination, recoveryProgress(recoveryMoving))
	if err != nil {
		// Copy cancellation can safely discard staging immediately. A verified
		// move is instead recovered forward on the next normal launch.
		if current, e := s.load(); e == nil && current.Pending != nil && current.Pending.Phase == "copying" {
			if e = s.recover(activateMovedInstallation); e != nil {
				logf("move staging recovery: %v", e)
			}
		}
		return err
	}
	getUI().finishOnly.Store(true)
	getUI().setStatus("%s", uiText("move.status.finishing"))
	if err := s.recover(activateMovedInstallation); err != nil {
		return fmt.Errorf("the verified copy is safe; open Try Omarchy again to finish switching locations: %w", err)
	}
	uiDone()
	infoBox(uiTextWith("move.done", map[string]string{"moved": m.Destination, "original": m.Source}))
	return nil
}

func markMovedGuestReady(dir string) bool {
	s := hostMoveStore()
	lock, err := lockMoveStore(s)
	if err != nil {
		logf("recording moved guest readiness: %v", err)
		return false
	}
	defer lock.Close()
	if err := s.markBooted(dir); err != nil {
		logf("recording moved guest readiness: %v", err)
		return false
	}
	return true
}

// FILE_SHARE_DELETE lets cleanup remove the stopped original while this handle
// still excludes readers/writers such as an orphaned QEMU process.
func openMoveCleanupDisk(path string) (*os.File, error) {
	ptr, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	h, err := syscall.CreateFile(ptr, syscall.GENERIC_READ, syscall.FILE_SHARE_DELETE, nil, syscall.OPEN_EXISTING, syscall.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		return nil, err
	}
	return os.NewFile(uintptr(h), path), nil
}

func rejectMoveStreams(path string) error {
	ptr, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return err
	}
	var data struct {
		Size int64
		Name [296]uint16
	}
	first := kernel32.NewProc("FindFirstStreamW")
	next := kernel32.NewProc("FindNextStreamW")
	h, _, callErr := first.Call(uintptr(unsafe.Pointer(ptr)), 0, uintptr(unsafe.Pointer(&data)), 0)
	if h == ^uintptr(0) {
		if callErr == syscall.Errno(38) {
			return nil
		}
		return fmt.Errorf("checking file streams for %s: %v", path, callErr)
	}
	defer syscall.FindClose(syscall.Handle(h))
	for {
		if name := syscall.UTF16ToString(data.Name[:]); name != "::$DATA" {
			return uiError(uiTextWith("error.move.data_stream", map[string]string{"path": path}), nil)
		}
		r, _, e := next.Call(h, uintptr(unsafe.Pointer(&data)))
		if r == 0 {
			if e == syscall.Errno(38) {
				return nil
			}
			return fmt.Errorf("checking file streams: %v", e)
		}
	}
}

func checkMovedUninstall(dir string) error {
	_, err := hostMoveStore().checkUninstall(dir)
	return err
}

func forgetMovedInstallation(dir string) error {
	s := hostMoveStore()
	guard, err := lockMoveStore(s)
	if err != nil {
		return err
	}
	defer guard.Close()
	if err := s.forgetInstallation(dir); err != nil {
		return err
	}
	if err := guard.Close(); err != nil {
		return err
	}
	return s.pruneEmpty()
}
