//go:build windows

package main

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// The Windows-key forwarder, ported from scripts/winkey-forwarder.ps1.
// While the QEMU window is foreground: swallow Win on the host and forward it
// to the guest as Super (meta_l) over a dedicated QMP socket. Otherwise the key
// behaves normally. Pair with SDL_GRAB_KEYBOARD=0 so SDL never installs its own
// (system-wide) hook. Print Screen gets the same treatment: Windows opens its
// own screen capture on it system-wide, so Omarchy's screenshot binding would
// otherwise fire together with Snipping Tool. Ctrl+Alt+End stands in for
// Windows-reserved Ctrl+Alt+Delete while Omarchy is focused.

var (
	qemuPid        atomic.Uint32                // current QEMU child, set by the supervisor
	guestUp        atomic.Bool                  // supervisor handshake succeeded for this launch
	lwinKey        = routedKey{qcode: "meta_l"} // hook-thread only
	rwinKey        = routedKey{qcode: "meta_l"} // hook-thread only
	printKey       = routedKey{qcode: "print"}  // hook-thread only
	altTab         altTabForwarder              // hook-thread only
	ctrlAltEndSent bool                         // hook-thread only
	hookChaining   bool                         // hook-thread only
	keyEvents      = make(chan forwardedKey, 128)
	pointerScripts = make(chan dragScript, 1)
	// dragButtonHeld is set while a drag has pressed the tablet button and not
	// yet released it; a reconnect releases it first. QMP drain goroutine only.
	dragButtonHeld bool
)

const (
	// keyReleaseRoom is the part of keyEvents that only releases may use, so
	// a burst of presses can never crowd out the release of a held key.
	keyReleaseRoom = 32
	// keyReleaseWait bounds how long a release waits for room when even that
	// is full. Windows drops a hook that takes much longer (~300ms+).
	keyReleaseWait = 50 * time.Millisecond
	// rehookInterval paces the keyboard rehook in runWinKeyHook.
	rehookInterval = 800 * time.Millisecond
)

// sendKeys hands key changes to the QMP drain. The hook thread is the only
// sender. Presses never block it and are dropped when the queue is nearly
// full; a release is never dropped unless the drain stalls past
// keyReleaseWait, so a forwarded key cannot stay held in the guest.
func sendKeys(keys []forwardedKey) {
	for _, key := range keys {
		if key.down {
			if len(keyEvents) < cap(keyEvents)-keyReleaseRoom {
				select {
				case keyEvents <- key:
				default:
				}
			}
			continue
		}
		select {
		case keyEvents <- key:
			continue
		default:
		}
		wait := time.NewTimer(keyReleaseWait)
		select {
		case keyEvents <- key:
		case <-wait.C:
			logf("winkey: dropped a %s release; the QMP drain is stalled", key.qcode)
		}
		wait.Stop()
	}
}

func queueKeys(keys ...forwardedKey) bool {
	if len(keyEvents)+len(keys) > cap(keyEvents)-keyReleaseRoom {
		return false
	}
	for _, key := range keys {
		keyEvents <- key
	}
	return true
}

// routedKeyFor is the routing state of a key the hook sends to the side
// that saw it go down, or nil.
func routedKeyFor(vk uint32) *routedKey {
	switch vk {
	case vkLwin:
		return &lwinKey
	case vkRwin:
		return &rwinKey
	case vkSnapshot:
		return &printKey
	}
	return nil
}

func isAltVK(vk uint32) bool {
	return vk == vkMenu || vk == 0xA4 || vk == 0xA5
}

func isCtrlOrAltVK(vk uint32) bool {
	switch vk {
	case vkControl, vkMenu, 0xA2, 0xA3, 0xA4, 0xA5:
		return true
	default:
		return false
	}
}

func hookCallback(nCode, wParam, lParam uintptr) uintptr {
	// While runWinKeyHook swaps hooks, both are briefly installed and this
	// one's CallNextHookEx can reach the other. Route each event once.
	if int32(nCode) >= 0 && !hookChaining {
		vk := *(*uint32)(unsafe.Pointer(lParam))  // KBDLLHOOKSTRUCT.vkCode
		if vk == vkF4 && wParam == wmSyskeydown { // Alt+F4 on the VM window
			if pid := qemuPid.Load(); pid != 0 && foregroundPid() == pid {
				requestQuitConfirm()
				return 1 // swallow; the close guard takes it from here
			}
		}
		if key := routedKeyFor(vk); key != nil {
			down := wParam == wmKeydown || wParam == wmSyskeydown
			pid := qemuPid.Load()
			held, _, _ := procGetAsyncKeyState.Call(uintptr(vk)) // state before this change
			keys, swallow := key.route(pid != 0 && foregroundPid() == pid, held&0x8000 != 0, down)
			sendKeys(keys)
			if swallow {
				return 1 // QMP delivers it to the guest
			}
			if vk != vkSnapshot {
				// Windows owns this Win press: approve the key and SKIP the
				// rest of the hook chain. QEMU installs its own LL hook on
				// every grab which swallows Win even when unfocused;
				// returning 0 without CallNextHookEx bypasses it, so the
				// release still reaches Windows after the VM takes focus.
				return 0
			}
		}
		// Windows consumes Alt+Tab before SDL can deliver the chord to the
		// guest. Forward it while the VM has focus (see altTabForwarder),
		// unless Settings leaves Alt+Tab to Windows.
		if vk == vkTab {
			pid := qemuPid.Load()
			control, _, _ := procGetAsyncKeyState.Call(vkControl)
			// Ctrl+Alt+Tab is Windows' keyboard-only escape to its task
			// switcher. A plain Ctrl+Alt release would conflict with AltGr.
			forward := pid != 0 && foregroundPid() == pid &&
				(altTab.tabDown || !altTabToWindows.Load() && control&0x8000 == 0 && (wParam == wmSyskeydown || wParam == wmSyskeyup))
			keys, swallow := altTab.tab(forward, wParam == wmKeydown || wParam == wmSyskeydown)
			sendKeys(keys)
			if swallow {
				return 1
			}
		}
		// The physical Alt release still reaches Windows and SDL; it also
		// ends a forwarded Alt+Tab, wherever focus is by then.
		if isAltVK(vk) && (wParam == wmKeyup || wParam == wmSyskeyup) {
			sendKeys(altTab.altReleased())
		}
		if ctrlAltEndSent && (wParam == wmKeyup || wParam == wmSyskeyup) && isCtrlOrAltVK(vk) {
			ctrlAltEndSent = false
		}
		if vk == vkEnd {
			down := wParam == wmKeydown || wParam == wmSyskeydown
			pid := qemuPid.Load()
			focused := pid != 0 && foregroundPid() == pid
			control, _, _ := procGetAsyncKeyState.Call(vkControl)
			menu, _, _ := procGetAsyncKeyState.Call(vkMenu)
			switch classifyCtrlAltEnd(focused, control&0x8000 != 0, menu&0x8000 != 0, ctrlAltEndSent, down) {
			case ctrlAltEndSend:
				ctrlAltEndSent = queueKeys(
					forwardedKey{qcode: "ctrl", down: true},
					forwardedKey{qcode: "alt", down: true},
					forwardedKey{qcode: "delete", down: true},
					forwardedKey{qcode: "delete", down: false},
					forwardedKey{qcode: "alt", down: false},
					forwardedKey{qcode: "ctrl", down: false},
				)
				return 1
			case ctrlAltEndSwallow:
				if !down {
					ctrlAltEndSent = false
				}
				return 1
			}
		}
	}
	chaining := hookChaining
	hookChaining = true
	r, _, _ := procCallNextHookEx.Call(0, nCode, wParam, lParam)
	hookChaining = chaining
	return r
}

// runWinKeyHook owns the hook and its message pump. LL hooks run newest-first
// and QEMU re-installs its own on every grab toggle, so the keyboard hook is
// re-installed every rehookInterval to stay at the front of the chain. The new
// hook goes in before the old one comes out, so no key event slips past both.
func runWinKeyHook() {
	runtime.LockOSThread()
	cb := syscall.NewCallback(hookCallback)
	mcb := syscall.NewCallback(mouseHookCallback)
	install := func() uintptr {
		h, _, _ := procSetWindowsHookExW.Call(whKeyboardLL, cb, 0, 0)
		return h
	}
	h := install()
	if h == 0 {
		logf("winkey: SetWindowsHookEx failed - Super forwarding disabled")
		return
	}
	// The close guard's mouse hook shares this thread's pump. Installed once;
	// only the keyboard hook needs the front-of-chain rehook dance (QEMU's
	// competing hook is keyboard-only).
	if mh, _, _ := procSetWindowsHookExW.Call(whMouseLL, mcb, 0, 0); mh == 0 {
		logf("closeguard: mouse hook failed - X clicks will be ignored (window-close=off)")
	}
	// The VM window events share this pump too.
	installQemuWindowHooks()
	var m msgStruct
	rehooked := time.Now()
	for {
		// Every key event wakes this wait, so the rehook is paced by time.
		wait := max(rehookInterval-time.Since(rehooked), 0)
		procMsgWaitForMultipleObj.Call(0, 0, 0, uintptr(wait.Milliseconds()), qsAllinput)
		for {
			r, _, _ := procPeekMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0, pmRemove)
			if r == 0 {
				break
			}
		}
		if time.Since(rehooked) < rehookInterval {
			continue
		}
		rehooked = time.Now()
		if next := install(); next != 0 {
			procUnhookWindowsHookEx.Call(h)
			h = next
		}
		// On failure the old hook stays in, behind QEMU's at worst; retry
		// on the next interval.
	}
}

// runWinKeyQmp drains forwarded key events into the guest, reconnecting to the
// forwarder QMP socket whenever QEMU restarts. It never dials before the
// supervisor's handshake succeeds: a QMP connection during early guest boot
// reliably wedges QEMU's main loop under WHPX (see docs/FINDINGS.md).
func runWinKeyQmp() {
	for {
		if !guestUp.Load() {
			time.Sleep(time.Second)
			continue
		}
		c := qmpConnect(qmpFwdPort, 8*time.Second)
		if c == nil {
			time.Sleep(2 * time.Second)
			continue
		}
		logf("winkey: QMP connected on %d", qmpFwdPort)
		if dragButtonHeld {
			// The last connection broke mid-drag; let go of the button.
			if c.writeLine(`{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}`) == nil {
				dragButtonHeld = false
			}
		}
		lines := c.readLines()
	drain:
		for {
			select {
			case key := <-keyEvents:
				if key.qcode != "meta_l" && key.down {
					logf("winkey: forwarded %s to the guest", key.qcode)
				}
				ev := fmt.Sprintf(`{"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"down":%t,"key":{"type":"qcode","data":%q}}}]}}`, key.down, key.qcode)
				if err := c.writeLine(ev); err != nil {
					break drain
				}
			case script := <-pointerScripts:
				// A late script would press on a helper the guest gave up on.
				if time.Now().After(script.deadline) || !guestUp.Load() {
					logf("file drop: skipped a drag that was no longer wanted")
					break
				}
				for _, step := range script.steps {
					if err := c.writeLine(pointerStepCommand(step)); err != nil {
						break drain
					}
					if step.button >= 0 {
						dragButtonHeld = step.button == 1
					}
					time.Sleep(step.pause)
				}
			case _, ok := <-lines:
				if !ok {
					break drain
				}
			}
		}
		c.close()
		time.Sleep(2 * time.Second)
	}
}

// runTitleEnforcer keeps the VM window branded (QEMU resets its title on every
// grab toggle, so this reasserts every second), maximizes it once per launch
// when the window first appears, and stamps our icon over QEMU's on the
// window + taskbar (the SDL window belongs to qemu-system-*.exe, so without
// this the taskbar shows the QEMU logo - the last piece of QEMU chrome).
// It also remembers where the user leaves the window: the placement is saved
// whenever it changes and restored, in place of the maximized default, on
// the next windowed launch if that spot is still on a connected display.
func runTitleEnforcer(dir string, fullscreen bool, fullscreenDisplay string) {
	hInst, _, _ := procGetModuleHandleW.Call(0)
	appIcon, _, _ := procLoadIconW.Call(hInst, 1) // the embedded Omarchy .ico
	for {
		if pid := qemuPid.Load(); pid != 0 {
			enforceDisplayWindows(pid, dir, fullscreen, fullscreenDisplay, appIcon)
		} else {
			enumTitlePid = 0
			qemuHwnd.Store(0)
		}
		wait := time.Second
		if curtainPolling() {
			wait = 250 * time.Millisecond
		}
		select {
		case <-qemuWindowChanged:
			// Let a burst of window events settle, then look once.
			time.Sleep(30 * time.Millisecond)
		case <-time.After(wait):
		}
	}

}

// runCursorReleaseGuard keeps the SDL frontend from confining the Windows
// cursor to the VM. SDL re-applies its grab whenever the window gains focus,
// so this must watch the full lifetime rather than run only at launch.
func runCursorReleaseGuard() {
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for range ticker.C {
		releaseQemuCursor()
	}
}

func pointerStepCommand(step pointerStep) string {
	events := fmt.Sprintf(`{"type":"abs","data":{"axis":"x","value":%d}},{"type":"abs","data":{"axis":"y","value":%d}}`, step.x, step.y)
	if step.button >= 0 {
		events += fmt.Sprintf(`,{"type":"btn","data":{"down":%t,"button":"left"}}`, step.button == 1)
	}
	return `{"execute":"input-send-event","arguments":{"events":[` + events + `]}}`
}
