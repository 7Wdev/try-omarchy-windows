//go:build windows

package main

import (
	"errors"
	"fmt"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"syscall"
	"unsafe"
)

// Each allowed link type is registered for this Windows user under
// HKCU\Software\Classes\<scheme>. A key belongs to an installation only when
// it names that installation's folder and runs its launcher, so a second
// install or another app's handler is never changed or removed.
const urlSchemeClassesKey = `Software\Classes`
const urlSchemeOwnerValue = "TryOmarchyDir"

var (
	procAssocQueryStringW = syscall.NewLazyDLL("shlwapi.dll").NewProc("AssocQueryStringW")
	procRegDeleteKeyW     = advapi32.NewProc("RegDeleteKeyW")
)

const (
	assocfIsProtocol  = 0x00001000
	assocstrCommand   = 1
	errNoAssociation  = 0x80070483 // HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION)
	urlSchemeKeyWrite = syscall.KEY_WRITE | syscall.KEY_READ
	// DELETE access right
	urlSchemeDeleteAccess = 0x00010000
)

// urlSchemeCommand is what Windows runs for a link. Paths cannot contain
// double quotes; the link itself is checked by parseURLActivation.
func urlSchemeCommand(target, dir string) string {
	return `"` + target + `" -dir "` + dir + `" -open-url "%1"`
}

// windowsURLHandler returns the command Windows itself would run for the
// scheme, resolving the user's choice and machine-wide handlers. free is
// true only when Windows reports no association at all.
func windowsURLHandler(scheme string) (command string, free bool) {
	name, err := syscall.UTF16PtrFromString(scheme)
	if err != nil {
		return "", false
	}
	size := uint32(0)
	r, _, _ := procAssocQueryStringW.Call(assocfIsProtocol, assocstrCommand, uintptr(unsafe.Pointer(name)), 0, 0, uintptr(unsafe.Pointer(&size)))
	if uint32(r) == errNoAssociation {
		return "", true
	}
	if (r != 0 && r != 1) || size == 0 || size > 32<<10 {
		return "", false
	}
	buf := make([]uint16, size)
	r, _, _ = procAssocQueryStringW.Call(assocfIsProtocol, assocstrCommand, uintptr(unsafe.Pointer(name)), 0, uintptr(unsafe.Pointer(&buf[0])), uintptr(unsafe.Pointer(&size)))
	if r != 0 {
		return "", false
	}
	return syscall.UTF16ToString(buf), false
}

// urlSchemeHandledByWindows reports whether another app, or an unreadable
// association, already handles the scheme. Our own registration is not.
func urlSchemeHandledByWindows(scheme, target, dir string) bool {
	command, free := windowsURLHandler(scheme)
	return !free && !strings.EqualFold(command, urlSchemeCommand(target, dir))
}

type urlSchemeKeyState int

const (
	urlSchemeAbsent urlSchemeKeyState = iota
	urlSchemeForeign
	urlSchemeOwned
)

const regOpenedExistingKey = 2

var errRegistryValueType = errors.New("registry value is not a string")

// readRegistryString tells a missing value from one that is unreadable, of
// another type or too long; only a present, readable REG_SZ is a value.
func readRegistryString(key syscall.Handle, name string) (value string, present bool, err error) {
	valueName, _ := syscall.UTF16PtrFromString(name)
	var valueType, size uint32
	err = syscall.RegQueryValueEx(key, valueName, nil, &valueType, nil, &size)
	if errors.Is(err, syscall.ERROR_FILE_NOT_FOUND) {
		return "", false, nil
	}
	if err != nil {
		return "", true, err
	}
	if valueType != syscall.REG_SZ || size > 32<<10 || size%2 != 0 {
		return "", true, errRegistryValueType
	}
	buf := make([]uint16, size/2+1)
	if err := syscall.RegQueryValueEx(key, valueName, nil, &valueType, (*byte)(unsafe.Pointer(&buf[0])), &size); err != nil {
		return "", true, err
	}
	if valueType != syscall.REG_SZ {
		return "", true, errRegistryValueType
	}
	return syscall.UTF16ToString(buf), true, nil
}

// urlSchemeRegistration is what an open HKCU\Software\Classes\<scheme> key
// says about its owner. A value that is missing, unreadable or not a string
// leaves its ok flag false, and such a key is never ours. An unmarked key
// holding exactly what writeURLSchemeKey writes before its marker is a
// leftover of an interrupted first registration.
type urlSchemeRegistration struct {
	marker, command     string
	markerOK, commandOK bool
	leftover            bool
	leftoverCommand     string
	leftoverHasCommand  bool
}

func readURLSchemeRegistration(key syscall.Handle, scheme string) (urlSchemeRegistration, error) {
	var reg urlSchemeRegistration
	marker, markerPresent, err := readRegistryString(key, urlSchemeOwnerValue)
	if err != nil && !errors.Is(err, errRegistryValueType) {
		return reg, err
	}
	reg.marker, reg.markerOK = marker, markerPresent && err == nil && marker != ""
	if !markerPresent {
		if reg.leftover, reg.leftoverCommand, reg.leftoverHasCommand, err = urlSchemeFootprint(key, scheme); err != nil {
			return reg, err
		}
	}
	commandPath, _ := syscall.UTF16PtrFromString(`shell\open\command`)
	var sub syscall.Handle
	if err := syscall.RegOpenKeyEx(key, commandPath, 0, syscall.KEY_READ, &sub); errors.Is(err, syscall.ERROR_FILE_NOT_FOUND) {
		return reg, nil
	} else if err != nil {
		return reg, err
	}
	defer syscall.RegCloseKey(sub)
	command, present, err := readRegistryString(sub, "")
	if err != nil && !errors.Is(err, errRegistryValueType) {
		return reg, err
	}
	reg.command, reg.commandOK = command, present && err == nil && command != ""
	return reg, nil
}

func registryCounts(key syscall.Handle) (subkeys, values uint32, err error) {
	err = syscall.RegQueryInfoKey(key, nil, nil, nil, &subkeys, nil, nil, &values, nil, nil, nil, nil)
	return subkeys, values, err
}

// urlSchemeFootprint reports whether a key holds nothing but our default
// value and URL Protocol, each optional, and at most the chain
// shell\open\command whose only value is a command string.
func urlSchemeFootprint(key syscall.Handle, scheme string) (ok bool, command string, hasCommand bool, err error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	subkeys, values, err := registryCounts(key)
	if err != nil {
		return false, "", false, err
	}
	present := uint32(0)
	for _, want := range [][2]string{{"", "URL:" + scheme + " (Try Omarchy)"}, {"URL Protocol", ""}} {
		value, found, err := readRegistryString(key, want[0])
		if errors.Is(err, errRegistryValueType) {
			return false, "", false, nil
		}
		if err != nil {
			return false, "", false, err
		}
		if found {
			if value != want[1] {
				return false, "", false, nil
			}
			present++
		}
	}
	if values != present {
		return false, "", false, nil
	}
	current := key
	for _, name := range []string{"shell", "open", "command"} {
		if subkeys == 0 {
			return true, "", false, nil
		}
		if subkeys != 1 {
			return false, "", false, nil
		}
		buf := make([]uint16, 256)
		n := uint32(len(buf))
		if err := syscall.RegEnumKeyEx(current, 0, &buf[0], &n, nil, nil, nil, nil); err != nil {
			return false, "", false, err
		}
		if !strings.EqualFold(syscall.UTF16ToString(buf[:n]), name) {
			return false, "", false, nil
		}
		var sub syscall.Handle
		if err := syscall.RegOpenKeyEx(current, &buf[0], 0, syscall.KEY_READ, &sub); err != nil {
			return false, "", false, err
		}
		defer syscall.RegCloseKey(sub)
		current = sub
		if subkeys, values, err = registryCounts(current); err != nil {
			return false, "", false, err
		}
		if name != "command" && values != 0 {
			return false, "", false, nil
		}
	}
	if subkeys != 0 || values > 1 {
		return false, "", false, nil
	}
	if values == 0 {
		return true, "", false, nil
	}
	command, found, err := readRegistryString(current, "")
	if errors.Is(err, errRegistryValueType) || err == nil && !found {
		return false, "", false, nil
	}
	if err != nil {
		return false, "", false, err
	}
	return true, command, true, nil
}

// runs reports whether the key names dir and runs command.
func (reg urlSchemeRegistration) runs(dir, command string) bool {
	return reg.markerOK && reg.commandOK && pathsEqual(reg.marker, dir) && strings.EqualFold(reg.command, command)
}

// recoverable reports whether an interrupted first registration of command
// left this key. The command is written first, so a key without it is
// never ours.
func (reg urlSchemeRegistration) recoverable(command string) bool {
	return reg.leftover && reg.leftoverHasCommand && strings.EqualFold(reg.leftoverCommand, command)
}

func ownedBy(target, dir string) func(urlSchemeRegistration) bool {
	command := urlSchemeCommand(target, dir)
	return func(reg urlSchemeRegistration) bool { return reg.runs(dir, command) || reg.recoverable(command) }
}

func openURLSchemeKey(scheme string) (syscall.Handle, bool, error) {
	path, _ := syscall.UTF16PtrFromString(urlSchemeClassesKey + `\` + scheme)
	var key syscall.Handle
	if err := syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, path, 0, syscall.KEY_READ, &key); err != nil {
		if errors.Is(err, syscall.ERROR_FILE_NOT_FOUND) {
			return 0, false, nil
		}
		return 0, false, fmt.Errorf("reading link type %s: %w", scheme, err)
	}
	return key, true, nil
}

// urlSchemeOwner classifies HKCU\Software\Classes\<scheme> for owns.
func urlSchemeOwner(scheme string, owns func(urlSchemeRegistration) bool) (urlSchemeKeyState, error) {
	key, found, err := openURLSchemeKey(scheme)
	if err != nil {
		return urlSchemeForeign, err
	}
	if !found {
		return urlSchemeAbsent, nil
	}
	defer syscall.RegCloseKey(key)
	reg, err := readURLSchemeRegistration(key, scheme)
	if err != nil {
		return urlSchemeForeign, fmt.Errorf("reading link type %s: %w", scheme, err)
	}
	if owns(reg) {
		return urlSchemeOwned, nil
	}
	return urlSchemeForeign, nil
}

var errURLSchemeNotOurs = errors.New("the link type now belongs to another app")

// writeURLSchemeKey sets only this installation's values. An existing key
// is checked again on the handle it was opened with, so a key another app
// created after the caller looked is left alone. The command goes first and
// the marker last, so an interrupted write leaves either an empty key, which
// is never touched, or a leftover with our command (see recoverable).
func writeURLSchemeKey(scheme, target, dir string, accept func(urlSchemeRegistration) bool) error {
	create := func(parent syscall.Handle, path string) (syscall.Handle, uint32, error) {
		p, _ := syscall.UTF16PtrFromString(path)
		var key syscall.Handle
		var disposition uint32
		r, _, err := procRegCreateKeyExW.Call(uintptr(parent), uintptr(unsafe.Pointer(p)), 0, 0,
			regOptionNonVolatile, urlSchemeKeyWrite, 0, uintptr(unsafe.Pointer(&key)), uintptr(unsafe.Pointer(&disposition)))
		if r != 0 {
			return 0, 0, fmt.Errorf("registering link type %s: %v", scheme, err)
		}
		return key, disposition, nil
	}
	key, disposition, err := create(syscall.HKEY_CURRENT_USER, urlSchemeClassesKey+`\`+scheme)
	if err != nil {
		return err
	}
	defer syscall.RegCloseKey(key)
	if disposition == regOpenedExistingKey {
		reg, err := readURLSchemeRegistration(key, scheme)
		if err != nil {
			return fmt.Errorf("reading link type %s: %w", scheme, err)
		}
		if !accept(reg) {
			return fmt.Errorf("registering link type %s: %w", scheme, errURLSchemeNotOurs)
		}
	}
	command, _, err := create(key, `shell\open\command`)
	if err != nil {
		return err
	}
	err = regSetString(command, "", urlSchemeCommand(target, dir))
	syscall.RegCloseKey(command)
	if err != nil {
		return err
	}
	for _, value := range [][2]string{{"", "URL:" + scheme + " (Try Omarchy)"}, {"URL Protocol", ""}} {
		if err := regSetString(key, value[0], value[1]); err != nil {
			return err
		}
	}
	return regSetString(key, urlSchemeOwnerValue, dir)
}

// deleteURLSchemeKey removes the key only if accept holds on the handle it
// deletes through. RegDeleteKeyW then removes the emptied key, and fails if
// something filled it again in between; that key is left alone.
func deleteURLSchemeKey(scheme string, accept func(urlSchemeRegistration) bool) error {
	const access = urlSchemeDeleteAccess | syscall.KEY_ENUMERATE_SUB_KEYS | syscall.KEY_QUERY_VALUE | syscall.KEY_SET_VALUE
	path, _ := syscall.UTF16PtrFromString(urlSchemeClassesKey + `\` + scheme)
	var key syscall.Handle
	if err := syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, path, 0, access, &key); err != nil {
		if errors.Is(err, syscall.ERROR_FILE_NOT_FOUND) {
			return nil
		}
		return fmt.Errorf("removing link type %s: %w", scheme, err)
	}
	reg, err := readURLSchemeRegistration(key, scheme)
	if err != nil {
		syscall.RegCloseKey(key)
		return fmt.Errorf("reading link type %s: %w", scheme, err)
	}
	if !accept(reg) {
		syscall.RegCloseKey(key)
		return nil
	}
	r, _, callErr := procRegDeleteTreeW.Call(uintptr(key), 0)
	syscall.RegCloseKey(key)
	if r != 0 {
		return fmt.Errorf("removing link type %s: %v", scheme, callErr)
	}
	parent, _ := syscall.UTF16PtrFromString(urlSchemeClassesKey)
	var classes syscall.Handle
	if err := syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, parent, 0, urlSchemeKeyWrite, &classes); err != nil {
		return fmt.Errorf("removing link type %s: %w", scheme, err)
	}
	defer syscall.RegCloseKey(classes)
	name, _ := syscall.UTF16PtrFromString(scheme)
	if r, _, _ := procRegDeleteKeyW.Call(uintptr(classes), uintptr(unsafe.Pointer(name))); r != 0 && r != uintptr(syscall.ERROR_FILE_NOT_FOUND) {
		logf("links: link type %s left in place (refilled during removal, %d)", scheme, r)
	}
	return nil
}

// ownedURLSchemes lists the registrations that belong to dir. It only reads
// the registry, so it also works after the folder is gone.
func ownedURLSchemes(dir string) ([]string, error) {
	return listURLSchemeKeys(ownedBy(filepath.Join(dir, stableLauncherName), dir))
}

func listURLSchemeKeys(match func(urlSchemeRegistration) bool) ([]string, error) {
	// Successive RegEnumKeyEx calls must stay on one OS thread.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	parent, _ := syscall.UTF16PtrFromString(urlSchemeClassesKey)
	var key syscall.Handle
	if err := syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, parent, 0, syscall.KEY_READ|syscall.KEY_ENUMERATE_SUB_KEYS, &key); err != nil {
		if errors.Is(err, syscall.ERROR_FILE_NOT_FOUND) {
			return nil, nil
		}
		return nil, fmt.Errorf("listing link types: %w", err)
	}
	defer syscall.RegCloseKey(key)
	var found []string
	var problems []error
	for index := uint32(0); ; index++ {
		name := make([]uint16, 256)
		nameLen := uint32(len(name))
		err := syscall.RegEnumKeyEx(key, index, &name[0], &nameLen, nil, nil, nil, nil)
		if errors.Is(err, syscall.Errno(259)) { // ERROR_NO_MORE_ITEMS
			break
		}
		if errors.Is(err, syscall.ERROR_MORE_DATA) {
			continue // longer than any scheme this launcher registers
		}
		if err != nil {
			return nil, fmt.Errorf("listing link types: %w", err)
		}
		// Only lowercase names in the scheme grammar can be ours; this skips
		// file extensions, CLSID and the like without opening them.
		scheme := syscall.UTF16ToString(name[:nameLen])
		if !validURLScheme(scheme) {
			continue
		}
		state, err := urlSchemeOwner(scheme, match)
		if err != nil {
			problems = append(problems, err)
		} else if state == urlSchemeOwned {
			found = append(found, scheme)
		}
	}
	return found, errors.Join(problems...)
}

// syncURLSchemeRegistrations registers the allowed link types that Windows
// does not already handle and removes this installation's other ones.
// Callers hold the host mutation lock (lockMoveStore), so this launcher's
// processes never interleave registry changes; another app's installer
// changing the same key at the same moment is not guarded against.
func syncURLSchemeRegistrations(dir, target string, approved []string) error {
	owns := ownedBy(target, dir)
	var problems []error
	for _, scheme := range approved {
		if !validURLScheme(scheme) {
			continue
		}
		state, err := urlSchemeOwner(scheme, owns)
		if err != nil {
			problems = append(problems, err)
			continue
		}
		if state == urlSchemeForeign || urlSchemeHandledByWindows(scheme, target, dir) {
			logf("links: link type %s is used by another Windows app; not registered", scheme)
			continue
		}
		if err := writeURLSchemeKey(scheme, target, dir, owns); err != nil {
			problems = append(problems, err)
		}
	}
	owned, err := listURLSchemeKeys(owns)
	if err != nil {
		problems = append(problems, err)
	}
	for _, scheme := range owned {
		if !slices.Contains(approved, scheme) {
			if err := deleteURLSchemeKey(scheme, owns); err != nil {
				problems = append(problems, err)
			}
		}
	}
	return errors.Join(problems...)
}

// movedURLScheme accepts a key of this move in any state its writes can
// leave: marker and command each naming the source or the destination.
func movedURLScheme(source, destination string) func(urlSchemeRegistration) bool {
	commands := []string{urlSchemeCommand(filepath.Join(source, stableLauncherName), source),
		urlSchemeCommand(filepath.Join(destination, stableLauncherName), destination)}
	return func(reg urlSchemeRegistration) bool {
		if slices.ContainsFunc(commands, reg.recoverable) {
			return true
		}
		return reg.markerOK && reg.commandOK && (pathsEqual(reg.marker, source) || pathsEqual(reg.marker, destination)) &&
			slices.ContainsFunc(commands, func(command string) bool { return strings.EqualFold(reg.command, command) })
	}
}

// moveURLSchemeRegistrations points the source installation's link types at
// the destination, command first and marker last. A retry after any
// interrupted write finds the key again and finishes it.
func moveURLSchemeRegistrations(source, destination string) error {
	accept := movedURLScheme(source, destination)
	target := filepath.Join(destination, stableLauncherName)
	schemes, err := listURLSchemeKeys(accept)
	var problems []error
	if err != nil {
		problems = append(problems, err)
	}
	for _, scheme := range schemes {
		if err := writeURLSchemeKey(scheme, target, destination, accept); err != nil {
			problems = append(problems, err)
		}
	}
	return errors.Join(problems...)
}

// removeOwnedURLSchemes deletes registrations that still belong to dir.
func removeOwnedURLSchemes(dir string, schemes []string) error {
	owns := ownedBy(filepath.Join(dir, stableLauncherName), dir)
	var problems []error
	for _, scheme := range schemes {
		if err := deleteURLSchemeKey(scheme, owns); err != nil {
			problems = append(problems, err)
		}
	}
	return errors.Join(problems...)
}
