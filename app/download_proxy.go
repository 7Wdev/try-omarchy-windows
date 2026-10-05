package main

import (
	"fmt"
	"net"
	"net/url"
	"path"
	"strings"
)

func systemProxyURL(target *url.URL, servers, bypass string) (*url.URL, error) {
	host := strings.ToLower(target.Hostname())
	for _, item := range strings.FieldsFunc(strings.ToLower(bypass), func(r rune) bool { return r == ';' || r == ' ' }) {
		if item == "<local>" && !strings.Contains(host, ".") && net.ParseIP(host) == nil {
			return nil, nil
		}
		if match, _ := path.Match(item, host); match {
			return nil, nil
		}
		if item == strings.ToLower(target.Host) {
			return nil, nil
		}
	}
	selected := ""
	for _, item := range strings.FieldsFunc(servers, func(r rune) bool { return r == ';' || r == ' ' }) {
		scheme, proxy, mapped := strings.Cut(item, "=")
		if mapped {
			if strings.EqualFold(scheme, target.Scheme) {
				selected = proxy
				break
			}
		} else if selected == "" {
			selected = item
		}
	}
	if selected == "" {
		return nil, nil
	}
	if !strings.Contains(selected, "://") {
		selected = "http://" + selected
	}
	proxy, err := url.Parse(selected)
	if err != nil || proxy.Host == "" || proxy.User != nil || proxy.Path != "" || (proxy.Scheme != "http" && proxy.Scheme != "https") {
		return nil, fmt.Errorf("Windows returned an unsupported proxy")
	}
	return proxy, nil
}
