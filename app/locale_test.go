package main

import (
	"strings"
	"testing"
)

func TestIANAZoneForWindowsUsesTheCLDRTable(t *testing.T) {
	for key, want := range map[string]string{
		"Eastern Standard Time": "America/New_York", "W. Europe Standard Time": "Europe/Berlin",
		"UTC": "Etc/UTC", "Tokyo Standard Time": "Asia/Tokyo", "AUS Eastern Standard Time": "Australia/Sydney",
		"Nowhere Standard Time": "", "": "",
	} {
		if got := ianaZoneForWindows(key); got != want {
			t.Errorf("%q: got %q, want %q", key, got, want)
		}
	}
}

func TestXKBForKLIDPrefersFullIdentifiers(t *testing.T) {
	for klid, want := range map[string][2]string{
		"00000409": {"us", ""}, "00020409": {"us", "intl"}, "00010409": {"us", "dvorak"},
		"00000407": {"de", ""}, "0000040C": {"fr", ""}, "00000809": {"gb", ""}, "00000416": {"br", ""},
		"0000080a": {"latam", ""}, "00000419": {"ru", ""}, "00000411": {"jp", ""},
		"0001041f": {"tr", "f"}, "0000041f": {"tr", ""}, "zzzz0409": {"", ""}, "0000ffff": {"", ""}, "409": {"", ""}, "": {"", ""},
	} {
		layout, variant := xkbForKLID(klid)
		if layout != want[0] || variant != want[1] {
			t.Errorf("%q: got %q/%q, want %q/%q", klid, layout, variant, want[0], want[1])
		}
	}
}

func TestHostLocaleCmdlineOnlyCarriesValidValues(t *testing.T) {
	if got := hostLocaleCmdline("America/New_York", "de", "", ""); got != " tryomarchy.tz=America/New_York tryomarchy.kb=de" {
		t.Fatalf("plain: %q", got)
	}
	if got := hostLocaleCmdline("Europe/Berlin", "us", "intl", ""); got != " tryomarchy.tz=Europe/Berlin tryomarchy.kb=us:intl" {
		t.Fatalf("variant: %q", got)
	}
	if got := hostLocaleCmdline("", "", "", ""); got != "" {
		t.Fatalf("empty: %q", got)
	}
	for _, bad := range []string{"../etc", "us us", "a b", "Etc UTC", "us;rm"} {
		for _, got := range []string{hostLocaleCmdline(bad, "us", "", ""), hostLocaleCmdline("Etc/UTC", bad, "", ""), hostLocaleCmdline("Etc/UTC", "us", bad, ""), hostLocaleCmdline("Etc/UTC", "us", "", bad)} {
			if strings.Contains(got, bad) {
				t.Errorf("unsafe value %q reached the command line: %q", bad, got)
			}
		}
	}
	if got := hostLocaleCmdline("../etc", "us", "", ""); got != " tryomarchy.kb=us" {
		t.Fatalf("a bad zone must not drop the keyboard: %q", got)
	}
}

func TestSplitKeyboardSpec(t *testing.T) {
	for spec, want := range map[string][2]string{"de": {"de", ""}, " us:intl ": {"us", "intl"}, "": {"", ""}} {
		layout, variant := splitKeyboardSpec(spec)
		if layout != want[0] || variant != want[1] {
			t.Errorf("%q: got %q/%q", spec, layout, variant)
		}
	}
}

func TestPosixLocaleForWindows(t *testing.T) {
	for name, want := range map[string]string{"en-150": "en_GB", "es-419": "es_MX", "fr-029": "", "de-DE": "de_DE", "pt-BR": "pt_BR", "en-US": "en_US", " ja-JP ": "ja_JP", "sr-Latn-RS": "", "de": "", "": "", "x-y": ""} {
		if got := posixLocaleForWindows(name); got != want {
			t.Errorf("%q: got %q, want %q", name, got, want)
		}
	}
	if got := hostLocaleCmdline("Europe/Berlin", "de", "", "de_DE"); got != " tryomarchy.tz=Europe/Berlin tryomarchy.locale=de_DE tryomarchy.kb=de" {
		t.Fatalf("locale on the command line: %q", got)
	}
}

func TestKeyboardSubstitutes(t *testing.T) {
	for _, tt := range []struct {
		preload     string
		substitutes map[string]string
		want        [2]string
	}{
		{"d0000409", map[string]string{"d0000409": "00010409"}, [2]string{"us", "dvorak"}},
		{"d0010409", map[string]string{"d0010409": "00020409"}, [2]string{"us", "intl"}},
		{"D000041F", map[string]string{"d000041f": "0001041f"}, [2]string{"tr", "f"}},
		{"d0000409", map[string]string{"d0000409": "d0010409", "d0010409": "00020409"}, [2]string{"us", "intl"}},
		{"d0000409", map[string]string{"d0000409": "d0010409", "d0010409": "d0000409"}, [2]string{}},
		{"d0000409", map[string]string{"d0000409": "garbage"}, [2]string{}},
		{"00000407", nil, [2]string{"de", ""}},
	} {
		klid := resolveKeyboardSubstitute(tt.preload, func(id string) string { return tt.substitutes[id] })
		layout, variant := xkbForKLID(klid)
		if [2]string{layout, variant} != tt.want {
			t.Errorf("%s: %s/%s want %v", tt.preload, layout, variant, tt.want)
		}
	}
	calls := 0
	if got := resolveKeyboardSubstitute("d0000409", func(id string) string { calls++; return "d0000409" }); got != "" || calls != 1 {
		t.Fatal("unbounded cycle", got, calls)
	}
}

func TestPreferredUILanguageIgnoresRegionalFormats(t *testing.T) {
	// The UI language list is independent of de-DE regional formats.
	buf := []uint16{'e', 'n', '-', 'U', 'S', 0, 'd', 'e', '-', 'D', 'E', 0, 0}
	if got := posixLocaleForWindows(preferredUILanguage(buf)); got != "en_US" {
		t.Fatal(got)
	}
	for _, buf := range [][]uint16{nil, {0, 0}, {'e', 'n'}} {
		if got := preferredUILanguage(buf); got != "" {
			t.Fatal(got)
		}
	}
}

func TestReadPreferredUILanguage(t *testing.T) {
	languages := []uint16{'e', 'n', '-', 'U', 'S', 0, 'd', 'e', '-', 'D', 'E', 0, 0}
	calls := 0
	got := readPreferredUILanguage(func(count, size *uint32, buf []uint16) bool {
		calls++
		*count = 2
		*size = uint32(len(languages))
		if buf != nil {
			copy(buf, languages)
		}
		return true
	})
	if got != "en-US" || calls != 2 {
		t.Fatal(got, calls)
	}
	for _, mode := range []string{"failure", "oversized", "changed size", "empty list"} {
		got := readPreferredUILanguage(func(count, size *uint32, buf []uint16) bool {
			*size = 6
			*count = 1
			switch mode {
			case "failure":
				return false
			case "oversized":
				*size = 65537
			case "changed size":
				if buf != nil {
					*size = 7
				}
			case "empty list":
				*count = 0
			}
			return true
		})
		if got != "" {
			t.Fatalf("%s: %s", mode, got)
		}
	}
	for _, name := range []string{"en-150", "es-419"} {
		locale := posixLocaleForWindows(name)
		if got := hostLocaleCmdline("", "", "", locale); got != " tryomarchy.locale="+locale {
			t.Fatal(got)
		}
	}
}
