package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

// Template-free archives require the durable exact-release reset identity.
// Files describes only bytes actually serialized, never an absent template.
func templateFreeReceipt(dir string) ([]byte, error) {
	guest := filepath.Join(dir, "guest")
	data, err := os.ReadFile(filepath.Join(guest, installReceiptFilename))
	if err != nil {
		return nil, err
	}
	var receipt installReceipt
	if len(data) > maxInstallReceiptBytes || json.Unmarshal(data, &receipt) != nil || receipt.Version != installReceiptVersion || receipt.Release == "" {
		return nil, fmt.Errorf("invalid template-free receipt")
	}
	sums, err := readPortableManifest(filepath.Join(guest, "SHA256SUMS"), receipt.ManifestSHA256)
	if err != nil {
		return nil, err
	}
	sums["SHA256SUMS"] = receipt.ManifestSHA256
	normalized := map[string]verifiedArtifact{}
	for _, name := range preparedBootArtifacts {
		entry, ok := receipt.Files[name]
		if !ok || normalizedSHA256(entry.SHA256) != normalizedSHA256(sums[name]) {
			return nil, fmt.Errorf("template-free receipt has no authenticated %s", name)
		}
		verified, err := verifyFileSHA256(filepath.Join(guest, name), sums[name], nil)
		if err != nil {
			return nil, err
		}
		if !verified {
			return nil, fmt.Errorf("template-free guest file is damaged: %s", name)
		}
		info, err := os.Lstat(filepath.Join(guest, name))
		if err != nil {
			return nil, err
		}
		entry.Size, entry.ModTimeUnixNano = info.Size(), info.ModTime().UnixNano()
		normalized[name] = entry
	}
	if _, err := readGuestArtifactSizes(filepath.Join(guest, "guest-manifest.json"), sums); err != nil {
		return nil, err
	}
	receipt.Files = normalized
	return json.Marshal(receipt)
}

func validateTemplateFreeArchive(receiptData, sumsData, metadata []byte, entries map[string]backupEntry) error {
	var receipt installReceipt
	if len(receiptData) > maxInstallReceiptBytes || json.Unmarshal(receiptData, &receipt) != nil || receipt.Version != installReceiptVersion || receipt.Release == "" {
		return fmt.Errorf("invalid template-free receipt")
	}
	sums, err := parseVerifiedSums(sumsData, receipt.ManifestSHA256)
	if err != nil {
		return err
	}
	sums["SHA256SUMS"] = receipt.ManifestSHA256
	if len(receipt.Files) != len(preparedBootArtifacts) {
		return fmt.Errorf("template-free receipt lists absent or unsupported files")
	}
	for _, name := range preparedBootArtifacts {
		expected, ok := receipt.Files[name]
		archive, included := entries["guest/"+name]
		if !ok || !included || expected.Size != archive.Size || normalizedSHA256(expected.SHA256) != normalizedSHA256(sums[name]) || normalizedSHA256(archive.SHA256) != normalizedSHA256(sums[name]) {
			return fmt.Errorf("template-free identity mismatch for %s", name)
		}
	}
	// Authenticate bytes before interpreting size metadata.
	if artifactDigest(metadata) != sums["guest-manifest.json"] {
		return fmt.Errorf("factory metadata checksum mismatch")
	}
	_, err = parseGuestArtifactSizes(metadata, sums)
	return err
}

func artifactDigest(data []byte) string {
	digest := sha256.Sum256(data)
	return hex.EncodeToString(digest[:])
}
