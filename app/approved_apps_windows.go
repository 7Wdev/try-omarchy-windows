//go:build windows

package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
)

func validateApprovedExecutable(path string) error {
	if !filepath.IsAbs(path) || strings.HasPrefix(path, `\\`) || strings.HasPrefix(path, `//`) || filepath.Clean(path) != path || !strings.EqualFold(filepath.Ext(path), ".exe") {
		return uiError(uiText("error.apps.local_exe"), nil)
	}
	info, err := os.Lstat(path)
	if err != nil {
		return fmt.Errorf("Windows app is unavailable: %w", err)
	}
	if !info.Mode().IsRegular() {
		return uiError(uiText("error.apps.ordinary_exe"), nil)
	}
	return nil
}

func approveWindowsExecutable(prefs *approvedAppPreferences, path string) error {
	if err := validateApprovedExecutable(path); err != nil {
		return err
	}
	if len(prefs.Apps) >= maximumApprovedApps {
		return uiError(uiTextWith("error.apps.limit", map[string]string{"count": fmt.Sprint(maximumApprovedApps)}), nil)
	}
	for _, app := range prefs.Apps {
		if strings.EqualFold(app.Path, path) {
			return uiError(uiText("error.apps.duplicate"), nil)
		}
	}
	id, err := newApprovedAppID()
	if err != nil {
		return err
	}
	name := strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
	name = strings.TrimSpace(name)
	if name == "" || len([]rune(name)) > 80 {
		return uiError(uiText("error.apps.name_length"), nil)
	}
	updated := *prefs
	updated.Apps = append(append([]approvedWindowsApp{}, prefs.Apps...), approvedWindowsApp{ID: id, Name: name, Path: path})
	if err := updated.validate(); err != nil {
		return err
	}
	*prefs = updated
	return nil
}

func launchApprovedWindowsApp(dir, id string) error {
	if !validApprovedAppID(id) {
		return fmt.Errorf("invalid Windows app ID")
	}
	prefs, err := loadApprovedWindowsApps(dir)
	if err != nil {
		return err
	}
	for _, app := range prefs.Apps {
		if app.ID != id {
			continue
		}
		if err := validateApprovedExecutable(app.Path); err != nil {
			return err
		}
		cmd := exec.Command(app.Path)
		cmd.Dir = filepath.Dir(app.Path)
		cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
		if err := cmd.Start(); err != nil {
			return fmt.Errorf("start approved Windows app: %w", err)
		}
		if hwnd := qemuHwnd.Load(); hwnd != 0 {
			procShowWindow.Call(hwnd, swShowMinimized)
		}
		_ = cmd.Process.Release()
		return nil
	}
	return fmt.Errorf("Windows app is not approved")
}
