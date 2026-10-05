package main

import (
	"context"
	"sync/atomic"
	"time"
)

const runtimeLoaderTimeout = 5 * time.Minute

var runtimeLoaderPreparing atomic.Bool

// The supervisor owns this state across retries and guest reboots. Even a
// failed preflight is consumed: it must not add five minutes to every retry.
type runtimeLoaderPreflight struct{ attempted bool }

func (p *runtimeLoaderPreflight) run(ctx context.Context, attempt int, executable string, timeout time.Duration, load func(context.Context, string) error) (bool, error) {
	if p.attempted || attempt != 1 {
		return false, nil
	}
	if err := ctx.Err(); err != nil {
		return false, err
	}
	p.attempted = true
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	err := load(ctx, executable)
	// CommandContext reports a killed process on cancellation. Preserve the
	// deadline/cancel reason so the log distinguishes it from a loader error.
	if ctx.Err() != nil {
		err = ctx.Err()
	}
	return true, err
}
