package main

import (
	"reflect"
	"testing"
)

func TestForwardAdapterFollowsDHCPWithoutChangingSavedAddress(t *testing.T) {
	saved, err := parseForward("tcp:192.168.1.5:8080:80")
	if err != nil {
		t.Fatal(err)
	}
	bindings := map[string]string{saved.String(): "adapter-A"}
	resolved, err := resolveForwardAdapters([]portForward{saved}, bindings, []lanAdapter{{Name: "Ethernet", Address: "192.168.1.20", Identity: "ADAPTER-a"}})
	if err != nil {
		t.Fatal(err)
	}
	if resolved[0].bind != "192.168.1.20" || saved.bind != "192.168.1.5" {
		t.Fatal("did not resolve DHCP address independently")
	}
	if _, err := resolveForwardAdapters([]portForward{saved}, bindings, nil); err == nil {
		t.Fatal("accepted missing adapter")
	}
}

func TestForwardAdapterPreservesSelectedAlias(t *testing.T) {
	saved, _ := parseForward("tcp:192.168.1.5:8080:80")
	resolved, err := resolveForwardAdapters([]portForward{saved}, map[string]string{saved.String(): "a"}, []lanAdapter{{Address: "192.168.1.2", Identity: "a"}, {Address: "192.168.1.5", Identity: "a"}, {Address: "192.168.1.8", Identity: "a"}})
	if err != nil || resolved[0].bind != saved.bind {
		t.Fatalf("lost selected alias: %v %v", resolved, err)
	}
}

func TestForwardAdapterRejectsResolvedCollision(t *testing.T) {
	first, _ := parseForward("tcp:192.168.1.5:8080:80")
	second, _ := parseForward("tcp:192.168.1.20:8080:81")
	if _, err := resolveForwardAdapters([]portForward{first, second}, map[string]string{first.String(): "a"}, []lanAdapter{{Address: "192.168.1.20", Identity: "a"}}); err == nil {
		t.Fatal("accepted conflicting resolved bindings")
	}
}

func TestFilterUnavailableForwardAdapters(t *testing.T) {
	local, _ := parseForward("tcp:2222:22")
	dock, _ := parseForward("tcp:192.168.1.5:8080:80")
	dockUDP, _ := parseForward("udp:192.168.1.5:8081:81")
	wifi, _ := parseForward("tcp:192.168.2.5:8080:80")
	wildcard, _ := parseForward("tcp:0.0.0.0:9000:90")
	saved := []portForward{local, dock, wifi, dockUDP, wildcard}
	original := append([]portForward(nil), saved...)
	bindings := map[string]string{dock.String(): "dock", dockUDP.String(): "dock", wifi.String(): "wifi"}
	active, paused := filterUnavailableForwardAdapters(saved, bindings, []lanAdapter{{Address: "192.168.2.20", Identity: "WIFI"}, {Address: "0.0.0.0"}})
	if !reflect.DeepEqual(active, []portForward{local, wifi, wildcard}) || !reflect.DeepEqual(paused, []portForward{dock, dockUDP}) {
		t.Fatalf("active=%v paused=%v", active, paused)
	}
	resolved, err := resolveForwardAdapters(active, bindings, []lanAdapter{{Address: "192.168.2.20", Identity: "WIFI"}})
	if err != nil || len(resolved) != 3 || resolved[1].bind != "192.168.2.20" {
		t.Fatalf("unaffected forwards failed: %v %v", resolved, err)
	}
	if !reflect.DeepEqual(saved, original) || bindings[dock.String()] != "dock" || bindings[dockUDP.String()] != "dock" {
		t.Fatal("changed saved preferences")
	}
	active, paused = filterUnavailableForwardAdapters(saved, bindings, []lanAdapter{{Address: "192.168.1.20", Identity: "DOCK"}, {Address: "192.168.2.20", Identity: "wifi"}})
	if len(paused) != 0 || !reflect.DeepEqual(active, saved) {
		t.Fatalf("rules did not resume: active=%v paused=%v", active, paused)
	}
}

func TestFilterAllLANAdaptersMissingKeepsLocalAndNeverWidens(t *testing.T) {
	dock, _ := parseForward("tcp:192.168.1.5:8080:80")
	local, _ := parseForward("tcp:2222:22")
	bindings := map[string]string{dock.String(): "dock"}
	active, paused := filterUnavailableForwardAdapters([]portForward{dock, local}, bindings, nil)
	if !reflect.DeepEqual(active, []portForward{local}) || !reflect.DeepEqual(paused, []portForward{dock}) {
		t.Fatalf("active=%v paused=%v", active, paused)
	}
	active, paused = filterUnavailableForwardAdapters([]portForward{dock}, bindings, nil)
	if len(active) != 0 || len(paused) != 1 || paused[0].bind != dock.bind {
		t.Fatalf("missing adapter acquired a binding: active=%v paused=%v", active, paused)
	}
}
