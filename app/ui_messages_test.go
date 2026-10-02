package main

import (
	"encoding/json"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"testing"
	"testing/fstest"
	"unicode"
)

func TestEmbeddedLauncherMessages(t *testing.T) {
	catalogs, err := readUICatalogs(uiLocaleFiles)
	if err != nil {
		t.Fatal(err)
	}
	if got := selectUILanguage([]string{"zh-CN"}, catalogs); got != "zh-Hans" {
		t.Fatalf("Simplified Chinese Windows language must select zh-Hans, got %q", got)
	}
	translator := uiTranslator{language: "zh-Hans", catalogs: catalogs}
	if got := translator.text("about.title"); got != "关于 Try Omarchy" {
		t.Errorf("Simplified Chinese title is incorrect: %q", got)
	}
	message := uiTextWith("about.body", map[string]string{
		"version": "v0.5.0", "website": "https://tryomarchy.com", "source": "https://github.com/omacom/try-omarchy-windows",
	})
	for _, want := range []string{"Try Omarchy v0.5.0", "https://tryomarchy.com", "https://github.com/omacom/try-omarchy-windows"} {
		if !strings.Contains(message, want) {
			t.Errorf("About message is missing %q", want)
		}
	}
	if uiPlaceholder.MatchString(message) {
		t.Errorf("About message has an unfilled placeholder: %q", message)
	}
	if got := selectUILanguage([]string{"ko-KR"}, catalogs); got != "ko" {
		t.Fatalf("Korean Windows language must select ko, got %q", got)
	}
	if got := (uiTranslator{language: "ko", catalogs: catalogs}).text("about.title"); got != "Try Omarchy 정보" {
		t.Errorf("Korean title is incorrect: %q", got)
	}
	if got := (uiTranslator{language: "en", catalogs: catalogs}).text("about.title"); got != "About Try Omarchy" {
		t.Fatalf("English catalog changed unexpectedly: %q", got)
	}
	for _, key := range []string{"about.no_update", "about.update_available"} {
		message := uiTextWith(key, map[string]string{"installed": "v0.5.0", "latest": "v0.6.0"})
		if !strings.Contains(message, "v0.5.0") || !strings.Contains(message, "v0.6.0") || uiPlaceholder.MatchString(message) {
			t.Errorf("%s: broken version substitution: %q", key, message)
		}
	}
}

func TestLauncherLanguageSelectionKeepsChineseScriptsSeparate(t *testing.T) {
	catalogs := map[string]map[string]string{
		"en": {"button": "Close"}, "zh-Hans": {"button": "关闭"}, "zh-Hant": {"button": "關閉"}, "es": {"button": "Cerrar"},
	}
	for _, tc := range []struct {
		preferred []string
		want      string
	}{
		{[]string{"zh-CN"}, "zh-Hans"},
		{[]string{"zh-SG"}, "zh-Hans"},
		{[]string{"zh-TW"}, "zh-Hant"},
		{[]string{"zh-HK"}, "zh-Hant"},
		{[]string{"zh-Hant-TW"}, "zh-Hant"},
		{[]string{"es-MX"}, "es"},
		{[]string{"fr-FR", "zh-CN"}, "zh-Hans"},
		{[]string{"ja-JP"}, "en"},
	} {
		if got := selectUILanguage(tc.preferred, catalogs); got != tc.want {
			t.Errorf("%v: got %q, want %q", tc.preferred, got, tc.want)
		}
	}
	if got := selectUILanguage([]string{"zh-TW"}, map[string]map[string]string{"en": {}, "zh-Hans": {}}); got != "en" {
		t.Errorf("Traditional Chinese must not use Simplified text, got %q", got)
	}
	translator := uiTranslator{language: "zh-Hans", catalogs: map[string]map[string]string{
		"en": {"button": "Close", "help": "Help"}, "zh-Hans": {"button": "关闭"},
	}}
	if got := translator.text("button"); got != "关闭" {
		t.Errorf("translated message: %q", got)
	}
	if got := translator.text("help"); got != "Help" {
		t.Errorf("missing translation fallback: %q", got)
	}
}

func TestLauncherCatalogRejectsBrokenPlaceholders(t *testing.T) {
	files := fstest.MapFS{
		"ui-locales/en.json":      {Data: []byte(`{"prompt":"Remove {path}?"}`)},
		"ui-locales/zh-Hans.json": {Data: []byte(`{"prompt":"删除 {file}？"}`)},
	}
	if _, err := readUICatalogs(files); err == nil || !strings.Contains(err.Error(), "placeholders differ") {
		t.Fatalf("expected placeholder validation, got %v", err)
	}
	files["ui-locales/zh-Hans.json"] = &fstest.MapFile{Data: []byte(`{"prompt":"删除 {path}？","unknown":"x"}`)}
	if _, err := readUICatalogs(files); err == nil || !strings.Contains(err.Error(), "unknown message") {
		t.Fatalf("expected unknown-key validation, got %v", err)
	}
}

// TestLauncherTranslationCoverage lists what each language still needs.
// Untranslated messages fall back to English, so this never fails; run
//
//	go test -run TestLauncherTranslationCoverage -v
//
// to see the list.
func TestLauncherTranslationCoverage(t *testing.T) {
	catalogs, err := readUICatalogs(uiLocaleFiles)
	if err != nil {
		t.Fatal(err)
	}
	keys := slices.Sorted(func(yield func(string) bool) {
		for key := range catalogs["en"] {
			if !yield(key) {
				return
			}
		}
	})
	languages := slices.Sorted(func(yield func(string) bool) {
		for language := range catalogs {
			if language != "en" && !yield(language) {
				return
			}
		}
	})
	// TRY_OMARCHY_UI_LANGUAGE=ko lists one language as JSON lines, ready to
	// paste into ui-locales/ko.json and translate.
	only := os.Getenv("TRY_OMARCHY_UI_LANGUAGE")
	for _, language := range languages {
		if only != "" && language != only {
			continue
		}
		var missing []string
		for _, key := range keys {
			if catalogs[language][key] == "" {
				missing = append(missing, key)
			}
		}
		t.Logf("%s: %d of %d messages translated", language, len(keys)-len(missing), len(keys))
		for _, key := range missing {
			name, _ := json.Marshal(key)
			value, _ := json.Marshal(catalogs["en"][key])
			t.Logf("  %s: %s,", name, value)
		}
	}
}

// TestLauncherTextComesFromTheCatalog keeps English out of the code: text
// passed to a dialog, status line, menu or control must come from uiText or
// uiTextWith, so every language can translate it.
func TestLauncherTextComesFromTheCatalog(t *testing.T) {
	sinks := map[string]bool{
		"msgBox": true, "errorBox": true, "infoBox": true, "fatal": true,
		"chooseAction": true, "chooseActionWithTextHeight": true, "chooseInstallAction": true,
		"chooseRecoveryPath": true, "chooseWindowsPath": true, "browseForFolder": true,
		"setText": true, "usbSetText": true, "appendItem": true, "setStatus": true, "uiStatus": true,
		"notificationText": true, "control": true, "mk": true, "button": true, "add": true,
		"addHeader": true, "note": true, "check": true, "section": true,
	}
	stores := map[string]bool{"cancelMessage": true, "cancelStatus": true, "cameraState": true, "status": true}
	// Window classes, key names and format verbs are not words to translate.
	allowed := map[string]bool{
		"": true, "%s": true, "STATIC": true, "BUTTON": true, "EDIT": true, "LISTBOX": true,
		"COMBOBOX": true, "msctls_trackbar32": true, "WINDOWS  ·  ": true, "Omarchy-": true, ".zip": true,
		"SUPER+SPACE": true, "SUPER+K": true, "SUPER+RETURN": true, "SUPER+W": true,
	}
	files, err := filepath.Glob("*.go")
	if err != nil {
		t.Fatal(err)
	}
	fset := token.NewFileSet()
	for _, name := range files {
		if strings.HasSuffix(name, "_test.go") {
			continue
		}
		file, err := parser.ParseFile(fset, name, nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		ast.Inspect(file, func(node ast.Node) bool {
			call, ok := node.(*ast.CallExpr)
			if !ok {
				return true
			}
			switch function := call.Fun.(type) {
			case *ast.Ident:
				if !sinks[function.Name] {
					return true
				}
			case *ast.SelectorExpr:
				receiver, ok := function.X.(*ast.SelectorExpr)
				if function.Sel.Name != "Store" || !ok || !stores[receiver.Sel.Name] {
					return true
				}
			default:
				return true
			}
			for _, argument := range call.Args {
				for _, literal := range textLiterals(argument) {
					value, _ := strconv.Unquote(literal.Value)
					if !allowed[value] && strings.IndexFunc(value, unicode.IsLetter) >= 0 {
						t.Errorf("%s: %q is shown to users; move it to ui-locales/en.json", fset.Position(literal.Pos()), value)
					}
				}
			}
			return true
		})
	}
}

// textLiterals returns the string literals that become part of an
// argument's text, without looking inside calls such as uiText.
func textLiterals(expression ast.Expr) []*ast.BasicLit {
	switch e := expression.(type) {
	case *ast.BasicLit:
		if e.Kind == token.STRING {
			return []*ast.BasicLit{e}
		}
	case *ast.BinaryExpr:
		return append(textLiterals(e.X), textLiterals(e.Y)...)
	case *ast.ParenExpr:
		return textLiterals(e.X)
	}
	return nil
}

// TestLauncherMessageKeysMatchTheCatalog reads the launcher's source: every
// message it asks for must be in the English catalog, with the placeholders
// the code fills in, and every English message must still be used.
func TestLauncherMessageKeysMatchTheCatalog(t *testing.T) {
	catalogs, err := readUICatalogs(uiLocaleFiles)
	if err != nil {
		t.Fatal(err)
	}
	english := catalogs["en"]
	files, err := filepath.Glob("*.go")
	if err != nil {
		t.Fatal(err)
	}
	used := map[string]bool{}
	fset := token.NewFileSet()
	for _, name := range files {
		// ui_messages.go is the catalog itself.
		if strings.HasSuffix(name, "_test.go") || name == "ui_messages.go" {
			continue
		}
		file, err := parser.ParseFile(fset, name, nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		ast.Inspect(file, func(node ast.Node) bool {
			call, ok := node.(*ast.CallExpr)
			if !ok {
				return true
			}
			function, ok := call.Fun.(*ast.Ident)
			if !ok || function.Name != "uiText" && function.Name != "uiTextWith" || len(call.Args) == 0 {
				return true
			}
			position := fset.Position(call.Pos())
			literal, ok := call.Args[0].(*ast.BasicLit)
			if !ok || literal.Kind != token.STRING {
				t.Errorf("%s: %s needs a literal key so it can be checked", position, function.Name)
				return true
			}
			key, _ := strconv.Unquote(literal.Value)
			message, exists := english[key]
			if !exists {
				t.Errorf("%s: %q is not in ui-locales/en.json", position, key)
				return true
			}
			used[key] = true
			want := sortedPlaceholders(message)
			if function.Name == "uiText" {
				if len(want) > 0 {
					t.Errorf("%s: %q has placeholders %v; use uiTextWith", position, key, want)
				}
				return true
			}
			values, ok := call.Args[1].(*ast.CompositeLit)
			if !ok {
				return true // Built elsewhere; uiTextWith checks it when it runs.
			}
			var got []string
			for _, element := range values.Elts {
				pair, ok := element.(*ast.KeyValueExpr)
				if !ok {
					continue
				}
				if name, ok := pair.Key.(*ast.BasicLit); ok {
					value, _ := strconv.Unquote(name.Value)
					got = append(got, "{"+value+"}")
				}
			}
			slices.Sort(got)
			if !slices.Equal(slices.Compact(want), got) {
				t.Errorf("%s: %q fills in %v but the message has %v", position, key, got, want)
			}
			return true
		})
	}
	for key := range english {
		if !used[key] {
			t.Errorf("ui-locales/en.json: %q is not used by the launcher", key)
		}
	}
}

func TestPseudoLanguageKeepsPlaceholdersAndLines(t *testing.T) {
	got := pseudoLocalize("Remove {path} from Windows?\n\nNothing else changes.")
	if !strings.Contains(got, "{path}") || strings.Count(got, "\n") != 2 {
		t.Fatalf("pseudo text broke a placeholder or a line: %q", got)
	}
	if strings.Contains(got, "Remove") || !strings.HasPrefix(got, "[Rémóvé") {
		t.Fatalf("pseudo text is not accented: %q", got)
	}
	if len([]rune(got)) < len([]rune("Remove {path} from Windows?\n\nNothing else changes."))*5/4 {
		t.Fatalf("pseudo text should be longer than English: %q", got)
	}
	catalogs := map[string]map[string]string{"en": {"button": "Close"}, "ko": {"button": "닫기"}}
	if got := launcherUILanguage("qps-ploc", []string{"ko-KR"}, catalogs); got != uiPseudoLanguage {
		t.Fatalf("pseudo language not selected: %q", got)
	}
	if got := (uiTranslator{language: uiPseudoLanguage, catalogs: catalogs}).text("button"); got != "[Çlóšé ~~]" {
		t.Fatalf("pseudo message: %q", got)
	}
}

func TestLanguageOverrideWinsOverWindows(t *testing.T) {
	catalogs := map[string]map[string]string{"en": {"button": "Close"}, "ko": {"button": "닫기"}, "zh-Hans": {"button": "关闭"}}
	for _, tc := range []struct {
		override  string
		preferred []string
		want      string
	}{
		{"", []string{"ko-KR"}, "ko"},
		{"zh-Hans", []string{"ko-KR"}, "zh-Hans"},
		{" ko ", []string{"en-US"}, "ko"},
		{"fr", []string{"ko-KR"}, "en"},
	} {
		if got := launcherUILanguage(tc.override, tc.preferred, catalogs); got != tc.want {
			t.Errorf("override %q with %v: got %q, want %q", tc.override, tc.preferred, got, tc.want)
		}
	}
}
