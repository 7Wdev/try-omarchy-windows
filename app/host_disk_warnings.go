package main

import (
	"encoding/json"
	"strings"
)

type diskVolumeSpace struct{ available, total int64 }

type hostDiskWarning int

const (
	hostDiskOK hostDiskWarning = iota
	hostDiskLow
	hostDiskCritical
)

type hostDiskWarningState struct{ low, critical bool }

// 5 GiB leaves room for Reclaim's 4 GiB reserve. Percentages warn earlier on
// large volumes; 1 GiB / 1% is the urgent tier. Recovery margins prevent
// repeated notices when another application hovers around a threshold.
func hostDiskThresholds(total int64) (low, critical, lowMargin, criticalMargin int64) {
	return max(5<<30, total/20), max(1<<30, total/100),
		max(1<<30, total/100), max(256<<20, total/400)
}

func (s *hostDiskWarningState) observe(space diskVolumeSpace) hostDiskWarning {
	if space.total <= 0 || space.available < 0 || space.available > space.total {
		return hostDiskOK
	}
	low, critical, lowMargin, criticalMargin := hostDiskThresholds(space.total)
	if space.available >= low+lowMargin {
		s.low = false
	}
	if space.available >= critical+criticalMargin {
		s.critical = false
	}
	if space.available < critical {
		// Going straight to critical should not later show a weaker notice
		// until the volume has recovered past the low warning's margin.
		s.low = true
		if !s.critical {
			s.critical = true
			return hostDiskCritical
		}
	} else if space.available < low && !s.low {
		s.low = true
		return hostDiskLow
	}
	return hostDiskOK
}

func boundedDiskBytes(bytes uint64) int64 {
	if bytes > uint64(1<<63-1) {
		return 1<<63 - 1
	}
	return int64(bytes)
}

// QMP's structured nospace flag distinguishes a full host disk from other
// storage errors. Only a stop action means QEMU paused the guest.
func diskFullPauseEvent(line string) bool {
	if !strings.Contains(line, "BLOCK_IO_ERROR") {
		return false
	}
	var event struct {
		Event string `json:"event"`
		Data  struct {
			Action  string `json:"action"`
			NoSpace bool   `json:"nospace"`
		} `json:"data"`
	}
	return json.Unmarshal([]byte(line), &event) == nil && event.Event == "BLOCK_IO_ERROR" &&
		event.Data.Action == "stop" && event.Data.NoSpace
}
