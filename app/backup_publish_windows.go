//go:build windows

package main

import (
	"errors"
	"syscall"
)

func backupHardLinksUnsupported(err error) bool {
	return errors.Is(err, syscall.Errno(1)) || // ERROR_INVALID_FUNCTION
		errors.Is(err, syscall.Errno(50)) // ERROR_NOT_SUPPORTED
}
