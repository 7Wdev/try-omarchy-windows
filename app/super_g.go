package main

// Xbox Game Bar takes Win+G and Win+Alt+G before any hook can stop it, so the
// ordinary G event never reaches SDL. Route these chords over the same ordered
// QMP queue. Once G belongs to the guest, keep its release there even if Super
// comes up first or focus leaves the VM.
//
// Game Bar can also take the Alt release, so a chord with Alt presses that Alt
// over QMP too and lets go of it when the hook sees the physical Alt come up.
type superGForwarder struct {
	key routedKey
	alt string // Alt qcode pressed with the chord, until the physical Alt comes up
}

// alt is "alt" or "alt_r" while that Alt is held, otherModifiers is Ctrl or Shift.
func (f *superGForwarder) route(focused, guestSuper bool, alt string, otherModifiers, hostHeld, down bool) ([]forwardedKey, bool) {
	if f.key.side == keySideNone && !(focused && guestSuper && !otherModifiers && !hostHeld && down) {
		return nil, false
	}
	f.key.qcode = "g"
	keys, swallow := f.key.route(focused, hostHeld, down)
	if alt != "" && f.alt == "" && down && f.key.side == keySideGuest {
		f.alt = alt
		keys = append([]forwardedKey{{qcode: alt, down: true}}, keys...)
	}
	return keys, swallow
}

// altReleased lets go of the Alt a chord pressed in the guest.
func (f *superGForwarder) altReleased() []forwardedKey {
	if f.alt == "" {
		return nil
	}
	keys := []forwardedKey{{qcode: f.alt, down: false}}
	f.alt = ""
	return keys
}
