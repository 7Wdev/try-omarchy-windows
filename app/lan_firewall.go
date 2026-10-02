package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const networkIdentityFilename = "network-identity"

type lanFirewallRule struct {
	Protocol string `json:"protocol"`
	Address  string `json:"address"`
	Port     int    `json:"port"`
}
type lanFirewallPlan struct {
	Group      string            `json:"group"`
	Program    string            `json:"program"`
	Public     bool              `json:"public"`
	Rules      []lanFirewallRule `json:"rules"`
	Generation string            `json:"generation"`
}

func networkIdentity(dir string, create bool) (string, error) {
	path := filepath.Join(dir, networkIdentityFilename)
	if err := validateMovePath(path); err != nil {
		return "", err
	}
	if info, err := os.Lstat(path); err == nil && (!info.Mode().IsRegular() || info.Size() > 64) {
		return "", fmt.Errorf("invalid network identity file")
	}
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) && !create {
		return "", nil
	}
	if os.IsNotExist(err) {
		var id [16]byte
		if _, err := rand.Read(id[:]); err != nil {
			return "", err
		}
		data = []byte(hex.EncodeToString(id[:]))
		f, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
		if err != nil {
			return "", err
		}
		_, err = f.Write(data)
		if err == nil {
			err = f.Sync()
		}
		closeErr := f.Close()
		if err != nil {
			return "", err
		}
		if closeErr != nil {
			return "", closeErr
		}
	} else if err != nil {
		return "", err
	}
	value := strings.TrimSpace(string(data))
	if !validCheckpointID(value) {
		return "", fmt.Errorf("invalid network identity")
	}
	return value, nil
}

func makeLANFirewallPlan(dir, program string, public bool, forwards []portForward) (lanFirewallPlan, error) {
	plan := lanFirewallPlan{Program: program, Public: public, Rules: []lanFirewallRule{}}
	for _, forward := range forwards {
		if forward.exposedToLAN() {
			plan.Rules = append(plan.Rules, lanFirewallRule{forward.proto, forward.address(), forward.hostPort})
		}
	}
	id, err := networkIdentity(dir, len(plan.Rules) > 0)
	if err != nil {
		return plan, err
	}
	if id == "" {
		return plan, nil
	}
	plan.Group = "TryOmarchy-" + id
	data, err := json.Marshal(plan)
	if err != nil {
		return plan, err
	}
	digest := sha256.Sum256(data)
	plan.Generation = hex.EncodeToString(digest[:8])
	return plan, plan.validate()
}

func (p lanFirewallPlan) validate() error {
	if !strings.HasPrefix(p.Group, "TryOmarchy-") || !validCheckpointID(strings.TrimPrefix(p.Group, "TryOmarchy-")) {
		return fmt.Errorf("invalid firewall ownership")
	}
	if len(p.Generation) != 16 {
		return fmt.Errorf("invalid firewall generation")
	}
	if _, err := hex.DecodeString(p.Generation); err != nil {
		return err
	}
	if len(p.Rules) > 64 {
		return uiError(uiText("error.lan.limit"), nil)
	}
	if len(p.Rules) > 0 && !filepath.IsAbs(p.Program) {
		return fmt.Errorf("firewall program must be an absolute path")
	}
	for _, rule := range p.Rules {
		forward, err := parseForward(fmt.Sprintf("%s:%s:%d:1", rule.Protocol, rule.Address, rule.Port))
		if err != nil || !forward.exposedToLAN() {
			return fmt.Errorf("invalid LAN firewall rule")
		}
	}
	return nil
}
