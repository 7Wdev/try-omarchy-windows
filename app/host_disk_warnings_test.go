package main

import (
	"math"
	"path/filepath"
	"testing"
)

func TestHostDiskWarningThresholds(t *testing.T) {
	for _, tc := range []struct {
		name                                            string
		total, low, critical, lowMargin, criticalMargin int64
	}{
		{"small drive", 40 << 30, 5 << 30, 1 << 30, 1 << 30, 256 << 20},
		{"100 GiB drive", 100 << 30, 5 << 30, 1 << 30, 1 << 30, 256 << 20},
		{"large drive", 1000 << 30, 50 << 30, 10 << 30, 10 << 30, 2560 << 20},
	} {
		t.Run(tc.name, func(t *testing.T) {
			low, critical, lowMargin, criticalMargin := hostDiskThresholds(tc.total)
			if low != tc.low || critical != tc.critical || lowMargin != tc.lowMargin || criticalMargin != tc.criticalMargin {
				t.Fatalf("thresholds=%d %d margins=%d %d", low, critical, lowMargin, criticalMargin)
			}
			for _, step := range []struct {
				available int64
				want      hostDiskWarning
			}{
				{low, hostDiskOK}, {low - 1, hostDiskLow}, {critical, hostDiskLow}, {critical - 1, hostDiskCritical}, {0, hostDiskCritical},
			} {
				var state hostDiskWarningState
				if got := state.observe(diskVolumeSpace{step.available, tc.total}); got != step.want {
					t.Fatalf("available=%d: got %v want %v", step.available, got, step.want)
				}
			}
		})
	}
}

func TestHostDiskWarningsOncePerCrossingWithHysteresis(t *testing.T) {
	for _, total := range []int64{40 << 30, 1000 << 30} {
		low, critical, lowMargin, criticalMargin := hostDiskThresholds(total)
		var state hostDiskWarningState
		for i, step := range []struct {
			available int64
			want      hostDiskWarning
		}{
			{low, hostDiskOK}, {low - 1, hostDiskLow}, {low - 1, hostDiskOK},
			{low + lowMargin - 1, hostDiskOK}, {low - 1, hostDiskOK},
			{critical - 1, hostDiskCritical}, {0, hostDiskOK},
			{critical + criticalMargin - 1, hostDiskOK}, {critical - 1, hostDiskOK},
			{critical + criticalMargin, hostDiskOK}, {critical - 1, hostDiskCritical},
			{low + lowMargin, hostDiskOK}, {low - 1, hostDiskLow},
			{low + lowMargin, hostDiskOK}, {0, hostDiskCritical},
			{critical + criticalMargin, hostDiskOK}, {low - 1, hostDiskOK},
		} {
			if got := state.observe(diskVolumeSpace{step.available, total}); got != step.want {
				t.Fatalf("total=%d step=%d available=%d: got %v want %v", total, i, step.available, got, step.want)
			}
		}
	}
}

func TestHostDiskWarningInvalidSamplesPreserveState(t *testing.T) {
	state := hostDiskWarningState{low: true, critical: true}
	for _, space := range []diskVolumeSpace{{-1, 40 << 30}, {1, 0}, {1, -1}, {41 << 30, 40 << 30}} {
		if got := state.observe(space); got != hostDiskOK || state != (hostDiskWarningState{true, true}) {
			t.Fatalf("invalid sample changed warning state: %v %v", got, state)
		}
	}
	low, critical, lowMargin, criticalMargin := hostDiskThresholds(math.MaxInt64)
	if low+lowMargin <= 0 || critical+criticalMargin <= 0 || boundedDiskBytes(math.MaxUint64) != math.MaxInt64 {
		t.Fatal("large volume overflow")
	}
}

func TestHostDiskVolumeSpaceUsesDataDirectory(t *testing.T) {
	dir := t.TempDir()
	space, err := platformDiskVolumeSpace(filepath.Join(dir, "not-yet-created", "vm"))
	if err != nil || space.available < 0 || space.total <= 0 || space.available > space.total {
		t.Fatalf("volume space=%v err=%v", space, err)
	}
	free, err := platformDiskFreeBytes(dir)
	if err != nil || free < 0 || free > space.total {
		t.Fatalf("existing preflight free bytes=%d err=%v", free, err)
	}
}

func TestDiskFullPauseEvent(t *testing.T) {
	for _, tc := range []struct {
		line string
		want bool
	}{
		{`{"event":"BLOCK_IO_ERROR","data":{"action":"stop","nospace":true,"operation":"write"}}`, true},
		{`{"event":"BLOCK_IO_ERROR","data":{"action":"stop","nospace":false}}`, false},
		{`{"event":"BLOCK_IO_ERROR","data":{"action":"report","nospace":true}}`, false},
		{`{"event":"BLOCK_IO_ERROR","data":{"action":"ignore","nospace":true}}`, false},
		{`{"event":"BLOCK_IO_ERROR","data":{"action":"stop","reason":"No space left on device"}}`, false},
		{`{"event":"STOP","data":{"action":"stop","nospace":true}}`, false},
		{`{"return":{"status":"io-error"}}`, false},
		{`{"event":"BLOCK_IO_ERROR"`, false},
	} {
		if got := diskFullPauseEvent(tc.line); got != tc.want {
			t.Errorf("%s: got %v want %v", tc.line, got, tc.want)
		}
	}
}
