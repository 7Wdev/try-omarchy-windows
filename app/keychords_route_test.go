package main

import (
	"slices"
	"testing"
)

type routeStep struct {
	focused, hostHeld, down bool
	wantKeys                []forwardedKey
	wantSwallow             bool
}

func runRoute(t *testing.T, k *routedKey, steps []routeStep) {
	t.Helper()
	for i, step := range steps {
		keys, swallow := k.route(step.focused, step.hostHeld, step.down)
		if swallow != step.wantSwallow || !slices.Equal(keys, step.wantKeys) {
			t.Fatalf("step %d route(focused=%v, hostHeld=%v, down=%v) = %v, %v; want %v, %v",
				i, step.focused, step.hostHeld, step.down, keys, swallow, step.wantKeys, step.wantSwallow)
		}
	}
	if k.side != keySideNone {
		t.Fatalf("side = %v after the release, want none", k.side)
	}
}

func TestRoutedKeyFocusedPressGoesToGuest(t *testing.T) {
	runRoute(t, &routedKey{qcode: "meta_l"}, []routeStep{
		{focused: true, down: true, wantKeys: []forwardedKey{{"meta_l", true}}, wantSwallow: true},
		{focused: true, down: true, wantSwallow: true}, // autorepeat
		{focused: true, down: true, wantSwallow: true},
		{focused: true, wantKeys: []forwardedKey{{"meta_l", false}}, wantSwallow: true},
	})
}

func TestRoutedKeyUnfocusedPressStaysWithWindows(t *testing.T) {
	runRoute(t, &routedKey{qcode: "meta_l"}, []routeStep{
		{down: true},
		{hostHeld: true, down: true}, // autorepeat
		{hostHeld: true},
	})
}

// Win+1 on the taskbar focuses the VM before Win comes up. Windows saw the
// press, so it must see the release, or it keeps Win held.
func TestRoutedKeyReleaseAfterVMTakesFocusGoesToWindows(t *testing.T) {
	runRoute(t, &routedKey{qcode: "meta_l"}, []routeStep{
		{down: true},
		{focused: true, hostHeld: true, down: true}, // autorepeat after focus moved
		{focused: true, hostHeld: true},
	})
}

// The guest saw the press, so it gets the release; Windows never sees it and
// cannot open Start.
func TestRoutedKeyReleaseAfterFocusLeavesGoesToGuest(t *testing.T) {
	runRoute(t, &routedKey{qcode: "meta_l"}, []routeStep{
		{focused: true, down: true, wantKeys: []forwardedKey{{"meta_l", true}}, wantSwallow: true},
		{wantKeys: []forwardedKey{{"meta_l", false}}, wantSwallow: true},
	})
}

func TestRoutedKeyRepeatAfterFocusLeavesMovesToWindows(t *testing.T) {
	runRoute(t, &routedKey{qcode: "meta_l"}, []routeStep{
		{focused: true, down: true, wantKeys: []forwardedKey{{"meta_l", true}}, wantSwallow: true},
		{down: true, wantKeys: []forwardedKey{{"meta_l", false}}}, // released in the guest, pressed in Windows
		{hostHeld: true, down: true},
		{hostHeld: true},
	})
}

// Super+Enter a few times while focus moves between Windows and the VM.
func TestRoutedKeyAlternatingFocus(t *testing.T) {
	runRoute(t, &routedKey{qcode: "meta_l"}, []routeStep{
		{focused: true, down: true, wantKeys: []forwardedKey{{"meta_l", true}}, wantSwallow: true},
		{wantKeys: []forwardedKey{{"meta_l", false}}, wantSwallow: true},
		{down: true},
		{focused: true, hostHeld: true},
		{focused: true, down: true, wantKeys: []forwardedKey{{"meta_l", true}}, wantSwallow: true},
		{focused: true, wantKeys: []forwardedKey{{"meta_l", false}}, wantSwallow: true},
	})
}

// A Windows release the hook never saw must not keep later presses there.
func TestRoutedKeyRecoversFromLostWindowsRelease(t *testing.T) {
	k := &routedKey{qcode: "meta_l"}
	k.route(false, false, true)
	runRoute(t, k, []routeStep{
		{focused: true, down: true, wantKeys: []forwardedKey{{"meta_l", true}}, wantSwallow: true},
		{focused: true, wantKeys: []forwardedKey{{"meta_l", false}}, wantSwallow: true},
	})
}

func TestRoutedKeyUnpairedRelease(t *testing.T) {
	runRoute(t, &routedKey{qcode: "meta_l"}, []routeStep{
		{focused: true, hostHeld: true}, // Windows holds it: let it go there
		{focused: true, wantSwallow: true},
		{},
	})
}

func TestRoutedKeyPrintScreen(t *testing.T) {
	runRoute(t, &routedKey{qcode: "print"}, []routeStep{
		{down: true},
		{focused: true, hostHeld: true},
		{focused: true, down: true, wantKeys: []forwardedKey{{"print", true}}, wantSwallow: true},
		{focused: true, down: true, wantSwallow: true},
		{wantKeys: []forwardedKey{{"print", false}}, wantSwallow: true},
	})
}
