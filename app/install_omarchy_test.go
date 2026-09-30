package main

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func installDir(t *testing.T, name string) string {
	t.Helper()
	dir := t.TempDir()
	if name != "" {
		if err := os.MkdirAll(filepath.Join(dir, "vm"), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "vm", name), []byte("disk"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

type fakeInstall struct {
	locked, fastStartup, fastStartupKnown bool
	bitLocker                             map[string]bitLockerState
}

func (f fakeInstall) probes() installProbes {
	return installProbes{
		diskLocked:  func(string) bool { return f.locked },
		fastStartup: func() (bool, bool) { return f.fastStartup, f.fastStartupKnown },
		bitLocker:   func(drive string) bitLockerState { return f.bitLocker[drive] },
		freeBytes:   func(string) (int64, error) { return 200 << 30, nil },
		systemDrive: func() string { return "C:" },
	}
}

var readyInstall = fakeInstall{fastStartupKnown: true, bitLocker: map[string]bitLockerState{"C:": bitLockerOff}}

func buttonActions(buttons []installButton) []installAction {
	actions := make([]installAction, len(buttons))
	for i, button := range buttons {
		actions[i] = button.action
	}
	return actions
}

func sameActions(got []installButton, want ...installAction) bool {
	actions := buttonActions(got)
	if len(actions) != len(want) {
		return false
	}
	for i := range want {
		if actions[i] != want[i] {
			return false
		}
	}
	return true
}

func TestBitLockerFromShell(t *testing.T) {
	// Values measured on Windows 11 for each state of a test volume.
	for value, want := range map[int32]bitLockerState{
		0: bitLockerUnknown,
		1: bitLockerOn,         // on
		2: bitLockerOff,        // off
		3: bitLockerOn,         // encrypting
		4: bitLockerDecrypting, // decrypting
		5: bitLockerOn,         // suspended, still encrypted
		6: bitLockerOn,         // locked
		8: bitLockerOn,         // anything newer stays on the safe side
	} {
		if got := bitLockerFromShell(value); got != want {
			t.Errorf("bitLockerFromShell(%d) = %v, want %v", value, got, want)
		}
	}
}

func TestInstallDrives(t *testing.T) {
	if got := installDrives("C:", t.TempDir()); runtime.GOOS != "windows" && (len(got) != 1 || got[0] != "C:") {
		t.Fatalf("drives = %v", got)
	}
	if runtime.GOOS == "windows" {
		if got := installDrives("C:", `d:\TryOmarchy`); len(got) != 2 || got[0] != "C:" || got[1] != "D:" {
			t.Fatalf("drives = %v", got)
		}
		if got := installDrives("C:", `C:\Users\Ada\AppData\Local\TryOmarchy`); len(got) != 1 {
			t.Fatalf("drives = %v", got)
		}
		if got := installDrives("C:", `\\server\share\TryOmarchy`); len(got) != 1 {
			t.Fatalf("drives = %v", got)
		}
	}
}

func TestInstallReadyGoesStraightToTheSteps(t *testing.T) {
	r := assessInstallReadiness(installDir(t, "disk.raw"), readyInstall.probes())
	if r.Running || r.Portable || r.DiskMissing || r.FastStartup || r.BitLocker != bitLockerOff || r.SystemFree != 200<<30 {
		t.Fatalf("unexpected readiness %+v", r)
	}
	body, buttons := installPage(r)
	for _, want := range []string{"ready", "Shrink C:", "200.0 GiB free", importCommand, "Keep Try Omarchy installed"} {
		if !strings.Contains(body, want) {
			t.Errorf("steps lack %q:\n%s", want, body)
		}
	}
	if !sameActions(buttons, installDiskManagement, installGuide, installDone) {
		t.Fatalf("buttons = %v", buttonActions(buttons))
	}
}

func TestInstallListsOnlyWhatIsLeft(t *testing.T) {
	f := fakeInstall{locked: true, fastStartup: true, fastStartupKnown: true,
		bitLocker: map[string]bitLockerState{"C:": bitLockerOn}}
	r := assessInstallReadiness(installDir(t, "disk.raw"), f.probes())
	body, buttons := installPage(r)
	for _, want := range []string{"Shut down Omarchy", "Turn off Fast Startup", "Turn off BitLocker on C:"} {
		if !strings.Contains(body, want) {
			t.Errorf("list lacks %q:\n%s", want, body)
		}
	}
	if strings.Contains(body, importCommand) {
		t.Errorf("list shows the steps too early:\n%s", body)
	}
	if !sameActions(buttons, installShutDown, installFastStartup, installBitLocker, installRecheck, installDone) {
		t.Fatalf("buttons = %v", buttonActions(buttons))
	}

	f.locked = false
	body, buttons = installPage(assessInstallReadiness(installDir(t, "disk.raw"), f.probes()))
	if strings.Contains(body, "Omarchy.") || !sameActions(buttons, installFastStartup, installBitLocker, installRecheck, installDone) {
		t.Fatalf("buttons = %v:\n%s", buttonActions(buttons), body)
	}
}

func TestInstallWaitsForShutdownAndDecryption(t *testing.T) {
	f := fakeInstall{locked: true, fastStartupKnown: true, bitLocker: map[string]bitLockerState{"C:": bitLockerDecrypting}}
	r := assessInstallReadiness(installDir(t, "disk.raw"), f.probes())
	r.ShutdownRequested = true
	body, buttons := installPage(r)
	if !strings.Contains(body, "shutting down") || !strings.Contains(body, "C: to finish decrypting") {
		t.Fatalf("list:\n%s", body)
	}
	if !sameActions(buttons, installRecheck, installDone) {
		t.Fatalf("buttons = %v", buttonActions(buttons))
	}
}

func TestInstallTurnsOffFastStartupItCannotRead(t *testing.T) {
	f := readyInstall
	f.fastStartupKnown = false
	body, buttons := installPage(assessInstallReadiness(installDir(t, "disk.raw"), f.probes()))
	if !strings.Contains(body, "Fast Startup") || buttons[0].action != installFastStartup {
		t.Fatalf("buttons = %v:\n%s", buttonActions(buttons), body)
	}
}

func TestInstallUnknownBitLockerDoesNotBlock(t *testing.T) {
	f := readyInstall
	f.bitLocker = nil
	body, _ := installPage(assessInstallReadiness(installDir(t, "disk.raw"), f.probes()))
	if !strings.Contains(body, importCommand) {
		t.Fatalf("unknown BitLocker state blocked the steps:\n%s", body)
	}
}

func TestInstallPortableAndMissing(t *testing.T) {
	portable := assessInstallReadiness(installDir(t, "disk.qcow2"), readyInstall.probes())
	body, buttons := installPage(portable)
	if !portable.Portable || !strings.Contains(body, "try-omarchy-export") || buttons[0].action != installExportGuide {
		t.Fatalf("portable %+v:\n%s", portable, body)
	}
	missing := assessInstallReadiness(installDir(t, ""), readyInstall.probes())
	if !missing.DiskMissing {
		t.Fatalf("missing %+v", missing)
	}
	if _, buttons := installPage(missing); !sameActions(buttons, installDone) {
		t.Fatalf("buttons = %v", buttonActions(buttons))
	}
}

func TestInstallTextHasNoDashes(t *testing.T) {
	everything := fakeInstall{locked: true, fastStartup: true, fastStartupKnown: true,
		bitLocker: map[string]bitLockerState{"C:": bitLockerOn}}
	for _, r := range []installReadiness{
		assessInstallReadiness(installDir(t, "disk.raw"), readyInstall.probes()),
		assessInstallReadiness(installDir(t, "disk.raw"), everything.probes()),
		assessInstallReadiness(installDir(t, "disk.qcow2"), readyInstall.probes()),
		assessInstallReadiness(installDir(t, ""), readyInstall.probes()),
	} {
		body, buttons := installPage(r)
		for _, text := range append(installButtonLabels(buttons), body) {
			if strings.ContainsAny(text, "\u2014\u2013") {
				t.Fatalf("dash in user text: %q", text)
			}
		}
	}
}
