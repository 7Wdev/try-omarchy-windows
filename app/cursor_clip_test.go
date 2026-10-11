package main

import "testing"

func TestFullscreenCursorClipSingleMonitor(t *testing.T) {
	monitor := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}
	tests := []struct {
		name    string
		monitor screenRect
		taskbar screenRect
		want    screenRect
		clip    bool
	}{
		{
			name:    "hidden bottom taskbar keeps the pointer off its strip",
			taskbar: screenRect{Left: 0, Top: 1078, Right: 1920, Bottom: 1126},
			want:    screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1078},
			clip:    true,
		},
		{
			name:    "hidden top taskbar",
			taskbar: screenRect{Left: 0, Top: -46, Right: 1920, Bottom: 2},
			want:    screenRect{Left: 0, Top: 2, Right: 1920, Bottom: 1080},
			clip:    true,
		},
		{
			name:    "hidden left taskbar",
			taskbar: screenRect{Left: -62, Top: 0, Right: 2, Bottom: 1080},
			want:    screenRect{Left: 2, Top: 0, Right: 1920, Bottom: 1080},
			clip:    true,
		},
		{
			name:    "hidden right taskbar",
			taskbar: screenRect{Left: 1918, Top: 0, Right: 1982, Bottom: 1080},
			want:    screenRect{Left: 0, Top: 0, Right: 1918, Bottom: 1080},
			clip:    true,
		},
		{
			name:    "negative secondary monitor",
			monitor: screenRect{Left: -1920, Top: 0, Right: 0, Bottom: 1080},
			taskbar: screenRect{Left: -1920, Top: 1078, Right: 0, Bottom: 1126},
			want:    screenRect{Left: -1920, Top: 0, Right: 0, Bottom: 1078},
			clip:    true,
		},
		{
			name:    "a shown taskbar pulls the edge in by a bounded amount",
			taskbar: screenRect{Left: 0, Top: 1040, Right: 1920, Bottom: 1080},
			want:    screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1076},
			clip:    true,
		},
		{
			name:    "one-pixel fallback strip",
			taskbar: screenRect{Left: 0, Top: 1079, Right: 1920, Bottom: 1080},
			want:    screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1079},
			clip:    true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			m := tt.monitor
			if m == (screenRect{}) {
				m = monitor
			}
			got, clip := fullscreenCursorClip(true, m, m, tt.taskbar)
			if got != tt.want || clip != tt.clip {
				t.Fatalf("fullscreenCursorClip() = %#v, %t; want %#v, %t", got, clip, tt.want, tt.clip)
			}
		})
	}
}

func TestFullscreenCursorClipReleases(t *testing.T) {
	monitor := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}
	taskbar := screenRect{Left: 0, Top: 1079, Right: 1920, Bottom: 1080}
	tests := []struct {
		name       string
		fullscreen bool
		virtual    screenRect
		monitor    screenRect
		taskbar    screenRect
	}{
		{name: "not fullscreen releases", fullscreen: false, virtual: monitor, monitor: monitor, taskbar: taskbar},
		{name: "no taskbar releases", fullscreen: true, virtual: monitor, monitor: monitor},
		{
			name: "taskbar on another monitor releases", fullscreen: true,
			virtual: screenRect{Left: 0, Top: 0, Right: 3840, Bottom: 1080}, monitor: monitor,
			taskbar: screenRect{Left: 1920, Top: 1079, Right: 3840, Bottom: 1080},
		},
		{name: "empty monitor releases", fullscreen: true, virtual: monitor, monitor: screenRect{}, taskbar: taskbar},
		{name: "empty virtual releases", fullscreen: true, virtual: screenRect{}, monitor: monitor, taskbar: taskbar},
		{
			name: "taskbar not on a monitor edge releases", fullscreen: true,
			virtual: monitor, monitor: monitor, taskbar: screenRect{Left: 100, Top: 500, Right: 200, Bottom: 600},
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got, clip := fullscreenCursorClip(tt.fullscreen, tt.virtual, tt.monitor, tt.taskbar); clip || got != (screenRect{}) {
				t.Fatalf("fullscreenCursorClip() = %#v, %t; want no clip", got, clip)
			}
		})
	}
}

func TestFullscreenCursorClipSideBySideMonitors(t *testing.T) {
	virtual := screenRect{Left: 0, Top: 0, Right: 3840, Bottom: 1080}
	tests := []struct {
		name    string
		monitor screenRect
	}{
		{name: "VM on the left", monitor: screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}},
		{name: "VM on the right", monitor: screenRect{Left: 1920, Top: 0, Right: 3840, Bottom: 1080}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			taskbar := screenRect{Left: tt.monitor.Left, Top: tt.monitor.Bottom - 1, Right: tt.monitor.Right, Bottom: tt.monitor.Bottom}
			got, clip := fullscreenCursorClip(true, virtual, tt.monitor, taskbar)
			want := screenRect{Left: 0, Top: 0, Right: 3840, Bottom: 1079}
			if got != want || !clip {
				t.Fatalf("fullscreenCursorClip() = %#v, %t; want %#v, true", got, clip, want)
			}
		})
	}
}

func TestFullscreenCursorClipInternalTaskbarEdgeFallsBackToMonitor(t *testing.T) {
	// The VM's monitor sits above another display, so its bottom taskbar edge
	// is internal to the desktop and the clip falls back to the monitor alone.
	virtual := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 2160}
	monitor := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}
	taskbar := screenRect{Left: 0, Top: 1079, Right: 1920, Bottom: 1080}
	got, clip := fullscreenCursorClip(true, virtual, monitor, taskbar)
	want := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1079}
	if got != want || !clip {
		t.Fatalf("fullscreenCursorClip() = %#v, %t; want %#v, true", got, clip, want)
	}
}

func TestWindowIsFullscreen(t *testing.T) {
	monitor := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}
	tests := []struct {
		name      string
		window    screenRect
		monitor   screenRect
		maximized bool
		captioned bool
		want      bool
	}{
		{name: "exact fullscreen", window: monitor, monitor: monitor, want: true},
		{name: "fullscreen with border overshoot", window: screenRect{Left: -8, Top: -8, Right: 1928, Bottom: 1088}, monitor: monitor, want: true},
		{name: "maximized over an auto-hidden taskbar", window: screenRect{Left: -8, Top: -8, Right: 1928, Bottom: 1088}, monitor: monitor, maximized: true, captioned: true, want: false},
		{name: "captioned window covering the monitor", window: monitor, monitor: monitor, captioned: true, want: false},
		{name: "maximized windowed leaves the taskbar", window: screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1040}, monitor: monitor, want: false},
		{name: "floating window", window: screenRect{Left: 100, Top: 100, Right: 900, Bottom: 700}, monitor: monitor, want: false},
		{name: "empty window", window: screenRect{}, monitor: monitor, want: false},
		{name: "empty monitor", window: monitor, monitor: screenRect{}, want: false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := windowIsFullscreen(tt.window, tt.monitor, tt.maximized, tt.captioned); got != tt.want {
				t.Fatalf("windowIsFullscreen(%#v, %#v, %t, %t) = %t; want %t", tt.window, tt.monitor, tt.maximized, tt.captioned, got, tt.want)
			}
		})
	}
}

func TestCursorReleaseChord(t *testing.T) {
	tests := []struct {
		name                                           string
		down, alreadyHeld, control, alt, focused, want bool
	}{
		{name: "first press while focused", down: true, control: true, alt: true, focused: true, want: true},
		{name: "auto-repeat", down: true, alreadyHeld: true, control: true, alt: true, focused: true},
		{name: "release", control: true, alt: true, focused: true},
		{name: "without Ctrl", down: true, alt: true, focused: true},
		{name: "AltGr only", down: true, control: true, focused: true},
		{name: "another window focused", down: true, control: true, alt: true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := isCursorReleaseChord(tt.down, tt.alreadyHeld, tt.control, tt.alt, tt.focused); got != tt.want {
				t.Fatalf("isCursorReleaseChord = %t; want %t", got, tt.want)
			}
		})
	}
}
