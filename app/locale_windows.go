package main

import (
	"syscall"
	"unsafe"
)

// hostTimeZoneKey reads the Windows time zone key name, for example
// "Eastern Standard Time", which the CLDR table maps to an IANA zone.
func hostTimeZoneKey() string {
	path, _ := syscall.UTF16PtrFromString(`SYSTEM\CurrentControlSet\Control\TimeZoneInformation`)
	var key syscall.Handle
	if syscall.RegOpenKeyEx(syscall.HKEY_LOCAL_MACHINE, path, 0, syscall.KEY_READ, &key) != nil {
		return ""
	}
	defer syscall.RegCloseKey(key)
	return registryString(key, "TimeZoneKeyName")
}

// hostKeyboardLayoutID reads the user's default input language as the
// eight-digit keyboard layout identifier at the head of the Preload list.
func hostKeyboardLayoutID() string {
	path, _ := syscall.UTF16PtrFromString(`Keyboard Layout\Preload`)
	var key syscall.Handle
	if syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, path, 0, syscall.KEY_READ, &key) != nil {
		return ""
	}
	defer syscall.RegCloseKey(key)
	klid := registryString(key, "1")
	path, _ = syscall.UTF16PtrFromString(`Keyboard Layout\Substitutes`)
	var substitutes syscall.Handle
	if syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, path, 0, syscall.KEY_READ, &substitutes) != nil {
		return klid
	}
	defer syscall.RegCloseKey(substitutes)
	return resolveKeyboardSubstitute(klid, func(id string) string { return registryString(substitutes, id) })
}

var procGetUserPreferredUILanguages = kernel32.NewProc("GetUserPreferredUILanguages")

// hostLocaleName reads the preferred UI language, independently of regional formats.
func hostLocaleName() string {
	return readPreferredUILanguage(func(count, size *uint32, buf []uint16) bool {
		var buffer *uint16
		if len(buf) > 0 {
			buffer = &buf[0]
		}
		ok, _, _ := procGetUserPreferredUILanguages.Call(8, uintptr(unsafe.Pointer(count)), uintptr(unsafe.Pointer(buffer)), uintptr(unsafe.Pointer(size)))
		return ok != 0
	})
}

// hostLocale resolves what the guest should follow, honoring the explicit
// overrides: "" follows Windows, "keep" leaves the guest alone, anything else
// is used as given.
func hostLocale(zoneOverride, keyboardOverride, localeOverride string) (zone, layout, variant, locale string) {
	switch localeOverride {
	case "":
		name := hostLocaleName()
		locale = posixLocaleForWindows(name)
		if locale == "" {
			logf("host UI language %q has no locale mapping; keeping guest locale", name)
		} else if name == "en-150" || name == "es-419" {
			logf("host UI language %q uses guest locale fallback %s", name, locale)
		}
	case "keep":
	default:
		locale = localeOverride
	}
	switch zoneOverride {
	case "":
		zone = ianaZoneForWindows(hostTimeZoneKey())
	case "keep":
	default:
		zone = zoneOverride
	}
	switch keyboardOverride {
	case "":
		klid := hostKeyboardLayoutID()
		layout, variant = xkbForKLID(klid)
		if layout == "" {
			logf("host keyboard %q has no XKB mapping; keeping guest keyboard", klid)
		} else if _, exact := keyboardLayoutForKLID[klid]; !exact && len(klid) == 8 && klid[:4] != "0000" {
			logf("host keyboard %q uses language fallback %s:%s", klid, layout, variant)
		}
	case "keep":
	default:
		layout, variant = splitKeyboardSpec(keyboardOverride)
	}
	return zone, layout, variant, locale
}
