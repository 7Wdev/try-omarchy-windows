//go:build windows

package main

import "testing"

func TestSendKeysKeepsRoomForReleases(t *testing.T) {
	defer func() {
		for len(keyEvents) > 0 {
			<-keyEvents
		}
	}()
	for range cap(keyEvents) {
		sendKeys([]forwardedKey{{qcode: "a", down: true}})
	}
	if got, want := len(keyEvents), cap(keyEvents)-keyReleaseRoom; got != want {
		t.Fatalf("queued %d presses, want %d", got, want)
	}
	for range keyReleaseRoom {
		sendKeys([]forwardedKey{{qcode: "a", down: false}})
	}
	if len(keyEvents) != cap(keyEvents) {
		t.Fatalf("queued %d keys, want a full queue of %d", len(keyEvents), cap(keyEvents))
	}
	if queueKeys(forwardedKey{qcode: "a", down: true}, forwardedKey{qcode: "a", down: false}) {
		t.Fatal("queueKeys used the room kept for releases")
	}
	// A full queue makes a release wait for the drain.
	go func() { <-keyEvents }()
	sendKeys([]forwardedKey{{qcode: "meta_l", down: false}})
	if len(keyEvents) != cap(keyEvents) {
		t.Fatalf("release was dropped: queued %d keys", len(keyEvents))
	}
}
