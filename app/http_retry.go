package main

import (
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"time"
)

func getWithSetupRetry(client *http.Client, source string, attempts int) (*http.Response, error) {
	if attempts < 1 {
		attempts = 1
	}
	var lastErr error
	for attempt := 0; attempt < attempts; attempt++ {
		req, err := http.NewRequestWithContext(setupContext(), http.MethodGet, source, nil)
		if err != nil {
			return nil, err
		}
		resp, err := client.Do(req)
		if err == nil && !retryableHTTPStatus(resp.StatusCode) {
			return resp, nil
		}
		if err == nil {
			lastErr = fmt.Errorf("HTTP %d", resp.StatusCode)
			resp.Body.Close()
		} else {
			lastErr = err
		}
		if attempt+1 == attempts {
			break
		}
		delay := time.Second << min(attempt, 3)
		if err := sleepDuringSetup(delay); err != nil {
			return nil, err
		}
	}
	return nil, lastErr
}

func retryableHTTPStatus(status int) bool {
	return status == http.StatusRequestTimeout || status == http.StatusTooManyRequests || status >= 500
}

func setupFailureHelp(err error) string {
	var dnsErr *net.DNSError
	if errors.As(err, &dnsErr) {
		return uiText("setup.help.dns")
	}
	// Setup writes several GB, and an image update needs room for the old and
	// new copies at once. Retrying on connection advice never clears that.
	if errors.Is(err, errInsufficientDiskSpace) || isDiskFull(err) {
		return uiText("setup.help.disk_full")
	}
	if errors.Is(err, os.ErrPermission) {
		return uiText("setup.help.permission")
	}
	var networkErr net.Error
	if errors.As(err, &networkErr) {
		return uiText("setup.help.network")
	}
	return uiText("setup.help.retry")
}
