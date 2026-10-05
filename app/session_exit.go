package main

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
)

const endSessionBudget = 10 * time.Second
const guestExitFilename = "guest-exit.json"

type guestExitRecord struct {
	Clean  bool      `json:"clean"`
	Reason string    `json:"reason"`
	At     time.Time `json:"at"`
}

var guestExitMu sync.Mutex

func recordGuestExit(dir string, clean bool, reason string) error {
	guestExitMu.Lock()
	defer guestExitMu.Unlock()
	data, err := json.Marshal(guestExitRecord{clean, reason, time.Now()})
	if err != nil {
		return err
	}
	path := filepath.Join(dir, guestExitFilename)
	f, err := os.OpenFile(path+".part", os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	if _, err = f.Write(append(data, '\n')); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(path+".part", path)
}

func cleanGuestShutdown(line string) bool {
	var event struct {
		Event string `json:"event"`
		Data  struct {
			Guest  bool   `json:"guest"`
			Reason string `json:"reason"`
		} `json:"data"`
	}
	return json.Unmarshal([]byte(line), &event) == nil && event.Event == "SHUTDOWN" && event.Data.Guest &&
		(event.Data.Reason == "guest-shutdown" || event.Data.Reason == "guest-reset")
}

func previousGuestExitUnclean(dir string) (bool, error) {
	data, err := os.ReadFile(filepath.Join(dir, guestExitFilename))
	if os.IsNotExist(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	var record guestExitRecord
	if err := json.Unmarshal(data, &record); err != nil {
		return false, err
	}
	return !record.Clean, nil
}

// The same deadline covers negotiation, poweroff and confirmation. A command
// acknowledgement is not proof of a clean exit; wait for the supervisor's
// recorded SHUTDOWN and process exit. Never spend a fresh budget per poll.
func endGuestSession(ctx context.Context, request func(context.Context) error, clean func() bool) bool {
	if clean() {
		return true
	}
	if request(ctx) != nil {
		return clean()
	}
	ticker := time.NewTicker(25 * time.Millisecond)
	defer ticker.Stop()
	for {
		if clean() {
			return true
		}
		select {
		case <-ctx.Done():
			return clean()
		case <-ticker.C:
		}
	}
}
