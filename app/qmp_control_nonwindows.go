//go:build !windows

package main

import (
	"errors"
	"os"
	"path/filepath"
	"syscall"
)

func platformQMPControlDirectory() (string, error) {
	cache, err := os.UserCacheDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(cache, "TryOmarchyIPC"), nil
}

func isQMPControlSocket(path string) bool {
	info, err := os.Lstat(path)
	return err == nil && info.Mode()&os.ModeSocket != 0
}

func qmpConnectionRefused(err error) bool { return errors.Is(err, syscall.ECONNREFUSED) }

func platformQMPControlRoles() []int {
	return []int{qmpToolsPort, qmpFwdPort, qmpSupPort}
}
