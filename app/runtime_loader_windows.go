//go:build windows

package main

import (
	"context"
	"os/exec"
	"syscall"
	"time"
)

func runtimeLoaderCommand(ctx context.Context, executable string) *exec.Cmd {
	cmd := exec.CommandContext(ctx, executable, "--version")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	return cmd
}

func loadRuntime(ctx context.Context, executable string) error {
	runtimeLoaderPreparing.Store(true)
	getUI().setStatus("%s", uiText("status.preparing_runtime"))
	refreshRuntimeLoaderTray()
	defer func() {
		runtimeLoaderPreparing.Store(false)
		refreshRuntimeLoaderTray()
		if !setupCancelled() && !windowsSessionEnding.Load() {
			getUI().setStatus("%s", uiText("status.starting_omarchy"))
		}
	}()

	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	// Setup/tray quit cancels the parent context. Windows sign-out has its
	// own flag because it follows a separate graceful guest shutdown path.
	done := make(chan struct{})
	go func() {
		defer close(done)
		ticker := time.NewTicker(100 * time.Millisecond)
		defer ticker.Stop()
		for {
			if windowsSessionEnding.Load() {
				cancel()
				return
			}
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
			}
		}
	}()
	started := time.Now()
	logf("runtime loader preflight: %s --version (timeout %s)", executable, runtimeLoaderTimeout)
	err := runtimeLoaderCommand(ctx, executable).Run()
	cancel()
	<-done
	logf("runtime loader preflight finished after %s: %v", time.Since(started).Round(time.Millisecond), err)
	return err
}

func refreshRuntimeLoaderTray() {
	if hwnd := trayWindow.Load(); hwnd != 0 {
		procPostMessageW.Call(hwnd, trayPowerStateMessage, 0, 0)
	}
}
