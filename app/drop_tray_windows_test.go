//go:build windows

package main

import (
	"errors"
	"reflect"
	"testing"
)

func TestExplorerDropMessageFilter(t *testing.T) {
	var messages []uintptr
	err := allowExplorerDrops(42, func(hwnd, message uintptr) error {
		if hwnd != 42 {
			t.Fatal(hwnd)
		}
		messages = append(messages, message)
		return nil
	})
	if err != nil || !reflect.DeepEqual(messages, []uintptr{0x233, 0x4a, 0x49}) {
		t.Fatal(messages, err)
	}
	failure := errors.New("filter denied")
	if got := allowExplorerDrops(42, func(uintptr, uintptr) error { return failure }); got != failure {
		t.Fatal(got)
	}
}

func TestDropPathLengthBounds(t *testing.T) {
	for _, n := range []uintptr{0, 32769, ^uintptr(0)} {
		if validDropPathLength(n) {
			t.Fatal(n)
		}
	}
	for _, n := range []uintptr{1, 260, 32768} {
		if !validDropPathLength(n) {
			t.Fatal(n)
		}
	}
}

func TestReceivedDropPlacement(t *testing.T) {
	work := screenRect{Left: -1920, Top: 40, Right: 0, Bottom: 1080}
	x0, y0 := dropWindowPosition(work, 0)
	x1, y1 := dropWindowPosition(work, 1)
	if x0 == x1 || y0 == y1 {
		t.Fatal("windows did not cascade")
	}
	for i := uint32(0); i < 32; i++ {
		x, y := dropWindowPosition(work, i)
		if x < work.Left || y < work.Top || x+580 > work.Right || y+380 > work.Bottom {
			t.Fatal(x, y)
		}
	}
	x, y := dropWindowPosition(screenRect{Left: 10, Top: 20, Right: 400, Bottom: 300}, 7)
	if x != 10 || y != 20 {
		t.Fatal("small monitor lost title bar", x, y)
	}
}

func TestTrayAddRetryBoundAndRecovery(t *testing.T) {
	r := trayAddRetry{}
	for i := 0; i < 10; i++ {
		if !r.afterAdd(false) {
			t.Fatal("retry stopped early", i)
		}
	}
	if r.afterAdd(false) {
		t.Fatal("unbounded retry")
	}
	r = trayAddRetry{} // TaskbarCreated starts a fresh bounded recovery.
	if !r.afterAdd(false) || r.afterAdd(true) {
		t.Fatal("recovery/success timer policy")
	}
}

func TestTrayMetricIconOwnership(t *testing.T) {
	icon, owned := selectTrayIcon(func() (uintptr, bool) { return 42, true }, func() uintptr { t.Fatal("unneeded fallback"); return 0 })
	if icon != 42 || !owned {
		t.Fatal(icon, owned)
	}
	for _, result := range []struct {
		icon uintptr
		ok   bool
	}{{0, true}, {0, false}, {42, false}} {
		icon, owned = selectTrayIcon(func() (uintptr, bool) { return result.icon, result.ok }, func() uintptr { return 7 })
		if icon != 7 || owned {
			t.Fatal(icon, owned)
		}
	}
}

func TestSettingsEstimateLiveGraphics(t *testing.T) {
	if settingsEstimateGPU(renderGPU, 42, bstChecked) {
		t.Fatal("saved GPU overrode live CPU")
	}
	if !settingsEstimateGPU(renderCPU, 42, 0) {
		t.Fatal("saved CPU overrode live GPU/Automatic")
	}
	if settingsEstimateGPU(renderCPU, 0, 0) || !settingsEstimateGPU(renderGPU, 0, 0) {
		t.Fatal("initialization fallback")
	}
}

func TestTrayTransfersReuseAndReopen(t *testing.T) {
	trayTransfersOpen.Store(false)
	trayTransfersWindow.Store(0)
	defer trayTransfersOpen.Store(false)
	defer trayTransfersWindow.Store(0)
	opened, activated := 0, 0
	open := func() { opened++ }
	activate := func(hwnd uintptr) {
		if hwnd != 42 {
			t.Fatal(hwnd)
		}
		activated++
	}
	requestTrayTransfers(open, activate)
	requestTrayTransfers(open, activate) // still creating
	trayTransfersWindow.Store(42)
	requestTrayTransfers(open, activate)
	requestTrayTransfers(open, activate)
	if opened != 1 || activated != 2 {
		t.Fatal(opened, activated)
	}
	trayTransfersWindow.CompareAndSwap(42, 0) // WM_DESTROY
	trayTransfersOpen.Store(false)            // message loop ended
	requestTrayTransfers(open, activate)
	if opened != 2 {
		t.Fatal("did not reopen", opened)
	}
}
