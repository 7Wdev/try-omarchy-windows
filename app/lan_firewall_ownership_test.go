package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

func testPortableFirewallOwner(t *testing.T) portableFirewallOwner {
	t.Helper()
	dir := t.TempDir()
	f, _ := parseForward("tcp:192.168.1.5:9000:80")
	p, err := makeLANFirewallPlan(dir, filepath.Join(dir, "runtime", "bin", lanFirewallQEMUName), false, []portForward{f})
	if err != nil {
		t.Fatal(err)
	}
	p.Rules = nil
	return portableFirewallOwner{Plan: p, PID: 123, Dir: dir}
}

func TestPortableFirewallCleanupRetainsFailedAndRunningOwners(t *testing.T) {
	root := t.TempDir()
	old, running := testPortableFirewallOwner(t), testPortableFirewallOwner(t)
	running.PID = 456
	for _, owner := range []portableFirewallOwner{old, running} {
		if err := savePortableFirewallOwner(root, owner); err != nil {
			t.Fatal(err)
		}
	}
	sentinel := filepath.Join(root, "unrelated.txt")
	os.WriteFile(sentinel, []byte("keep"), 0600)
	calls := 0
	fail := func(p lanFirewallPlan) error {
		calls++
		if p.Group != old.Plan.Group || len(p.Rules) != 0 {
			t.Fatal("wrong cleanup scope")
		}
		return errors.New("UAC declined")
	}
	if err := cleanupStalePortableFirewallOwners(root, func(pid int) bool { return pid == 456 }, fail); err == nil {
		t.Fatal("ignored refusal")
	}
	owners, err := loadPortableFirewallOwners(root)
	if err != nil || len(owners) != 2 || calls != 1 {
		t.Fatalf("lost cleanup record: %v %v", owners, err)
	}
	calls = 0
	if err := cleanupStalePortableFirewallOwners(root, func(pid int) bool { return pid == 456 }, func(p lanFirewallPlan) error {
		calls++
		if p.Group != old.Plan.Group {
			t.Fatal("removed running installation")
		}
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	owners, err = loadPortableFirewallOwners(root)
	if err != nil || len(owners) != 1 || owners[0].Plan.Group != running.Plan.Group || calls != 1 {
		t.Fatalf("cleanup scope: %v %v", owners, err)
	}
	if _, err := os.Stat(sentinel); err != nil {
		t.Fatal("removed unrelated file")
	}
	if err := cleanupPortableFirewallOwner(root, running, func(lanFirewallPlan) error { return nil }); err != nil {
		t.Fatal(err)
	}
	owners, err = loadPortableFirewallOwners(root)
	if err != nil || len(owners) != 0 {
		t.Fatal("explicit cleanup record remains")
	}
}

func TestPortableFirewallOwnerRefusals(t *testing.T) {
	valid := testPortableFirewallOwner(t)
	for _, mutate := range []func(*portableFirewallOwner){
		func(o *portableFirewallOwner) { o.Dir = "relative" }, func(o *portableFirewallOwner) { o.PID = 0 }, func(o *portableFirewallOwner) { o.PID = -1 }, func(o *portableFirewallOwner) { o.PID = int(int64(0x100000000)) }, func(o *portableFirewallOwner) { o.Plan.Group = "OtherApplication" }, func(o *portableFirewallOwner) { o.Plan.Generation = "bad" }, func(o *portableFirewallOwner) { o.Plan.Program = filepath.Join(t.TempDir(), "evil.exe") }, func(o *portableFirewallOwner) {
			o.Plan.Rules = []lanFirewallRule{{Protocol: "tcp", Address: "0.0.0.0", Port: 9000}}
		},
	} {
		owner := valid
		mutate(&owner)
		called := false
		if err := savePortableFirewallOwner(t.TempDir(), owner); err == nil {
			t.Fatal("saved invalid owner")
		}
		if err := cleanupPortableFirewallOwner(t.TempDir(), owner, func(lanFirewallPlan) error { called = true; return nil }); err == nil || called {
			t.Fatal("invalid owner reached helper")
		}
	}
	if err := savePortableFirewallOwner("relative", valid); err == nil {
		t.Fatal("accepted relative host inventory")
	}
	if _, err := loadPortableFirewallOwners("relative"); err == nil {
		t.Fatal("read relative host inventory")
	}
}

func TestPortableFirewallInventoryRefusesUnsafeFiles(t *testing.T) {
	for _, test := range []string{"malformed", "oversize", "directory", "wrong-name", "symlink", "root-link", "ancestor-link", "too-many", "invalid-owner", "root-file"} {
		t.Run(test, func(t *testing.T) {
			root := t.TempDir()
			owner := testPortableFirewallOwner(t)
			path := filepath.Join(root, owner.Plan.Group+".json")
			data, _ := json.Marshal(owner)
			switch test {
			case "malformed":
				data = []byte("{")
			case "oversize":
				data = make([]byte, 4097)
			case "invalid-owner":
				owner.PID = 0
				data, _ = json.Marshal(owner)
			case "root-file":
				root = filepath.Join(t.TempDir(), "file")
				os.WriteFile(root, []byte("file"), 0600)
			case "directory":
				os.Mkdir(path, 0700)
			case "wrong-name":
				path = filepath.Join(root, "TryOmarchy-wrong.json")
			case "symlink":
				target := filepath.Join(t.TempDir(), "owner")
				os.WriteFile(target, data, 0600)
				if err := os.Symlink(target, path); err != nil {
					t.Skip(err)
				}
			case "root-link", "ancestor-link":
				target := root
				root = filepath.Join(t.TempDir(), "link")
				if err := os.Symlink(target, root); err != nil {
					t.Skip(err)
				}
				if test == "ancestor-link" {
					root = filepath.Join(root, "child")
				}
			case "too-many":
				for i := 0; i < 129; i++ {
					os.WriteFile(filepath.Join(root, fmt.Sprintf("%d.json", i)), data, 0600)
				}
			}
			if test == "malformed" || test == "oversize" || test == "wrong-name" || test == "invalid-owner" {
				os.WriteFile(path, data, 0600)
			}
			called := false
			if err := cleanupStalePortableFirewallOwners(root, func(int) bool { return false }, func(lanFirewallPlan) error { called = true; return nil }); err == nil || called {
				t.Fatal("unsafe inventory reached cleanup")
			}
		})
	}
}

func TestPortableFirewallOwnerWriteRefusesLinks(t *testing.T) {
	owner := testPortableFirewallOwner(t)
	root := t.TempDir()
	target := filepath.Join(t.TempDir(), "untouched")
	os.WriteFile(target, []byte("keep"), 0600)
	if err := os.Symlink(target, filepath.Join(root, owner.Plan.Group+".json")); err != nil {
		t.Skip(err)
	}
	if err := savePortableFirewallOwner(root, owner); err == nil {
		t.Fatal("overwrote linked owner")
	}
	data, _ := os.ReadFile(target)
	if string(data) != "keep" {
		t.Fatal("modified link target")
	}
}

func TestLANFirewallDHCPChangeRetainsNarrowScope(t *testing.T) {
	dir := t.TempDir()
	program := filepath.Join(dir, "runtime", "bin", lanFirewallQEMUName)
	a, _ := parseForward("tcp:192.168.1.5:9000:80")
	b := a
	b.bind = "192.168.1.6"
	old, _ := makeLANFirewallPlan(dir, program, false, []portForward{a})
	next, _ := makeLANFirewallPlan(dir, program, false, []portForward{b})
	if old.Group != next.Group || old.Generation == next.Generation || next.Rules[0].Address != b.bind || next.Public {
		t.Fatal("DHCP change widened or lost rule ownership")
	}
}

func writePortableFirewallProgram(t *testing.T, owner portableFirewallOwner) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(owner.Plan.Program), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(owner.Plan.Program, []byte("QEMU"), 0600); err != nil {
		t.Fatal(err)
	}
}

func TestPortableFirewallSameInstallRelaunchSkipsCleanup(t *testing.T) {
	root := t.TempDir()
	owner := testPortableFirewallOwner(t)
	writePortableFirewallProgram(t, owner)
	if err := savePortableFirewallOwner(root, owner); err != nil {
		t.Fatal(err)
	}
	for launch := 0; launch < 2; launch++ {
		if err := cleanupStalePortableFirewallOwners(root, func(int) bool { return false }, func(lanFirewallPlan) error {
			t.Fatal("requested cleanup for the same portable installation")
			return nil
		}); err != nil {
			t.Fatal(err)
		}
	}
	owners, err := loadPortableFirewallOwners(root)
	if err != nil || len(owners) != 1 {
		t.Fatalf("lost persistent owner: %v %v", owners, err)
	}
}

func TestPortableFirewallGoneInstallCleanup(t *testing.T) {
	for _, scenario := range []string{"missing-program", "different-identity", "missing-identity", "legacy-missing-program", "legacy-existing-program", "invalid-identity"} {
		t.Run(scenario, func(t *testing.T) {
			root := t.TempDir()
			owner := testPortableFirewallOwner(t)
			if scenario != "missing-program" && scenario != "legacy-missing-program" {
				writePortableFirewallProgram(t, owner)
			}
			switch scenario {
			case "different-identity":
				other := testPortableFirewallOwner(t)
				data, err := os.ReadFile(filepath.Join(other.Dir, networkIdentityFilename))
				if err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join(owner.Dir, networkIdentityFilename), data, 0600); err != nil {
					t.Fatal(err)
				}
			case "missing-identity":
				if err := os.Remove(filepath.Join(owner.Dir, networkIdentityFilename)); err != nil {
					t.Fatal(err)
				}
			case "invalid-identity":
				if err := os.WriteFile(filepath.Join(owner.Dir, networkIdentityFilename), []byte("invalid"), 0600); err != nil {
					t.Fatal(err)
				}
			case "legacy-missing-program", "legacy-existing-program":
				owner.Dir = ""
			}
			if err := savePortableFirewallOwner(root, owner); err != nil {
				t.Fatal(err)
			}
			calls := 0
			err := cleanupStalePortableFirewallOwners(root, func(int) bool { return false }, func(plan lanFirewallPlan) error {
				calls++
				if plan.Group != owner.Plan.Group || len(plan.Rules) != 0 {
					t.Fatal("wrong cleanup scope")
				}
				return nil
			})
			want := 1
			if scenario == "legacy-existing-program" || scenario == "invalid-identity" {
				want = 0
			}
			if calls != want || (err != nil) != (scenario == "invalid-identity") {
				t.Fatalf("calls=%d err=%v", calls, err)
			}
		})
	}
}

func TestPortableFirewallCleanupOneOwnerPerLaunch(t *testing.T) {
	for _, outcome := range []string{"approved", "declined", "failed"} {
		t.Run(outcome, func(t *testing.T) {
			root := t.TempDir()
			for i := 0; i < 3; i++ {
				if err := savePortableFirewallOwner(root, testPortableFirewallOwner(t)); err != nil {
					t.Fatal(err)
				}
			}
			calls := 0
			apply := func(lanFirewallPlan) error {
				calls++
				if outcome != "approved" {
					return errors.New(outcome)
				}
				return nil
			}
			for launch := 0; launch < 2; launch++ {
				err := cleanupStalePortableFirewallOwners(root, func(int) bool { return false }, apply)
				if calls != launch+1 || (err != nil) != (outcome != "approved") {
					t.Fatalf("calls=%d err=%v", calls, err)
				}
			}
			owners, err := loadPortableFirewallOwners(root)
			want := 3
			if outcome == "approved" {
				want = 1
			}
			if err != nil || len(owners) != want {
				t.Fatalf("owners=%v err=%v", owners, err)
			}
		})
	}
}

func TestPortableFirewallCleanupFailureKeepsOwnForwards(t *testing.T) {
	lan, _ := parseForward("tcp:192.168.1.5:9000:80")
	local, _ := parseForward("tcp:8080:80")
	for _, ownFailure := range []bool{false, true} {
		root := t.TempDir()
		if err := savePortableFirewallOwner(root, testPortableFirewallOwner(t)); err != nil {
			t.Fatal(err)
		}
		calls, notices, setups := 0, 0, 0
		err := ensureLANFirewallAfterCleanup(func() error {
			return cleanupStalePortableFirewallOwners(root, func(int) bool { return false }, func(lanFirewallPlan) error {
				calls++
				return errors.New("UAC declined")
			})
		}, func(error) { notices++ }, func() error {
			setups++
			if ownFailure {
				return errors.New("own firewall failed")
			}
			return nil
		})
		active := []portForward{lan, local}
		if err != nil {
			active, _ = filterLANForwards(active)
		}
		want := 2
		if ownFailure {
			want = 1
		}
		if len(active) != want || calls != 1 || notices != 1 || setups != 1 || (err != nil) != ownFailure {
			t.Fatalf("active=%v cleanup=%d notices=%d setup=%d err=%v", active, calls, notices, setups, err)
		}
	}
}
