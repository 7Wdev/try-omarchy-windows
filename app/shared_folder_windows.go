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
var sharedFolderIsLocal = dataLocationIsLocal

const (
	fileAttributeReparsePoint = 0x400
	invalidFileAttributes     = 0xFFFFFFFF
	ioReparseTagMountPoint    = 0xA0000003
	ioReparseTagSymlink       = 0xA000000C
)

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
	if strings.TrimSpace(home) == "" || !filepath.IsAbs(home) {
		return "", uiError(uiText("error.share.home_unavailable"), nil)
	}
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
	attributes, _, attributesErr := procGetFileAttributesW.Call(uintptr(unsafe.Pointer(pathPtr)))
	if uint32(attributes) == invalidFileAttributes {
		return "", fmt.Errorf("reading the shared folder attributes: %v", attributesErr)
	}
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
	if !sharedFolderIsLocal(canonical) {
		return "", uiError(uiText("error.share.network"), nil)
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
	warnAboutSharedFolderLinks(canonical)
	return canonical, nil
}

// warnAboutSharedFolderLinks logs symbolic links and junctions directly
// inside the share. QEMU's 9p export (security_model=none) follows them, so
// the guest can read and write wherever they point, for example a link to
// the user's profile. The checks above cover only the share itself: links
// deeper in the tree, or ones created while the VM runs, cannot be found
// here, and only QEMU could refuse them. This is a warning, not a refusal,
// so a share that already works is not disabled.
func warnAboutSharedFolderLinks(share string) {
	links, err := sharedFolderLinks(share, 5)
	if err != nil {
		logf("shared folder: could not check %s for links: %v", share, err)
		return
	}
	if len(links) > 0 {
		logf("shared folder: %s contains links the guest can follow out of the share: %s", share, strings.Join(links, ", "))
	}
}

// sharedFolderLinks returns up to limit names of symbolic links and
// junctions directly inside dir. Other reparse points, such as OneDrive
// placeholders and deduplicated files, stay inside the share and are not
// reported.
func sharedFolderLinks(dir string, limit int) ([]string, error) {
	pattern, err := syscall.UTF16PtrFromString(filepath.Join(dir, "*"))
	if err != nil {
		return nil, err
	}
	var data syscall.Win32finddata
	handle, err := syscall.FindFirstFile(pattern, &data)
	if err != nil {
		return nil, err
	}
	defer syscall.FindClose(handle)
	var links []string
	for {
		if isLinkReparsePoint(data.FileAttributes, data.Reserved0) {
			links = append(links, syscall.UTF16ToString(data.FileName[:]))
			if len(links) >= limit {
				return links, nil
			}
		}
		if err := syscall.FindNextFile(handle, &data); err != nil {
			if err == syscall.ERROR_NO_MORE_FILES {
				return links, nil
			}
			return links, err
		}
	}
}

func isLinkReparsePoint(attributes, tag uint32) bool {
	return attributes&fileAttributeReparsePoint != 0 && (tag == ioReparseTagSymlink || tag == ioReparseTagMountPoint)
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
	if strings.TrimSpace(path) == "" || !filepath.IsAbs(path) {
		return "", false
	}
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
