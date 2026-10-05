package main

import (
	"fmt"
	"net"
	"os"
	"path/filepath"
	"testing"
)

func TestForwardBindingsRejectOccupiedTCPAndUDP(t *testing.T) {
	tcp, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer tcp.Close()
	udp, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer udp.Close()
	for _, forward := range []portForward{{proto: "tcp", hostPort: tcp.Addr().(*net.TCPAddr).Port, guestPort: 80}, {proto: "udp", hostPort: udp.LocalAddr().(*net.UDPAddr).Port, guestPort: 80}} {
		if _, paused := filterForwardBindings([]portForward{forward}); len(paused) != 1 {
			t.Fatal("accepted an occupied port")
		}
	}
}
func TestForwardBindingProbesReleaseSockets(t *testing.T) {
	probe, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := probe.Addr().String()
	port := probe.Addr().(*net.TCPAddr).Port
	probe.Close()
	if _, paused := filterForwardBindings([]portForward{{proto: "tcp", hostPort: port, guestPort: 80}}); len(paused) != 0 {
		t.Fatal(paused)
	}
	probe, err = net.Listen("tcp4", address)
	if err != nil {
		t.Fatal("probe kept QEMU's port occupied", err)
	}
	probe.Close()
}
func TestNetworkStartupErrorDoesNotMasqueradeAsGPUFailure(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "qemu-stderr.log")
	for _, test := range []struct {
		text  string
		match bool
	}{{"qemu: Could not set up host forwarding rule 'tcp:127.0.0.1:8080-:80'", true}, {"qemu: OpenGL initialization failed", false}} {
		if err := os.WriteFile(path, []byte(test.text), 0600); err != nil {
			t.Fatal(err)
		}
		if forwardStartupProblem(dir) != test.match {
			t.Fatal("incorrect startup diagnosis")
		}
	}
}

func TestForwardBindingsPauseOnlyOccupied(t *testing.T) {
	tcp, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer tcp.Close()
	udp, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer udp.Close()
	free, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := free.Addr().(*net.TCPAddr).Port
	free.Close()
	saved := []portForward{{proto: "tcp", hostPort: tcp.Addr().(*net.TCPAddr).Port, guestPort: 80}, {proto: "udp", hostPort: udp.LocalAddr().(*net.UDPAddr).Port, guestPort: 53}, {proto: "tcp", hostPort: port, guestPort: 8080}}
	original := append([]portForward(nil), saved...)
	active, paused := filterForwardBindings(saved)
	if len(active) != 1 || active[0] != saved[2] || len(paused) != 2 {
		t.Fatalf("active %v paused %v", active, paused)
	}
	for i := range saved {
		if saved[i] != original[i] {
			t.Fatal("modified saved rules")
		}
	}
	probe, err := net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		t.Fatal("probe not released", err)
	}
	probe.Close()
}

func TestForwardStartupRacePausesNamedRule(t *testing.T) {
	dir := t.TempDir()
	a, _ := parseForward("tcp:127.0.0.1:8080:80")
	b, _ := parseForward("udp:127.0.0.1:8080:53")
	for _, test := range []struct {
		text  string
		count int
	}{
		{"qemu: Could not set up host forwarding rule 'tcp:127.0.0.1:8080-:80'", 1},
		{"qemu: Could not set up host forwarding rule 'udp:127.0.0.1:8080-:53'", 1},
		{"qemu: Could not set up host forwarding rule 'tcp:127.0.0.1:8080-:81'", 0},
		{"qemu: OpenGL initialization failed", 0},
	} {
		os.WriteFile(filepath.Join(dir, "qemu-stderr.log"), []byte(test.text), 0600)
		active, paused := failedStartupForwards(dir, []portForward{a, b})
		if len(paused) != test.count || len(active)+len(paused) != 2 {
			t.Fatalf("%s: %v / %v", test.text, active, paused)
		}
	}
}

func TestPausedForwardsStayPausedForRebootAndLiveSettings(t *testing.T) {
	reset := func() {
		pausedLaunchForwards.Lock()
		pausedLaunchForwards.rules, pausedLaunchForwards.lan = nil, false
		pausedLaunchForwards.Unlock()
	}
	reset()
	defer reset()
	local, _ := parseForward("tcp:8080:80")
	other, _ := parseForward("tcp:8081:80")
	lan, _ := parseForward("tcp:192.168.1.5:9000:80")
	pauseLaunchForwards([]portForward{local}, false)
	pauseLaunchForwards([]portForward{lan}, true)
	explicit := local
	explicit.bind = "127.0.0.1"
	changed := lan
	changed.bind = "192.168.1.6"
	saved := []portForward{explicit, other, changed}
	got := filterPausedLaunchForwards(saved)
	if len(got) != 1 || got[0] != other {
		t.Fatalf("resumed paused rules: %v", got)
	}
	active, paused := filterLANForwards(saved)
	if len(active) != 2 || len(paused) != 1 {
		t.Fatal("firewall failure affected local forwards")
	}
	if saved[2] != changed {
		t.Fatal("modified saved rules")
	}
}
