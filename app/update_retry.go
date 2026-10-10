package main

import (
	"context"
	"time"
)

func updateRetryDelay(failure int) time.Duration {
	schedule := [...]time.Duration{time.Minute, 2 * time.Minute, 5 * time.Minute, 10 * time.Minute}
	if failure >= len(schedule) {
		return 15 * time.Minute
	}
	return schedule[failure]
}

// Only network failures retry. The caller logs each failed attempt and owns
// the existing one-notice-per-launch latch. Waiting never holds staging locks.
func retryUpdateStaging(ctx context.Context, attempt func() error, failed func(error), wait func(context.Context, time.Duration) error) error {
	for failure := 0; ; failure++ {
		if err := ctx.Err(); err != nil {
			return err
		}
		err := attempt()
		if err == nil || ctx.Err() != nil {
			return err
		}
		failed(err)
		if !updateStagingNotice(err) {
			return err
		}
		if err := wait(ctx, updateRetryDelay(failure)); err != nil {
			return err
		}
	}
}
