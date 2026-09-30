package main

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func ext4Image(state uint16, incompat uint32) []byte {
	image := make([]byte, 4096)
	binary.LittleEndian.PutUint16(image[1024+0x38:], ext4Magic)
	binary.LittleEndian.PutUint16(image[1024+0x3A:], state)
	binary.LittleEndian.PutUint32(image[1024+0x60:], incompat)
	return image
}

func TestExt4Unclean(t *testing.T) {
	for _, test := range []struct {
		name          string
		image         []byte
		unclean, isOK bool
	}{
		{"clean", ext4Image(1, 0x2c2), false, true},
		{"needs journal recovery", ext4Image(1, 0x2c2|ext4NeedsRecovery), true, true},
		{"not marked clean", ext4Image(0, 0x2c2), true, true},
		{"not ext4", make([]byte, 4096), false, false},
		{"too short", make([]byte, 100), false, false},
	} {
		unclean, ok := ext4Unclean(bytes.NewReader(test.image))
		if unclean != test.unclean || ok != test.isOK {
			t.Errorf("%s: got unclean=%v ok=%v", test.name, unclean, ok)
		}
	}
}

func installDir(t *testing.T, name string, data []byte) string {
	t.Helper()
	dir := t.TempDir()
	if name != "" {
		if err := os.MkdirAll(filepath.Join(dir, "vm"), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "vm", name), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func fakeInstallProbes(locked, fastStartup bool) installProbes {
	return installProbes{
		diskLocked:  func(string) bool { return locked },
		fastStartup: func() (bool, bool) { return fastStartup, true },
		freeBytes:   func(string) (int64, error) { return 200 << 30, nil },
		systemDrive: func() string { return "C:" },
	}
}

func buttonActions(buttons []installButton) []installAction {
	actions := make([]installAction, len(buttons))
	for i, button := range buttons {
		actions[i] = button.action
	}
	return actions
}

func TestInstallReadinessReady(t *testing.T) {
	dir := installDir(t, "disk.raw", ext4Image(1, 0))
	r := assessInstallReadiness(dir, fakeInstallProbes(false, false))
	if r.Running || r.Unclean || r.Portable || r.DiskMissing || r.FastStartup || r.SystemFree != 200<<30 {
		t.Fatalf("unexpected readiness %+v", r)
	}
	body, buttons := installChecklist(r)
	for _, want := range []string{"Done: Omarchy is shut down", "Done: Fast Startup is off", "BitLocker", "shrinking C:", "Keep Try Omarchy installed"} {
		if !strings.Contains(body, want) {
			t.Errorf("checklist lacks %q:\n%s", want, body)
		}
	}
	if got := buttonActions(buttons); len(got) != 3 || got[0] != installEncryption || got[1] != installNext || got[2] != installDone {
		t.Fatalf("buttons = %v", got)
	}
}

func TestInstallReadinessBlockers(t *testing.T) {
	dir := installDir(t, "disk.raw", ext4Image(1, 0))
	r := assessInstallReadiness(dir, fakeInstallProbes(true, true))
	if !r.Running || !r.FastStartup {
		t.Fatalf("unexpected readiness %+v", r)
	}
	body, buttons := installChecklist(r)
	if !strings.Contains(body, "To do: shut Omarchy down") || !strings.Contains(body, "To do: turn off Fast Startup") {
		t.Fatalf("checklist:\n%s", body)
	}
	if got := buttonActions(buttons); got[0] != installFastStartup || got[len(got)-2] != installRecheck {
		t.Fatalf("buttons = %v", got)
	}
}

func TestInstallReadinessUncleanDisk(t *testing.T) {
	dir := installDir(t, "disk.raw", ext4Image(0, ext4NeedsRecovery))
	r := assessInstallReadiness(dir, fakeInstallProbes(false, false))
	body, _ := installChecklist(r)
	if !r.Unclean || !strings.Contains(body, "not shut down cleanly") {
		t.Fatalf("readiness %+v:\n%s", r, body)
	}
}

func TestInstallReadinessPortableAndMissing(t *testing.T) {
	portable := assessInstallReadiness(installDir(t, "disk.qcow2", []byte("QFI\xfb")), fakeInstallProbes(false, false))
	body, buttons := installChecklist(portable)
	if !portable.Portable || !strings.Contains(body, "try-omarchy-export") || buttons[0].action != installExportGuide {
		t.Fatalf("portable %+v:\n%s", portable, body)
	}
	missing := assessInstallReadiness(installDir(t, "", nil), fakeInstallProbes(false, false))
	if !missing.DiskMissing {
		t.Fatalf("missing %+v", missing)
	}
	if _, buttons := installChecklist(missing); len(buttons) != 1 || buttons[0].action != installDone {
		t.Fatalf("buttons = %v", buttons)
	}
}

func TestInstallStepsShowTheImportCommand(t *testing.T) {
	body, buttons := installSteps()
	if !strings.Contains(body, importCommand) || !strings.Contains(body, "dual boot guide") {
		t.Fatalf("steps:\n%s", body)
	}
	if got := buttonActions(buttons); got[len(got)-1] != installDone {
		t.Fatalf("buttons = %v", got)
	}
	for _, text := range []string{body, importCommand} {
		if strings.ContainsAny(text, "\u2014\u2013") {
			t.Fatalf("dash in user text: %q", text)
		}
	}
}
