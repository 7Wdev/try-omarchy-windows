package main

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// Folder import deliberately accepts only a single release matching the
// running launcher's embedded pins. Newer signed releases remain feed updates.
func stagePinnedPayloadFolder(ctx context.Context, folder, dir, root string, pins pinnedPayloadUpdate) error {
	if pins.Digest != pins.RuntimeDigest || !validSHA256(pins.Digest) {
		return fmt.Errorf("folder install requires matching guest and runtime release pins")
	}
	if failedUpdateVersion(dir) == currentVersion {
		return fmt.Errorf("this payload previously failed to boot")
	}
	if err := validateMovePath(folder); err != nil {
		return err
	}
	// Validate before creating anything in the update cache.
	if err := verifyUpdatePayload(ctx, folder, pins.Digest); err != nil {
		return err
	}
	if err := validateMovePath(root); err != nil {
		return err
	}
	names := append([]string{"SHA256SUMS"}, updatePayloadNames()...)
	var required int64 = diskSpaceReserve
	for _, name := range names {
		info, err := os.Lstat(filepath.Join(folder, name))
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("release file is not regular: %s", name)
		}
		required += info.Size()
	}
	if err := os.MkdirAll(root, 0700); err != nil {
		return err
	}
	if err := requireDiskSpace(root, required); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(root, ".folder-import-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	for _, name := range names {
		if err := copyFolderUpdateFile(ctx, filepath.Join(folder, name), filepath.Join(stage, name)); err != nil {
			return err
		}
	}
	// Authenticate our private copies as well: changes to the selected folder
	// during copying cannot publish a ready update.
	if err := verifyUpdatePayload(ctx, stage, pins.Digest); err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	final := filepath.Join(root, pins.Digest)
	if _, err := os.Lstat(final); os.IsNotExist(err) {
		if err := os.Rename(stage, final); err != nil {
			return err
		}
	} else if err != nil {
		return err
	} else if err := verifyUpdatePayload(ctx, final, pins.Digest); err != nil {
		return err
	}
	// The ordinary staging path re-verifies the cache and writes the same marker.
	return stagePinnedPayloadUpdate(ctx, nil, dir, root, pins)
}

func copyFolderUpdateFile(ctx context.Context, source, dest string) error {
	in, err := os.Open(source)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dest, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer out.Close()
	buf := make([]byte, 1<<20)
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		n, readErr := in.Read(buf)
		if n > 0 {
			if _, err := out.Write(buf[:n]); err != nil {
				return err
			}
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			return readErr
		}
	}
	if err := out.Sync(); err != nil {
		return err
	}
	return out.Close()
}
