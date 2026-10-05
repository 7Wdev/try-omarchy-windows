package main

import (
	"context"
	"errors"
	"testing"
	"time"
)

func TestRuntimeLoaderPreflightRunsOncePerSession(t *testing.T) {
	for _, tc := range []struct {
		name    string
		failure error
	}{{"success", nil}, {"failure", errors.New("loader failed")}} {
		t.Run(tc.name, func(t *testing.T) {
			var preflight runtimeLoaderPreflight
			calls := 0
			load := func(ctx context.Context, executable string) error {
				calls++
				if executable != "runtime/bin/qemu-system-x86_64w.exe" {
					t.Fatalf("wrong runtime: %q", executable)
				}
				if deadline, ok := ctx.Deadline(); !ok || time.Until(deadline) > runtimeLoaderTimeout {
					t.Fatal("loader must have the preflight deadline")
				}
				return tc.failure
			}
			for i, attempt := range []int{1, 2, 3, 1} { // Last 1 is a guest reboot.
				ran, err := preflight.run(context.Background(), attempt, "runtime/bin/qemu-system-x86_64w.exe", runtimeLoaderTimeout, load)
				if ran != (i == 0) || (i == 0 && err != tc.failure) || (i != 0 && err != nil) {
					t.Fatalf("step %d: ran=%t err=%v", i, ran, err)
				}
			}
			if calls != 1 {
				t.Fatalf("preflight ran %d times", calls)
			}
		})
	}
}

func TestRuntimeLoaderPreflightSkipsLaterAttempts(t *testing.T) {
	var preflight runtimeLoaderPreflight
	ran, err := preflight.run(context.Background(), 2, "qemu", runtimeLoaderTimeout, func(context.Context, string) error {
		t.Fatal("later attempt must not preflight even with unused session state")
		return nil
	})
	if ran || err != nil || preflight.attempted {
		t.Fatalf("ran=%t err=%v attempted=%t", ran, err, preflight.attempted)
	}
}

func TestRuntimeLoaderPreflightTimeoutAndQuit(t *testing.T) {
	for _, quit := range []bool{false, true} {
		t.Run(map[bool]string{false: "timeout", true: "quit"}[quit], func(t *testing.T) {
			var preflight runtimeLoaderPreflight
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			timeout, want := 20*time.Millisecond, context.DeadlineExceeded
			if quit {
				timeout, want = runtimeLoaderTimeout, context.Canceled
			}
			started := time.Now()
			ran, err := preflight.run(ctx, 1, "qemu", timeout, func(ctx context.Context, _ string) error {
				if quit {
					cancel()
				}
				<-ctx.Done()
				return errors.New("process killed") // CommandContext's process error.
			})
			if !ran || !errors.Is(err, want) || time.Since(started) > time.Second {
				t.Fatalf("ran=%t err=%v elapsed=%s", ran, err, time.Since(started))
			}
			// Timeout/failure is advisory: the caller proceeds to normal startup
			// and neither its retry nor a guest reboot waits for a second scan.
			ran, err = preflight.run(context.Background(), 1, "qemu", timeout, func(context.Context, string) error {
				t.Fatal("completed or failed preflight must not repeat")
				return nil
			})
			if ran || err != nil {
				t.Fatalf("repeat: ran=%t err=%v", ran, err)
			}
		})
	}
}

func TestRuntimeLoaderPreflightDoesNotStartAfterQuit(t *testing.T) {
	var preflight runtimeLoaderPreflight
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	ran, err := preflight.run(ctx, 1, "qemu", runtimeLoaderTimeout, func(context.Context, string) error {
		t.Fatal("cancelled session must not start a process")
		return nil
	})
	if ran || !errors.Is(err, context.Canceled) || preflight.attempted {
		t.Fatalf("ran=%t err=%v attempted=%t", ran, err, preflight.attempted)
	}
}
