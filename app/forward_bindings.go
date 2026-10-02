package main

import (
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Keep every probe open until the complete set has been checked, then release
// them for QEMU. A later bind race is still diagnosed from QEMU's own error.
func checkForwardBindings(forwards []portForward) error {
	var opened []io.Closer
	defer func() {
		for _, socket := range opened {
			socket.Close()
		}
	}()
	for _, forward := range forwards {
		address := net.JoinHostPort(forward.address(), strconv.Itoa(forward.hostPort))
		var socket io.Closer
		var err error
		if forward.proto == "udp" {
			socket, err = net.ListenPacket("udp4", address)
		} else {
			socket, err = net.Listen("tcp4", address)
		}
		if err != nil {
			return uiError(uiTextWith("error.forward.bind", map[string]string{"protocol": strings.ToUpper(forward.proto), "address": address, "error": err.Error()}), err)
		}
		opened = append(opened, socket)
	}
	return nil
}
func forwardStartupProblem(dir string) bool {
	f, err := os.Open(filepath.Join(dir, "qemu-stderr.log"))
	if err != nil {
		return false
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, 64<<10))
	if err != nil {
		return false
	}
	return strings.Contains(string(data), "Could not set up host forwarding rule")
}
