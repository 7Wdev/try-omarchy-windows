package main

import (
	"fmt"
	"net/http"
	"os"
	"path/filepath"
)

// Keep the full trust envelope, including artifacts omitted by this operation.
func resolveGuestManifest(cfg *config, client *http.Client, release, digest string, network bool) ([]byte, map[string]string, error) {
	paths := []string{filepath.Join(cfg.guestDir, "SHA256SUMS")}
	if cfg.payloadDir != "" {
		paths = append(paths, filepath.Join(portablePayloadDirectory(cfg.payloadDir, digest), "SHA256SUMS"))
	}
	for _, path := range paths {
		if _, err := os.Lstat(path); os.IsNotExist(err) {
			continue
		} else if err != nil {
			return nil, nil, err
		}
		data, err := readVerifiedManifestData(path, digest)
		if err != nil {
			return nil, nil, err
		}
		sums, err := parseVerifiedSums(data, digest)
		return data, sums, err
	}
	if normalizedSHA256(digest) == normalizedSHA256(defaultSumsSHA256) {
		if sums, err := parseVerifiedSums(defaultSums, digest); err == nil {
			return defaultSums, sums, nil
		}
	}
	if !network {
		return nil, nil, fmt.Errorf("authenticated factory metadata is not available locally")
	}
	data, err := fetchSmallFile(client, normalizedRelease(release)+"/SHA256SUMS", maxSumsBytes)
	if err != nil {
		return nil, nil, err
	}
	sums, err := parseVerifiedSums(data, digest)
	return data, sums, err
}

func resolveGuestSums(cfg *config, client *http.Client, release, digest string, network bool) (map[string]string, error) {
	_, sums, err := resolveGuestManifest(cfg, client, release, digest, network)
	return sums, err
}

func cacheGuestSums(cfg *config, client *http.Client, release, digest string, factory bool) (map[string]string, error) {
	data, sums, err := resolveGuestManifest(cfg, client, release, digest, factory || !payloadIsLocal(cfg, digest))
	if err != nil {
		return nil, err
	}
	if err := writeUpdateFile(filepath.Join(cfg.guestDir, "SHA256SUMS"), data); err != nil {
		return nil, err
	}
	return sums, nil
}

// Mutation decisions authenticate metadata afresh, even after a receipt fast path.
// Legacy full receipts can prove their ORIGINAL factory's size without a fetch.
func installedFactoryFloor(guest string) (int64, error) {
	release, digest, ok := installReceiptIdentity(guest)
	if !ok {
		return 0, fmt.Errorf("disk completeness is unknown; keep the disk and restore verified metadata or use Start fresh")
	}
	cfg := &config{guestDir: guest}
	metadata := filepath.Join(guest, "guest-manifest.json")
	if _, err := os.Lstat(metadata); err == nil {
		sums, err := resolveGuestSums(cfg, nil, release, digest, false)
		if err != nil {
			return 0, err
		}
		verified, err := verifyFileSHA256(metadata, sums["guest-manifest.json"], nil)
		if err != nil {
			return 0, err
		}
		if !verified {
			return 0, fmt.Errorf("factory size metadata is damaged; your disk has been kept")
		}
		sizes, err := readGuestArtifactSizes(metadata, sums)
		if err != nil {
			return 0, err
		}
		return sizes["rootfs.ext4"], nil
	} else if !os.IsNotExist(err) {
		return 0, err
	}
	// Legacy receipts lack durable metadata. Hash their actual original template,
	// rather than interpreting a newer release's size as disk provenance.
	hash, ok := installReceiptArtifactSHA256(guest, "rootfs.ext4")
	if ok {
		verified, err := verifyFileSHA256(filepath.Join(guest, "rootfs.ext4"), hash, nil)
		if err != nil {
			return 0, err
		}
		if verified {
			info, err := os.Stat(filepath.Join(guest, "rootfs.ext4"))
			if err != nil {
				return 0, err
			}
			return info.Size(), nil
		}
	}
	return 0, fmt.Errorf("disk completeness is unknown; your disk has been kept. Restore verified metadata or use Start fresh")
}
