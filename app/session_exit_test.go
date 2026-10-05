package main

import (
	"context"
	"errors"
	"testing"
	"time"
)

func TestEndSessionSingleBudget(t *testing.T) {
	if endSessionBudget >= 5*time.Second {
		t.Fatal("budget exceeds Windows response allowance")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 40*time.Millisecond)
	defer cancel()
	started := time.Now()
	clean := endGuestSession(ctx, func(ctx context.Context) error {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(25 * time.Millisecond):
			return nil
		}
	}, func() bool { return false })
	if clean || time.Since(started) > 200*time.Millisecond {
		t.Fatal("unbounded end-session wait")
	}
	if ctx.Err() == nil {
		t.Fatal("request acknowledgement mistaken for clean shutdown")
	}
}

func TestEndSessionCleanAndFailure(t *testing.T) {
	clean := false
	if !endGuestSession(context.Background(), func(context.Context) error { clean = true; return nil }, func() bool { return clean }) {
		t.Fatal("lost clean exit")
	}
	clean = false
	if endGuestSession(context.Background(), func(context.Context) error { return errors.New("offline") }, func() bool { return clean }) {
		t.Fatal("failed request recorded clean")
	}
	clean = true
	if !endGuestSession(context.Background(), func(context.Context) error { t.Fatal("powered down an absent guest"); return nil }, func() bool { return clean }) {
		t.Fatal("missing already clean exit")
	}
}

func TestGuestExitRecordRecovery(t *testing.T) {
	dir := t.TempDir()
	if unclean, err := previousGuestExitUnclean(dir); err != nil || unclean {
		t.Fatalf("first launch: %t %v", unclean, err)
	}
	for _, clean := range []bool{false, true, false} {
		if err := recordGuestExit(dir, clean, "test"); err != nil {
			t.Fatal(err)
		}
		if unclean, err := previousGuestExitUnclean(dir); err != nil || unclean == clean {
			t.Fatalf("clean=%t unclean=%t error=%v", clean, unclean, err)
		}
	}
}

func TestCleanExitRequiresGuestShutdown(t *testing.T) {
	for _, tc := range []struct {
		line  string
		clean bool
	}{
		{`{"event":"SHUTDOWN","data":{"guest":true,"reason":"guest-shutdown"}}`, true},
		{`{"event":"SHUTDOWN","data":{"guest":true,"reason":"guest-reset"}}`, true},
		{`{"event":"SHUTDOWN","data":{"guest":false,"reason":"host-qmp-quit"}}`, false},
		{`{"event":"SHUTDOWN"}`, false},
		{`{"return":{}}`, false},
	} {
		if got := cleanGuestShutdown(tc.line); got != tc.clean {
			t.Fatalf("%s: clean=%t", tc.line, got)
		}
	}
}
