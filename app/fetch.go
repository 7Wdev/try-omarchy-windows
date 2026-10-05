package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/klauspost/compress/zstd"
)

// Updates prepare only boot components. Factory acquisition is an explicit
// creation/reset operation and completes before any guest or disk publication.
func ensureGuest(cfg *config, release, sumsSHA256 string) (result error) {
	if cfg.fresh && !cfg.resetPayloadPrepared {
		if err := rejectPendingReset(cfg); err != nil {
			return err
		}
	}
	defer func() {
		if result == nil && cfg.fresh {
			cfg.resetPayloadPrepared = true
		}
	}()

	factory := cfg.fresh
	if _, err := os.Lstat(cfg.disk); os.IsNotExist(err) {
		factory = true
	} else if err != nil {
		return err
	}
	ready, err := installReceiptMatches(cfg.guestDir, release, sumsSHA256, bootGuestArtifacts)
	if err != nil {
		return err
	}
	if ready {
		if factory {
			return ensureFactory(cfg, release, sumsSHA256)
		}
		return validateInstalledDiskBacking(cfg)
	}
	oldRelease, oldManifest, haveOldReceipt := installReceiptIdentity(cfg.guestDir)
	update := haveOldReceipt && (!releaseLocationsEquivalent(oldRelease, release) || oldManifest != normalizedSHA256(sumsSHA256))
	if !update {
		if err := ensureGuestFiles(cfg, release, sumsSHA256); err != nil {
			return err
		}
		if factory {
			return ensureFactory(cfg, release, sumsSHA256)
		}
		return validateInstalledDiskBacking(cfg)
	}
	getUI().setStatus("%s", uiText("status.preparing_image_update"))
	staged := filepath.Join(cfg.dir, "guest.next")
	if err := os.RemoveAll(staged); err != nil {
		return err
	}
	next := *cfg
	next.guestDir = staged
	if err := ensureGuestFiles(&next, release, sumsSHA256); err != nil {
		_ = os.RemoveAll(staged)
		return err
	}
	if factory {
		// The staged tree is not the active overlay's backing. Acquisition failures
		// leave the old tree, disk, receipt and ready marker untouched.
		if err := ensureFactory(&next, release, sumsSHA256); err != nil {
			return err
		}
	}
	// A small existing raw disk must be assessed using its ORIGINAL identity.
	if !factory && !cfg.portable {
		data, err := os.ReadFile(filepath.Join(staged, "build-spec.json"))
		if err != nil {
			return err
		}
		var spec buildSpec
		if err := json.Unmarshal(data, &spec); err != nil {
			return err
		}
		if err := prepareDisk(cfg, spec.Runtime.Storage.ExpandedSizeMiB); err != nil {
			return err
		}
	}
	if err := preparePortablePayloadTransition(cfg, release, sumsSHA256); err != nil {
		return uiError(uiTextWith("fatal.portable_disk_update", map[string]string{"error": err.Error()}), err)
	}
	if err := recordPayloadUpdate(cfg.dir, releaseVersion(release), true, false); err != nil {
		return err
	}
	if err := publishDirectoryUpdate(cfg.guestDir, staged, filepath.Join(cfg.dir, "guest.previous")); err != nil {
		return fmt.Errorf("publishing image update: %w", err)
	}
	return nil
}

func validateInstalledDiskBacking(cfg *config) error {
	if !cfg.portable {
		return nil
	}
	disk, err := inspectInstallationDisk(cfg.dir)
	if err != nil {
		return err
	}
	if disk.Backing == "" {
		return nil
	}
	ok, err := verifyFileSHA256(disk.Backing, disk.BackingSHA256, nil)
	if err != nil {
		return err
	}
	if !ok {
		return fmt.Errorf("original portable factory image is missing or damaged; restore the matching installation")
	}
	return nil
}

func ensureGuestFiles(cfg *config, release, sumsSHA256 string) error {
	if err := checkSetupCancelled(); err != nil {
		return err
	}
	if err := os.MkdirAll(cfg.guestDir, 0755); err != nil {
		return err
	}
	client := newDownloadClient()
	sums, err := cacheGuestSums(cfg, client, release, sumsSHA256, false)
	if err != nil {
		return fmt.Errorf("authenticating SHA256SUMS: %w", err)
	}
	ui := getUI()
	for index, name := range downloadedGuestArtifacts {
		if !validSHA256(sums[name]) {
			return fmt.Errorf("release manifest has no valid SHA256 for %s", name)
		}
		dest := filepath.Join(cfg.guestDir, name)
		status := uiTextWith("status.checking_cached_file", map[string]string{"file": name})
		if payloadIsLocal(cfg, sumsSHA256) {
			err = ensureVerifiedPortableCopy(filepath.Join(portablePayloadDirectory(cfg.payloadDir, sumsSHA256), name), dest, sums[name], status, ui)
		} else {
			status = uiTextWith("status.downloading_omarchy", map[string]string{"part": fmt.Sprint(index + 1), "total": fmt.Sprint(len(downloadedGuestArtifacts))})
			err = ensureVerifiedDownload(client, normalizedRelease(release)+"/"+name, dest, sums[name], status, ui)
		}
		if err != nil {
			return fmt.Errorf("preparing %s: %w", name, err)
		}
	}
	if _, err := readGuestArtifactSizes(filepath.Join(cfg.guestDir, "guest-manifest.json"), sums); err != nil {
		return err
	}
	names := append([]string{}, preparedBootArtifacts...)
	// Preserve an existing full layout when repairing the same installed build.
	if ok, err := verifyFileSHA256(filepath.Join(cfg.guestDir, "rootfs.ext4"), sums["rootfs.ext4"], nil); err != nil {
		return err
	} else if ok {
		names = append(names, "rootfs.ext4")
	}
	sums["SHA256SUMS"] = normalizedSHA256(sumsSHA256)
	return writeInstallReceipt(cfg.guestDir, release, sumsSHA256, names, sums)
}

// Acquisition uses a private sibling area, never the updater's verified cache.
// Publish the verified template before extending the still-valid boot receipt.
func ensureFactory(cfg *config, release, digest string) (result error) {
	defer func() {
		if result != nil {
			result = fmt.Errorf("%s: %w", uiTextWith("error.factory.unavailable", map[string]string{"version": factoryReleaseLabel(release)}), result)
		}
	}()
	if err := checkSetupCancelled(); err != nil {
		return err
	}
	client := newDownloadClient()
	// A staged guest tree cannot be an active backing even if cfg.dir still
	// identifies the original installation.
	if pathsEqual(cfg.guestDir, filepath.Join(cfg.dir, "guest")) {
		if _, err := os.Lstat(filepath.Join(cfg.dir, "vm", "disk.qcow2")); err == nil {
			disk, err := inspectInstallationDisk(cfg.dir)
			if err != nil {
				return err
			}
			if disk.Backing != "" {
				oldRelease, oldDigest, ok := installReceiptIdentity(cfg.guestDir)
				if !ok || !releaseLocationsEquivalent(oldRelease, release) || oldDigest != normalizedSHA256(digest) {
					return fmt.Errorf("cannot replace an active portable backing image")
				}
				if err := validateInstalledDiskBacking(cfg); err != nil {
					return err
				}
			}
		} else if !os.IsNotExist(err) {
			return err
		}
	}
	stage := filepath.Join(filepath.Dir(cfg.guestDir), ".factory-"+normalizedSHA256(digest))
	if !validSHA256(normalizedSHA256(digest)) {
		return fmt.Errorf("invalid factory identity")
	}
	if err := validateMovePath(stage); err != nil {
		return err
	}
	if err := os.MkdirAll(stage, 0700); err != nil {
		return err
	}
	// Resolve authentication from installed metadata before cache/embedded/network.
	manifestData, sums, err := resolveGuestManifest(cfg, client, release, digest, true)
	if err != nil {
		return err
	}
	if !validSHA256(sums["rootfs.ext4"]) || !validSHA256(sums["rootfs.ext4.zst"]) {
		return fmt.Errorf("factory hashes are missing")
	}
	for _, name := range bootGuestArtifacts {
		ok, err := verifyFileSHA256(filepath.Join(cfg.guestDir, name), sums[name], nil)
		if err != nil {
			return err
		}
		if !ok {
			return fmt.Errorf("installed boot file is damaged: %s", name)
		}
	}
	ui := getUI()
	meta := filepath.Join(stage, "guest-manifest.json")
	installedMeta := filepath.Join(cfg.guestDir, "guest-manifest.json")
	if ok, err := verifyFileSHA256(installedMeta, sums["guest-manifest.json"], nil); err != nil {
		return err
	} else if ok {
		if err := copyPortableArtifact(installedMeta, meta, sums["guest-manifest.json"], nil); err != nil {
			return err
		}
	} else if err := acquireFactoryArtifact(cfg, client, release, digest, "guest-manifest.json", meta, sums["guest-manifest.json"], ui); err != nil {
		return err
	}
	sizes, err := readGuestArtifactSizes(meta, sums)
	if err != nil {
		return err
	}
	rootfs := filepath.Join(cfg.guestDir, "rootfs.ext4")
	ui.setStatus("%s", uiText("status.checking_cached_system"))
	if cfg.portable {
		ui.setStatus("%s", uiText("status.checking_portable_system"))
	}
	ok, err := verifyFileSHA256(rootfs, sums["rootfs.ext4"], ui.setProgress)
	if err != nil {
		return err
	}
	if !ok {
		zst := filepath.Join(stage, "rootfs.ext4.zst")
		allocated, err := rootfsInstallBytes(sizes["rootfs.ext4"], cfg.portable)
		if err != nil {
			return err
		}
		required, err := guestInstallSpaceRequired(remainingFileBytes(zst, sizes["rootfs.ext4.zst"]), allocated)
		if err != nil {
			return err
		}
		if err := requireDiskSpace(stage, required); err != nil {
			return err
		}
		if err := acquireFactoryArtifact(cfg, client, release, digest, "rootfs.ext4.zst", zst, sums["rootfs.ext4.zst"], ui); err != nil {
			return err
		}
		if err := requireDiskSpace(stage, allocated+diskSpaceReserve); err != nil {
			return err
		}
		next := filepath.Join(stage, "rootfs.ext4")
		ui.setStatus("%s", uiText("status.unpacking_system"))
		if err := decompress(zst, next, sums["rootfs.ext4"], ui); err != nil {
			return err
		}
		if err := checkSetupCancelled(); err != nil {
			return err
		}
		if err := publishMoveFile(next, rootfs); err != nil {
			return err
		}
	}
	// Persist the exact authenticated sums and metadata for future reset/restore.
	if err := writeUpdateFile(filepath.Join(cfg.guestDir, "SHA256SUMS"), manifestData); err != nil {
		return err
	}
	if err := copyPortableArtifact(meta, installedMeta, sums["guest-manifest.json"], nil); err != nil {
		return err
	}
	sums["SHA256SUMS"] = normalizedSHA256(digest)
	if err := writeInstallReceipt(cfg.guestDir, release, digest, append(append([]string{}, preparedBootArtifacts...), "rootfs.ext4"), sums); err != nil {
		return err
	}
	_ = os.RemoveAll(stage)
	ui.setStatus("%s", uiText("status.ready_starting"))
	return nil
}

func acquireFactoryArtifact(cfg *config, client *http.Client, release, digest, name, dest, sum string, ui *progressUI) error {
	cached := filepath.Join(portablePayloadDirectory(cfg.payloadDir, digest), name)
	if cfg.payloadDir != "" {
		ok, err := verifyFileSHA256(cached, sum, nil)
		if err != nil {
			return err
		}
		if ok {
			return ensureVerifiedPortableCopy(cached, dest, sum, "", ui)
		}
	}
	return ensureVerifiedDownload(client, normalizedRelease(release)+"/"+name, dest, sum, uiTextWith("status.checking_cached_file", map[string]string{"file": name}), ui)
}

func ensureInstalledFactory(cfg *config) error {
	release, digest, ok := installReceiptIdentity(cfg.guestDir)
	if !ok {
		return fmt.Errorf("verified factory release identity is missing")
	}
	return ensureFactory(cfg, release, digest)
}

func factoryReleaseLabel(release string) string {
	label := filepath.Base(normalizedRelease(release))
	if label == "." || label == "/" || label == "" {
		return normalizedRelease(release)
	}
	return label
}

func releaseVersion(release string) string {
	parts := strings.Split(normalizedRelease(release), "/")
	if len(parts) == 0 {
		return currentVersion
	}
	version := parts[len(parts)-1]
	if _, ok := parseReleaseVersion(version); ok {
		return version
	}
	return currentVersion
}

func ensureVerifiedPortableCopy(src, dest, wantSum, status string, ui *progressUI) error {
	if _, err := os.Lstat(dest); err == nil {
		ui.setStatus("%s", uiTextWith("status.checking_cached_file", map[string]string{"file": filepath.Base(dest)}))
	} else if !os.IsNotExist(err) {
		return err
	}
	ok, err := verifyFileSHA256(dest, wantSum, ui.setProgress)
	if err != nil {
		return err
	}
	if ok {
		return nil
	}
	if err := removeCachedFile(dest); err != nil {
		return err
	}
	ui.setStatus("%s", status)
	return copyPortableArtifact(src, dest, wantSum, ui.setProgress)
}

func ensureVerifiedDownload(client *http.Client, url, dest, wantSum, status string, ui *progressUI) error {
	if _, err := os.Lstat(dest); err == nil {
		ui.setStatus("%s", uiTextWith("status.checking_cached_file", map[string]string{"file": filepath.Base(dest)}))
	} else if !os.IsNotExist(err) {
		return err
	}
	ok, err := verifyFileSHA256(dest, wantSum, ui.setProgress)
	if err != nil {
		return err
	}
	if ok {
		return nil
	}
	if err := removeCachedFile(dest); err != nil {
		return err
	}
	ui.setStatus("%s", status)
	return download(client, url, dest, wantSum, ui)
}

func removeCachedFile(path string) error {
	err := os.Remove(path)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

func download(client *http.Client, url, dest, wantSum string, ui *progressUI) error {
	phase := ""
	return downloadVerified(client, url, dest, wantSum, func(next string, done, total int64) {
		if next != phase {
			if next == downloadPhaseVerify {
				ui.setStatus("%s", uiTextWith("status.checking_downloaded_file", map[string]string{"file": filepath.Base(dest)}))
			} else if phase == downloadPhaseVerify {
				ui.setStatus("%s", uiTextWith("status.resuming_file", map[string]string{"file": filepath.Base(dest)}))
			}
		}
		phase = next
		ui.setProgress(done, total)
	})
}

func decompress(src, dest, wantSum string, ui *progressUI) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	st, err := in.Stat()
	if err != nil {
		return err
	}
	counted := &countingReader{r: in}
	// Larger windows compress repeated guest packages without removing features.
	// Keep enough history to avoid repeatedly copying the whole window.
	dec, err := zstd.NewReader(counted, zstd.WithDecoderLowmem(false))
	if err != nil {
		return err
	}
	defer dec.Close()
	tmp := dest + ".part"
	if err := removeCachedFile(tmp); err != nil {
		return err
	}
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	writeOK := false
	defer func() {
		if !writeOK {
			out.Close()
			os.Remove(tmp)
		}
	}()
	// The rootfs is mostly zeros. NTFS stores the skipped blocks sparsely;
	// exFAT allocates them when the file is truncated but uses the same copy
	// loop so progress continues to update during the full-size fallback.
	_ = setSparse(out)
	if err := sparseCopyStream(out, dec, st.Size(), counted, ui); err != nil {
		return err
	}
	if err := out.Sync(); err != nil {
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	ui.setStatus("%s", uiText("status.checking_unpacked_system"))
	ok, err := verifyFileSHA256(tmp, wantSum, ui.setProgress)
	if err != nil {
		return err
	}
	if !ok {
		return fmt.Errorf("checksum mismatch - the unpacked image is corrupt, try again")
	}
	if err := os.Rename(tmp, dest); err != nil {
		return err
	}
	writeOK = true
	return nil
}

type countingReader struct {
	r io.Reader
	n int64
}

func (c *countingReader) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.n += int64(n)
	return n, err
}

func sparseCopyStream(dst *os.File, src io.Reader, srcTotal int64, counted *countingReader, ui *progressUI) error {
	buf := make([]byte, 1<<20)
	zero := make([]byte, 1<<20)
	var off int64
	for {
		if err := checkSetupCancelled(); err != nil {
			return err
		}
		n, err := io.ReadFull(src, buf)
		if n > 0 {
			if !bytes.Equal(buf[:n], zero[:n]) {
				if _, werr := dst.WriteAt(buf[:n], off); werr != nil {
					return werr
				}
			}
			off += int64(n)
			ui.setProgress(counted.n, srcTotal)
		}
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			return dst.Truncate(off)
		}
		if err != nil {
			return err
		}
	}
}
