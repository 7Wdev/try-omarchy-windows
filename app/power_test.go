package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"reflect"
	"sync"
	"testing"
	"time"
)

type powerPeer struct {
	mu       sync.Mutex
	state    string
	commands []string
	fail     string
	lost     bool
	events   []string
	stallAt  int
	delay    time.Duration
}

func (s *powerPeer) client(t *testing.T) *qmpClient {
	return qmpTestPeer(t, func(conn net.Conn, reader *bufio.Reader) {
		for {
			line, err := reader.ReadBytes('\n')
			if err != nil {
				return
			}
			var request struct{ Execute, ID string }
			if json.Unmarshal(line, &request) != nil {
				return
			}
			s.mu.Lock()
			s.commands = append(s.commands, request.Execute)
			stall := len(s.commands) == s.stallAt
			events := s.events
			s.events = nil
			fail := s.fail == request.Execute
			lost := fail && s.lost
			if lost && request.Execute == "stop" {
				s.state = "paused"
			}
			if !fail {
				if request.Execute == "stop" {
					s.state = "paused"
					events = append(events, "STOP")
				}
				if request.Execute == "cont" {
					s.state = "running"
					events = append(events, "RESUME")
				}
			}
			state := s.state
			s.mu.Unlock()
			if stall {
				reader.ReadByte() // the command deadline closes the connection
				return
			}
			if s.delay > 0 {
				time.Sleep(s.delay)
			}
			if lost {
				return
			}
			for _, event := range events {
				fmt.Fprintf(conn, "{\"event\":%q}\n", event)
			}
			if fail {
				fmt.Fprintf(conn, "{\"error\":{\"class\":\"GenericError\",\"desc\":\"injected failure\"},\"id\":%q}\n", request.ID)
			} else if request.Execute == "query-status" {
				fmt.Fprintf(conn, "{\"return\":{\"running\":%t,\"status\":%q},\"id\":%q}\n", state == "running", state, request.ID)
			} else {
				fmt.Fprintf(conn, "{\"return\":{},\"id\":%q}\n", request.ID)
			}
		}
	})
}

func (s *powerPeer) assert(t *testing.T, state string, operations ...string) {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	var actual []string
	for _, command := range s.commands {
		if command != "query-status" {
			actual = append(actual, command)
		}
	}
	if s.state != state || !reflect.DeepEqual(actual, operations) {
		t.Fatalf("state=%s operations=%v; want %s %v", s.state, actual, state, operations)
	}
}

func powerFixture(t *testing.T, peer *powerPeer) *guestPowerState {
	p := &guestPowerState{dial: func(context.Context) (*qmpClient, error) { return peer.client(t), nil }}
	t.Cleanup(p.close)
	return p
}

func TestPowerRepeatedTransitions(t *testing.T) {
	peer := &powerPeer{state: "running"}
	p := powerFixture(t, peer)
	p.handle(pbtApmResumeAutomatic)
	p.handle(pbtApmResumeSuspend)
	peer.assert(t, "running")
	for i := 0; i < 3; i++ {
		p.handle(pbtApmSuspend)
		p.handle(pbtApmSuspend)
		peer.assert(t, "paused", repeatPowerOps(i, true)...)
		p.handle(pbtApmResumeAutomatic)
		p.handle(pbtApmResumeSuspend)
		peer.assert(t, "running", repeatPowerOps(i+1, false)...)
	}
}

func repeatPowerOps(cycles int, paused bool) []string {
	var ops []string
	for i := 0; i < cycles; i++ {
		ops = append(ops, "stop", "cont")
	}
	if paused {
		ops = append(ops, "stop")
	}
	return ops
}

func TestPowerPreservesNonRunningGuests(t *testing.T) {
	for _, state := range []string{"paused", "prelaunch", "inmigrate", "shutdown", "suspended", "internal-error"} {
		t.Run(state, func(t *testing.T) {
			peer := &powerPeer{state: state}
			p := powerFixture(t, peer)
			p.handle(pbtApmSuspend)
			p.handle(pbtApmResumeAutomatic)
			p.handle(pbtApmResumeSuspend)
			peer.assert(t, state)
		})
	}
}

func TestPowerManualChangesWhileAsleep(t *testing.T) {
	for _, state := range []string{"running", "paused"} {
		t.Run(state, func(t *testing.T) {
			peer := &powerPeer{state: "running"}
			p := powerFixture(t, peer)
			p.handle(pbtApmSuspend)
			peer.mu.Lock()
			peer.state = state
			peer.events = []string{"RESUME"}
			if state == "paused" {
				peer.events = append(peer.events, "STOP")
			}
			peer.mu.Unlock()
			p.handle(pbtApmResumeAutomatic)
			p.handle(pbtApmResumeSuspend)
			peer.assert(t, state, "stop")
		})
	}
}

func TestPowerCommandFailures(t *testing.T) {
	for _, tc := range []struct {
		name, command string
		lost          bool
	}{
		{"query rejected", "query-status", false}, {"stop rejected", "stop", false},
		{"query disconnected", "query-status", true}, {"lost stop reply", "stop", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			peer := &powerPeer{state: "running", fail: tc.command, lost: tc.lost}
			p := powerFixture(t, peer)
			p.handle(pbtApmSuspend)
			p.handle(pbtApmResumeAutomatic)
			p.handle(pbtApmResumeSuspend)
			state := "running"
			if tc.command == "stop" && tc.lost {
				state = "paused"
			}
			if tc.command == "stop" {
				peer.assert(t, state, "stop")
			} else {
				peer.assert(t, state)
			}
			if p.client != nil || p.owned {
				t.Fatal("retained failed ownership")
			}
		})
	}
}

func TestPowerResumeFailureAndRetry(t *testing.T) {
	for _, lost := range []bool{false, true} {
		t.Run(fmt.Sprint(lost), func(t *testing.T) {
			peer := &powerPeer{state: "running"}
			p := powerFixture(t, peer)
			p.handle(pbtApmSuspend)
			peer.mu.Lock()
			peer.fail = "cont"
			peer.lost = lost
			peer.mu.Unlock()
			p.handle(pbtApmResumeAutomatic)
			peer.assert(t, "paused", "stop", "cont")
			peer.mu.Lock()
			peer.fail = ""
			peer.mu.Unlock()
			p.handle(pbtApmResumeSuspend)
			peer.assert(t, "paused", "stop", "cont")
			if !p.recoveryNeeded {
				t.Fatal("missing explicit recovery state")
			}
			p.manualResume()
			peer.assert(t, "running", "stop", "cont", "cont")
		})
	}
}

func TestPowerRuntimeExitAndReplacement(t *testing.T) {
	peer := &powerPeer{state: "running"}
	p := powerFixture(t, peer)
	p.handle(pbtApmSuspend)
	p.client.Close()
	replacement := &powerPeer{state: "paused"}
	p.dial = func(context.Context) (*qmpClient, error) { return replacement.client(t), nil }
	p.handle(pbtApmResumeAutomatic)
	p.handle(pbtApmResumeSuspend)
	replacement.assert(t, "paused")
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	replacement.assert(t, "paused")
}

func TestPowerMissedSuspendAndPairedResume(t *testing.T) {
	now := time.Now()
	notices := 0
	p := &guestPowerState{now: func() time.Time { return now }, notifyResume: func() { notices++ }, dial: func(context.Context) (*qmpClient, error) { return nil, errors.New("not ready") }}
	p.handle(pbtApmResumeAutomatic)
	p.handle(pbtApmResumeSuspend)
	if notices != 1 {
		t.Fatalf("resume notifications=%d", notices)
	}
	now = now.Add(time.Minute)
	p.handle(pbtApmResumeAutomatic)
	if notices != 2 {
		t.Fatal("missed next suspend prevented time correction")
	}
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	if notices != 3 {
		t.Fatal("new suspend cycle was deduplicated")
	}
}

func TestPowerTimeoutAtEveryCommand(t *testing.T) {
	for step := 1; step <= 6; step++ {
		t.Run(fmt.Sprint(step), func(t *testing.T) {
			peer := &powerPeer{state: "running", stallAt: step}
			p := powerFixture(t, peer)
			p.stepBudget = 20 * time.Millisecond
			notified := 0
			phase := ""
			p.onRecovery = func(error) { notified++ }
			p.changed = func(s string) { phase = s }
			p.handle(pbtApmSuspend)
			if phase != "suspended" {
				t.Fatalf("phase during sleep=%s", phase)
			}
			p.handle(pbtApmResumeAutomatic)
			p.handle(pbtApmResumeSuspend)
			if step == 1 {
				if p.recoveryNeeded {
					t.Fatal("claimed pause before sending stop")
				}
			} else {
				if !p.recoveryNeeded || phase != "recovery" || notified != 1 {
					t.Fatalf("recovery=%t phase=%s snapshots=%d", p.recoveryNeeded, phase, notified)
				}
				p.manualResume()
				peer.mu.Lock()
				state := peer.state
				peer.mu.Unlock()
				if state != "running" || p.recoveryNeeded {
					t.Fatalf("manual recovery state=%s pending=%t", state, p.recoveryNeeded)
				}
			}
		})
	}
}

func TestPowerEachStepGetsFreshBudget(t *testing.T) {
	peer := &powerPeer{state: "running", delay: 20 * time.Millisecond}
	p := powerFixture(t, peer)
	p.stepBudget = 100 * time.Millisecond
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	peer.assert(t, "running", "stop", "cont")
	if p.recoveryNeeded {
		t.Fatal("shared deadline exhausted")
	}
}

func TestPowerRecoveryDoesNotResumeReplacement(t *testing.T) {
	peer := &powerPeer{state: "running", fail: "stop", lost: true}
	p := powerFixture(t, peer)
	id := uint64(1)
	p.identity = func() uint64 { return id }
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	replacement := &powerPeer{state: "paused"}
	p.dial = func(context.Context) (*qmpClient, error) { return replacement.client(t), nil }
	id++
	p.manualResume()
	replacement.assert(t, "paused")
	if p.recoveryNeeded {
		t.Fatal("old recovery state retained")
	}
}

func TestPowerDialTimeout(t *testing.T) {
	p := &guestPowerState{stepBudget: time.Millisecond, dial: func(ctx context.Context) (*qmpClient, error) { <-ctx.Done(); return nil, ctx.Err() }}
	p.handle(pbtApmSuspend)
	p.handle(pbtApmResumeAutomatic)
	if p.recoveryNeeded || p.client != nil {
		t.Fatal("dial failure claimed pause")
	}
	p.recoveryNeeded = true
	p.dial = func(context.Context) (*qmpClient, error) { return nil, errors.New("offline") }
	p.manualResume()
	if !p.recoveryNeeded {
		t.Fatal("manual retry hid a failure")
	}
}
