//go:build windows

package main

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"
)

const pbtApmSuspend = 0x0004

var (
	procRegisterSuspendResumeNotification   = user32.NewProc("RegisterSuspendResumeNotification")
	procUnregisterSuspendResumeNotification = user32.NewProc("UnregisterSuspendResumeNotification")
)

// Register before entering the tray loop. Modern Standby's Desktop Activity
// Moderator sends these broadcasts only to desktop apps that opt in.
// https://learn.microsoft.com/en-us/windows/win32/w8cookbook/desktop-activity-moderator
func registerPowerNotifications(hwnd uintptr) (func(), error) {
	return subscribePowerNotifications(hwnd,
		func(hwnd uintptr) (uintptr, error) {
			handle, _, err := procRegisterSuspendResumeNotification.Call(hwnd, 0) // DEVICE_NOTIFY_WINDOW_HANDLE
			return handle, err
		},
		func(handle uintptr) error {
			ok, _, err := procUnregisterSuspendResumeNotification.Call(handle)
			if ok == 0 {
				return err
			}
			return nil
		})
}

func subscribePowerNotifications(hwnd uintptr, register func(uintptr) (uintptr, error), unregister func(uintptr) error) (func(), error) {
	handle, err := register(hwnd)
	if handle == 0 {
		return func() {}, fmt.Errorf("suspend/resume notification registration failed: %w", err)
	}
	logf("power: registered suspend/resume notifications")
	var once sync.Once
	return func() {
		once.Do(func() {
			if err := unregister(handle); err != nil {
				logf("power: suspend/resume notification cleanup failed: %v", err)
			}
		})
	}, nil
}

// Used only on the tray thread. Keep the same connection across sleep so a
// reboot or replacement runtime can never inherit an old resume obligation.
// Reading STOP/RESUME events also detects manual changes during our pause.
type guestPowerState struct {
	suspended bool
	client    *qmpClient
	owned     bool
	dial      func(context.Context) (*qmpClient, error)
}

func newGuestPowerState() *guestPowerState {
	return &guestPowerState{dial: func(ctx context.Context) (*qmpClient, error) {
		// Preserve the supervisor's early-boot QMP quiet period.
		if !guestUp.Load() || qemuPid.Load() == 0 {
			return nil, fmt.Errorf("guest controls are not ready")
		}
		return dialQMPControl(ctx, qmpPowerRole)
	}}
}

func (p *guestPowerState) close() {
	if p.client != nil {
		p.client.Close()
		p.client = nil
	}
	p.owned = false
}

func (p *guestPowerState) handle(event uintptr) {
	switch event {
	case pbtApmSuspend:
		if p.suspended {
			return
		}
		p.suspended = true
		p.pause()
	case pbtApmResumeAutomatic, pbtApmResumeSuspend:
		firstResume := p.suspended
		p.suspended = false
		p.resume()
		if firstResume {
			logf("windows resumed from sleep")
			select {
			case hostResumed <- struct{}{}:
			default:
			}
		}
	}
}

func (p *guestPowerState) pause() {
	// A rejected cont can leave an owned pause pending for a later resume.
	if p.client != nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	c, err := p.dial(ctx)
	if err != nil {
		logf("power: cannot pause for host sleep: %v", err)
		return
	}
	p.client = c
	var state vmRuntimeStatus
	if err = c.Call(ctx, "query-status", nil, &state); err != nil {
		logf("power: cannot inspect guest before sleep: %v", err)
		p.close()
		return
	}
	if !state.Running || state.Status != "running" {
		// A manual pause, incoming restore, shutdown or guest suspend is not ours.
		p.close()
		return
	}
	if err = c.Call(ctx, "stop", nil, nil); err != nil {
		logf("power: pause for host sleep failed: %v; guest may be paused, resume manually if needed", err)
		p.close()
		return
	}
	p.owned = true
	c.onEvent = func(event string) {
		if event == "STOP" || event == "RESUME" {
			p.owned = false
		}
	}
	if err = c.Call(ctx, "query-status", nil, &state); err != nil || state.Running || state.Status != "paused" || !p.owned {
		logf("power: guest pause could not be confirmed: state=%s error=%v; resume manually if needed", state.Status, err)
		p.close()
		return
	}
	logf("power: paused the guest for host sleep")
}

func (p *guestPowerState) resume() {
	if p.client == nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	var state vmRuntimeStatus
	if err := p.client.Call(ctx, "query-status", nil, &state); err != nil {
		// Reconnecting would lose intervening manual events and runtime identity.
		logf("power: cannot inspect guest after sleep: %v; resume manually if needed", err)
		p.close()
		return
	}
	if !p.owned || state.Running || state.Status != "paused" {
		p.close()
		return
	}
	if err := p.client.Call(ctx, "cont", nil, nil); err != nil {
		logf("power: resume after sleep failed: %v; resume manually if needed", err)
		var remote *qmpCommandError
		if !errors.As(err, &remote) {
			p.close()
		}
		return
	}
	if err := p.client.Call(ctx, "query-status", nil, &state); err != nil || !state.Running {
		logf("power: guest resume could not be confirmed: state=%s error=%v", state.Status, err)
	} else {
		logf("power: resumed the guest after host sleep")
	}
	p.close()
}
