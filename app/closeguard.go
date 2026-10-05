//go:build windows

package main

import (
	"context"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// Closing the VM window must not hard-kill a running OS (it did: SDL's default
// close quits QEMU instantly - unsaved work in the guest, gone, no questions).
// QEMU now launches with window-close=off so the X does nothing by itself; a
// low-level mouse hook spots clicks on the caption's close button (the window
// itself reports what's under the cursor via WM_NCHITTEST) and the keyboard
// hook catches Alt+F4. Both funnel into one confirmation; Yes performs a
// GRACEFUL guest shutdown over QMP - autologin makes the next start seamless.

var (
	qemuHwnd              atomic.Uintptr // current VM window, set by the title enforcer
	confirmQuit           = make(chan struct{}, 1)
	confirmOpen           atomic.Bool
	procWindowFromPoint   = user32.NewProc("WindowFromPoint")
	windowsSessionEnding  atomic.Bool
	guestExitedCleanly    atomic.Bool
	guestLaunchInProgress atomic.Bool
)

// WM_QUERYENDSESSION only registers the reason. Powering off is deferred until
// WM_ENDSESSION confirms that sign-out/restart was not canceled by another app.
func queryEndSession(hwnd uintptr) {
	reason, _ := syscall.UTF16PtrFromString(uiText("shutdown.windows_reason"))
	if ok, _, err := user32.NewProc("ShutdownBlockReasonCreate").Call(hwnd, uintptr(unsafe.Pointer(reason))); ok == 0 {
		logf("session: could not register shutdown reason: %v", err)
	}
}

func finishEndSession(hwnd uintptr, ending bool) {
	defer user32.NewProc("ShutdownBlockReasonDestroy").Call(hwnd)
	if !ending || windowsSessionEnding.Swap(true) {
		return
	}
	intentionalUpdateQuit.Store(true)
	cancelBackgroundUpdate()
	ctx, cancel := context.WithTimeout(context.Background(), endSessionBudget)
	defer cancel()
	launchPending := guestLaunchInProgress.Load()
	noGuest := qemuPid.Load() == 0 && !launchPending
	clean := endGuestSession(ctx, func(ctx context.Context) error {
		for guestLaunchInProgress.Load() {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(10 * time.Millisecond):
			}
		}
		if qemuPid.Load() == 0 {
			if launchPending {
				noGuest = true
			}
			return nil
		}
		c, err := dialQMPControl(ctx, qmpToolsPort)
		if err != nil {
			return err
		}
		defer c.Close()
		var state vmRuntimeStatus
		if err = c.Call(ctx, "query-status", nil, &state); err != nil {
			return err
		}
		// A paused CPU cannot process the ACPI power button.
		if state.Status == "paused" {
			if err = c.Call(ctx, "cont", nil, nil); err != nil {
				return err
			}
		}
		return c.Call(ctx, "system_powerdown", nil, nil)
	}, func() bool { return noGuest || guestExitedCleanly.Load() })
	logf("session: Windows end-session shutdown finished: clean=%t", clean)
}

const (
	whMouseLL       = 14
	wmLbuttondown   = 0x0201
	htCloseBtn      = 20
	vkF4            = 0x73
	mbDefbutton2    = 0x100
	mbSetForeground = 0x10000
	mbTopmost       = 0x40000
	// mbYesNo, mbIconQuestion, idYes: setup.go
)

// mouseHookCallback swallows left-clicks on the VM window's close button and
// asks for confirmation instead. Runs on the shared hook thread.
func mouseHookCallback(nCode, wParam, lParam uintptr) uintptr {
	if int32(nCode) >= 0 && wParam == wmLbuttondown {
		pt := *(*[2]int32)(unsafe.Pointer(lParam))
		packed := uintptr(uint64(uint32(pt[1]))<<32 | uint64(uint32(pt[0])))
		hwnd, _, _ := procWindowFromPoint.Call(packed)
		if isQemuDisplayWindow(hwnd, qemuPid.Load()) {
			lp := uintptr(uint32(pt[0])&0xffff | uint32(pt[1])<<16)
			if hit, _, _ := procSendMessageW.Call(hwnd, wmNchittest, 0, lp); hit == htCloseBtn {
				qemuHwnd.Store(hwnd)
				requestQuitConfirm()
				return 1
			}
		}
	}
	r, _, _ := procCallNextHookEx.Call(0, nCode, wParam, lParam)
	return r
}

func requestQuitConfirm() {
	select {
	case confirmQuit <- struct{}{}:
	default:
	}
}

// runCloseGuard owns the confirmation dialog and the graceful shutdown.
func runCloseGuard() {
	text, _ := syscall.UTF16PtrFromString(uiText("shutdown.confirm"))
	caption, _ := syscall.UTF16PtrFromString(appTitle)
	for range confirmQuit {
		if confirmOpen.Swap(true) {
			continue // dialog already up
		}
		// Owned by the VM window + SETFOREGROUND, or the dialog opens BEHIND
		// the (foreground, topmost-ish) SDL window it is asking about.
		r, _, _ := procMessageBoxW.Call(qemuHwnd.Load(), uintptr(unsafe.Pointer(text)), uintptr(unsafe.Pointer(caption)),
			mbYesNo|mbIconQuestion|mbDefbutton2|mbTopmost|mbSetForeground)
		if r == idYes {
			logf("close confirmed - graceful guest shutdown")
			ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
			c, err := dialQMPControl(ctx, qmpToolsPort)
			if err == nil {
				err = c.Call(ctx, "system_powerdown", nil, nil)
				if err == nil {
					intentionalUpdateQuit.Store(true)
					cancelBackgroundUpdate()
					guestCompositorHealth.stop()
				}
				c.Close()
			}
			cancel()
			if err != nil {
				errorBox(uiTextWith("shutdown.unacknowledged", map[string]string{"error": err.Error()}))
			}

			// The guest shuts down; the supervisor reaps/exits as usual.
		}
		confirmOpen.Store(false)
	}
}
