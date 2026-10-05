package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// This inventory stays on the Windows host, not on the removable drive.
// It contains only deletion plans, never instructions to create allow rules.
type portableFirewallOwner struct {
	Plan lanFirewallPlan `json:"plan"`
	PID  int             `json:"pid"`
	Dir  string          `json:"dir,omitempty"`
}

func (o portableFirewallOwner) validate() error {
	if o.PID <= 0 || int64(o.PID) > 0xffffffff || len(o.Plan.Rules) != 0 {
		return fmt.Errorf("invalid portable firewall owner")
	}
	if err := o.Plan.validate(); err != nil {
		return err
	}
	if o.Dir != "" {
		if !filepath.IsAbs(o.Dir) || filepath.Clean(o.Dir) != o.Dir {
			return fmt.Errorf("invalid portable firewall data folder")
		}
		if err := validateMovePath(o.Dir); err != nil {
			return err
		}
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
	info, err := os.Lstat(root)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if !info.IsDir() {
		return nil, fmt.Errorf("firewall host inventory must be a directory")
	}
	entries, err := os.ReadDir(root)
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
		stale, err := portableFirewallOwnerStale(owner)
		if err != nil {
			return fmt.Errorf("%s: %w", owner.Plan.Group, err)
		}
		if !stale {
			continue
		}
		// Attempt one owner per launch, even on success. A refusal or failure
		// leaves the record for a later launch and cannot trigger another prompt.
		if err := cleanupPortableFirewallOwner(root, owner, apply); err != nil {
			return fmt.Errorf("%s: %w", owner.Plan.Group, err)
		}
		break
	}
	return nil
}

func portableFirewallOwnerStale(owner portableFirewallOwner) (bool, error) {
	if _, err := os.Stat(owner.Plan.Program); os.IsNotExist(err) {
		return true, nil
	} else if err != nil {
		return false, err
	}
	// Old records have no data folder. An existing program alone cannot prove
	// that its installation is gone, so retain those rules conservatively.
	if owner.Dir == "" {
		return false, nil
	}
	id, err := networkIdentity(owner.Dir, false)
	if err != nil {
		return false, err
	}
	// This identity survives runtime updates, unlike a runtime receipt, and
	// distinguishes a different installation reusing the same drive and path.
	return id != strings.TrimPrefix(owner.Plan.Group, "TryOmarchy-"), nil
}

func ensureLANFirewallAfterCleanup(cleanup func() error, notice func(error), ensure func() error) error {
	if err := cleanup(); err != nil {
		notice(err)
	}
	return ensure()
}
