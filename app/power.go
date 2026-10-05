package main

import (
	"context"
	"errors"
	"fmt"
	"time"
)

const (
	pbtApmSuspend         = 0x0004
	pbtApmResumeSuspend   = 0x0007
	pbtApmResumeAutomatic = 0x0012
	powerStepBudget       = 2 * time.Second
)

// One worker owns this state and its private monitor across sleep. Reconnecting
// loses manual STOP/RESUME events, so an interrupted command requires an
// explicit Resume action rather than guessing who owns a pause.
type guestPowerState struct {
	suspended        bool
	client           *qmpClient
	owned            bool
	dial             func(context.Context) (*qmpClient, error)
	identity         func() uint64
	runtime          uint64
	lastResume       time.Time
	now              func() time.Time
	ctx              context.Context
	stepBudget       time.Duration
	notifyResume     func()
	changed          func(string)
	onRecovery       func(error)
	recoveryNeeded   bool
	recoveryNotified bool
}

func (p *guestPowerState) phase(phase string) {
	if p.changed != nil {
		p.changed(phase)
	}
}

func (p *guestPowerState) close() {
	if p.client != nil {
		p.client.Close()
		p.client = nil
	}
	p.owned = false
}

func (p *guestPowerState) step(f func(context.Context) error) error {
	parent := p.ctx
	if parent == nil {
		parent = context.Background()
	}
	budget := p.stepBudget
	if budget == 0 {
		budget = powerStepBudget
	}
	ctx, cancel := context.WithTimeout(parent, budget)
	defer cancel()
	return f(ctx)
}

func (p *guestPowerState) call(command string, result any) error {
	return p.step(func(ctx context.Context) error { return p.client.Call(ctx, command, nil, result) })
}

func (p *guestPowerState) recover(err error) {
	logf("power: guest may be paused after host sleep: %v", err)
	p.close()
	p.recoveryNeeded = true
	if p.suspended {
		p.phase("suspended")
		return
	}
	p.phase("recovery")
	p.reportRecovery(err)
}

func (p *guestPowerState) reportRecovery(err error) {
	if p.recoveryNotified {
		return
	}
	p.recoveryNotified = true
	if p.onRecovery != nil {
		p.onRecovery(err)
	}
}

func (p *guestPowerState) handle(event uintptr) {
	switch event {
	case pbtApmSuspend:
		if p.suspended {
			return
		}
		p.suspended = true
		p.phase("suspended")
		p.pause()
	case pbtApmResumeAutomatic, pbtApmResumeSuspend:
		now := time.Now()
		if p.now != nil {
			now = p.now()
		}
		first := p.suspended || p.lastResume.IsZero() || now.Sub(p.lastResume) >= 5*time.Second
		p.suspended = false
		if first {
			p.lastResume = now
			p.phase("resuming")
			if p.notifyResume != nil {
				p.notifyResume()
			}
		}
		p.resume()
		if p.recoveryNeeded {
			p.phase("recovery")
			p.reportRecovery(fmt.Errorf("host sleep pause could not be reconciled"))
		} else {
			p.phase("running")
		}
	}
}

func (p *guestPowerState) pause() {
	if p.client != nil || p.recoveryNeeded {
		return
	}
	if p.identity != nil {
		p.runtime = p.identity()
	}
	err := p.step(func(ctx context.Context) (err error) { p.client, err = p.dial(ctx); return })
	if err != nil {
		logf("power: cannot pause for host sleep: %v", err)
		return
	}
	if p.identity != nil && p.identity() != p.runtime {
		p.close()
		return
	}
	var state vmRuntimeStatus
	if err = p.call("query-status", &state); err != nil {
		// No stop was sent, so this failure cannot have paused the guest.
		logf("power: cannot inspect guest before sleep: %v", err)
		p.close()
		return
	}
	if !state.Running || state.Status != "running" {
		p.close()
		return
	}
	if err = p.call("stop", nil); err != nil {
		var remote *qmpCommandError
		if errors.As(err, &remote) {
			p.close()
		} else {
			p.recover(err)
		}
		return
	}
	p.owned = true
	p.client.onEvent = func(event string) {
		if event == "STOP" || event == "RESUME" {
			p.owned = false
		}
	}
	if err = p.call("query-status", &state); err != nil {
		p.recover(err)
		return
	}
	if state.Status != "paused" || !p.owned {
		p.close()
		return
	}
	logf("power: paused the guest for host sleep")
}

func (p *guestPowerState) resume() {
	if p.recoveryNeeded && p.identity != nil && p.identity() != p.runtime {
		p.recoveryNeeded = false
		p.recoveryNotified = false
	}
	if p.client == nil {
		return
	}
	if p.identity != nil && p.identity() != p.runtime {
		p.close()
		return
	}
	var state vmRuntimeStatus
	if err := p.call("query-status", &state); err != nil {
		p.recover(err)
		return
	}
	if !p.owned || state.Running || state.Status != "paused" {
		p.close()
		return
	}
	if err := p.call("cont", nil); err != nil {
		p.recover(err)
		return
	}
	if err := p.call("query-status", &state); err != nil {
		p.recover(err)
		return
	}
	if !state.Running {
		p.recover(fmt.Errorf("resume returned state %s", state.Status))
		return
	}
	logf("power: resumed the guest after host sleep")
	p.close()
}

func (p *guestPowerState) manualResume() {
	if !p.recoveryNeeded {
		return
	}
	if p.identity != nil && (p.runtime == 0 || p.identity() != p.runtime) {
		p.recoveryNeeded = false
		p.recoveryNotified = false
		p.phase("running")
		return
	}
	p.phase("resuming")
	if err := p.step(func(ctx context.Context) (err error) { p.client, err = p.dial(ctx); return }); err != nil {
		p.recover(err)
		return
	}
	if p.identity != nil && p.identity() != p.runtime {
		p.close()
		p.recoveryNeeded = false
		p.recoveryNotified = false
		p.phase("running")
		return
	}
	var state vmRuntimeStatus
	if err := p.call("query-status", &state); err != nil {
		p.recover(err)
		return
	}
	if state.Status == "paused" {
		if err := p.call("cont", nil); err != nil {
			p.recover(err)
			return
		}
		if err := p.call("query-status", &state); err != nil {
			p.recover(err)
			return
		}
	}
	if !state.Running {
		p.recover(fmt.Errorf("guest state is %s", state.Status))
		return
	}
	p.close()
	p.recoveryNeeded = false
	p.recoveryNotified = false
	p.phase("running")
	if p.notifyResume != nil {
		p.notifyResume()
	}
}
