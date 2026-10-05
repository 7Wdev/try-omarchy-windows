//go:build windows

package main

import (
	"context"
	"fmt"
	"sync"
	"time"
)

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

func newGuestPowerState() *guestPowerState {
	return &guestPowerState{identity: func() uint64 { return guestRuntimeGeneration.Load()<<32 | uint64(qemuPid.Load()) }, changed: func(phase string) {
		setHostPowerTransition(phase)
		if phase == "suspended" || phase == "resuming" {
			guestCompositorHealth.power(time.Now(), phase == "suspended")
		}
	}, notifyResume: func() {
		select {
		case hostResumed <- struct{}{}:
		default:
		}
	}, dial: func(ctx context.Context) (*qmpClient, error) {
		// Preserve the supervisor's early-boot QMP quiet period.
		if !guestUp.Load() || qemuPid.Load() == 0 {
			return nil, fmt.Errorf("guest controls are not ready")
		}
		return dialQMPControl(ctx, qmpPowerRole)
	}}
}
