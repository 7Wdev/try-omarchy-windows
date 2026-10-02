package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeCatalogs(t *testing.T, files map[string]string) string {
	t.Helper()
	dir := t.TempDir()
	for name, data := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(data), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

const englishCatalog = `{
  "b.second": "Remove {path}?",
  "a.first": "Close",
  "c.third": "Done"
}
`

func TestMissingFollowsEnglishOrder(t *testing.T) {
	dir := writeCatalogs(t, map[string]string{"en.json": englishCatalog, "ko.json": "{\n  \"a.first\": \"닫기\"\n}\n"})
	var out bytes.Buffer
	if err := run([]string{"missing", "ko"}, dir, &out); err != nil {
		t.Fatal(err)
	}
	want := "{\n  \"b.second\": \"Remove {path}?\",\n  \"c.third\": \"Done\"\n}\n"
	if out.String() != want {
		t.Fatalf("missing:\n%s\nwant:\n%s", out.String(), want)
	}
}

func TestMergeCreatesALanguageInEnglishOrder(t *testing.T) {
	dir := writeCatalogs(t, map[string]string{
		"en.json":   englishCatalog,
		"todo.json": "{\n  \"c.third\": \"완료\",\n  \"b.second\": \"{path}을(를) 제거할까요?\",\n  \"a.first\": \"\"\n}\n",
	})
	if err := run([]string{"merge", "ko", filepath.Join(dir, "todo.json")}, dir, &bytes.Buffer{}); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(dir, "ko.json"))
	if err != nil {
		t.Fatal(err)
	}
	// Empty values stay untranslated, so English keeps showing for them.
	want := "{\n  \"b.second\": \"{path}을(를) 제거할까요?\",\n  \"c.third\": \"완료\"\n}\n"
	if string(got) != want {
		t.Fatalf("ko.json:\n%s\nwant:\n%s", got, want)
	}
}

func TestMergeRejectsUnknownKeysAndChangedPlaceholders(t *testing.T) {
	dir := writeCatalogs(t, map[string]string{
		"en.json":   englishCatalog,
		"todo.json": "{\n  \"b.second\": \"Supprimer {chemin} ?\",\n  \"z.old\": \"Ancien\"\n}\n",
	})
	err := run([]string{"merge", "fr", filepath.Join(dir, "todo.json")}, dir, &bytes.Buffer{})
	if err == nil || !strings.Contains(err.Error(), "z.old") || !strings.Contains(err.Error(), "{path}") {
		t.Fatalf("want unknown-key and placeholder errors, got %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "fr.json")); !os.IsNotExist(err) {
		t.Fatal("a rejected merge must not write the catalog")
	}
}

func TestLanguageTagsAreChecked(t *testing.T) {
	dir := writeCatalogs(t, map[string]string{"en.json": englishCatalog})
	for _, language := range []string{"en", "../ko", "korean"} {
		if err := run([]string{"missing", language}, dir, &bytes.Buffer{}); err == nil {
			t.Fatalf("%q should be rejected", language)
		}
	}
}
