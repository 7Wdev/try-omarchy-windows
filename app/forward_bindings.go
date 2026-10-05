package main

import (
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
)

// Keep successful probes open until the set has been checked, then release
// them for QEMU. A bind race is handled from QEMU's own startup diagnostic.
func filterForwardBindings(forwards []portForward) (active, paused []portForward) {
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
			paused = append(paused, forward)
			continue
		}
		opened = append(opened, socket)
		active = append(active, forward)
	}
	return
}

var pausedLaunchForwards struct {
	sync.Mutex
	rules []portForward
	lan   bool
}

func pauseLaunchForwards(forwards []portForward, lan bool) {
	pausedLaunchForwards.Lock()
	defer pausedLaunchForwards.Unlock()
	pausedLaunchForwards.rules = append(pausedLaunchForwards.rules, forwards...)
	pausedLaunchForwards.lan = pausedLaunchForwards.lan || lan
}

func filterPausedLaunchForwards(forwards []portForward) (active []portForward) {
	pausedLaunchForwards.Lock()
	defer pausedLaunchForwards.Unlock()
	for _, forward := range forwards {
		paused := pausedLaunchForwards.lan && forward.exposedToLAN()
		for _, rule := range pausedLaunchForwards.rules {
			if canonicalForward(rule) == canonicalForward(forward) {
				paused = true
				break
			}
		}
		if !paused {
			active = append(active, forward)
		}
	}
	return
}

func filterLANForwards(forwards []portForward) (active, paused []portForward) {
	for _, forward := range forwards {
		if forward.exposedToLAN() {
			paused = append(paused, forward)
		} else {
			active = append(active, forward)
		}
	}
	return
}

func failedStartupForwards(dir string, forwards []portForward) (active, paused []portForward) {
	data, err := readForwardStartupLog(dir)
	if err != nil {
		return append([]portForward(nil), forwards...), nil
	}
	for _, forward := range forwards {
		diagnostic := "Could not set up host forwarding rule '" + hostfwdRule(forward) + "-:" + strconv.Itoa(forward.guestPort) + "'"
		if strings.Contains(string(data), diagnostic) {
			paused = append(paused, forward)
		} else {
			active = append(active, forward)
		}
	}
	return
}
func readForwardStartupLog(dir string) ([]byte, error) {
	f, err := os.Open(filepath.Join(dir, "qemu-stderr.log"))
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return io.ReadAll(io.LimitReader(f, 64<<10))
}

func forwardStartupProblem(dir string) bool {
	data, err := readForwardStartupLog(dir)
	return err == nil && strings.Contains(string(data), "Could not set up host forwarding rule")
}
