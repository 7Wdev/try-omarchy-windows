package main

import (
	"context"
	"errors"
	"net"
	"sync/atomic"
)

// updateStagingNotice reports whether a failed attempt to stage the Omarchy
// system update deserves a message to the user.
//
// Intentional skips (a disabled update check, portable policy, an update that
// is already current) never return an error at all, so they are quiet. A full
// disk has its own message, a cancelled launch is shutting down, and a local
// or validation problem is not something "check your connection" would fix.
// What is left are the download failures worth showing.
func updateStagingNotice(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, errInsufficientDiskSpace) || isDiskFull(err) {
		return false
	}
	if errors.Is(err, context.Canceled) {
		return false
	}
	return isDownloadFailure(err)
}

// isDownloadFailure reports whether err came from fetching update payloads
// over the network: a dial, DNS, TLS or timeout failure, a request that kept
// returning a retryable HTTP status, or a transfer that never finished.
// Connection errors surface as *url.Error, which itself satisfies net.Error;
// the retry loop marks the failures it exhausts. Local filesystem and
// validation errors stay false.
func isDownloadFailure(err error) bool {
	var downloadErr downloadFailure
	if errors.As(err, &downloadErr) {
		return true
	}
	var netErr net.Error
	return errors.As(err, &netErr)
}

// updateDownloadNotices shows at most one "could not download the update"
// message per launch, however many staging attempts fail. Each value keeps its
// own latch so the decision can be tested in isolation.
type updateDownloadNotices struct{ shown atomic.Bool }

func (n *updateDownloadNotices) shouldShow(err error) bool {
	return updateStagingNotice(err) && n.shown.CompareAndSwap(false, true)
}
