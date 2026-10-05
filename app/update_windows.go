//go:build windows

package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

var procOpenProcess = kernel32.NewProc("OpenProcess")

const synchronizeProcess = 0x00100000

var (
	updateAvailable       atomic.Bool
	restartForUpdate      atomic.Bool
	intentionalUpdateQuit atomic.Bool
	backgroundUpdates     struct {
		sync.Mutex
		start  func()
		cancel context.CancelFunc
	}
)

// Installed users do no network work until the guest's readiness message has
// committed any prior transaction. Only one background job runs per launch.
func configureBackgroundUpdates(cfg *config, feed string, enabled bool) func() {
	ctx, cancel := context.WithCancel(setupContext())
	snapshot := *cfg
	var once sync.Once
	start := func() {
		if !enabled {
			return
		}
		once.Do(func() {
			go func() {
				runtime.LockOSThread()
				defer runtime.UnlockOSThread()
				thread, _, _ := kernel32.NewProc("GetCurrentThread").Call()
				priority := kernel32.NewProc("SetThreadPriority")
				if ok, _, _ := priority.Call(thread, 0x10000); ok != 0 {
					defer priority.Call(thread, 0x20000)
				}
				if feed == defaultUpdateURL && !updateCheckDue(snapshot.dir, time.Now()) {
					return
				}
				// Settings may have disabled automatic updates since this launch started.
				prefs, err := loadDesktopPreferences(snapshot.dir)
				if err != nil || prefs.AutomaticUpdatesDisabled || ctx.Err() != nil {
					return
				}
				_ = recordUpdateCheck(snapshot.dir, time.Now())
				key, err := updatePublicKey()
				if err != nil {
					logf("update check skipped: %v", err)
					return
				}
				_, digest, _ := installReceiptIdentity(snapshot.guestDir)
				client := newDownloadClient()
				client.Transport = backgroundUpdateTransport{ctx: ctx, base: client.Transport}
				defer client.CloseIdleConnections()
				if snapshot.portable {
					logf("%s", uiText("status.preparing_portable_update"))
				}
				manifest, err := stageSignedUpdate(ctx, client, feed, snapshot.dir,
					updatePayloadRoot(snapshot.dir, snapshot.payloadDir, snapshot.portable), currentVersion, digest, key)
				if err != nil {
					if ctx.Err() == nil {
						logf("background update skipped: %v", err)
						if errors.Is(err, errInsufficientDiskSpace) {
							showTrayNotice(uiText("update.notice.title"), uiTextWith("update.notice.space", map[string]string{"error": err.Error()}))
						}
					}
					return
				}
				if manifest != nil {
					updateAvailable.Store(true)
					showTrayNotice(uiText("update.notice.title"), uiTextWith("update.notice.ready", map[string]string{"version": manifest.Version}))
					logf("update %s verified and staged for the next start", manifest.Version)
				}
			}()
		})
	}
	backgroundUpdates.Lock()
	backgroundUpdates.start, backgroundUpdates.cancel = start, cancel
	backgroundUpdates.Unlock()
	return cancel
}

func startReadyUpdateCheck() {
	backgroundUpdates.Lock()
	start := backgroundUpdates.start
	backgroundUpdates.Unlock()
	if start != nil {
		start()
	}
}

func cancelBackgroundUpdate() {
	backgroundUpdates.Lock()
	cancel := backgroundUpdates.cancel
	backgroundUpdates.Unlock()
	if cancel != nil {
		cancel()
	}
}

func startStagedLauncherUpdate(cfg *config, manifest *updateManifest, restartArgs []string) (bool, error) {
	getUI().setStatus("%s", uiTextWith("status.updating_launcher", map[string]string{"version": manifest.Version}))
	encodedArgs, err := encodeRestartArgs(restartArgs)
	if err != nil {
		return false, err
	}
	state := &launcherUpdateState{Schema: updateStateVersion, Version: manifest.Version,
		SHA256: manifest.Launcher.SHA256, Portable: cfg.portable, ManifestSHA256: manifest.ManifestSHA256}
	if err := writeLauncherUpdateState(cfg.dir, state); err != nil {
		return false, err
	}
	cmd := exec.Command(stagedLauncherPath(cfg.dir, manifest.Version), "-dir", cfg.dir,
		"-apply-launcher-update", "-update-wait-pid", strconv.Itoa(os.Getpid()), "-update-restart-args", encodedArgs)
	if err := cmd.Start(); err != nil {
		_ = clearLauncherUpdateMarker(cfg.dir)
		return false, err
	}
	_ = cmd.Process.Release()
	return true, nil
}

func applyLauncherUpdate(dir string, waitPID int, encodedArgs string, rollback bool) error {
	if waitPID <= 0 {
		return fmt.Errorf("invalid update parent process")
	}
	args, err := decodeRestartArgs(encodedArgs)
	if err != nil {
		return fmt.Errorf("decode restart arguments: %w", err)
	}
	waitForProcess(waitPID)
	self, err := os.Executable()
	if err != nil {
		return err
	}
	state, err := readLauncherUpdateState(dir)
	if err != nil {
		return err
	}
	if state == nil {
		return fmt.Errorf("pending update state is missing")
	}
	portable := state.Portable
	target, err := launcherUpdateTarget(dir, portable)
	if err != nil {
		return err
	}
	if rollback {
		if err := copyLauncher(self, target, replaceLauncher); err != nil {
			return fmt.Errorf("restore previous launcher: %w", err)
		}
		// This process is running from the previous-launcher backup, so Windows
		// will not let us delete that file until it exits. Removing the marker is
		// enough to make the restored launcher authoritative; a later successful
		// update cleans the old backup.
		if err := clearLauncherUpdateMarker(dir); err != nil {
			return err
		}
	} else {
		state, err := readLauncherUpdateState(dir)
		if err != nil {
			return fmt.Errorf("read pending update state: %w", err)
		}
		if state == nil {
			return fmt.Errorf("pending update state is missing")
		}
		if ok, err := verifyFileSHA256(self, state.SHA256, nil); err != nil || !ok {
			return fmt.Errorf("staged launcher authentication failed")
		}
		if _, err := os.Stat(target); err == nil {
			if err := copyLauncher(target, previousLauncherPath(dir), replaceLauncher); err != nil {
				return fmt.Errorf("back up launcher: %w", err)
			}
			state.HasPrevious = true
		}
		// Commit the rollback marker before replacing the working launcher. A
		// power loss after the replacement must still leave enough information
		// for the new launcher to restore the previous signed executable.
		if err := writeLauncherUpdateState(dir, state); err != nil {
			return err
		}
		if err := copyLauncher(self, target, replaceLauncher); err != nil {
			if state.HasPrevious {
				_ = copyLauncher(previousLauncherPath(dir), target, replaceLauncher)
			}
			_ = clearLauncherUpdateMarker(dir)
			return err
		}
	}
	cmd := exec.Command(target, args...)
	if err := cmd.Start(); err != nil {
		if !rollback {
			_ = copyLauncher(previousLauncherPath(dir), target, replaceLauncher)
		}
		return fmt.Errorf("restart updated launcher: %w", err)
	}
	return nil
}

func recoverLauncherUpdate(dir, encodedArgs string) (bool, error) {
	state, err := readLauncherUpdateState(dir)
	if err != nil {
		// A damaged local state file must not brick an otherwise working app.
		_ = clearLauncherUpdateState(dir)
		return false, err
	}
	if state == nil || state.Version != currentVersion {
		return false, nil
	}
	if !state.Started || state.Interrupted {
		state.Started = true
		state.Interrupted = false
		return false, writeLauncherUpdateState(dir, state)
	}
	if !state.HasPrevious {
		_ = clearLauncherUpdateState(dir)
		return false, nil
	}
	recordFailedUpdate(dir, state.Version)
	_ = removeUpdateFile(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename))
	previous := previousLauncherPath(dir)
	cmd := exec.Command(previous,
		"-dir", dir,
		"-apply-launcher-rollback",
		"-update-wait-pid", strconv.Itoa(os.Getpid()),
		"-update-restart-args", encodedArgs,
	)
	if err := cmd.Start(); err != nil {
		return false, err
	}
	return true, nil
}

func commitLauncherUpdate(dir string) {
	state, err := readLauncherUpdateState(dir)
	if err != nil || state == nil || state.Version != currentVersion {
		return
	}
	if err := clearLauncherUpdateMarker(dir); err != nil {
		logf("clearing successful launcher update: %v", err)
		return
	}
	_ = removeUpdateFile(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename))
	_ = os.RemoveAll(filepath.Join(launcherUpdateDir(dir), currentVersion))
	logf("launcher update %s confirmed after healthy boot", currentVersion)
}

func waitForProcess(pid int) {
	handle, _, _ := procOpenProcess.Call(synchronizeProcess, 0, uintptr(uint32(pid)))
	if handle == 0 {
		time.Sleep(2 * time.Second)
		return
	}
	defer procCloseHandle.Call(handle)
	procWaitForSingleObject.Call(handle, uintptr(0xFFFFFFFF))
}

func pruneCommittedUpdatePayloads(cfg *config) {
	if state, err := readPayloadUpdateState(cfg.dir); err != nil || state != nil {
		return
	}
	_, active, ok := installReceiptIdentity(cfg.guestDir)
	if !ok {
		return
	}
	_, previous, _ := installReceiptIdentity(filepath.Join(cfg.dir, "guest.previous"))
	_, runtimeActive, _ := runtimeReceiptIdentity(filepath.Join(cfg.dir, "runtime"))
	_, runtimePrevious, _ := runtimeReceiptIdentity(filepath.Join(cfg.dir, "runtime.previous"))
	if err := pruneUpdatePayloads(updatePayloadRoot(cfg.dir, cfg.payloadDir, cfg.portable), active, previous, runtimeActive, runtimePrevious); err != nil {
		logf("pruning superseded update payloads: %v", err)
	}
	// Payload-only updates use the same ready marker but need no launcher helper.
	if data, err := os.ReadFile(filepath.Join(launcherUpdateDir(cfg.dir), stagedUpdateFilename)); err == nil && string(data) == currentVersion {
		_ = removeUpdateFile(filepath.Join(launcherUpdateDir(cfg.dir), stagedUpdateFilename))
	}
}
