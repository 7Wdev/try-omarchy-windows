package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// This inventory stays on the Windows host, not on the removable drive.
// It contains only deletion plans, never instructions to create allow rules.
type portableFirewallOwner struct {
	Plan lanFirewallPlan `json:"plan"`
	PID  int             `json:"pid"`
}

func (o portableFirewallOwner) validate() error {
	if o.PID <= 0 || int64(o.PID) > 0xffffffff || len(o.Plan.Rules) != 0 {
		return fmt.Errorf("invalid portable firewall owner")
	}
	if err := o.Plan.validate(); err != nil {
		return err
	}
	return validateLANFirewallProgram(o.Plan.Program)
}

func portableFirewallOwnerPath(root string, owner portableFirewallOwner) (string, error) {
	if err := owner.validate(); err != nil {
		return "", err
	}
	if !filepath.IsAbs(root) {
		return "", fmt.Errorf("firewall host inventory must be absolute")
	}
	path := filepath.Join(root, owner.Plan.Group+".json")
	return path, validateMovePath(path)
}

func savePortableFirewallOwner(root string, owner portableFirewallOwner) error {
	path, err := portableFirewallOwnerPath(root, owner)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(root, 0700); err != nil {
		return err
	}
	data, err := json.Marshal(owner)
	if err != nil {
		return err
	}
	f, err := os.CreateTemp(root, ".firewall-owner-")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	_, err = f.Write(data)
	if err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(f.Name(), path)
}

func loadPortableFirewallOwners(root string) ([]portableFirewallOwner, error) {
	if !filepath.IsAbs(root) {
		return nil, fmt.Errorf("firewall host inventory must be absolute")
	}
	if err := validateMovePath(root); err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(root)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if len(entries) > 128 {
		return nil, fmt.Errorf("firewall host inventory is too large")
	}
	var owners []portableFirewallOwner
	for _, entry := range entries {
		if filepath.Ext(entry.Name()) != ".json" {
			continue
		}
		path := filepath.Join(root, entry.Name())
		if err := validateMovePath(path); err != nil {
			return nil, err
		}
		info, err := os.Lstat(path)
		if err != nil {
			return nil, err
		}
		if !info.Mode().IsRegular() || info.Size() > 4096 {
			return nil, fmt.Errorf("invalid firewall owner file")
		}
		f, err := os.Open(path)
		if err != nil {
			return nil, err
		}
		data, err := io.ReadAll(io.LimitReader(f, 4097))
		f.Close()
		if err != nil {
			return nil, err
		}
		if len(data) > 4096 {
			return nil, fmt.Errorf("invalid firewall owner file")
		}
		var owner portableFirewallOwner
		if err := json.Unmarshal(data, &owner); err != nil {
			return nil, err
		}
		if err := owner.validate(); err != nil {
			return nil, err
		}
		if entry.Name() != owner.Plan.Group+".json" {
			return nil, fmt.Errorf("firewall owner filename does not match its group")
		}
		owners = append(owners, owner)
	}
	return owners, nil
}

func cleanupPortableFirewallOwner(root string, owner portableFirewallOwner, apply func(lanFirewallPlan) error) error {
	path, err := portableFirewallOwnerPath(root, owner)
	if err != nil {
		return err
	}
	if err := apply(owner.Plan); err != nil {
		return err
	}
	// Keep the host record until removal is verified, including on UAC refusal.
	return os.Remove(path)
}

func cleanupStalePortableFirewallOwners(root string, running func(int) bool, apply func(lanFirewallPlan) error) error {
	owners, err := loadPortableFirewallOwners(root)
	if err != nil {
		return err
	}
	for _, owner := range owners {
		if running(owner.PID) {
			continue
		}
		if err := cleanupPortableFirewallOwner(root, owner, apply); err != nil {
			return fmt.Errorf("%s: %w", owner.Plan.Group, err)
		}
	}
	return nil
}
