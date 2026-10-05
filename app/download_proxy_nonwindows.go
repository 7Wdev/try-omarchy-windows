//go:build !windows

package main

import (
	"net/http"
	"net/url"
)

func downloadProxy(req *http.Request) (*url.URL, error) { return http.ProxyFromEnvironment(req) }
