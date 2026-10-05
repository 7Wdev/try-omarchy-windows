package main

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Versioned payloads allow the previous launcher to keep using its own manifest
// during rollback. Publishing a new payload never replaces the old directory.
func portablePayloadDirectory(root, digest string) string {
	digest = normalizedSHA256(digest)
	if validSHA256(digest) {
		candidate := filepath.Join(root, digest)
		if info, err := os.Stat(candidate); err == nil && info.IsDir() {
			return candidate
		}
	}
	return root
}

func updatePayloadNames() []string {
	return append([]string{runtimeZip, "rootfs.ext4.zst"}, downloadedGuestArtifacts...)
}

func verifyUpdatePayload(ctx context.Context, root, digest string) error {
	if err := validateMovePath(root); err != nil {
		return err
	}
	sums, err := readPortableManifest(filepath.Join(root, "SHA256SUMS"), digest)
	if err != nil {
		return err
	}
	for _, name := range updatePayloadNames() {
		ok, err := verifyFileSHA256Context(ctx, filepath.Join(root, name), sums[name], nil)
		if err != nil {
			return err
		}
		if !ok {
			return fmt.Errorf("stored update payload is damaged: %s", name)
		}
	}
	_, err = readGuestArtifactSizes(filepath.Join(root, "guest-manifest.json"), sums)
	return err
}

func stagePortablePayload(root, release, digest string, client *http.Client, report downloadProgress) error {
	return stageUpdatePayload(setupContext(), root, release, digest, client, report)
}

func stageUpdatePayload(ctx context.Context, root, release, digest string, client *http.Client, report downloadProgress) error {
	digest = normalizedSHA256(digest)
	if !validSHA256(digest) {
		return fmt.Errorf("invalid update payload identity")
	}
	if err := validateMovePath(root); err != nil {
		return err
	}
	if err := os.MkdirAll(root, 0700); err != nil {
		return err
	}
	final := filepath.Join(root, digest)
	if _, err := os.Lstat(final); err == nil {
		if err := verifyUpdatePayload(ctx, final, digest); err != nil {
			if ctx.Err() == nil {
				_ = os.RemoveAll(final)
			}
			return err
		}
		return nil
	} else if !os.IsNotExist(err) {
		return err
	}
	// Do not remove a partial transfer on cancellation or transient network failure.
	stage := filepath.Join(root, ".payload-staging-"+digest)
	if err := validateMovePath(stage); err != nil {
		return err
	}
	if err := os.MkdirAll(stage, 0700); err != nil {
		return err
	}
	opts := defaultDownloadOptions()
	opts.ctx = ctx
	ensure := func(name, sum string) error {
		dest := filepath.Join(stage, name)
		ok, err := verifyFileSHA256Context(ctx, dest, sum, nil)
		if err != nil {
			return err
		}
		if ok {
			return nil
		}
		if err := removeUpdateFile(dest); err != nil {
			return err
		}
		return downloadVerifiedWithOptions(client, normalizedRelease(release)+"/"+name, dest, sum, report, opts)
	}
	if err := ensure("SHA256SUMS", digest); err != nil {
		return err
	}
	sums, err := readPortableManifest(filepath.Join(stage, "SHA256SUMS"), digest)
	if err != nil {
		_ = os.RemoveAll(stage)
		return err
	}
	for _, name := range updatePayloadNames() {
		if !validSHA256(sums[name]) {
			return fmt.Errorf("update is missing %s", name)
		}
	}
	// This small authenticated file gives the rootfs sizes before the large transfer.
	if err := ensure("guest-manifest.json", sums["guest-manifest.json"]); err != nil {
		return err
	}
	sizes, err := readGuestArtifactSizes(filepath.Join(stage, "guest-manifest.json"), sums)
	if err != nil {
		_ = os.RemoveAll(stage)
		return err
	}
	required := diskSpaceReserve
	for _, name := range updatePayloadNames() {
		if name == "guest-manifest.json" {
			continue
		}
		size := sizes[name]
		if size == 0 {
			size, err = updateDownloadSize(ctx, client, normalizedRelease(release)+"/"+name)
			if err != nil {
				return fmt.Errorf("checking update size for %s: %w", name, err)
			}
		}
		required += remainingFileBytes(filepath.Join(stage, name), size)
	}
	if err := requireDiskSpace(stage, required); err != nil {
		return err
	}
	for _, name := range updatePayloadNames() {
		if err := ctx.Err(); err != nil {
			return err
		}
		if err := ensure(name, sums[name]); err != nil {
			return err
		}
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if _, err := os.Lstat(final); !os.IsNotExist(err) {
		return fmt.Errorf("payload destination appeared during staging")
	}
	return os.Rename(stage, final)
}

func removeUpdateFile(path string) error {
	err := os.Remove(path)
	if os.IsNotExist(err) {
		return nil
	}
	return err
}

func updateDownloadSize(ctx context.Context, client *http.Client, source string) (int64, error) {
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodHead, source, nil)
	if err != nil {
		return 0, err
	}
	resp, err := client.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK || resp.ContentLength <= 0 || resp.ContentLength > maxGuestArtifactBytes {
		return 0, fmt.Errorf("server did not report a supported download size")
	}
	return resp.ContentLength, nil
}

// Only digest directories owned by the updater are eligible. The original
// portable payload and the most recent rollback copy are retained.
func pruneUpdatePayloads(root, active, previous string, retained ...string) error {
	if err := validateMovePath(root); err != nil {
		return err
	}
	entries, err := os.ReadDir(root)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	keep := map[string]bool{active: true, previous: true}
	for _, digest := range retained {
		keep[digest] = true
	}
	for _, entry := range entries {
		name := entry.Name()
		digest := strings.TrimPrefix(name, ".payload-staging-")
		if !entry.IsDir() || !validSHA256(digest) || keep[digest] {
			continue
		}
		if err := os.RemoveAll(filepath.Join(root, name)); err != nil {
			return err
		}
	}
	return nil
}
