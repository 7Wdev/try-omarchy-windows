//go:build windows

package main

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
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

func TestLANFirewallElevationOutcomes(t *testing.T) {
	forward, _ := parseForward("tcp:192.168.1.5:9000:80")
	plan, err := makeLANFirewallPlan(t.TempDir(), filepath.Join(t.TempDir(), "runtime", "bin", lanFirewallQEMUName), false, []portForward{forward})
	if err != nil {
		t.Fatal(err)
	}
	for _, test := range []struct {
		name       string
		code       int
		elevateErr error
		checks     int
		recheckErr error
		wantErr    bool
	}{
		{name: "already matches", checks: 1},
		{name: "approved", checks: 2},
		{name: "declined", code: errorCancelled, checks: 1, wantErr: true},
		{name: "helper failed", code: 1, checks: 1, wantErr: true},
		{name: "elevation failed", elevateErr: errors.New("ShellExecuteEx failed"), checks: 1, wantErr: true},
		{name: "verification failed", checks: 2, recheckErr: errors.New("rules missing"), wantErr: true},
	} {
		t.Run(test.name, func(t *testing.T) {
			calls := 0
			elevations := 0
			execute := func(p lanFirewallPlan, apply bool) error {
				calls++
				if apply || p.Group != plan.Group {
					t.Fatal("unexpected direct apply")
				}
				if calls == 1 && test.name != "already matches" {
					return errors.New("mismatch")
				}
				return test.recheckErr
			}
			elevate := func(args string) (int, error) {
				elevations++
				if !strings.HasPrefix(args, "-firewall-plan ") {
					t.Fatal("wrong helper")
				}
				return test.code, test.elevateErr
			}
			err := ensureLANFirewallPlan(plan, execute, elevate, func() error { return nil })
			if (err != nil) != test.wantErr || calls != test.checks {
				t.Fatalf("checks %d error %v", calls, err)
			}
			if test.name == "already matches" && elevations != 0 {
				t.Fatal("asked for unnecessary UAC")
			}
		})
	}
	called := false
	invalid := plan
	invalid.Program = `C:\Windows\System32\cmd.exe`
	if err := ensureLANFirewallPlan(invalid, func(lanFirewallPlan, bool) error { called = true; return nil }, func(string) (int, error) { called = true; return 0, nil }, func() error { return nil }); err == nil || called {
		t.Fatal("invalid program reached runner")
	}
	if err := ensureLANFirewallPlan(plan, func(lanFirewallPlan, bool) error { return errors.New("mismatch") }, func(string) (int, error) { t.Fatal("elevated after cancellation"); return 0, nil }, func() error { return errSetupCancelled }); !errors.Is(err, errSetupCancelled) {
		t.Fatal("ignored cancellation")
	}
}

func TestLANFirewallApplyRefusesNonRegularProgram(t *testing.T) {
	for _, kind := range []string{"directory", "symlink"} {
		t.Run(kind, func(t *testing.T) {
			dir := t.TempDir()
			program := filepath.Join(dir, "runtime", "bin", lanFirewallQEMUName)
			os.MkdirAll(filepath.Dir(program), 0755)
			if kind == "directory" {
				os.Mkdir(program, 0755)
			} else {
				target := filepath.Join(dir, "target.exe")
				os.WriteFile(target, []byte("fake"), 0600)
				if err := os.Symlink(target, program); err != nil {
					t.Skip(err)
				}
			}
			forward, _ := parseForward("tcp:192.168.1.5:9000:80")
			plan, err := makeLANFirewallPlan(dir, program, false, []portForward{forward})
			if err != nil {
				t.Fatal(err)
			}
			if err := executeLANFirewall(plan, true); err == nil {
				t.Fatal("accepted non-regular program")
			}
		})
	}
}

func TestLANFirewallEncodedRefusals(t *testing.T) {
	for _, value := range []string{strings.Repeat("a", 28001), "bad=", base64.RawURLEncoding.EncodeToString([]byte("{")), base64.RawURLEncoding.EncodeToString([]byte(`{"group":"OtherApplication","generation":"0000000000000000","rules":[]}`))} {
		if err := applyEncodedLANFirewall(value); err == nil {
			t.Fatal("accepted invalid encoded plan")
		}
	}
}

func TestPortableFirewallProcessRunning(t *testing.T) {
	if !portableFirewallProcessRunning(os.Getpid()) {
		t.Fatal("current launcher appears stale")
	}
	if portableFirewallProcessRunning(int(uint32(0xffffffff))) {
		t.Fatal("nonexistent PID appears running")
	}
}

func TestLANFirewallCleanupQueryErrors(t *testing.T) {
	owner := testPortableFirewallOwner(t)
	data, err := json.Marshal(owner.Plan)
	if err != nil {
		t.Fatal(err)
	}
	for _, test := range []struct {
		category string
		success  bool
	}{{"ObjectNotFound", true}, {"ResourceUnavailable", false}, {"PermissionDenied", false}} {
		t.Run(test.category, func(t *testing.T) {
			// Exercise the actual script without modifying this machine's firewall.
			mock := `function Get-NetFirewallRule { Write-Error -Message 'mock query failure' -Category ` + test.category + ` };` + "\n"
			cmd := exec.Command(system32("WindowsPowerShell\\v1.0\\powershell.exe"), "-NoProfile", "-NonInteractive", "-Command", mock+lanFirewallScript)
			cmd.Env = append(os.Environ(), "TRYOMARCHY_FIREWALL="+string(data), "TRYOMARCHY_FIREWALL_APPLY=1")
			configureDiskTool(cmd)
			output, err := cmd.CombinedOutput()
			if (err == nil) != test.success {
				t.Fatalf("cleanup result %v: %s", err, output)
			}
		})
	}
}
