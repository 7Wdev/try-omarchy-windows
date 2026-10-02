//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

var procGetFileAttributesW = kernel32.NewProc("GetFileAttributesW")

const fileAttributeReparsePoint = 0x400

func configureRecommendedSharedFolder(cfg *config, s *settings, settingsFile, home string, explicitShare bool) error {
	if !shouldOfferRecommendedShare(*s, cfg.portable, explicitShare) {
		return nil
	}
	accepted := getUI().chooseSharedFolder()
	if setupCancelled() {
		return errSetupCancelled
	}
	s.SharedFolderPrompted = true
	if accepted {
		path, err := createRecommendedSharedFolder(home)
		if err != nil {
			return err
		}
		path, err = validateWindowsSharedFolder(path, cfg.dir, home)
		if err != nil {
			return err
		}
		s.Share = path
		s.ShareDisabled = false
		cfg.share = path
	}
	return saveSettings(settingsFile, *s)
}

func validateWindowsSharedFolder(path, dataDir, home string) (string, error) {
	path = strings.TrimSpace(path)
	if path == "" || !filepath.IsAbs(path) {
		return "", uiError(uiText("error.share.absolute"), nil)
	}
	if strings.ContainsAny(path, "\x00\r\n") {
		return "", uiError(uiText("error.share.character"), nil)
	}
	volume := filepath.VolumeName(path)
	if strings.HasPrefix(volume, `\\`) || strings.HasPrefix(path, `\\?\`) || strings.HasPrefix(path, `\\.\`) {
		return "", uiError(uiText("error.share.network"), nil)
	}
	clean, err := filepath.Abs(path)
	if err != nil {
		return "", fmt.Errorf("resolving the shared folder: %w", err)
	}
	info, err := os.Lstat(clean)
	if err != nil {
		return "", fmt.Errorf("the shared folder is unavailable: %w", err)
	}
	if info.Mode()&os.ModeSymlink != 0 {
		return "", uiError(uiText("error.share.link"), nil)
	}
	pathPtr, err := syscall.UTF16PtrFromString(clean)
	if err != nil {
		return "", fmt.Errorf("reading the shared folder path: %w", err)
	}
	attributes, _, _ := procGetFileAttributesW.Call(uintptr(unsafe.Pointer(pathPtr)))
	if uint32(attributes)&fileAttributeReparsePoint != 0 {
		return "", uiError(uiText("error.share.link"), nil)
	}
	if !info.IsDir() {
		return "", uiError(uiText("error.share.not_folder"), nil)
	}
	canonical, err := filepath.EvalSymlinks(clean)
	if err != nil {
		return "", fmt.Errorf("resolving the shared folder: %w", err)
	}
	canonical, err = filepath.Abs(canonical)
	if err != nil {
		return "", fmt.Errorf("resolving the shared folder: %w", err)
	}
	if filepath.Dir(canonical) == canonical {
		return "", uiError(uiText("error.share.drive"), nil)
	}
	if pathWithinWindows(home, canonical) {
		return "", uiError(uiText("error.share.home"), nil)
	}
	for _, protected := range []string{
		os.Getenv("SystemRoot"), os.Getenv("ProgramFiles"), os.Getenv("ProgramFiles(x86)"),
		os.Getenv("ProgramData"), os.Getenv("PUBLIC"), os.Getenv("LOCALAPPDATA"), os.Getenv("APPDATA"),
		filepath.Join(home, "AppData"),
	} {
		if protected != "" && pathWithinWindows(canonical, protected) {
			return "", uiError(uiText("error.share.system"), nil)
		}
	}
	if dataDir != "" && pathsOverlapWindows(canonical, dataDir) {
		return "", uiError(uiText("error.share.overlap"), nil)
	}
	return canonical, nil
}

func sameWindowsPath(a, b string) bool {
	if a == "" || b == "" {
		return false
	}
	aa, okA := canonicalWindowsComparisonPath(a)
	bb, okB := canonicalWindowsComparisonPath(b)
	return okA && okB && aa == bb
}

func canonicalWindowsComparisonPath(path string) (string, bool) {
	path, err := filepath.Abs(path)
	if err != nil {
		return "", false
	}
	if canonical, err := filepath.EvalSymlinks(path); err == nil {
		path = canonical
	}
	return strings.ToLower(filepath.Clean(path)), true
}

func pathWithinWindows(path, parent string) bool {
	path, okPath := canonicalWindowsComparisonPath(path)
	parent, okParent := canonicalWindowsComparisonPath(parent)
	if !okPath || !okParent {
		return false
	}
	rel, err := filepath.Rel(parent, path)
	return err == nil && (rel == "." || (rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))))
}

func pathsOverlapWindows(a, b string) bool {
	return pathWithinWindows(a, b) || pathWithinWindows(b, a)
}
