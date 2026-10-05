//go:build windows

package main

import (
	"fmt"
	"net/http"
	"net/url"
	"os"
	"syscall"
	"time"
	"unsafe"
)

var (
	winhttp                     = syscall.NewLazyDLL("winhttp.dll")
	procWinHTTPGetIEProxyConfig = winhttp.NewProc("WinHttpGetIEProxyConfigForCurrentUser")
	procWinHTTPOpen             = winhttp.NewProc("WinHttpOpen")
	procWinHTTPClose            = winhttp.NewProc("WinHttpCloseHandle")
	procWinHTTPTimeouts         = winhttp.NewProc("WinHttpSetTimeouts")
	procWinHTTPGetProxy         = winhttp.NewProc("WinHttpGetProxyForUrl")
	procGlobalFreeProxy         = kernel32.NewProc("GlobalFree")
	systemProxyGate             = make(chan struct{}, 1)
)

type ieProxyConfig struct {
	autoDetect             int32
	autoURL, proxy, bypass *uint16
}
type autoProxyOptions struct {
	flags, detectFlags uint32
	configURL          *uint16
	reserved           uintptr
	reservedFlags      uint32
	autoLogon          int32
}
type winHTTPProxyInfo struct {
	accessType    uint32
	proxy, bypass *uint16
}

func proxyString(p *uint16) string {
	if p == nil {
		return ""
	}
	return syscall.UTF16ToString(unsafe.Slice(p, proxyStringLength(p)))
}
func proxyStringLength(p *uint16) int {
	for i := 0; i < 32768; i++ {
		if *(*uint16)(unsafe.Add(unsafe.Pointer(p), i*2)) == 0 {
			return i
		}
	}
	return 32768
}
func freeProxyStrings(values ...*uint16) {
	for _, p := range values {
		if p != nil {
			procGlobalFreeProxy.Call(uintptr(unsafe.Pointer(p)))
		}
	}
}

func downloadProxy(req *http.Request) (*url.URL, error) {
	for _, name := range []string{"HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy"} {
		if os.Getenv(name) != "" {
			return http.ProxyFromEnvironment(req)
		}
	}
	// The synchronous Windows PAC API has no cancellation parameter. Bound the
	// caller and allow at most one native lookup in flight, even if WPAD stalls.
	timer := time.NewTimer(5 * time.Second)
	defer timer.Stop()
	select {
	case systemProxyGate <- struct{}{}:
	case <-req.Context().Done():
		return nil, req.Context().Err()
	case <-timer.C:
		return nil, fmt.Errorf("Windows proxy lookup timed out")
	}
	type result struct {
		proxy *url.URL
		err   error
	}
	done := make(chan result, 1)
	go func() {
		defer func() { <-systemProxyGate }()
		proxy, err := resolveWindowsProxy(req.URL)
		done <- result{proxy, err}
	}()
	select {
	case r := <-done:
		return r.proxy, r.err
	case <-req.Context().Done():
		return nil, req.Context().Err()
	case <-timer.C:
		return nil, fmt.Errorf("Windows proxy lookup timed out")
	}
}

func resolveWindowsProxy(target *url.URL) (*url.URL, error) {
	var cfg ieProxyConfig
	ok, _, err := procWinHTTPGetIEProxyConfig.Call(uintptr(unsafe.Pointer(&cfg)))
	if ok == 0 {
		return nil, fmt.Errorf("reading Windows proxy settings: %w", err)
	}
	defer freeProxyStrings(cfg.autoURL, cfg.proxy, cfg.bypass)
	if cfg.autoURL == nil && cfg.autoDetect == 0 {
		return systemProxyURL(target, proxyString(cfg.proxy), proxyString(cfg.bypass))
	}
	agent, _ := syscall.UTF16PtrFromString("Try Omarchy updater")
	session, _, err := procWinHTTPOpen.Call(uintptr(unsafe.Pointer(agent)), 1, 0, 0, 0)
	if session == 0 {
		return nil, fmt.Errorf("opening Windows proxy session: %w", err)
	}
	defer procWinHTTPClose.Call(session)
	if ok, _, err := procWinHTTPTimeouts.Call(session, 4000, 4000, 4000, 4000); ok == 0 {
		return nil, fmt.Errorf("setting Windows proxy timeouts: %w", err)
	}
	options := autoProxyOptions{configURL: cfg.autoURL}
	if cfg.autoURL != nil {
		options.flags = 2
	} else {
		options.flags = 1
		options.detectFlags = 3
	}
	source, _ := syscall.UTF16PtrFromString(target.String())
	var info winHTTPProxyInfo
	ok, _, err = procWinHTTPGetProxy.Call(session, uintptr(unsafe.Pointer(source)), uintptr(unsafe.Pointer(&options)), uintptr(unsafe.Pointer(&info)))
	if ok == 0 {
		// WPAD finding no configuration is normal on a direct network. A failed
		// explicit PAC configuration must not silently bypass the user's policy.
		if cfg.autoURL == nil && err == syscall.Errno(12180) {
			return systemProxyURL(target, proxyString(cfg.proxy), proxyString(cfg.bypass))
		}
		return nil, fmt.Errorf("resolving Windows automatic proxy: %w", err)
	}
	defer freeProxyStrings(info.proxy, info.bypass)
	if info.accessType == 1 {
		return nil, nil
	}
	return systemProxyURL(target, proxyString(info.proxy), proxyString(info.bypass))
}
