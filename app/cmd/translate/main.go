// Command translate helps with launcher translations. Run it from app/:
//
//	go run ./cmd/translate missing ko > ko-todo.json
//	go run ./cmd/translate merge ko ko-todo.json
//
// missing prints the messages a language has not translated yet, in the
// order of ui-locales/en.json. merge checks a file of translated messages and
// writes them into ui-locales/<language>.json, creating it for a new language.
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
)

var placeholder = regexp.MustCompile(`\{[a-z][a-z0-9_]*\}`)

type message struct{ key, value string }

func main() {
	if err := run(os.Args[1:], "ui-locales", os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, "translate:", err)
		os.Exit(1)
	}
}

func run(args []string, dir string, out io.Writer) error {
	if len(args) < 2 || args[0] == "missing" && len(args) != 2 || args[0] == "merge" && len(args) != 3 {
		return errors.New("usage: translate missing LANGUAGE | translate merge LANGUAGE FILE")
	}
	language := args[1]
	if language == "en" || !regexp.MustCompile(`^[a-z]{2,3}(-[A-Za-z0-9]{2,8})*$`).MatchString(language) {
		return fmt.Errorf("%q is not a language tag such as ko, de or zh-Hans", language)
	}
	english, err := readCatalog(filepath.Join(dir, "en.json"))
	if err != nil {
		return err
	}
	path := filepath.Join(dir, language+".json")
	current := map[string]string{}
	if existing, err := readCatalog(path); err == nil {
		for _, m := range existing {
			current[m.key] = m.value
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	switch args[0] {
	case "missing":
		var todo []message
		for _, m := range english {
			if current[m.key] == "" {
				todo = append(todo, m)
			}
		}
		data, err := encode(todo)
		if err != nil {
			return err
		}
		_, err = out.Write(data)
		return err
	case "merge":
		translated, err := readCatalog(args[2])
		if err != nil {
			return err
		}
		if err := merge(english, current, translated); err != nil {
			return err
		}
		var result []message
		for _, m := range english {
			if value := current[m.key]; value != "" {
				result = append(result, message{m.key, value})
			}
		}
		data, err := encode(result)
		if err != nil {
			return err
		}
		if err := os.WriteFile(path, data, 0o644); err != nil {
			return err
		}
		fmt.Fprintf(os.Stderr, "%s: %d of %d messages translated\n", path, len(result), len(english))
		return nil
	}
	return fmt.Errorf("unknown command %q", args[0])
}

// merge adds translated messages to current after checking each one against
// English: the key must exist and keep exactly the same placeholders.
func merge(english []message, current map[string]string, translated []message) error {
	source := map[string]string{}
	for _, m := range english {
		source[m.key] = m.value
	}
	var problems []string
	for _, m := range translated {
		want, ok := source[m.key]
		switch {
		case !ok:
			problems = append(problems, fmt.Sprintf("%q is not in en.json", m.key))
		case !slices.Equal(placeholders(want), placeholders(m.value)):
			problems = append(problems, fmt.Sprintf("%q must keep the placeholders %v", m.key, placeholders(want)))
		case strings.TrimSpace(m.value) == "":
			// Left empty: still untranslated, English is shown.
		default:
			current[m.key] = m.value
		}
	}
	if len(problems) > 0 {
		return errors.New(strings.Join(problems, "\n"))
	}
	return nil
}

func placeholders(value string) []string {
	found := placeholder.FindAllString(value, -1)
	slices.Sort(found)
	return slices.Compact(found)
}

// readCatalog keeps the file's order so output follows en.json.
func readCatalog(path string) ([]message, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	if token, err := decoder.Token(); err != nil || token != json.Delim('{') {
		return nil, fmt.Errorf("%s: expected a JSON object of messages", path)
	}
	var result []message
	seen := map[string]bool{}
	for decoder.More() {
		token, err := decoder.Token()
		if err != nil {
			return nil, fmt.Errorf("%s: %w", path, err)
		}
		key, _ := token.(string)
		var value string
		if err := decoder.Decode(&value); err != nil {
			return nil, fmt.Errorf("%s: %q: %w", path, key, err)
		}
		if seen[key] {
			return nil, fmt.Errorf("%s: %q appears twice", path, key)
		}
		seen[key] = true
		result = append(result, message{key, value})
	}
	return result, nil
}

// encode writes messages the way the catalogs are formatted: two-space
// indent, characters as they are, one message per line.
func encode(messages []message) ([]byte, error) {
	var b bytes.Buffer
	b.WriteString("{\n")
	for i, m := range messages {
		key, err := marshal(m.key)
		if err != nil {
			return nil, err
		}
		value, err := marshal(m.value)
		if err != nil {
			return nil, err
		}
		b.WriteString("  " + key + ": " + value)
		if i < len(messages)-1 {
			b.WriteString(",")
		}
		b.WriteString("\n")
	}
	b.WriteString("}\n")
	return b.Bytes(), nil
}

func marshal(value string) (string, error) {
	var b bytes.Buffer
	encoder := json.NewEncoder(&b)
	encoder.SetEscapeHTML(false)
	if err := encoder.Encode(value); err != nil {
		return "", err
	}
	return strings.TrimSuffix(b.String(), "\n"), nil
}
