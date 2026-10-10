package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Retained state must boot with its own payload identities. In particular, a
// newer launcher must not update the recovery copy before its first boot.
func preparePortableRecoveryPayload(dir, sourcePayload string) ([]string, error) {
	guestRelease, guestPin, ok := installReceiptIdentity(filepath.Join(dir, "guest"))
	if !ok {
		return nil, fmt.Errorf("retained guest receipt is missing")
	}
	runtimeRelease, runtimePin, ok := runtimeReceiptIdentity(filepath.Join(dir, "runtime"))
	if !ok {
		return nil, fmt.Errorf("retained runtime receipt is missing")
	}
	payload := filepath.Join(filepath.Dir(dir), "payload")
	for index, pin := range []string{guestPin, runtimePin} {
		var data []byte
		component := "guest"
		if index == 1 {
			component = "runtime"
		}
		source := filepath.Join(dir, component, "SHA256SUMS")
		if _, err := os.Lstat(source); os.IsNotExist(err) {
			if normalizedSHA256(pin) == normalizedSHA256(defaultSumsSHA256) {
				data = defaultSums
			} else {
				source = filepath.Join(portablePayloadDirectory(sourcePayload, pin), "SHA256SUMS")
			}
		} else if err != nil {
			return nil, err
		}
		if data == nil {
			var err error
			data, err = readVerifiedManifestData(source, pin)
			if err != nil {
				return nil, err
			}
		}
		if _, err := parseVerifiedSums(data, pin); err != nil {
			return nil, err
		}
		target := filepath.Join(payload, normalizedSHA256(pin))
		if err := validateMovePath(target); err != nil {
			return nil, err
		}
		if err := os.MkdirAll(target, 0700); err != nil {
			return nil, err
		}
		destination := filepath.Join(target, "SHA256SUMS")
		if _, err := os.Lstat(destination); err == nil {
			if _, err := readPortableManifest(destination, pin); err != nil {
				return nil, err
			}
			continue
		} else if !os.IsNotExist(err) {
			return nil, err
		}
		file, err := os.CreateTemp(target, ".recovery-manifest-")
		if err != nil {
			return nil, err
		}
		defer os.Remove(file.Name())
		_, err = file.Write(data)
		if err == nil {
			err = file.Sync()
		}
		closeErr := file.Close()
		if err != nil {
			return nil, err
		}
		if closeErr != nil {
			return nil, closeErr
		}
		if err := publishMoveFile(file.Name(), destination); err != nil {
			return nil, err
		}
	}
	return []string{"-portable", "-no-update", "-release", guestRelease, "-sums-sha256", guestPin, "-runtime-release", runtimeRelease, "-runtime-sums-sha256", runtimePin}, nil
}

// Recovery commands disable delayed expansion and double percent signs. Reject
// quotes/newlines rather than allowing receipt data to become batch commands.
func portableRecoveryCommand(arguments []string) (string, error) {
	var quoted []string
	for _, argument := range arguments {
		if strings.ContainsAny(argument, "\"\r\n\x00") {
			return "", fmt.Errorf("invalid recovery launcher argument")
		}
		quoted = append(quoted, "\""+strings.ReplaceAll(argument, "%", "%%")+"\"")
	}
	return "@echo off\r\nsetlocal DisableDelayedExpansion\r\n\"%~dp0TryOmarchy.exe\" " + strings.Join(quoted, " ") + " %*\r\n", nil
}
