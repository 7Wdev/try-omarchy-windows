package main

import (
	"context"
	"crypto/ed25519"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
)

const pinnedPayloadFilename = "pinned-payload.json"

// These identities come from the running executable, never from the feed or
// installed receipts. The executable's embedded hashes are the trust roots.
type pinnedPayloadUpdate struct {
	Release, Digest               string
	RuntimeRelease, RuntimeDigest string
}

type pinnedPayloadArtifacts struct {
	release, digest string
	names           []string
}

func (p pinnedPayloadUpdate) artifacts() []pinnedPayloadArtifacts {
	if p.Digest == p.RuntimeDigest {
		return []pinnedPayloadArtifacts{{p.Release, p.Digest, updatePayloadNames()}}
	}
	return []pinnedPayloadArtifacts{{p.Release, p.Digest, append([]string{"rootfs.ext4.zst"}, downloadedGuestArtifacts...)},
		{p.RuntimeRelease, p.RuntimeDigest, []string{runtimeZip}}}
}

func ownPayloadUpdate(cfg *config, complete, recovery bool, explicit map[string]bool, pins pinnedPayloadUpdate) *pinnedPayloadUpdate {
	if !complete || cfg.fresh || recovery || cfg.portable || failedUpdateVersion(cfg.dir) == currentVersion {
		return nil
	}
	for _, name := range []string{"release", "sums-sha256", "runtime-release", "runtime-sums-sha256"} {
		if explicit[name] {
			return nil
		}
	}
	guestRelease, guestDigest, guestOK := installReceiptIdentity(cfg.guestDir)
	runtimeRelease, runtimeDigest, runtimeOK := runtimeReceiptIdentity(filepath.Join(cfg.dir, "runtime"))
	if guestOK && runtimeOK && releaseLocationsEquivalent(guestRelease, pins.Release) && guestDigest == pins.Digest &&
		releaseLocationsEquivalent(runtimeRelease, pins.RuntimeRelease) && runtimeDigest == pins.RuntimeDigest {
		return nil
	}
	return &pins
}

func stagePinnedPayloadUpdate(ctx context.Context, client *http.Client, dir, root string, pins pinnedPayloadUpdate) error {
	if failedUpdateVersion(dir) == currentVersion {
		return nil
	}
	for _, pin := range pins.artifacts() {
		if err := stagePayloadArtifacts(ctx, root, pin.release, pin.digest, client, nil, pin.names); err != nil {
			return err
		}
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	data, err := json.Marshal(pins)
	if err != nil {
		return err
	}
	if err := writeUpdateFile(filepath.Join(launcherUpdateDir(dir), currentVersion, pinnedPayloadFilename), data); err != nil {
		return err
	}
	return writeUpdateFile(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename), []byte(currentVersion))
}

func verifiedPinnedPayloadUpdate(ctx context.Context, dir, root string, pins pinnedPayloadUpdate) error {
	marker, err := os.ReadFile(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename))
	if err != nil || string(marker) != currentVersion {
		return fmt.Errorf("launcher payload is not staged")
	}
	data, err := os.ReadFile(filepath.Join(launcherUpdateDir(dir), currentVersion, pinnedPayloadFilename))
	if err != nil {
		return err
	}
	var stored pinnedPayloadUpdate
	if len(data) > maxUpdateManifestLen || json.Unmarshal(data, &stored) != nil || stored != pins {
		return fmt.Errorf("staged payload does not match launcher pins")
	}
	for _, pin := range pins.artifacts() {
		if err := verifyPayloadArtifacts(ctx, filepath.Join(root, pin.digest), pin.digest, pin.names); err != nil {
			return err
		}
	}
	return nil
}

func verifiedLauncherPayloadUpdate(ctx context.Context, dir, root string, pins pinnedPayloadUpdate, key ed25519.PublicKey) error {
	if _, err := os.Stat(filepath.Join(launcherUpdateDir(dir), currentVersion, pinnedPayloadFilename)); !os.IsNotExist(err) {
		return verifiedPinnedPayloadUpdate(ctx, dir, root, pins)
	}
	marker, err := os.ReadFile(filepath.Join(launcherUpdateDir(dir), stagedUpdateFilename))
	if err != nil || string(marker) != currentVersion {
		return fmt.Errorf("launcher payload is not staged")
	}
	manifest, err := verifiedStagedUpdate(ctx, dir, root, currentVersion, key)
	if err != nil {
		return err
	}
	if manifest.ManifestSHA256 != pins.Digest || pins.Digest != pins.RuntimeDigest ||
		!releaseLocationsEquivalent(manifest.Release, pins.Release) || !releaseLocationsEquivalent(manifest.Release, pins.RuntimeRelease) {
		return fmt.Errorf("signed payload does not match launcher pins")
	}
	return nil
}
