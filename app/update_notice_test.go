package main

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"path/filepath"
	"testing"
)

func TestUpdateStagingNoticeClassifiesFailures(t *testing.T) {
	dnsTimeout := &net.DNSError{Name: "github.com", IsTimeout: true}
	urlTimeout := &url.Error{Op: "Get", URL: "https://github.com/omacom/x.zip", Err: errors.New("dial tcp: lookup github.com: i/o timeout")}
	cases := []struct {
		name string
		err  error
		want bool
	}{
		{"no failure", nil, false},
		{"not enough disk space", fmt.Errorf("staging payload: %w", errInsufficientDiskSpace), false},
		{"launch shutting down", context.Canceled, false},
		{"shutdown wrapped", fmt.Errorf("staging payload: %w", context.Canceled), false},
		{"damaged manifest", errors.New("update is missing rootfs.ext4.zst"), false},
		{"network lookup timed out", dnsTimeout, true},
		{"network lookup wrapped", fmt.Errorf("checking update size: %w", dnsTimeout), true},
		{"request failed", urlTimeout, true},
		{"retries exhausted", fmt.Errorf("download failed after 4 attempts: %w", urlTimeout), true},
		{"retries exhausted on HTTP status", downloadFailure{errors.New("HTTP 503")}, true},
	}
	for _, tc := range cases {
		if got := updateStagingNotice(tc.err); got != tc.want {
			t.Errorf("%s: got %v, want %v", tc.name, got, tc.want)
		}
	}
}

func TestUpdateDownloadNoticeShowsOncePerLaunch(t *testing.T) {
	networkErr := &net.DNSError{Name: "github.com", IsTimeout: true}
	var notices updateDownloadNotices
	if !notices.shouldShow(networkErr) {
		t.Fatal("the first network failure of a launch should notify")
	}
	if notices.shouldShow(fmt.Errorf("retry: %w", networkErr)) {
		t.Fatal("a second network failure must stay quiet")
	}
	if notices.shouldShow(nil) {
		t.Fatal("no failure must not notify")
	}

	// A failure that should not notify must not consume the launch's one notice.
	var other updateDownloadNotices
	if other.shouldShow(errors.New("update is missing rootfs.ext4.zst")) {
		t.Fatal("a validation failure must not notify")
	}
	if other.shouldShow(errInsufficientDiskSpace) {
		t.Fatal("a full disk has its own message")
	}
	if !other.shouldShow(networkErr) {
		t.Fatal("a quiet failure must not consume the launch's notice")
	}
}

func TestDownloadFailuresKeepUpdateAndFactoryHints(t *testing.T) {
	for _, tc := range []struct {
		name                    string
		status                  int
		body                    string
		length                  string
		wantNotice, wantFactory bool
	}{
		{"service unavailable", 503, "", "", true, true},
		{"request timeout", 408, "", "", true, true},
		{"rate limited", 429, "", "", true, true},
		{"missing factory", 404, "", "", false, true},
		{"bad request", 400, "", "", false, false},
		{"truncated transfer", 200, "short", "100", true, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if tc.length != "" {
					w.Header().Set("Content-Length", tc.length)
				}
				w.WriteHeader(tc.status)
				_, _ = w.Write([]byte(tc.body))
			}))
			defer server.Close()
			_, smallErr := fetchSmallFileContext(context.Background(), server.Client(), server.URL, 1024)
			opts := fastDownloadOptions()
			opts.ctx = context.Background()
			opts.maxAttempts = 1
			downloadErr := downloadVerifiedWithOptions(server.Client(), server.URL, filepath.Join(t.TempDir(), "payload"), testSHA256([]byte("expected payload")), nil, opts)
			for _, err := range []error{smallErr, downloadErr} {
				if err == nil {
					t.Fatal("failure was not reported")
				}
				err = fmt.Errorf("staging: %w", err)
				if got := updateStagingNotice(err); got != tc.wantNotice {
					t.Errorf("update hint for %v = %v, want %v", err, got, tc.wantNotice)
				}
				if got := factoryUnavailable(err); got != tc.wantFactory {
					t.Errorf("factory hint for %v = %v, want %v", err, got, tc.wantFactory)
				}
			}
		})
	}
}
