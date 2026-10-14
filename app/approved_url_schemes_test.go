package main

import (
	"encoding/base64"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// The same vectors are in the guest's url-schemes tests; keep both in step.
func TestOpenURLSharedContractVectors(t *testing.T) {
	approved := []string{"zoommtg", "steam"}
	for _, test := range []struct{ raw, want string }{
		{"zoommtg:join?x=1", "zoommtg:join?x=1"},
		{"ZoomMtg:join", "zoommtg:join"},
		{"steam://run/10", "steam://run/10"},
		{"-x:y", ""},
		{" zoommtg:x", ""},
		{"\ufeffzoommtg:x", ""},
		{`c:\x`, ""},
		{`\\srv\share`, ""},
		{"zoommtg%3ax", ""},
		{"http://a", ""},
		{"ms-settings:x", ""},
		{"zoommtg:", ""},
		{"zoommtg:a b", ""},
		{`zoommtg:a"b`, ""},
		{`zoommtg:a\b`, ""},
		{"zoommtg:\xff\xfe", ""},
		{"zoommtg:" + strings.Repeat("a", 2049-len("zoommtg:")), ""},
		{"a:x", ""},
		{"slack:open", ""},
		// Host-only extras: the Kelvin sign lowercases to k outside ASCII,
		// and control characters anywhere are refused.
		{"\u212aoommtg:x", ""},
		{"zoom\u212amtg:x", ""},
		{"zoommtg:a\tb", ""},
		{"zoommtg:a\x7fb", ""},
	} {
		got, ok := validOpenURL(test.raw, approved)
		if got != test.want || ok != (test.want != "") {
			t.Errorf("validOpenURL(%q) = %q, %v; want %q", test.raw, got, ok, test.want)
		}
	}
	if got, ok := validOpenURL("zoommtg:"+strings.Repeat("a", 2048-len("zoommtg:")), approved); !ok || len(got) != 2048 {
		t.Fatal("a 2048-byte link was refused")
	}
}

func TestURLSchemeGrammarAndDenylist(t *testing.T) {
	for _, scheme := range []string{"zoommtg", "steam", "ab", "a+b.c-d", strings.Repeat("a", 32), "msteams", "mst"} {
		if !validURLScheme(scheme) {
			t.Errorf("%q refused", scheme)
		}
	}
	for _, scheme := range []string{"", "a", "c", "Zoommtg", "1abc", "-abc", "ab_c", strings.Repeat("a", 33), "http", "https", "file",
		"mailto", "javascript", "search-ms", "ms-settings", "ms-", "ldaps", "zoom mtg", "zo\u00f6m"} {
		if validURLScheme(scheme) {
			t.Errorf("%q accepted", scheme)
		}
	}
}

func TestApprovedURLSchemesStore(t *testing.T) {
	dir := t.TempDir()
	prefs, err := loadApprovedURLSchemes(dir)
	if err != nil || len(prefs.Schemes) != 0 {
		t.Fatalf("missing file: %+v, %v", prefs, err)
	}
	if err := saveApprovedURLSchemes(dir, approvedURLSchemePreferences{Schemes: []string{"zoommtg", "steam"}}); err != nil {
		t.Fatal(err)
	}
	prefs, err = loadApprovedURLSchemes(dir)
	if err != nil || !slices.Equal(prefs.Schemes, []string{"zoommtg", "steam"}) {
		t.Fatalf("loaded %+v, %v", prefs, err)
	}
	for _, bad := range [][]string{{"zoommtg", "zoommtg"}, {"https"}, {"ZoomMtg"}, slices.Repeat([]string{"x"}, 33)} {
		if err := saveApprovedURLSchemes(dir, approvedURLSchemePreferences{Schemes: bad}); err == nil {
			t.Errorf("saved %v", bad)
		}
	}
	for _, content := range []string{
		`{"schemaVersion":1,"schemes":[],"unknown":true}`,
		`{"schemaVersion":2,"schemes":[]}`,
		`{"schemaVersion":1,"schemes":["http"]}`,
		`{"schemaVersion":1,"schemes":[]} {}`,
	} {
		if err := os.WriteFile(approvedURLSchemesPath(dir), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
		// A damaged file is an error for Settings and allows nothing at run time.
		if prefs, err := loadApprovedURLSchemes(dir); err == nil || len(prefs.Schemes) != 0 {
			t.Errorf("%s: %+v, %v", content, prefs, err)
		}
	}
	if err := os.WriteFile(approvedURLSchemesPath(dir), []byte(strings.Repeat(" ", maximumApprovedURLSchemesBytes+1)), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := loadApprovedURLSchemes(dir); err == nil {
		t.Fatal("oversized file accepted")
	}
}

func TestGuestURLSchemesStoreWritesOnlyOnChange(t *testing.T) {
	dir := t.TempDir()
	if wrote, err := saveGuestURLSchemes(dir, []string{"zoommtg", "steam"}); err != nil || !wrote {
		t.Fatalf("first report: %v, %v", wrote, err)
	}
	if wrote, err := saveGuestURLSchemes(dir, []string{"steam", "zoommtg"}); err != nil || wrote {
		t.Fatalf("same report rewrote the file: %v, %v", wrote, err)
	}
	if got, err := loadGuestURLSchemes(dir); err != nil || !slices.Equal(got, []string{"steam", "zoommtg"}) {
		t.Fatalf("stored %v, %v", got, err)
	}
	if wrote, err := saveGuestURLSchemes(dir, []string{}); err != nil || !wrote {
		t.Fatalf("changed report: %v, %v", wrote, err)
	}
	if _, err := saveGuestURLSchemes(dir, []string{"http"}); err == nil {
		t.Fatal("denied scheme stored")
	}
	if err := os.WriteFile(guestURLSchemesPath(dir), []byte(`{"schemes":["file"]}`), 0600); err != nil {
		t.Fatal(err)
	}
	if got, err := loadGuestURLSchemes(dir); err == nil || len(got) != 0 {
		t.Fatalf("untrusted report accepted: %v, %v", got, err)
	}
	if empty, err := loadGuestURLSchemes(t.TempDir()); err != nil || len(empty) != 0 {
		t.Fatalf("missing report: %v, %v", empty, err)
	}
}

func TestURLAgentLines(t *testing.T) {
	if line := urlApprovedLine(nil); line != "url-approved "+base64.StdEncoding.EncodeToString([]byte("[]"))+"\n" {
		t.Fatalf("empty list %q", line)
	}
	if line := urlApprovedLine([]string{"zoommtg"}); line != "url-approved "+base64.StdEncoding.EncodeToString([]byte(`["zoommtg"]`))+"\n" {
		t.Fatalf("list %q", line)
	}
	report := func(json string) string {
		return "url-schemes " + base64.StdEncoding.EncodeToString([]byte(json)) + "\n"
	}
	if got, err := parseURLSchemesLine(report(`["zoommtg","steam"]`)); err != nil || !slices.Equal(got, []string{"zoommtg", "steam"}) {
		t.Fatalf("report %v, %v", got, err)
	}
	if got, err := parseURLSchemesLine(report(`[]`)); err != nil || len(got) != 0 {
		t.Fatalf("empty report %v, %v", got, err)
	}
	encoded := base64.StdEncoding.EncodeToString([]byte(`["zoommtg"]`))
	for _, line := range []string{
		report(`null`), report(`["https"]`), report(`["zoommtg","zoommtg"]`), report(`{"schemes":[]}`), report(`[] []`),
		report(`["` + strings.Join(slices.Repeat([]string{"ab"}, 33), `","`) + `"]`),
		"url-schemes " + encoded, "url-schemes " + encoded[:4] + "\r" + encoded[4:] + "\n", "url-schemes !!!!\n",
		"url-schemes " + strings.Repeat("A", maximumGuestURLSchemesBytes) + "\n",
	} {
		if got, err := parseURLSchemesLine(line); err == nil {
			t.Errorf("%q accepted as %v", line, got)
		}
	}
	link := "zoommtg:join?x=" + strings.Repeat("9", 2000)
	if got, ok := parseOpenURLLine(openURLLine(link)); !ok || got != link {
		t.Fatal("open-url line did not round-trip")
	}
	for _, line := range []string{"open-url \n", "open-url !!\n", "open-url " + base64.StdEncoding.EncodeToString([]byte(strings.Repeat("a", 2049))) + "\n",
		"open-url  " + base64.StdEncoding.EncodeToString([]byte("zoommtg:x")) + "\n", "open-urlx\n"} {
		if _, ok := parseOpenURLLine(line); ok {
			t.Errorf("%q accepted", line)
		}
	}
}

func TestURLActivationAcceptsOnlyTheRegisteredShape(t *testing.T) {
	dir := `C:\Users\me\AppData\Local\TryOmarchy`
	if got, url, activation, ok := parseURLActivation([]string{"-dir", dir, "-open-url", "zoommtg:x"}); !activation || !ok || got != dir || url != "zoommtg:x" {
		t.Fatalf("registered command: %q %q %v %v", got, url, activation, ok)
	}
	if _, _, activation, _ := parseURLActivation([]string{"-dir", dir, "-settings"}); activation {
		t.Fatal("ordinary start treated as a link")
	}
	for _, args := range [][]string{
		{"-open-url", "zoommtg:x"},
		{"-dir", dir, "-open-url", "zoommtg:a", "-share", `C:\`}, // a quote in the link split it
		{"-dir", dir, "-open-url", "zoommtg:a", "-uninstall"},
		{"-dir", dir, "-open-url", "zoommtg:x", "-dir", `D:\other`},
		{"-dir", dir, "-open-url", ""},
		{"-dir", "", "-open-url", "zoommtg:x"},
		{"-dir", "-uninstall", "-open-url", "zoommtg:x"},
		{"-dir", dir, "--open-url", "zoommtg:x"},
		{"-dir", dir, "-open-url=zoommtg:x", "x"},
		{"-open-url", "zoommtg:x", "-dir", dir},
		{"-uninstall", "-dir", dir, "-open-url"},
		{"-dir=" + dir, "-open-url", "zoommtg:x", "x"},
	} {
		if _, _, activation, ok := parseURLActivation(args); !activation || ok {
			t.Errorf("%q: activation %v, accepted %v", args, activation, ok)
		}
	}
}

func TestConcurrentStoreWritersUseTheirOwnTemporaryFiles(t *testing.T) {
	dir := t.TempDir()
	errs := make(chan error, 8)
	for i := 0; i < 8; i++ {
		schemes := []string{"zoommtg"}
		if i%2 == 1 {
			schemes = []string{"steam"}
		}
		go func() { _, err := saveGuestURLSchemes(dir, schemes); errs <- err }()
	}
	for i := 0; i < 8; i++ {
		if err := <-errs; err != nil {
			t.Fatal(err)
		}
	}
	if got, err := loadGuestURLSchemes(dir); err != nil || len(got) != 1 {
		t.Fatalf("stored %v, %v", got, err)
	}
	entries, _ := os.ReadDir(filepath.Dir(guestURLSchemesPath(dir)))
	if len(entries) != 1 {
		t.Fatalf("temporary files left behind: %v", entries)
	}
}
