package main

import (
	"slices"
	"testing"
	"time"
)

func TestClassifyCtrlAltEnd(t *testing.T) {
	tests := []struct {
		name                                     string
		focused, ctrl, alt, rightAlt, sent, down bool
		want                                     ctrlAltEndAction
	}{
		{name: "send focused ctrl alt down", focused: true, ctrl: true, alt: true, down: true, want: ctrlAltEndSend},
		{name: "pass AltGr End", focused: true, ctrl: true, alt: true, rightAlt: true, down: true, want: ctrlAltEndPass},
		{name: "pass when unfocused", ctrl: true, alt: true, down: true, want: ctrlAltEndPass},
		{name: "pass with ctrl only", focused: true, ctrl: true, down: true, want: ctrlAltEndPass},
		{name: "pass with alt only", focused: true, alt: true, down: true, want: ctrlAltEndPass},
		{name: "pass with no modifiers", focused: true, down: true, want: ctrlAltEndPass},
		{name: "pass key up before sending", focused: true, ctrl: true, alt: true, want: ctrlAltEndPass},
		{name: "swallow repeat down after sending", focused: true, sent: true, down: true, want: ctrlAltEndSwallow},
		{name: "swallow key up after sending", focused: true, sent: true, want: ctrlAltEndSwallow},
		{name: "swallow unfocused key up after sending", sent: true, want: ctrlAltEndSwallow},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got := classifyCtrlAltEnd(test.focused, test.ctrl, test.alt, test.rightAlt, test.sent, test.down)
			if got != test.want {
				t.Fatalf("classifyCtrlAltEnd() = %v, want %v", got, test.want)
			}
		})
	}
}

func TestAltTabForwarderKeepsAltHeldAcrossTabs(t *testing.T) {
	var f altTabForwarder
	var got []forwardedKey
	press := func(forward, down, wantSwallow bool) {
		t.Helper()
		keys, swallow := f.tab(forward, down)
		if swallow != wantSwallow {
			t.Fatalf("tab(%v, %v) swallow = %v, want %v", forward, down, swallow, wantSwallow)
		}
		got = append(got, keys...)
	}
	press(true, true, true)
	press(true, true, true) // autorepeat
	press(true, false, true)
	press(true, true, true)
	press(true, false, true)
	got = append(got, f.altReleased()...)
	got = append(got, f.altReleased()...)
	want := []forwardedKey{
		{"alt", true}, {"tab", true}, {"tab", false},
		{"tab", true}, {"tab", false}, {"alt", false},
	}
	if !slices.Equal(got, want) {
		t.Fatalf("keys = %v, want %v", got, want)
	}
}

func TestAltTabForwarderReleasesOnFocusLoss(t *testing.T) {
	var f altTabForwarder
	f.tab(true, true)
	keys, swallow := f.tab(false, false)
	if swallow {
		t.Fatal("unfocused Tab was swallowed")
	}
	want := []forwardedKey{{"tab", false}, {"alt", false}}
	if !slices.Equal(keys, want) {
		t.Fatalf("keys = %v, want %v", keys, want)
	}
	if keys, _ := f.tab(false, true); len(keys) != 0 {
		t.Fatalf("unfocused Tab forwarded %v", keys)
	}
	if keys := f.altReleased(); len(keys) != 0 {
		t.Fatalf("Alt released twice: %v", keys)
	}
}

func TestForwardedQueueOverflowReconcilesDeliveredKeys(t *testing.T) {
	q := newForwardedKeyQueue()
	q.session(42)
	delivered := make(deliveredKeys)
	q.add(42, []forwardedKey{{"meta_l", true}}, false)
	key, ok, _ := q.next()
	if !ok {
		t.Fatal("missing press")
	}
	delivered.writing(key) // a stalled or ambiguous write
	for range keyQueueSize {
		q.add(42, []forwardedKey{{"print", false}}, false)
	}
	q.add(42, []forwardedKey{{"meta_l", false}}, false)
	if _, ok, reset := q.next(); ok || !reset {
		t.Fatal("overflow did not request recovery first")
	}
	if !delivered["meta_l"] {
		t.Fatal("lost the uncertain press")
	}
	// The drain releases all delivered keys before consuming the new backlog.
	delivered.written(forwardedKey{"meta_l", false})
	key, ok, reset := q.next()
	if !ok || reset || key != (forwardedKey{"meta_l", false}) {
		t.Fatalf("backlog = %v %v %v", key, ok, reset)
	}
	if len(delivered) != 0 {
		t.Fatal("key remained held")
	}
}

func TestForwardedQueueReleaseReserveAndAtomicChord(t *testing.T) {
	q := newForwardedKeyQueue()
	q.session(42)
	for range keyQueueSize {
		q.add(42, []forwardedKey{{"tab", true}}, false)
	}
	if len(q.keys) != keyQueueSize-keyReleaseRoom {
		t.Fatalf("press queue size %d", len(q.keys))
	}
	if q.add(42, []forwardedKey{{"ctrl", true}, {"ctrl", false}}, true) {
		t.Fatal("chord used release reserve")
	}
	for range keyReleaseRoom {
		q.add(42, []forwardedKey{{"tab", false}}, false)
	}
	if len(q.keys) != keyQueueSize || q.reset {
		t.Fatal("release reserve failed")
	}
}

func TestForwardedQueueSessionDiscardsStaleInput(t *testing.T) {
	q := newForwardedKeyQueue()
	if q.add(42, []forwardedKey{{"meta_l", true}}, false) {
		t.Fatal("accepted boot input")
	}
	q.session(42)
	q.add(42, []forwardedKey{{"meta_l", true}}, false)
	q.session(0)
	if q.add(42, []forwardedKey{{"meta_l", false}}, false) {
		t.Fatal("accepted disconnected input")
	}
	q.session(43)
	if q.add(42, []forwardedKey{{"print", true}}, false) {
		t.Fatal("accepted previous process")
	}
	if _, ok, reset := q.next(); ok || reset {
		t.Fatal("replayed old input")
	}
}

func TestBothWinKeysHaveIndependentGuestState(t *testing.T) {
	left, right := routedKey{qcode: "meta_l"}, routedKey{qcode: "meta_r"}
	delivered := make(deliveredKeys)
	for _, event := range []struct {
		key  *routedKey
		down bool
	}{{&left, true}, {&right, true}, {&left, false}} {
		keys, swallow := event.key.route(true, false, event.down)
		if !swallow {
			t.Fatal("guest event passed to host")
		}
		for _, key := range keys {
			delivered.writing(key)
			delivered.written(key)
		}
	}
	if delivered["meta_l"] || !delivered["meta_r"] {
		t.Fatalf("held keys = %v", delivered)
	}
	keys, _ := right.route(false, false, false)
	for _, key := range keys {
		delivered.written(key)
	}
	if len(delivered) != 0 {
		t.Fatalf("release after focus loss left %v", delivered)
	}
}

func TestPointerSequenceLeavesDrainAvailableDuringPause(t *testing.T) {
	now := time.Unix(100, 0)
	p := pointerSequence{steps: []pointerStep{{button: 1, pause: 80 * time.Millisecond}, {button: 0}}, due: now}
	if step, ok := p.next(now); !ok || step.button != 1 {
		t.Fatal("missing button down")
	}
	if _, ok := p.next(now.Add(40 * time.Millisecond)); ok {
		t.Fatal("step ran before pause elapsed")
	}
	q := newForwardedKeyQueue()
	q.session(42)
	q.add(42, []forwardedKey{{"meta_l", false}}, false)
	if _, ok, _ := q.next(); !ok {
		t.Fatal("key blocked during pointer pause")
	}
	if step, ok := p.next(now.Add(80 * time.Millisecond)); !ok || step.button != 0 {
		t.Fatal("missing button up")
	}
}

func TestDeliveredKeysReconnectKeepsAmbiguousReleases(t *testing.T) {
	d := make(deliveredKeys)
	d.writing(forwardedKey{"meta_r", true})
	d.writing(forwardedKey{"meta_r", false}) // failed release does not call written
	if !d["meta_r"] {
		t.Fatal("failed release lost reconnect recovery")
	}
	d.written(forwardedKey{"meta_r", false})
	if len(d) != 0 {
		t.Fatal("successful recovery did not clear held key")
	}
}
