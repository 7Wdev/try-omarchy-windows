package main

import (
	"context"
	"errors"
	"net"
	"reflect"
	"testing"
	"time"
)

func TestUpdateRetryScheduleAndRecovery(t *testing.T) {
	configureSetupCancellation(false)
	ctx := context.Background()
	calls, notices := 0, 0
	var latch updateDownloadNotices
	var delays []time.Duration
	err := retryUpdateStaging(ctx, func() error {
		calls++
		if calls == 8 {
			return nil
		}
		return &net.DNSError{IsTimeout: true}
	}, func(err error) {
		if latch.shouldShow(err) {
			notices++
		}
	}, func(_ context.Context, delay time.Duration) error { delays = append(delays, delay); return nil })
	want := []time.Duration{time.Minute, 2 * time.Minute, 5 * time.Minute, 10 * time.Minute, 15 * time.Minute, 15 * time.Minute, 15 * time.Minute}
	if err != nil || calls != 8 || notices != 1 || !reflect.DeepEqual(delays, want) {
		t.Fatalf("err=%v calls=%d notices=%d delays=%v", err, calls, notices, delays)
	}
}

func TestUpdateRetryLocalFailuresStop(t *testing.T) {
	for _, err := range []error{errors.New("checksum mismatch"), errInsufficientDiskSpace, &downloadHTTPError{status: 404}, errors.New("permission denied")} {
		calls := 0
		got := retryUpdateStaging(context.Background(), func() error { calls++; return err }, func(error) {}, func(context.Context, time.Duration) error { t.Fatal("local failure retried"); return nil })
		if got != err || calls != 1 {
			t.Fatalf("got=%v calls=%d", got, calls)
		}
	}
}

func TestUpdateRetryCancellation(t *testing.T) {
	configureSetupCancellation(false)
	ctx, cancel := context.WithCancel(context.Background())
	calls := 0
	err := retryUpdateStaging(ctx, func() error { calls++; return &net.DNSError{IsTimeout: true} }, func(error) { cancel() }, sleepWithContext)
	if !errors.Is(err, context.Canceled) || calls != 1 {
		t.Fatalf("err=%v calls=%d", err, calls)
	}
	err = retryUpdateStaging(ctx, func() error { t.Fatal("cancelled retry started"); return nil }, func(error) {}, sleepWithContext)
	if !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
}
