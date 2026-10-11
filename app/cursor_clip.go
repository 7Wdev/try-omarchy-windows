package main

// maxAutoHideSliver bounds how far the clip is pulled in. A hidden taskbar
// leaves a strip of about two pixels on the monitor; the bound keeps a
// taskbar that is momentarily shown from shrinking the clip by its full size.
const maxAutoHideSliver = 4

// fullscreenCursorClip returns the rectangle that keeps an SDL cursor grab
// away from an auto-hidden taskbar without confining the pointer to the VM's
// monitor. The clip starts from the whole virtual screen so the pointer can
// still cross to other monitors, and only the edge where the taskbar sits is
// pulled in past the strip the hidden taskbar leaves on screen. A click on
// that strip reveals the taskbar and goes to Windows, so the pointer must not
// reach it.
func fullscreenCursorClip(fullscreen bool, virtual, monitor, taskbar screenRect) (screenRect, bool) {
	if !fullscreen || virtual.width() <= 0 || virtual.height() <= 0 || monitor.width() <= 0 || monitor.height() <= 0 {
		return screenRect{}, false
	}
	if taskbar.width() <= 0 || taskbar.height() <= 0 || !overlaps(taskbar, monitor) {
		return screenRect{}, false
	}

	spansWidth := taskbar.Left <= monitor.Left && taskbar.Right >= monitor.Right
	spansHeight := taskbar.Top <= monitor.Top && taskbar.Bottom >= monitor.Bottom
	top := spansWidth && taskbar.Top <= monitor.Top && taskbar.Bottom > monitor.Top
	bottom := spansWidth && taskbar.Bottom >= monitor.Bottom && taskbar.Top < monitor.Bottom
	left := spansHeight && taskbar.Left <= monitor.Left && taskbar.Right > monitor.Left
	right := spansHeight && taskbar.Right >= monitor.Right && taskbar.Left < monitor.Right
	if !top && !bottom && !left && !right {
		return screenRect{}, false
	}

	// A taskbar edge in the middle of the desktop (the VM's monitor touches
	// another display there) is not a true screen edge. A virtual-screen clip
	// would let the pointer cross the shared edge instead of stopping where the
	// taskbar reveals, so confine the pointer to the VM's monitor instead.
	internal := top && monitor.Top > virtual.Top ||
		bottom && monitor.Bottom < virtual.Bottom ||
		left && monitor.Left > virtual.Left ||
		right && monitor.Right < virtual.Right
	clip := virtual
	if internal {
		clip = monitor
	}
	inset := func(onMonitor int32) int32 {
		return max(1, min(onMonitor, maxAutoHideSliver))
	}
	if top {
		clip.Top += inset(min(taskbar.Bottom, monitor.Bottom) - monitor.Top)
	}
	if bottom {
		clip.Bottom -= inset(monitor.Bottom - max(taskbar.Top, monitor.Top))
	}
	if left {
		clip.Left += inset(min(taskbar.Right, monitor.Right) - monitor.Left)
	}
	if right {
		clip.Right -= inset(monitor.Right - max(taskbar.Left, monitor.Left))
	}
	if clip.width() <= 0 || clip.height() <= 0 {
		return screenRect{}, false
	}
	return clip, true
}

// windowIsFullscreen reports whether a window is SDL's borderless fullscreen:
// it covers the monitor's full bounds and has neither a caption nor the
// maximized state. A maximized window also covers the whole monitor when the
// taskbar auto-hides, but it is windowed use and must keep the taskbar
// reachable.
func windowIsFullscreen(window, monitor screenRect, maximized, captioned bool) bool {
	if maximized || captioned || window.width() <= 0 || window.height() <= 0 || monitor.width() <= 0 || monitor.height() <= 0 {
		return false
	}
	return window.Left <= monitor.Left && window.Top <= monitor.Top &&
		window.Right >= monitor.Right && window.Bottom >= monitor.Bottom
}

// isCursorReleaseChord reports the first press of QEMU's release chord, Left
// Ctrl+Left Alt+G, while the VM is focused. Right Alt is AltGr on many layouts
// and does not release, as in QEMU. Auto-repeat does not toggle again.
func isCursorReleaseChord(down, alreadyHeld, leftControl, leftAlt, focused bool) bool {
	return down && !alreadyHeld && leftControl && leftAlt && focused
}

func overlaps(a, b screenRect) bool {
	return a.Left < b.Right && a.Right > b.Left && a.Top < b.Bottom && a.Bottom > b.Top
}
