//go:build windows

package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLANFirewallProgramWindowsPaths(t *testing.T) {
	for _, program := range []string{
		`C:\Users\a\AppData\Local\TryOmarchy\runtime\bin\qemu-system-x86_64w.exe`,
		`C:\WINQ-EMU\bin\qemu-system-x86_64w.exe`,
		`C:\Program Files\qemu\qemu-system-x86_64w.exe`,
	} {
		if err := validateLANFirewallProgram(program); err != nil {
			t.Errorf("%s: %v", program, err)
		}
	}
	for _, program := range []string{
		`\\server\share\bin\qemu-system-x86_64w.exe`,
		`\\?\C:\runtime\bin\qemu-system-x86_64w.exe`,
		`C:\Windows\System32\cmd.exe`,
		`C:\Program Files\Other\qemu-system-x86_64w.exe`,
		`C:qemu\bin\qemu-system-x86_64w.exe`,
	} {
		if err := validateLANFirewallProgram(program); err == nil {
			t.Errorf("accepted firewall program %q", program)
		}
	}
}

func TestLANFirewallApplyRefusesMissingProgram(t *testing.T) {
	forward, _ := parseForward("tcp:0.0.0.0:59188:8080")
	program := filepath.Join(t.TempDir(), "runtime", "bin", lanFirewallQEMUName)
	plan, err := makeLANFirewallPlan(t.TempDir(), program, false, []portForward{forward})
	if err != nil {
		t.Fatal(err)
	}
	if err := executeLANFirewall(plan, true); err == nil {
		t.Fatal("applied rules for a program that does not exist")
	}
}

func TestNativeLANFirewallLifecycle(t *testing.T) {
	if os.Getenv("TRYOMARCHY_FIREWALL_TEST") != "1" {
		t.Skip("explicit disposable Windows firewall test")
	}
	configureSetupCancellation(false)
	self, err := os.ReadFile(os.Args[0])
	if err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(t.TempDir(), "runtime", "bin", lanFirewallQEMUName)
	if err := os.MkdirAll(filepath.Dir(program), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(program, self, 0o755); err != nil {
		t.Fatal(err)
	}
	forward, _ := parseForward("tcp:0.0.0.0:59188:8080")
	plan, err := makeLANFirewallPlan(t.TempDir(), program, false, []portForward{forward})
	if err != nil {
		t.Fatal(err)
	}
	sentinel, err := makeLANFirewallPlan(t.TempDir(), program, false, []portForward{forward})
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		plan.Rules = nil
		sentinel.Rules = nil
		if err := executeLANFirewall(plan, true); err != nil {
			t.Error(err)
		}
		if err := executeLANFirewall(sentinel, true); err != nil {
			t.Error(err)
		}
	}()
	if err := executeLANFirewall(sentinel, true); err != nil {
		t.Fatal(err)
	}
	if err := executeLANFirewall(plan, true); err != nil {
		t.Fatal(err)
	}
	if err := executeLANFirewall(plan, false); err != nil {
		t.Fatal("new rules did not match:", err)
	}
	plan.Rules = nil
	if err := executeLANFirewall(plan, true); err != nil {
		t.Fatal(err)
	}
	if err := executeLANFirewall(plan, false); err != nil {
		t.Fatal("removed rules remain:", err)
	}
	if err := executeLANFirewall(sentinel, false); err != nil {
		t.Fatal("changed another installation's rules:", err)
	}
}

func TestNativeLANAdapterDiscovery(t *testing.T) {
	configureSetupCancellation(false)
	adapters, err := availableLANAdapters()
	if err != nil {
		t.Fatal(err)
	}
	if len(adapters) == 0 || adapters[len(adapters)-1].Address != "0.0.0.0" {
		t.Fatal("missing all-adapters choice")
	}
	for _, adapter := range adapters[:len(adapters)-1] {
		if adapter.Name == "" || adapter.Identity == "" {
			t.Fatalf("missing stable adapter identity: %+v", adapter)
		}
		if _, err := parseForward("tcp:" + adapter.Address + ":8080:80"); err != nil {
			t.Fatal(err)
		}
	}
}
