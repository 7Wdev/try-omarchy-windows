//go:build windows

package main

import (
	"context"
	"net/http"
	"net/http/httptest"
	"net/url"
	"syscall"
	"testing"
	"time"
)

func TestNativeWindowsPACResolution(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/x-ns-proxy-autoconfig")
		w.Write([]byte(`function FindProxyForURL(url, host) { return "PROXY 127.0.0.1:18081"; }`))
	}))
	defer server.Close()
	pac, err := syscall.UTF16PtrFromString(server.URL + "/test.pac")
	if err != nil {
		t.Fatal(err)
	}
	target, _ := url.Parse("https://github.com/omacom/try-omarchy-windows/releases/latest/download/update-v2.json")
	proxy, err := windowsProxyForConfig(target, &ieProxyConfig{autoURL: pac})
	if err != nil {
		t.Fatal(err)
	}
	if proxy == nil || proxy.String() != "http://127.0.0.1:18081" {
		t.Fatalf("PAC result: %v", proxy)
	}
}

func TestWindowsProxyLookupCancellationDoesNotAccumulate(t *testing.T) {
	for _, name := range []string{"HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy"} {
		t.Setenv(name, "")
	}
	old := windowsDownloadProxy
	release := make(chan struct{})
	entered := make(chan struct{})
	finished := make(chan struct{})
	windowsDownloadProxy = func(*url.URL) (*url.URL, error) { close(entered); <-release; close(finished); return nil, nil }
	t.Cleanup(func() { close(release); <-finished; windowsDownloadProxy = old })
	ctx, cancel := context.WithCancel(context.Background())
	req, _ := http.NewRequestWithContext(ctx, http.MethodGet, "https://github.com/update", nil)
	result := make(chan error, 1)
	go func() { _, err := downloadProxy(req); result <- err }()
	<-entered
	cancel()
	select {
	case err := <-result:
		if err != context.Canceled {
			t.Fatalf("cancel: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("PAC cancellation stalled")
	}
	// One stalled native call owns the gate. A second cancelled caller must
	// return without spawning another native resolver.
	ctx2, cancel2 := context.WithCancel(context.Background())
	cancel2()
	req2, _ := http.NewRequestWithContext(ctx2, http.MethodGet, "https://github.com/update", nil)
	if _, err := downloadProxy(req2); err != context.Canceled {
		t.Fatalf("second lookup: %v", err)
	}
}
