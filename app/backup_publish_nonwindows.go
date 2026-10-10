//go:build !windows

package main

import (
	"errors"
	"syscall"
)

func backupHardLinksUnsupported(err error) bool {
	return errors.Is(err, syscall.ENOTSUP) || errors.Is(err, syscall.ENOSYS)
}
