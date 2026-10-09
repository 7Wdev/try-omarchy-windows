package main

import (
	"context"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync/atomic"
	"testing"
	"time"
)

func TestDownloadDialSeparatesLookupAndConnectBudgets(t *testing.T) {
	d := newDownloadDialer()
	if d.lookupTimeout != 30*time.Second || d.connectTimeout != 10*time.Second {
		t.Fatal("wrong production budgets", d)
	}
	d.lookupTimeout = 100 * time.Millisecond
	d.connectTimeout = 10 * time.Millisecond
	d.lookup = func(ctx context.Context, host string) ([]net.IPAddr, error) {
		deadline, _ := ctx.Deadline()
		if time.Until(deadline) < 50*time.Millisecond {
			t.Fatal("lookup used TCP budget")
		}
		time.Sleep(20 * time.Millisecond) // longer than the entire TCP budget
		return []net.IPAddr{{IP: net.ParseIP("127.0.0.1")}}, nil
	}
	sentinel := errors.New("connect result")
	d.dial = func(ctx context.Context, network, address string) (net.Conn, error) {
		deadline, _ := ctx.Deadline()
		if left := time.Until(deadline); left <= 0 || left > 10*time.Millisecond {
			t.Fatal("connect budget", left)
		}
		if address != "127.0.0.1:443" {
			t.Fatal(address)
		}
		return nil, sentinel
	}
	if _, err := d.DialContext(context.Background(), "tcp", "github.com:443"); !errors.Is(err, sentinel) {
		t.Fatal(err)
	}
}

func TestDownloadLookupRetriesOnceAndCancels(t *testing.T) {
	for _, cancelParent := range []bool{false, true} {
		ctx, cancel := context.WithCancel(context.Background())
		calls := 0
		d := newDownloadDialer()
		d.lookupTimeout = time.Millisecond
		d.lookup = func(ctx context.Context, _ string) ([]net.IPAddr, error) {
			calls++
			if cancelParent {
				cancel()
			}
			<-ctx.Done()
			return nil, &net.DNSError{IsTimeout: true}
		}
		d.dial = func(context.Context, string, string) (net.Conn, error) {
			t.Fatal("dial after lookup failure")
			return nil, nil
		}
		_, err := d.DialContext(ctx, "tcp", "github.com:443")
		cancel()
		want := 2
		if cancelParent {
			want = 1
		}
		if err == nil || calls != want {
			t.Fatalf("calls=%d err=%v", calls, err)
		}
	}
}

func TestDownloadDialHappyEyeballs(t *testing.T) {
	d := newDownloadDialer()
	d.fallbackDelay = time.Millisecond
	d.lookup = func(context.Context, string) ([]net.IPAddr, error) {
		return []net.IPAddr{{IP: net.ParseIP("::1")}, {IP: net.ParseIP("127.0.0.1")}}, nil
	}
	var v6 atomic.Bool
	cleaned := make(chan struct{})
	client, server := net.Pipe()
	defer server.Close()
	d.dial = func(ctx context.Context, network, address string) (net.Conn, error) {
		if address == "[::1]:443" {
			v6.Store(true)
			<-ctx.Done()
			close(cleaned)
			return nil, ctx.Err()
		}
		return client, nil
	}
	conn, err := d.DialContext(context.Background(), "tcp", "github.com:443")
	if err != nil || conn != client || !v6.Load() {
		t.Fatalf("conn=%v err=%v", conn, err)
	}
	conn.Close()
	select {
	case <-cleaned:
	case <-time.After(time.Second):
		t.Fatal("losing dial not cancelled")
	}
}

func TestDownloadDialUsesProxyWithoutResolvingTarget(t *testing.T) {
	proxy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Host != "unresolvable.test" {
			t.Errorf("proxy target=%s", r.URL)
		}
		w.WriteHeader(http.StatusNoContent)
	}))
	defer proxy.Close()
	endpoint, _ := url.Parse(proxy.URL)
	endpoint.Host = net.JoinHostPort("proxy.test", endpoint.Port())
	d := newDownloadDialer()
	d.lookup = func(ctx context.Context, host string) ([]net.IPAddr, error) {
		if host != "proxy.test" {
			t.Errorf("resolved target instead of proxy: %s", host)
		}
		return []net.IPAddr{{IP: net.ParseIP("127.0.0.1")}}, nil
	}
	client := newDownloadClient()
	defer client.CloseIdleConnections()
	transport := client.Transport.(*http.Transport)
	transport.Proxy = http.ProxyURL(endpoint)
	transport.DialContext = d.DialContext
	resp, err := client.Get("http://unresolvable.test/payload")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNoContent {
		t.Fatal(resp.StatusCode)
	}
}
