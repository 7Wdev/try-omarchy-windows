package main

import (
	"testing"
	"time"
)

func TestWindowPlacementRoundTrip(t *testing.T) {
	dir := t.TempDir()
	if p, err := loadWindowPlacement(dir); err != nil || p != nil {
		t.Fatalf("missing file should be nil: %v %v", p, err)
	}
	want := windowPlacement{Normal: screenRect{100, 50, 1380, 850}, Maximized: false, SavedAt: time.Date(2026, 9, 5, 0, 0, 0, 0, time.UTC)}
	if err := saveWindowPlacement(dir, want); err != nil {
		t.Fatal(err)
	}
	got, err := loadWindowPlacement(dir)
	if err != nil || got == nil || !got.sameAs(&want) || got.Schema != windowPlacementSchemaNow {
		t.Fatalf("round trip: %+v %v", got, err)
	}
	// Measured on Windows 11: a 1000x620 window has a 984x581 client area.
	if w, h := (&windowPlacement{Normal: screenRect{150, 40, 1150, 660}}).consoleSize(); w != 984 || h != 581 {
		t.Fatalf("console size %dx%d", w, h)
	}
	if w, h := got.consoleSize(); w != 1280-windowFrameWidth || h != 800-windowFrameHeight {
		t.Fatalf("console size %dx%d", w, h)
	}
}

func TestWindowPlacementUsableOnlyOnAPresentDisplay(t *testing.T) {
	primary := screenRect{0, 0, 1920, 1080}
	second := screenRect{1920, 0, 3840, 1080}
	cases := []struct {
		name   string
		p      *windowPlacement
		mons   []screenRect
		usable bool
	}{
		{"nil", nil, []screenRect{primary}, false},
		{"on primary", &windowPlacement{Normal: screenRect{100, 100, 1300, 900}}, []screenRect{primary}, true},
		{"on second monitor present", &windowPlacement{Normal: screenRect{2000, 100, 3200, 900}}, []screenRect{primary, second}, true},
		{"on second monitor gone", &windowPlacement{Normal: screenRect{2000, 100, 3200, 900}}, []screenRect{primary}, false},
		{"mostly off screen", &windowPlacement{Normal: screenRect{1800, 1000, 3000, 1800}}, []screenRect{primary}, false},
		{"too small", &windowPlacement{Normal: screenRect{0, 0, 300, 200}}, []screenRect{primary}, false},
		{"partly off the edge", &windowPlacement{Normal: screenRect{-200, -50, 1000, 700}}, []screenRect{primary}, true},
	}
	for _, c := range cases {
		if got := c.p.usable(c.mons); got != c.usable {
			t.Errorf("%s: usable=%v, want %v", c.name, got, c.usable)
		}
	}
}

func TestOnlyTheUsersMovesAreRemembered(t *testing.T) {
	placed := &windowPlacement{Normal: screenRect{150, 40, 1150, 660}}
	grown := &windowPlacement{Normal: screenRect{150, 40, 1598, 800}}
	maximized := &windowPlacement{Normal: placed.Normal, Maximized: true}
	cases := []struct {
		name                      string
		now, last, target         *windowPlacement
		userMoved, dragging, held bool
		want                      placementStep
	}{
		{"unchanged", placed, placed, placed, false, false, true, placementKeep},
		{"QEMU grew it while the guest boots", grown, placed, placed, false, false, true, placementRestore},
		{"moved once the guest follows the window (Snap, tools)", grown, placed, placed, false, false, false, placementSave},
		{"unchanged once the guest follows the window", placed, placed, placed, false, false, false, placementKeep},
		{"the user dragged it", grown, placed, placed, true, false, true, placementSave},
		{"the user is still dragging", grown, placed, placed, false, true, true, placementKeep},
		{"the user maximized it", maximized, placed, placed, false, false, true, placementSave},
		{"maximized windows are not held", maximized, maximized, maximized, false, false, true, placementKeep},
		{"minimized", nil, placed, placed, false, false, true, placementKeep},
		{"nothing remembered yet", placed, nil, nil, false, false, true, placementSave},
	}
	for _, c := range cases {
		if got := nextPlacementStep(c.now, c.last, c.target, c.userMoved, c.dragging, c.held); got != c.want {
			t.Errorf("%s: %v, want %v", c.name, got, c.want)
		}
	}
}

func TestRememberedWindowsAreFittedToTheirDisplay(t *testing.T) {
	laptop := screenRect{0, 0, 1366, 720} // 1366x768 minus the taskbar
	second := screenRect{1366, 0, 3286, 1040}
	cases := []struct {
		name string
		in   screenRect
		want screenRect
	}{
		{"inside stays put", screenRect{150, 40, 1150, 660}, screenRect{150, 40, 1150, 660}},
		{"grown past the right edge (v0.7.1)", screenRect{150, 40, 1536, 792}, screenRect{-8, 0, 1374, 728}},
		{"off the right edge moves back", screenRect{700, 40, 1700, 660}, screenRect{374, 40, 1374, 660}},
		{"snapped left keeps its invisible border", screenRect{-7, 0, 690, 727}, screenRect{-7, 0, 690, 727}},
		{"on the second display", screenRect{1500, 100, 2700, 900}, screenRect{1500, 100, 2700, 900}},
	}
	for _, c := range cases {
		got := (&windowPlacement{Normal: c.in}).fittedTo([]screenRect{laptop, second})
		if got.Normal != c.want {
			t.Errorf("%s: %+v, want %+v", c.name, got.Normal, c.want)
		}
	}
	if (*windowPlacement)(nil).fittedTo([]screenRect{laptop}) != nil {
		t.Fatal("nil placement")
	}
}
