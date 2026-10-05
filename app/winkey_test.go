//go:build windows

package main

import "testing"

func TestSendKeysDropsOfflineInput(t *testing.T) {
	oldQueue := keyEvents
	oldUp, oldPID := guestUp.Load(), qemuPid.Load()
	t.Cleanup(func() { keyEvents = oldQueue; guestUp.Store(oldUp); qemuPid.Store(oldPID) })
	keyEvents = newForwardedKeyQueue()
	qemuPid.Store(42)
	keyEvents.session(42)
	guestUp.Store(false)
	sendKeys([]forwardedKey{{"meta_l", true}, {"meta_l", false}})
	if queueKeys(forwardedKey{"delete", true}) {
		t.Fatal("accepted boot chord")
	}
	if _, ok, reset := keyEvents.next(); ok || reset {
		t.Fatal("queued boot input")
	}
	guestUp.Store(true)
	sendKeys([]forwardedKey{{"meta_l", true}})
	if key, ok, _ := keyEvents.next(); !ok || !key.down {
		t.Fatal("lost live input")
	}
	keyEvents.session(0)
	sendKeys([]forwardedKey{{"meta_l", false}})
	if _, ok, _ := keyEvents.next(); ok {
		t.Fatal("queued reconnect input")
	}
}

func TestWinKeysUseDistinctQcodes(t *testing.T) {
	if routedKeyFor(vkLwin).qcode != "meta_l" || routedKeyFor(vkRwin).qcode != "meta_r" {
		t.Fatal("physical Win keys must not share a guest key state")
	}
}
