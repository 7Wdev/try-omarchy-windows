package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestKeyboardRuntimeCapability(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "bin", "qemu.exe")
	if keyboardRuntimeSupportsExternalHook(exe) {
		t.Fatal("missing provenance advertised support")
	}
	if err := os.MkdirAll(filepath.Join(dir, "provenance"), 0755); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "provenance", "sources.lock.json")
	for _, tc := range []struct {
		data      string
		supported bool
	}{
		{`{`, false},
		{`{"qemu":{"patches":[{"file":"patches/qemu/0016-live-sdl-audio-routes.patch"}]}}`, false},
		{`{"qemu":{"patches":[{"file":"` + externalKeyboardHookPatch + `"}]}}`, true},
	} {
		if err := os.WriteFile(path, []byte(tc.data), 0600); err != nil {
			t.Fatal(err)
		}
		if got := keyboardRuntimeSupportsExternalHook(exe); got != tc.supported {
			t.Fatalf("support = %v, want %v", got, tc.supported)
		}
	}
}

func TestKeyboardEnvironment(t *testing.T) {
	env := []string{"Path=example", externalKeyboardHookEnv + "=0", "omarchy_sdl_external_keyboard_hook=1"}
	for _, enabled := range []bool{false, true} {
		want := []string{"Path=example"}
		if enabled {
			want = append(want, externalKeyboardHookEnv+"=1")
		}
		if got := keyboardEnvironment(env, enabled); !reflect.DeepEqual(got, want) {
			t.Fatalf("got %v, want %v", got, want)
		}
	}
}
