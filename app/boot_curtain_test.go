package main

import (
	"testing"
	"time"
)

func TestBootCurtainLiftsWhenTheDesktopIsDrawn(t *testing.T) {
	boot := time.Date(2026, 10, 1, 6, 0, 0, 0, time.UTC)
	times := bootCurtainTimes{boot: boot, ready: boot.Add(22 * time.Second), session: boot.Add(37 * time.Second)}
	if reason := times.liftReason(boot.Add(38*time.Second), false); reason != "" {
		t.Fatalf("lifted %q one second into the session with nothing drawn", reason)
	}
	if reason := times.liftReason(boot.Add(38*time.Second), true); reason == "" {
		t.Fatal("stayed up with the desktop drawn")
	}
	if reason := times.liftReason(boot.Add(37*time.Second+curtainPaintWait), false); reason == "" {
		t.Fatal("waited past the paint wait for a wallpaper that cannot be told from an empty screen")
	}
}

func TestBootCurtainWaitsForAMissingSessionAndASlowBoot(t *testing.T) {
	boot := time.Date(2026, 10, 1, 6, 0, 0, 0, time.UTC)
	ready := bootCurtainTimes{boot: boot, ready: boot.Add(20 * time.Second)}
	if reason := ready.liftReason(boot.Add(20*time.Second+curtainSessionWait-time.Second), false); reason != "" {
		t.Fatalf("lifted %q before the session wait ran out", reason)
	}
	if reason := ready.liftReason(boot.Add(20*time.Second+curtainSessionWait), false); reason == "" {
		t.Fatal("kept a login screen or a broken session invisible")
	}
	stuck := bootCurtainTimes{boot: boot}
	if stuck.checkPaint() {
		t.Fatal("read the screen before userspace was up")
	}
	if reason := stuck.liftReason(boot.Add(curtainBootWait-time.Second), false); reason != "" {
		t.Fatalf("lifted %q before the boot wait ran out", reason)
	}
	if reason := stuck.liftReason(boot.Add(curtainBootWait), false); reason == "" {
		t.Fatal("kept a boot that never came up invisible")
	}
}

// frame builds a BGRX frame of one colour, with fill painting parts of it.
func frame(width, height int, bgr [3]byte, fill func(x, y int) ([3]byte, bool)) []byte {
	pixels := make([]byte, width*height*4)
	for y := range height {
		for x := range width {
			color := bgr
			if fill != nil {
				if painted, ok := fill(x, y); ok {
					color = painted
				}
			}
			copy(pixels[(y*width+x)*4:], color[:])
		}
	}
	return pixels
}

func TestFramePaintedIgnoresBootOutputAndAnEmptyDesktop(t *testing.T) {
	const width, height = 984, 581
	black := [3]byte{0, 0, 0}
	hyprland := [3]byte{0x11, 0x11, 0x11}
	text := func(x, y int) ([3]byte, bool) {
		// Firmware text: a few lines at the top left.
		return [3]byte{170, 170, 170}, y < 140 && x < 600 && x%7 < 3 && y%16 < 10
	}
	bar := func(x, y int) ([3]byte, bool) {
		// The bar drawn before the wallpaper, a notification and the pointer.
		switch {
		case y < 26:
			return [3]byte{30, 27, 26}, true
		case x > 700 && x < 960 && y > 40 && y < 120:
			return [3]byte{60, 40, 40}, true
		case x > 480 && x < 500 && y > 280 && y < 300:
			return [3]byte{255, 255, 255}, true
		}
		return [3]byte{}, false
	}
	for name, pixels := range map[string][]byte{
		"black":                  frame(width, height, black, nil),
		"firmware text":          frame(width, height, black, text),
		"Hyprland's background":  frame(width, height, hyprland, nil),
		"bar and a notification": frame(width, height, hyprland, bar),
	} {
		if framePainted(pixels, width, height, width*4) {
			t.Errorf("%s counted as the desktop", name)
		}
	}
}

func TestFramePaintedSeesWallpapers(t *testing.T) {
	const width, height = 984, 581
	gradient := func(x, y int) ([3]byte, bool) {
		return [3]byte{byte(x / 8), byte(y / 4), byte((x + y) / 10)}, true
	}
	// White dots on black over the middle third, like Vantablack's hands.
	dots := func(x, y int) ([3]byte, bool) {
		return [3]byte{230, 230, 230}, y > 190 && y < 400 && x%6 < 2 && y%6 < 2
	}
	// A solid theme colour with a small logo in the middle (catppuccin).
	logo := func(x, y int) ([3]byte, bool) {
		return [3]byte{200, 200, 200}, x > 440 && x < 540 && y > 250 && y < 330
	}
	for name, pixels := range map[string][]byte{
		"photo":       frame(width, height, [3]byte{}, gradient),
		"dotted":      frame(width, height, [3]byte{}, dots),
		"plain theme": frame(width, height, [3]byte{0x2e, 0x1e, 0x1e}, logo),
		"light plain": frame(width, height, [3]byte{0xf5, 0xf0, 0xf2}, logo),
		"tokyo night": frame(width, height, [3]byte{0x26, 0x1b, 0x1a}, nil),
	} {
		if !framePainted(pixels, width, height, width*4) {
			t.Errorf("%s not counted as the desktop", name)
		}
	}
	// Rows padded past the picture.
	stride := width*4 + 64
	padded := make([]byte, stride*height)
	for y := range height {
		for x := range width {
			c, _ := gradient(x, y)
			copy(padded[y*stride+x*4:], c[:])
		}
	}
	if !framePainted(padded, width, height, stride) {
		t.Error("padded rows not read as the desktop")
	}
}

func TestFramePaintedRejectsShortBuffers(t *testing.T) {
	if framePainted(make([]byte, 100), 984, 581, 984*4) {
		t.Fatal("read past a short buffer")
	}
	if framePainted(nil, 10, 10, 40) {
		t.Fatal("judged a frame smaller than the sample grid")
	}
	// Wide and short: the sample blocks must stay inside the frame.
	framePainted(make([]byte, 3840*20*4), 3840, 20, 3840*4)
}
