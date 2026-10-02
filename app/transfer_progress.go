package main

import (
	"context"
	"fmt"
	"sync"
	"sync/atomic"
	"time"
)

type transferProgress struct {
	ctx    context.Context
	cancel context.CancelFunc
	text   atomic.Value
	done   chan struct{}
	once   sync.Once
}

func newTransferProgress(label string) *transferProgress {
	ctx, cancel := context.WithCancel(context.Background())
	p := &transferProgress{ctx: ctx, cancel: cancel, done: make(chan struct{})}
	p.text.Store(label)
	return p
}
func (p *transferProgress) report(current, total int64, phase string) {
	text := phase
	if total > 0 {
		text = uiTextWith("progress.amount", map[string]string{"label": phase,
			"done": fmt.Sprintf("%.1f", float64(current)/(1<<20)), "total": fmt.Sprintf("%.1f", float64(total)/(1<<20))})
	}
	p.text.Store(text)
}
func (p *transferProgress) finish() { p.once.Do(func() { close(p.done) }) }
func (b *clipBridge) progress(label string) *transferProgress {
	p := newTransferProgress(label)
	if b.showTransfer != nil {
		go b.showTransfer(p)
	}
	return p
}
func monitorClipboardTransfer(p *transferProgress, s *fileTransferService, id string) {
	defer p.finish()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	timeout := time.NewTimer(2 * time.Hour)
	defer timeout.Stop()
	for {
		select {
		case <-p.ctx.Done():
			s.Cancel(id)
			return
		case <-p.done:
			return
		case <-timeout.C:
			s.Cancel(id)
			return
		case <-ticker.C:
			status, ok := s.Status(id)
			if !ok {
				return
			}
			if status.Phase != "" {
				p.report(status.Bytes, status.Total, status.Phase)
			}
			if status.State == "sent" {
				return
			}
		}
	}
}
