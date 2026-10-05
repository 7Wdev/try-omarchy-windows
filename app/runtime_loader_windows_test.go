//go:build windows

package main

import (
	"context"
	"reflect"
	"testing"
)

func TestRuntimeLoaderCommandUsesExactExecutableAndHiddenVersionProbe(t *testing.T) {
	executable := `C:\Try Omarchy\data\runtime\bin\qemu-system-x86_64w.exe`
	cmd := runtimeLoaderCommand(context.Background(), executable)
	if cmd.Path != executable || !reflect.DeepEqual(cmd.Args, []string{executable, "--version"}) {
		t.Fatalf("path=%q args=%q", cmd.Path, cmd.Args)
	}
	if cmd.SysProcAttr == nil || !cmd.SysProcAttr.HideWindow || cmd.SysProcAttr.CreationFlags&createNoWindow == 0 {
		t.Fatalf("probe must be hidden: %+v", cmd.SysProcAttr)
	}
	if cmd.Cancel == nil {
		t.Fatal("probe must use CommandContext cancellation")
	}
}
