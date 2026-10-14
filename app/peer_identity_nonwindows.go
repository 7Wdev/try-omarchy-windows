//go:build !windows

package main

import "net"

// Without a Windows owner table there is no way to know the peer.
func connectionFromSameUser(net.Conn) bool { return false }

func processFromSameUser(uint32) bool { return false }
