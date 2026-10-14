package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"time"
	"unicode/utf8"
)

// Links clicked in Windows can open in Omarchy for the link types (URL
// schemes) the user allows in Settings. The guest reports which schemes its
// apps handle, Windows hands a link of an allowed scheme to the launcher, and
// the guest checks it again before xdg-open. See docs/WINDOWS-LINK-BRIDGE.md.
const approvedURLSchemesFilename = "approved-url-schemes.json"
const guestURLSchemesFilename = "guest-url-schemes.json"
const maximumURLSchemes = 32
const maximumApprovedURLSchemesBytes = 64 << 10
const maximumGuestURLSchemesBytes = 8 << 10
const maximumOpenURLBytes = 2048

// Schemes Windows or a browser must keep handling, and schemes that reach
// files, shares, scripts or system settings. Any "ms-" scheme is denied too.
var deniedURLSchemes = map[string]bool{
	"http": true, "https": true, "file": true, "mailto": true, "ftp": true, "ftps": true, "sftp": true,
	"data": true, "javascript": true, "vbscript": true, "about": true, "blob": true, "ws": true, "wss": true,
	"smb": true, "nfs": true, "news": true, "nntp": true, "telnet": true, "rlogin": true, "ldap": true, "ldaps": true,
	"shell": true, "res": true, "mk": true, "its": true, "search": true, "search-ms": true,
}

type approvedURLSchemePreferences struct {
	SchemaVersion int      `json:"schemaVersion"`
	Schemes       []string `json:"schemes"`
}

func approvedURLSchemesPath(dir string) string {
	return filepath.Join(dir, approvedURLSchemesFilename)
}

func guestURLSchemesPath(dir string) string {
	return filepath.Join(dir, "vm", guestURLSchemesFilename)
}

// validURLScheme accepts a lowercase scheme of 2 to 32 characters, so a
// drive letter such as "c:" never matches, that is not denied.
func validURLScheme(s string) bool {
	if len(s) < 2 || len(s) > 32 || s[0] < 'a' || s[0] > 'z' {
		return false
	}
	for i := 1; i < len(s); i++ {
		c := s[i]
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '+' || c == '.' || c == '-') {
			return false
		}
	}
	return !deniedURLSchemes[s] && !strings.HasPrefix(s, "ms-")
}

func validURLSchemeList(schemes []string) error {
	if len(schemes) > maximumURLSchemes {
		return fmt.Errorf("too many link types")
	}
	seen := make(map[string]bool, len(schemes))
	for _, scheme := range schemes {
		if !validURLScheme(scheme) || seen[scheme] {
			return fmt.Errorf("invalid link type")
		}
		seen[scheme] = true
	}
	return nil
}

// validOpenURL returns the link with its scheme lowercased when the scheme is
// approved. Nothing else is trimmed, decoded or rewritten: a link that needs
// that is refused.
func validOpenURL(raw string, approved []string) (string, bool) {
	if len(raw) > maximumOpenURLBytes || !utf8.ValidString(raw) {
		return "", false
	}
	for i := 0; i < len(raw); i++ {
		if c := raw[i]; c < 0x21 || c == 0x7f || c == '"' || c == '\\' {
			return "", false
		}
	}
	colon := strings.IndexByte(raw, ':')
	if colon < 0 || colon == len(raw)-1 {
		return "", false
	}
	// Lowercase ASCII only: strings.ToLower would turn the Kelvin sign into k.
	scheme := []byte(raw[:colon])
	for i, c := range scheme {
		if c >= 'A' && c <= 'Z' {
			scheme[i] = c + 'a' - 'A'
		}
	}
	if !validURLScheme(string(scheme)) || !slices.Contains(approved, string(scheme)) {
		return "", false
	}
	return string(scheme) + raw[colon:], true
}

func (prefs approvedURLSchemePreferences) validate() error {
	if prefs.SchemaVersion != 1 {
		return fmt.Errorf("invalid allowed link types file")
	}
	return validURLSchemeList(prefs.Schemes)
}

func loadApprovedURLSchemes(dir string) (approvedURLSchemePreferences, error) {
	prefs := approvedURLSchemePreferences{SchemaVersion: 1, Schemes: []string{}}
	data, err := readBoundedRegularFile(approvedURLSchemesPath(dir), maximumApprovedURLSchemesBytes)
	if errors.Is(err, os.ErrNotExist) {
		return prefs, nil
	}
	if err != nil {
		return prefs, err
	}
	if err := decodeStrictJSON(data, &prefs); err != nil {
		return approvedURLSchemePreferences{SchemaVersion: 1, Schemes: []string{}}, err
	}
	if err := prefs.validate(); err != nil {
		return approvedURLSchemePreferences{SchemaVersion: 1, Schemes: []string{}}, err
	}
	return prefs, nil
}

func saveApprovedURLSchemes(dir string, prefs approvedURLSchemePreferences) error {
	prefs.SchemaVersion = 1
	if prefs.Schemes == nil {
		prefs.Schemes = []string{}
	}
	if err := prefs.validate(); err != nil {
		return err
	}
	data, err := json.MarshalIndent(prefs, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	return replaceFile(approvedURLSchemesPath(dir), append(data, '\n'))
}

// The guest's report is written by the running launcher for the separate
// Settings process. It came from the guest, so every read checks it again.
type guestURLSchemeReport struct {
	Schemes []string `json:"schemes"`
}

func loadGuestURLSchemes(dir string) ([]string, error) {
	data, err := readBoundedRegularFile(guestURLSchemesPath(dir), maximumGuestURLSchemesBytes)
	if errors.Is(err, os.ErrNotExist) {
		return []string{}, nil
	}
	if err != nil {
		return []string{}, err
	}
	var report guestURLSchemeReport
	if err := decodeStrictJSON(data, &report); err != nil {
		return []string{}, err
	}
	if err := validURLSchemeList(report.Schemes); err != nil {
		return []string{}, err
	}
	return report.Schemes, nil
}

// saveGuestURLSchemes writes the report only when it differs from the stored
// one, and reports whether it wrote.
func saveGuestURLSchemes(dir string, schemes []string) (bool, error) {
	if err := validURLSchemeList(schemes); err != nil {
		return false, err
	}
	sorted := slices.Sorted(slices.Values(schemes))
	if sorted == nil {
		sorted = []string{}
	}
	if _, err := os.Lstat(guestURLSchemesPath(dir)); err == nil {
		if stored, err := loadGuestURLSchemes(dir); err == nil && slices.Equal(stored, sorted) {
			return false, nil
		}
	}
	data, err := json.Marshal(guestURLSchemeReport{Schemes: sorted})
	if err != nil {
		return false, err
	}
	if err := os.MkdirAll(filepath.Dir(guestURLSchemesPath(dir)), 0700); err != nil {
		return false, err
	}
	return true, replaceFile(guestURLSchemesPath(dir), append(data, '\n'))
}

func readBoundedRegularFile(path string, limit int64) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() > limit {
		return nil, fmt.Errorf("%s is not a regular file under %d KiB", filepath.Base(path), limit>>10)
	}
	data, err := os.ReadFile(path)
	if err == nil && int64(len(data)) > limit {
		return nil, fmt.Errorf("%s grew past %d KiB", filepath.Base(path), limit>>10)
	}
	return data, err
}

func decodeStrictJSON(data []byte, value any) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(value); err != nil {
		return err
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return fmt.Errorf("trailing data after JSON")
	}
	return nil
}

// replaceFile writes beside path and renames over it, so a reader in another
// process sees the old or the new file, never half of one.
// Each writer uses its own temporary name; CreateTemp makes it 0600.
func replaceFile(path string, data []byte) error {
	f, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".*.part")
	if err != nil {
		return err
	}
	tmp := f.Name()
	defer os.Remove(tmp)
	if _, err := f.Write(data); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return renameAudioRoute(tmp, path, os.Rename, time.Sleep)
}

// Agent lines carry compact JSON in standard base64 with padding.
func urlApprovedLine(schemes []string) string {
	if schemes == nil {
		schemes = []string{}
	}
	data, _ := json.Marshal(schemes)
	return "url-approved " + base64.StdEncoding.EncodeToString(data) + "\n"
}

// parseURLSchemesLine reads the guest's "url-schemes" report.
func parseURLSchemesLine(line string) ([]string, error) {
	payload, ok := strings.CutPrefix(line, "url-schemes ")
	if !ok || len(line) > maximumGuestURLSchemesBytes || !strings.HasSuffix(payload, "\n") {
		return nil, fmt.Errorf("malformed report")
	}
	payload = payload[:len(payload)-1]
	// The decoder skips CR and LF; a line carrying them is not ours.
	if strings.ContainsAny(payload, "\r\n") {
		return nil, fmt.Errorf("malformed report encoding")
	}
	data, err := base64.StdEncoding.Strict().DecodeString(payload)
	if err != nil {
		return nil, fmt.Errorf("malformed report encoding")
	}
	var schemes []string
	if err := decodeStrictJSON(data, &schemes); err != nil || schemes == nil {
		return nil, fmt.Errorf("malformed report JSON")
	}
	if err := validURLSchemeList(schemes); err != nil {
		return nil, err
	}
	return schemes, nil
}

// "open-url <base64 link>" is the same line from the -open-url process to the
// running launcher and from the launcher to the guest.
func openURLLine(url string) string {
	return "open-url " + base64.StdEncoding.EncodeToString([]byte(url)) + "\n"
}

func parseOpenURLLine(line string) (string, bool) {
	payload, ok := strings.CutPrefix(strings.TrimSuffix(line, "\n"), "open-url ")
	if !ok || payload == "" || len(payload) > base64.StdEncoding.EncodedLen(maximumOpenURLBytes) || strings.ContainsAny(payload, "\r\n") {
		return "", false
	}
	data, err := base64.StdEncoding.Strict().DecodeString(payload)
	if err != nil || len(data) == 0 || len(data) > maximumOpenURLBytes {
		return "", false
	}
	return string(data), true
}

// parseURLActivation recognizes the command Windows runs for an allowed link
// before any other flag is parsed. Once "-open-url" appears anywhere, only
// the exact registered shape is accepted, so a link that splits into extra
// arguments can never reach the ordinary launcher flags.
func parseURLActivation(args []string) (dir, url string, activation, ok bool) {
	for _, arg := range args {
		name := strings.TrimLeft(arg, "-")
		if len(name) < len(arg) && (name == "open-url" || strings.HasPrefix(name, "open-url=")) {
			activation = true
		}
	}
	if !activation {
		return "", "", false, false
	}
	if len(args) != 4 || args[0] != "-dir" || args[2] != "-open-url" || args[1] == "" || args[3] == "" ||
		strings.HasPrefix(args[1], "-") {
		return "", "", true, false
	}
	return args[1], args[3], true, true
}
