package main

import (
	"math"
	"strings"
)

type filesystemCapability struct {
	Name         string
	MaxFileBytes int64
	SparseFiles  bool
}

func filesystemFileLimit(name string) int64 {
	switch strings.ToUpper(name) {
	case "FAT", "FAT12", "FAT16", "FAT32":
		return (4 << 30) - 1
	default:
		return math.MaxInt64
	}
}

var queryFilesystemCapability = func(string) (filesystemCapability, error) {
	return filesystemCapability{MaxFileBytes: math.MaxInt64, SparseFiles: true}, nil
}

// Portable disks grow as the guest is used, even when the initial copy is small.
func requirePortableFilesystem(path string) error {
	_, err := portableFilesystemCapability(path)
	return err
}

func portableFilesystemCapability(path string) (filesystemCapability, error) {
	capability, err := queryFilesystemCapability(path)
	if err != nil {
		return capability, err
	}
	if capability.MaxFileBytes < 4<<30 {
		return capability, uiError(uiText("error.portable.filesystem"), nil)
	}
	return capability, nil
}
