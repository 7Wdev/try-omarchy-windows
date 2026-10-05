package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLANFirewallPlanOwnershipAndScope(t *testing.T) {
	dir := t.TempDir()
	program := filepath.Join(dir, "runtime", "bin", "qemu-system-x86_64w.exe")
	local, _ := parseForward("tcp:8080:80")
	lan, _ := parseForward("tcp:192.168.1.5:9000:80")
	empty, err := makeLANFirewallPlan(dir, program, false, []portForward{local})
	if err != nil || empty.Group != "" {
		t.Fatal("created rules for a loopback-only install")
	}
	plan, err := makeLANFirewallPlan(dir, program, false, []portForward{local, lan})
	if err != nil {
		t.Fatal(err)
	}
	if len(plan.Rules) != 1 || plan.Rules[0].Port != 9000 || plan.Public {
		t.Fatal("incorrect LAN exposure")
	}
	again, err := makeLANFirewallPlan(dir, program, false, []portForward{lan})
	if err != nil || again.Group != plan.Group || again.Generation != plan.Generation {
		t.Fatal("unstable rule ownership")
	}
	removed, err := makeLANFirewallPlan(dir, program, false, nil)
	if err != nil || removed.Group != plan.Group || len(removed.Rules) != 0 {
		t.Fatal("lost ownership when removing forwards")
	}
	if err := os.WriteFile(filepath.Join(dir, networkIdentityFilename), []byte("other-app"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := makeLANFirewallPlan(dir, program, false, nil); err == nil {
		t.Fatal("accepted malformed ownership")
	}
}

func TestLANFirewallPlanOnlyAdmitsQEMU(t *testing.T) {
	root := t.TempDir()
	lan, _ := parseForward("tcp:192.168.1.5:9000:80")
	for _, program := range []string{
		filepath.Join(root, "runtime", "bin", "qemu-system-x86_64w.exe"),
		filepath.Join(root, "WINQ-EMU", "BIN", "QEMU-SYSTEM-X86_64W.EXE"),
	} {
		if _, err := makeLANFirewallPlan(t.TempDir(), program, false, []portForward{lan}); err != nil {
			t.Errorf("%s: %v", program, err)
		}
	}
	for _, program := range []string{
		"",
		"qemu-system-x86_64w.exe",
		filepath.Join("runtime", "bin", "qemu-system-x86_64w.exe"),
		filepath.Join(root, "runtime", "bin", "evil.exe"),
		filepath.Join(root, "runtime", "qemu-system-x86_64w.exe"),
		strings.Join([]string{root, "runtime", "bin", "..", "bin", "qemu-system-x86_64w.exe"}, string(filepath.Separator)),
		filepath.Join(root, "runtime", "bin", "qemu-system-x86_64w.exe") + string(filepath.Separator),
	} {
		if _, err := makeLANFirewallPlan(t.TempDir(), program, false, []portForward{lan}); err == nil {
			t.Errorf("accepted firewall program %q", program)
		}
	}
	// Removing every rule needs no program, as uninstall does.
	if _, err := makeLANFirewallPlan(t.TempDir(), "", false, nil); err != nil {
		t.Fatal(err)
	}
}
