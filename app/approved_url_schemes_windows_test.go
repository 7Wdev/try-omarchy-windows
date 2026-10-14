//go:build windows

package main

import (
	"crypto/rand"
	"encoding/hex"
	"net"
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"syscall"
	"testing"
	"unsafe"
)

// Windows substitutes the clicked link for %1 without escaping it. Whatever
// the link contains, the split command line is the registered shape with
// the exact link, or it is refused.
func TestURLSchemeCommandLineCannotInjectFlags(t *testing.T) {
	dir := `C:\Users\me\AppData\Local\TryOmarchy`
	command := urlSchemeCommand(filepath.Join(dir, stableLauncherName), dir)
	for _, link := range []string{
		"zoommtg:join?x=1",
		`zoommtg:a" -uninstall "`,
		`zoommtg:a\" -dir "D:\evil`,
		`zoommtg:a\\`,
		`zoommtg:a\`,
		`zoommtg:" -share "C:\`,
		"zoommtg:a b -uninstall",
		`zoommtg:a"" -fresh`,
	} {
		line := strings.Replace(command, "%1", link, 1)
		utf16, _ := syscall.UTF16PtrFromString(line)
		var argc int32
		argv, err := syscall.CommandLineToArgv(utf16, &argc)
		if err != nil {
			t.Fatal(err)
		}
		args := make([]string, argc)
		for i := range args {
			args[i] = syscall.UTF16ToString((*argv[i])[:])
		}
		syscall.LocalFree(syscall.Handle(uintptr(unsafe.Pointer(argv))))
		gotDir, gotURL, activation, ok := parseURLActivation(args[1:])
		if !activation {
			t.Errorf("%q: not recognized as a link", link)
			continue
		}
		if !ok {
			continue
		}
		if gotDir != dir {
			t.Errorf("%q: data folder became %q", link, gotDir)
		}
		if url, valid := validOpenURL(gotURL, []string{"zoommtg"}); valid && url != link {
			t.Errorf("%q: forwarded as %q", link, url)
		}
	}
}

func TestConnectionFromSameUserAcceptsThisProcess(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	accepted := make(chan net.Conn, 1)
	go func() {
		if conn, err := listener.Accept(); err == nil {
			accepted <- conn
		}
	}()
	client, err := net.Dial("tcp", listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	server := <-accepted
	defer server.Close()
	if !connectionFromSameUser(server) || !connectionFromSameUser(client) {
		t.Fatal("this process was not recognized at either end")
	}
	if _, ok := processUserSID(0); ok {
		t.Fatal("the idle process had a readable user")
	}
}

// The registry tests write HKCU\Software\Classes keys with random names and
// remove them again. Set TRYOMARCHY_REGISTRY_TEST=1 to run them.
func testURLSchemeNames(t *testing.T, prefixes ...string) []string {
	t.Helper()
	if os.Getenv("TRYOMARCHY_REGISTRY_TEST") != "1" {
		t.Skip("set TRYOMARCHY_REGISTRY_TEST=1 to write test link types for this Windows user")
	}
	var random [6]byte
	rand.Read(random[:])
	var names []string
	for _, prefix := range prefixes {
		name := "tryomarchy-" + prefix + "-" + hex.EncodeToString(random[:])
		if !validURLScheme(name) {
			t.Fatalf("test link type %q is not a valid scheme", name)
		}
		names = append(names, name)
		t.Cleanup(func() { deleteURLSchemeKey(name, func(urlSchemeRegistration) bool { return true }) })
	}
	return names
}

// writeRawURLScheme leaves a key in the state a crash, another app or an
// earlier write could leave. Empty values are not written.
func writeRawURLScheme(t *testing.T, scheme, marker, command string, commandType uint32) {
	t.Helper()
	create := func(path string) syscall.Handle {
		p, _ := syscall.UTF16PtrFromString(path)
		var key syscall.Handle
		var disposition uint32
		if r, _, err := procRegCreateKeyExW.Call(uintptr(syscall.HKEY_CURRENT_USER), uintptr(unsafe.Pointer(p)), 0, 0,
			regOptionNonVolatile, uintptr(syscall.KEY_WRITE), 0, uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&disposition))); r != 0 {
			t.Fatal(err)
		}
		return key
	}
	key := create(urlSchemeClassesKey + `\` + scheme)
	defer syscall.RegCloseKey(key)
	if marker != "" {
		if err := regSetString(key, urlSchemeOwnerValue, marker); err != nil {
			t.Fatal(err)
		}
	}
	if command != "" {
		sub := create(urlSchemeClassesKey + `\` + scheme + `\shell\open\command`)
		defer syscall.RegCloseKey(sub)
		v, _ := syscall.UTF16FromString(command)
		if r, _, err := procRegSetValueExW.Call(uintptr(sub), 0, 0, uintptr(commandType), uintptr(unsafe.Pointer(&v[0])), uintptr(len(v)*2)); r != 0 {
			t.Fatal(err)
		}
	}
}

func TestURLSchemeRegistrationOwnership(t *testing.T) {
	names := testURLSchemeNames(t, "ours", "other", "nocmd", "expand", "empty")
	ours, foreign, noCommand, expand, empty := names[0], names[1], names[2], names[3], names[4]
	root := t.TempDir()
	dir, other := filepath.Join(root, "one"), filepath.Join(root, "two")
	target := filepath.Join(dir, stableLauncherName)
	command := urlSchemeCommand(target, dir)
	owns := ownedBy(target, dir)
	writeRawURLScheme(t, foreign, "", `"C:\Other\other.exe" "%1"`, syscall.REG_SZ)
	// Our marker without a readable string command is not ours: another
	// app may have replaced the command, or a write was interrupted.
	writeRawURLScheme(t, noCommand, dir, "", 0)
	writeRawURLScheme(t, expand, dir, command, syscall.REG_EXPAND_SZ)
	// An empty unmarked key is never ours, whoever created it.
	writeRawURLScheme(t, empty, "", "", 0)
	all := []string{ours, foreign, noCommand, expand, empty}
	if err := syncURLSchemeRegistrations(dir, target, all); err != nil {
		t.Fatal(err)
	}
	if state, err := urlSchemeOwner(ours, owns); err != nil || state != urlSchemeOwned {
		t.Fatalf("registered link type: %v, %v", state, err)
	}
	if resolved, _ := windowsURLHandler(ours); !strings.EqualFold(resolved, command) {
		t.Fatalf("Windows resolves the link type to %q", resolved)
	}
	if urlSchemeHandledByWindows(ours, target, dir) {
		t.Fatal("our own registration counted as another app's")
	}
	for _, scheme := range all[1:] {
		if state, _ := urlSchemeOwner(scheme, owns); state != urlSchemeForeign {
			t.Errorf("%s: state %v after registering", scheme, state)
		}
	}
	if err := writeURLSchemeKey(foreign, target, dir, owns); err == nil {
		t.Fatal("an existing foreign key was overwritten")
	}
	if state, _ := urlSchemeOwner(ours, ownedBy(filepath.Join(other, stableLauncherName), other)); state != urlSchemeForeign {
		t.Fatal("a second installation owns the first one's link type")
	}
	if err := syncURLSchemeRegistrations(other, filepath.Join(other, stableLauncherName), nil); err != nil {
		t.Fatal(err)
	}
	if state, _ := urlSchemeOwner(ours, owns); state != urlSchemeOwned {
		t.Fatal("a second installation removed the first one's link type")
	}
	if owned, err := ownedURLSchemes(dir); err != nil || !slices.Equal(owned, []string{ours}) {
		t.Fatalf("owned %v, %v", owned, err)
	}
	if err := removeOwnedURLSchemes(dir, all); err != nil {
		t.Fatal(err)
	}
	if state, _ := urlSchemeOwner(ours, owns); state != urlSchemeAbsent {
		t.Fatal("uninstall left the link type registered")
	}
	for _, scheme := range all[1:] {
		if state, _ := urlSchemeOwner(scheme, owns); state != urlSchemeForeign {
			t.Errorf("%s: uninstall changed a key that was not ours", scheme)
		}
	}
}

// Move writes the command, then the marker. Every state an interrupted move
// can leave is finished by the retry.
func TestURLSchemeMoveRecoversEveryInterruptedWrite(t *testing.T) {
	names := testURLSchemeNames(t, "before", "midway", "swapped", "done")
	root := t.TempDir()
	source, destination := filepath.Join(root, "old"), filepath.Join(root, "new")
	sourceCommand := urlSchemeCommand(filepath.Join(source, stableLauncherName), source)
	destinationCommand := urlSchemeCommand(filepath.Join(destination, stableLauncherName), destination)
	writeRawURLScheme(t, names[0], source, sourceCommand, syscall.REG_SZ)
	writeRawURLScheme(t, names[1], source, destinationCommand, syscall.REG_SZ)
	writeRawURLScheme(t, names[2], destination, sourceCommand, syscall.REG_SZ)
	writeRawURLScheme(t, names[3], destination, destinationCommand, syscall.REG_SZ)
	if err := moveURLSchemeRegistrations(source, destination); err != nil {
		t.Fatal(err)
	}
	owns := ownedBy(filepath.Join(destination, stableLauncherName), destination)
	for _, scheme := range names {
		if state, err := urlSchemeOwner(scheme, owns); err != nil || state != urlSchemeOwned {
			t.Errorf("%s: %v, %v after the move", scheme, state, err)
		}
	}
	if owned, err := ownedURLSchemes(source); err != nil || len(owned) != 0 {
		t.Fatalf("source still owns %v, %v", owned, err)
	}
}

// writeFirstRegistrationUpTo leaves the key a first registration leaves when
// it stops after step writes: create, command, default, URL Protocol. The
// marker would come next.
func writeFirstRegistrationUpTo(t *testing.T, scheme, dir string, steps int) {
	t.Helper()
	create := func(path string) syscall.Handle {
		p, _ := syscall.UTF16PtrFromString(path)
		var key syscall.Handle
		var disposition uint32
		if r, _, err := procRegCreateKeyExW.Call(uintptr(syscall.HKEY_CURRENT_USER), uintptr(unsafe.Pointer(p)), 0, 0,
			regOptionNonVolatile, uintptr(syscall.KEY_WRITE), 0, uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&disposition))); r != 0 {
			t.Fatal(err)
		}
		return key
	}
	key := create(urlSchemeClassesKey + `\` + scheme)
	defer syscall.RegCloseKey(key)
	if steps >= 1 {
		command := create(urlSchemeClassesKey + `\` + scheme + `\shell\open\command`)
		regSetString(command, "", urlSchemeCommand(filepath.Join(dir, stableLauncherName), dir))
		syscall.RegCloseKey(command)
	}
	if steps >= 2 {
		regSetString(key, "", "URL:"+scheme+" (Try Omarchy)")
	}
	if steps >= 3 {
		regSetString(key, "URL Protocol", "")
	}
}

func TestInterruptedFirstRegistrationIsRepairedOrRemoved(t *testing.T) {
	names := testURLSchemeNames(t, "step0", "step1", "step2", "step3", "extra", "text", "sibling", "far")
	root := t.TempDir()
	dir, other := filepath.Join(root, "one"), filepath.Join(root, "two")
	target := filepath.Join(dir, stableLauncherName)
	command := urlSchemeCommand(target, dir)
	owns := ownedBy(target, dir)
	steps, nearMisses := names[:4], names[4:]
	for step, scheme := range steps {
		writeFirstRegistrationUpTo(t, scheme, dir, step)
		want := urlSchemeOwned
		if step == 0 {
			want = urlSchemeForeign // an empty key could be anyone's
		}
		if state, err := urlSchemeOwner(scheme, owns); err != nil || state != want {
			t.Fatalf("after %d writes: %v, %v; want %v", step, state, err, want)
		}
	}
	// Unmarked keys that are not exactly our footprint stay foreign.
	writeFirstRegistrationUpTo(t, nearMisses[0], dir, 3)
	extra, _ := openURLSchemeKeyForTest(t, nearMisses[0])
	regSetString(extra, "Owner", "someone")
	syscall.RegCloseKey(extra)
	writeFirstRegistrationUpTo(t, nearMisses[1], dir, 3)
	text, _ := openURLSchemeKeyForTest(t, nearMisses[1])
	regSetString(text, "", "URL:Other App")
	syscall.RegCloseKey(text)
	writeFirstRegistrationUpTo(t, nearMisses[2], dir, 3)
	writeRawURLScheme(t, nearMisses[2]+`\shell\print`, "", "", 0)
	writeFirstRegistrationUpTo(t, nearMisses[3], other, 3)
	for _, scheme := range nearMisses {
		if state, err := urlSchemeOwner(scheme, owns); err != nil || state != urlSchemeForeign {
			t.Errorf("%s: %v, %v; want foreign", scheme, state, err)
		}
	}
	// A retry finishes every leftover that has our command.
	if err := syncURLSchemeRegistrations(dir, target, names); err != nil {
		t.Fatal(err)
	}
	for step, scheme := range steps {
		key, _ := openURLSchemeKeyForTest(t, scheme)
		reg, err := readURLSchemeRegistration(key, scheme)
		syscall.RegCloseKey(key)
		if err != nil || step > 0 && !reg.runs(dir, command) || step == 0 && reg.markerOK {
			t.Errorf("after %d writes and a retry: %+v, %v", step, reg, err)
		}
	}
	// Uninstall removes leftovers with our command, never the empty key or the others.
	for step, scheme := range steps {
		deleteURLSchemeKey(scheme, func(urlSchemeRegistration) bool { return true })
		writeFirstRegistrationUpTo(t, scheme, dir, step)
	}
	owned, err := ownedURLSchemes(dir)
	if err != nil {
		t.Fatal(err)
	}
	if err := removeOwnedURLSchemes(dir, owned); err != nil {
		t.Fatal(err)
	}
	for step, scheme := range steps {
		want := urlSchemeAbsent
		if step == 0 {
			want = urlSchemeForeign
		}
		if state, _ := urlSchemeOwner(scheme, owns); state != want {
			t.Errorf("after %d writes, uninstall left %v; want %v", step, state, want)
		}
	}
	for _, scheme := range nearMisses {
		if state, _ := urlSchemeOwner(scheme, owns); state != urlSchemeForeign {
			t.Errorf("%s was changed by uninstall", scheme)
		}
	}
}

// Another app's commandless keys look like the start of our footprint. No
// installation may complete, revoke, uninstall or move them.
func TestCommandlessVendorKeysAreNeverTouched(t *testing.T) {
	names := testURLSchemeNames(t, "vempty", "vproto", "vtext")
	writeRawURLScheme(t, names[0], "", "", 0)
	proto, _ := openURLSchemeKeyForTest(t, names[1])
	regSetString(proto, "URL Protocol", "")
	syscall.RegCloseKey(proto)
	text, _ := openURLSchemeKeyForTest(t, names[2])
	regSetString(text, "", "URL:"+names[2]+" (Try Omarchy)")
	syscall.RegCloseKey(text)
	snapshot := func() map[string]urlSchemeRegistration {
		out := map[string]urlSchemeRegistration{}
		for _, scheme := range names {
			key, _ := openURLSchemeKeyForTest(t, scheme)
			reg, err := readURLSchemeRegistration(key, scheme)
			syscall.RegCloseKey(key)
			if err != nil {
				t.Fatal(err)
			}
			out[scheme] = reg
		}
		return out
	}
	before := snapshot()
	root := t.TempDir()
	dir, destination := filepath.Join(root, "one"), filepath.Join(root, "two")
	target := filepath.Join(dir, stableLauncherName)
	if err := syncURLSchemeRegistrations(dir, target, nil); err != nil { // start with no approvals, or revoke
		t.Fatal(err)
	}
	if owned, err := ownedURLSchemes(dir); err != nil || len(owned) != 0 {
		t.Fatalf("uninstall would remove %v, %v", owned, err)
	}
	if err := removeOwnedURLSchemes(dir, names); err != nil {
		t.Fatal(err)
	}
	if err := moveURLSchemeRegistrations(dir, destination); err != nil {
		t.Fatal(err)
	}
	for _, scheme := range names {
		if state, _ := urlSchemeOwner(scheme, ownedBy(target, dir)); state != urlSchemeForeign {
			t.Errorf("%s: %v; want foreign", scheme, state)
		}
	}
	if after := snapshot(); !reflect.DeepEqual(before, after) {
		t.Fatalf("vendor keys changed:\n%+v\n%+v", before, after)
	}
}

// openURLSchemeKeyForTest opens the key, creating it when it is missing.
func openURLSchemeKeyForTest(t *testing.T, scheme string) (syscall.Handle, bool) {
	t.Helper()
	path, _ := syscall.UTF16PtrFromString(urlSchemeClassesKey + `\` + scheme)
	var key syscall.Handle
	var disposition uint32
	if r, _, err := procRegCreateKeyExW.Call(uintptr(syscall.HKEY_CURRENT_USER), uintptr(unsafe.Pointer(path)), 0, 0,
		regOptionNonVolatile, urlSchemeKeyWrite, 0, uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&disposition))); r != 0 {
		t.Fatal(err)
	}
	return key, true
}
