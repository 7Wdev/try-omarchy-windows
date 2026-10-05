package main

type forwardedKey struct {
	qcode string
	down  bool
}

// Windows consumes Alt+Tab before SDL can deliver the chord to the guest, so
// the hook forwards Tab itself while Omarchy is focused. It also presses Alt
// once in case SDL never delivered it. That Alt stays down across repeated Tab
// taps and is released with the physical Alt, so a guest switcher that stays
// open while Alt is held can keep cycling.
type altTabForwarder struct {
	tabDown, altDown bool
}

// tab returns the guest keys for a Tab change and whether the host should
// swallow it. When forward is false, focus has left Omarchy, so anything
// still held in the guest is released and Tab goes to Windows.
func (f *altTabForwarder) tab(forward, down bool) (keys []forwardedKey, swallow bool) {
	if forward && down {
		if !f.altDown {
			f.altDown = true
			keys = append(keys, forwardedKey{qcode: "alt", down: true})
		}
		if !f.tabDown {
			f.tabDown = true
			keys = append(keys, forwardedKey{qcode: "tab", down: true})
		}
		return keys, true
	}
	if f.tabDown {
		f.tabDown = false
		keys = append(keys, forwardedKey{qcode: "tab", down: false})
	}
	if !forward {
		keys = append(keys, f.altReleased()...)
	}
	return keys, forward
}

// altReleased releases the forwarded Alt when the physical Alt comes up.
func (f *altTabForwarder) altReleased() []forwardedKey {
	if !f.altDown {
		return nil
	}
	f.altDown = false
	return []forwardedKey{{qcode: "alt", down: false}}
}

// Windows reserves Ctrl+Alt+Delete for its security screen before any hook
// sees it, so Ctrl+Alt+End stands in for it while Omarchy is focused, as in
// Hyper-V. The guest receives one complete Ctrl+Alt+Delete press and release
// when End goes down, so a lost End release can never leave keys held there.
type ctrlAltEndAction int

const (
	ctrlAltEndPass    ctrlAltEndAction = iota // leave End to Windows and SDL
	ctrlAltEndSend                            // swallow End and send one Ctrl+Alt+Delete
	ctrlAltEndSwallow                         // swallow a repeat or release of a sent chord
)

func classifyCtrlAltEnd(focused, ctrl, alt, sent, down bool) ctrlAltEndAction {
	switch {
	case sent:
		return ctrlAltEndSwallow
	case focused && down && ctrl && alt:
		return ctrlAltEndSend
	}
	return ctrlAltEndPass
}

// Win and Print Screen go to the guest while Omarchy is focused and to
// Windows otherwise. Focus can change while one is held (Win+1 on the taskbar
// focuses the VM before Win comes up), so each press belongs to the side that
// saw it go down until it comes back up. A release sent to the wrong side
// leaves Windows treating every key and click as Win+something, or pops Start
// over the VM.
type keySide int

const (
	keySideNone     keySide = iota // up, or its press predates the hook
	keySideHost                    // Windows saw the press
	keySideGuest                   // the guest saw the press
	keySideDetached                // the guest saw the press, then focus left
)

type routedKey struct {
	qcode string
	side  keySide
}

// route returns the guest keys for a key change and whether the host should
// swallow it. hostHeld is Windows' state for the key before this change.
func (k *routedKey) route(focused, hostHeld, down bool) (keys []forwardedKey, swallow bool) {
	if !down {
		side := k.side
		k.side = keySideNone
		switch side {
		case keySideGuest:
			return []forwardedKey{{qcode: k.qcode, down: false}}, true
		case keySideDetached:
			return nil, true // already let go in the guest
		case keySideHost:
			return nil, false
		}
		// An unpaired release goes to Windows if it might hold the key.
		// Otherwise the guest gets it: QEMU's own hook can take a press while
		// it sits in front of ours between rehooks, and a release the guest
		// does not hold changes nothing.
		if focused && !hostHeld {
			return []forwardedKey{{qcode: k.qcode, down: false}}, true
		}
		return nil, false
	}
	switch k.side {
	case keySideGuest:
		if focused {
			return nil, true // autorepeat
		}
		// Focus left mid-press: let go in the guest, but keep the rest of
		// the press away from Windows, or its release pops Start.
		k.side = keySideDetached
		return []forwardedKey{{qcode: k.qcode, down: false}}, true
	case keySideDetached:
		if !focused {
			return nil, true // autorepeat
		}
		// Back on the VM with the key still held.
		k.side = keySideGuest
		return []forwardedKey{{qcode: k.qcode, down: true}}, true
	case keySideHost:
		if !focused || hostHeld {
			return nil, false // autorepeat stays with Windows
		}
		// Windows already let go; its release never reached the hook.
	}
	if focused {
		k.side = keySideGuest
		return []forwardedKey{{qcode: k.qcode, down: true}}, true
	}
	k.side = keySideHost
	return nil, false
}
