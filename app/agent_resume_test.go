package main

import (
	"testing"
	"time"
)

type fakeResumeTimer struct{ resets chan time.Duration }

func (f fakeResumeTimer) Stop() bool                 { return true }
func (f fakeResumeTimer) Reset(d time.Duration) bool { f.resets <- d; return true }

func TestAgentResumeRetryDoesNotBlockPeriodicWork(t *testing.T) {
	times, batteries, retries := make(chan time.Time), make(chan time.Time), make(chan time.Time)
	resumes, done, finished := make(chan struct{}), make(chan struct{}), make(chan struct{})
	events := make(chan string, 10)
	timer := fakeResumeTimer{make(chan time.Duration, 10)}
	go func() {
		defer close(finished)
		runAgentUpdates(times, batteries, resumes, done, retries, timer, func(reason string) { events <- "time:" + reason }, func() { events <- "battery" })
	}()
	defer func() { close(done); <-finished }()
	want := func(value string) {
		t.Helper()
		select {
		case got := <-events:
			if got != value {
				t.Fatalf("got %s want %s", got, value)
			}
		case <-time.After(time.Second):
			t.Fatalf("loop blocked waiting for %s", value)
		}
	}
	resumes <- struct{}{}
	want("time:resume")
	want("battery")
	if d := <-timer.resets; d != 5*time.Second {
		t.Fatalf("retry=%s", d)
	}
	times <- time.Now()
	want("time:")
	batteries <- time.Now()
	want("battery")
	resumes <- struct{}{}
	want("time:resume")
	want("battery")
	if d := <-timer.resets; d != 5*time.Second {
		t.Fatalf("reset=%s", d)
	}
	retries <- time.Now()
	want("time:resume")
	want("battery")
}
