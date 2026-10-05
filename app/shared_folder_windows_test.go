//go:build windows

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"unsafe"
)

func TestValidateWindowsSharedFolderPolicy(t *testing.T) {
	for _, name := range []string{"SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "ProgramData", "PUBLIC", "LOCALAPPDATA", "APPDATA"} {
		t.Setenv(name, "")
	}
	root := t.TempDir()
	home := filepath.Join(root, "home")
	share := filepath.Join(home, "Omarchy Shared")
	data := filepath.Join(home, "TryOmarchyData")
	for _, path := range []string{share, data} {
		if err := os.MkdirAll(path, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	got, err := validateWindowsSharedFolder(share, data, home)
	if err != nil {
		t.Fatalf("valid share = %q, %v", got, err)
	}
	want, err := filepath.EvalSymlinks(share)
	if err != nil {
		t.Fatal(err)
	}
	want, err = filepath.Abs(want)
	if err != nil {
		t.Fatal(err)
	}
	if !sameWindowsPath(got, want) {
		t.Fatalf("canonical share = %q, want %q", got, want)
	}
	if _, err := validateWindowsSharedFolder(home, data, home); err == nil {
		t.Fatal("the whole home folder was accepted")
	}
	if _, err := validateWindowsSharedFolder(root, data, home); err == nil {
		t.Fatal("an ancestor of the home folder was accepted")
	}
	if _, err := validateWindowsSharedFolder(data, data, home); err == nil {
		t.Fatal("the VM data directory was accepted")
	}
	if _, err := validateWindowsSharedFolder(home, share, root); err == nil {
		t.Fatal("a folder containing the VM data directory was accepted")
	}
	if _, err := validateWindowsSharedFolder(`\\server\share`, data, home); err == nil {
		t.Fatal("a UNC path was accepted")
	}
	protected := filepath.Join(root, "Windows")
	protectedChild := filepath.Join(protected, "Temp")
	if err := os.MkdirAll(protectedChild, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("SystemRoot", protected)
	if _, err := validateWindowsSharedFolder(protectedChild, data, home); err == nil {
		t.Fatal("a Windows system folder was accepted")
	}
}

func TestNotifyIconDataUsesCurrentWindowsLayout(t *testing.T) {
	if size := unsafe.Sizeof(notifyIconData{}); size != 976 {
		t.Fatalf("NOTIFYICONDATAW size = %d, want 976", size)
	}
}

func TestSharedFolderLinksFindsJunctions(t *testing.T) {
	share := t.TempDir()
	outside := t.TempDir()
	if err := os.Mkdir(filepath.Join(share, "folder"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(share, "file.txt"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	links, err := sharedFolderLinks(share, 5)
	if err != nil || len(links) != 0 {
		t.Fatalf("plain share links = %v, %v", links, err)
	}
	junction := filepath.Join(share, "escape")
	if out, err := exec.Command(system32("cmd.exe"), "/c", "mklink", "/J", junction, outside).CombinedOutput(); err != nil {
		t.Skipf("cannot create a junction: %v: %s", err, out)
	}
	links, err = sharedFolderLinks(share, 5)
	if err != nil || len(links) != 1 || links[0] != "escape" {
		t.Fatalf("share links = %v, %v", links, err)
	}
	// A junction is a warning, not a refusal.
	t.Setenv("LOCALAPPDATA", "")
	if _, err := validateWindowsSharedFolder(share, "", filepath.Join(outside, "home")); err != nil {
		t.Fatalf("a share with a junction inside was refused: %v", err)
	}
}

func TestIsLinkReparsePoint(t *testing.T) {
	for _, c := range []struct {
		attributes, tag uint32
		want            bool
	}{
		{fileAttributeReparsePoint, ioReparseTagSymlink, true},
		{fileAttributeReparsePoint | 0x10, ioReparseTagMountPoint, true},
		{fileAttributeReparsePoint, 0x9000001A, false}, // OneDrive placeholder
		{fileAttributeReparsePoint, 0x80000013, false}, // deduplicated file
		{0x10, ioReparseTagMountPoint, false},          // tag is meaningless without the attribute
	} {
		if got := isLinkReparsePoint(c.attributes, c.tag); got != c.want {
			t.Errorf("isLinkReparsePoint(%#x, %#x) = %v", c.attributes, c.tag, got)
		}
	}
}
