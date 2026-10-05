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
	return portableFirewallOwner{Plan: p, PID: 123}
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
		t.Fatal("clean shutdown record remains")
	}
}

func TestPortableFirewallOwnerRefusals(t *testing.T) {
	valid := testPortableFirewallOwner(t)
	for _, mutate := range []func(*portableFirewallOwner){
		func(o *portableFirewallOwner) { o.PID = 0 }, func(o *portableFirewallOwner) { o.PID = -1 }, func(o *portableFirewallOwner) { o.PID = int(int64(0x100000000)) }, func(o *portableFirewallOwner) { o.Plan.Group = "OtherApplication" }, func(o *portableFirewallOwner) { o.Plan.Generation = "bad" }, func(o *portableFirewallOwner) { o.Plan.Program = filepath.Join(t.TempDir(), "evil.exe") }, func(o *portableFirewallOwner) {
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
