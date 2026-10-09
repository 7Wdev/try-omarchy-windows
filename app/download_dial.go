package main

import (
	"context"
	"net"
	"net/netip"
	"time"
)

const (
	downloadLookupTimeout   = 30 * time.Second
	downloadConnectTimeout  = 10 * time.Second
	downloadMetadataTimeout = 3 * time.Minute
)

type downloadDialer struct {
	lookup                                       func(context.Context, string) ([]net.IPAddr, error)
	dial                                         func(context.Context, string, string) (net.Conn, error)
	lookupTimeout, connectTimeout, fallbackDelay time.Duration
}

func newDownloadDialer() downloadDialer {
	d := &net.Dialer{Timeout: downloadConnectTimeout, KeepAlive: 30 * time.Second}
	return downloadDialer{net.DefaultResolver.LookupIPAddr, d.DialContext,
		downloadLookupTimeout, downloadConnectTimeout, 300 * time.Millisecond}
}

// Resolve through the system resolver before starting the TCP budget. HTTP
// still sees the original hostname for TLS and resolves the proxy when used.
func (d downloadDialer) DialContext(ctx context.Context, network, address string) (net.Conn, error) {
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return nil, err
	}
	if _, err := netip.ParseAddr(host); err == nil {
		return d.dial(ctx, network, address)
	}
	var ips []net.IPAddr
	for attempt := 0; attempt < 2; attempt++ {
		lookupCtx, cancel := context.WithTimeout(ctx, d.lookupTimeout)
		ips, err = d.lookup(lookupCtx, host)
		cancel()
		if err == nil {
			break
		}
		if ctx.Err() != nil {
			return nil, ctx.Err()
		}
		if e, ok := err.(net.Error); !ok || (!e.Timeout() && !e.Temporary()) {
			return nil, err
		}
	}
	if err != nil {
		return nil, err
	}
	if len(ips) == 0 {
		return nil, &net.DNSError{Name: host, Err: "no addresses", IsNotFound: true}
	}
	primary, fallback := []string{}, []string{}
	firstV4 := ips[0].IP.To4() != nil
	for _, ip := range ips {
		v4 := ip.IP.To4() != nil
		if network == "tcp4" && !v4 || network == "tcp6" && v4 {
			continue
		}
		target := net.JoinHostPort(ip.String(), port)
		if v4 == firstV4 {
			primary = append(primary, target)
		} else {
			fallback = append(fallback, target)
		}
	}
	if len(primary) == 0 {
		primary, fallback = fallback, nil
	}
	if len(primary) == 0 {
		return nil, &net.DNSError{Name: host, Err: "no addresses for network", IsNotFound: true}
	}
	connectCtx, cancel := context.WithTimeout(ctx, d.connectTimeout)
	defer cancel()
	type result struct {
		conn net.Conn
		err  error
	}
	results := make(chan result)
	// Each family tries its addresses in order. Race the other family after
	// the usual Happy Eyeballs delay, or immediately if the first family fails.
	start := func(addresses []string) {
		go func() {
			var r result
			for index, target := range addresses {
				// Like net.Dialer, leave time for the remaining addresses
				// instead of letting one unreachable IP consume the family.
				deadline, _ := connectCtx.Deadline()
				remaining := time.Until(deadline)
				budget := remaining / time.Duration(len(addresses)-index)
				if minimum := 2 * time.Second; budget < minimum {
					budget = min(minimum, remaining)
				}
				addressCtx, cancelAddress := context.WithTimeout(connectCtx, budget)
				r.conn, r.err = d.dial(addressCtx, network, target)
				cancelAddress()
				if r.err == nil || connectCtx.Err() != nil {
					break
				}
			}
			select {
			case results <- r:
			case <-connectCtx.Done():
				if r.conn != nil {
					r.conn.Close()
				}
			}
		}()
	}
	start(primary)
	timer := time.NewTimer(d.fallbackDelay)
	defer timer.Stop()
	pending := 1
	startedFallback := len(fallback) == 0
	var firstErr error
	for {
		select {
		case r := <-results:
			pending--
			if r.err == nil {
				return r.conn, nil
			}
			if firstErr == nil {
				firstErr = r.err
			}
			if !startedFallback {
				start(fallback)
				pending++
				startedFallback = true
			}
			if pending == 0 {
				return nil, firstErr
			}
		case <-timer.C:
			if !startedFallback {
				start(fallback)
				pending++
				startedFallback = true
			}
		case <-connectCtx.Done():
			return nil, connectCtx.Err()
		}
	}
}
