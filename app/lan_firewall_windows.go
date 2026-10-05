//go:build windows

package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
)

// Only this installation's named rules are reconciled. Unrelated application
// and enterprise rules are never modified.
const lanFirewallScript = `$ErrorActionPreference='Stop'
$p=$env:TRYOMARCHY_FIREWALL | ConvertFrom-Json
try {
 $existing=@(Get-NetFirewallRule -PolicyStore PersistentStore -Group $p.group -ErrorAction Stop)
} catch {
 if($_.CategoryInfo.Category -eq [System.Management.Automation.ErrorCategory]::ObjectNotFound){$existing=@()}else{throw}
}
$names=@(); $matches=($existing.Count -eq $p.rules.Count)
for($i=0;$i -lt $p.rules.Count;$i++) {
 $wanted=$p.rules[$i]; $name=$p.group+'-'+$p.generation+'-'+$i; $names+=,$name
 $rule=@($existing | Where-Object Name -eq $name)
 if($rule.Count -ne 1){$matches=$false;continue}
 $rule=$rule[0]; $port=$rule | Get-NetFirewallPortFilter; $address=$rule | Get-NetFirewallAddressFilter; $app=$rule | Get-NetFirewallApplicationFilter
 $local=if($wanted.address -eq '0.0.0.0'){'Any'}else{$wanted.address}
 $profile=if($p.public){0}else{3};$protocol=if($wanted.protocol -eq 'tcp'){6}else{17}
 if($rule.Enabled -ne 'True' -or $rule.Direction -ne 'Inbound' -or $rule.Action -ne 'Allow' -or [int]$rule.Profile -ne $profile -or $port.Protocol -notin @($wanted.protocol,$protocol) -or [string]$port.LocalPort -ne [string]$wanted.port -or [string]$address.LocalAddress -ne $local -or [string]$address.RemoteAddress -ne 'LocalSubnet' -or $app.Program -ne $p.program){$matches=$false}
}
if($matches){exit 0}
if($env:TRYOMARCHY_FIREWALL_APPLY -ne '1'){exit 3}
$created=@()
try {
 for($i=0;$i -lt $p.rules.Count;$i++) {
  $wanted=$p.rules[$i];$name=$names[$i]
  $current=@($existing | Where-Object Name -eq $name)
  if($current.Count){$current | Remove-NetFirewallRule}
  $local=if($wanted.address -eq '0.0.0.0'){'Any'}else{$wanted.address}
  $profiles=if($p.public){@('Any')}else{@('Domain','Private')}
  New-NetFirewallRule -PolicyStore PersistentStore -Name $name -DisplayName ('Try Omarchy '+$wanted.protocol.ToUpper()+' '+$wanted.port) -Group $p.group -Direction Inbound -Action Allow -Enabled True -Profile $profiles -Program $p.program -Protocol $wanted.protocol -LocalPort $wanted.port -LocalAddress $local -RemoteAddress LocalSubnet -EdgeTraversalPolicy Block | Out-Null
  $created+=,$name
 }
 $existing | Where-Object {$_.Name -notin $names} | Remove-NetFirewallRule
} catch {
 foreach($name in $created){Get-NetFirewallRule -PolicyStore PersistentStore -Name $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule}
 throw
}
`

func executeLANFirewall(plan lanFirewallPlan, apply bool) error {
	if err := plan.validate(); err != nil {
		return err
	}
	if apply && len(plan.Rules) > 0 {
		// A link would let the rule follow a different program later.
		if info, err := os.Lstat(plan.Program); err != nil || !info.Mode().IsRegular() {
			return fmt.Errorf("firewall program is not a regular file: %s", plan.Program)
		}
	}
	data, err := json.Marshal(plan)
	if err != nil {
		return err
	}
	cmd := exec.CommandContext(setupContext(), system32("WindowsPowerShell\\v1.0\\powershell.exe"), "-NoProfile", "-NonInteractive", "-Command", lanFirewallScript)
	cmd.Env = append(os.Environ(), "TRYOMARCHY_FIREWALL="+string(data), fmt.Sprintf("TRYOMARCHY_FIREWALL_APPLY=%d", map[bool]int{false: 0, true: 1}[apply]))
	configureDiskTool(cmd)
	var detail diskToolErrors
	cmd.Stderr = &detail
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("Windows firewall: %w: %s", err, detail.String())
	}
	return nil
}

func applyEncodedLANFirewall(encoded string) error {
	if len(encoded) > 28000 {
		return fmt.Errorf("firewall request is too large")
	}
	data, err := base64.RawURLEncoding.DecodeString(encoded)
	if err != nil {
		return err
	}
	var plan lanFirewallPlan
	if err := json.Unmarshal(data, &plan); err != nil {
		return err
	}
	return executeLANFirewall(plan, true)
}

func ensureLANFirewall(cfg *config) error {
	plan, err := makeLANFirewallPlan(cfg.dir, cfg.qemu, cfg.lanPublic, cfg.forwards)
	if err != nil {
		return err
	}
	return ensureLANFirewallPlan(plan, executeLANFirewall, runElevated, checkSetupCancelled)
}

func ensureLANFirewallPlan(plan lanFirewallPlan, execute func(lanFirewallPlan, bool) error, elevate func(string) (int, error), cancelled func() error) error {
	if plan.Group == "" {
		return nil
	}
	if err := plan.validate(); err != nil {
		return err
	}
	if execute(plan, false) == nil {
		return nil
	}
	if err := cancelled(); err != nil {
		return err
	}
	data, err := json.Marshal(plan)
	if err != nil {
		return err
	}
	encoded := base64.RawURLEncoding.EncodeToString(data)
	if len(encoded) > 28000 {
		return fmt.Errorf("LAN configuration is too large")
	}
	code, err := elevate("-firewall-plan " + encoded)
	if err != nil {
		return err
	}
	if code == errorCancelled {
		return uiError(uiText("error.lan.permission"), nil)
	}
	if code != 0 {
		return uiError(uiTextWith("error.lan.firewall", map[string]string{"code": fmt.Sprint(code)}), nil)
	}
	return execute(plan, false)
}

func noticePausedForwards(key string, forwards []portForward, detail string) {
	if len(forwards) == 0 {
		return
	}
	rules := make([]string, 0, len(forwards))
	for _, forward := range forwards {
		rules = append(rules, forward.String())
	}
	values := map[string]string{"rules": strings.Join(rules, ", "), "error": detail}
	title := uiText("tray.forward.title")
	message := uiTextWith("tray.forward.paused", values)
	if key == "tray.lan.firewall_paused" {
		title = uiText("tray.lan.title")
		message = uiTextWith("tray.lan.firewall_paused", values)
	}
	logf("%s", message)
	showTrayNotice(title, message)
}

func portableFirewallProcessRunning(pid int) bool {
	const synchronize = 0x100000
	handle, _, err := procOpenProcess.Call(synchronize, 0, uintptr(uint32(pid)))
	if handle == 0 {
		// ERROR_INVALID_PARAMETER means the PID no longer exists. Access denial
		// is ambiguous: leave its rules alone rather than disrupt another VM.
		return err != syscall.Errno(87)
	}
	defer procCloseHandle.Call(handle)
	result, _, _ := procWaitForSingleObject.Call(handle, 0)
	return result != 0
}

func prepareLANFirewall(cfg *config) error {
	local := os.Getenv("LOCALAPPDATA")
	root := filepath.Join(local, "TryOmarchy", "portable-host", "firewall-owners")
	notice := func(err error) { firewallCleanupNotice(err.Error(), "") }
	return ensureLANFirewallAfterCleanup(func() error {
		if !filepath.IsAbs(local) {
			return fmt.Errorf("Windows local application data is unavailable")
		}
		return cleanupStalePortableFirewallOwners(root, portableFirewallProcessRunning, func(plan lanFirewallPlan) error {
			return ensureLANFirewallPlan(plan, executeLANFirewall, runElevated, checkSetupCancelled)
		})
	}, notice, func() error {
		if cfg.portable && filepath.IsAbs(local) {
			// Keep a host-local deletion record across exits and relaunches.
			// Inventory failures are reported but do not block our own setup.
			plan, err := makeLANFirewallPlan(cfg.dir, cfg.qemu, cfg.lanPublic, cfg.forwards)
			if err == nil && plan.Group != "" {
				plan.Rules = nil
				err = savePortableFirewallOwner(root, portableFirewallOwner{Plan: plan, PID: os.Getpid(), Dir: cfg.dir})
			}
			if err != nil {
				notice(err)
			}
		}
		return ensureLANFirewall(cfg)
	})
}

func firewallCleanupNotice(detail, group string) {
	message := uiTextWith("tray.lan.cleanup_failed", map[string]string{"error": detail, "rules": group})
	logf("%s", message)
	showTrayNotice(uiText("tray.lan.cleanup_title"), message)
}
