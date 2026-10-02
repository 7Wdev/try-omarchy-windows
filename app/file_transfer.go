package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"strings"
)

// Both clipboard and drag adapters use the same archive inventory checks.
// Large transfers are streamed through private files instead of base64 frames.
type fileTransferLimits struct {
	Entries      int
	Bytes        int64
	ArchiveBytes int64
}

func (limits fileTransferLimits) valid() bool {
	return limits.Entries > 0 && limits.Entries <= backupMaxFiles && limits.Bytes > 0 && limits.Bytes <= backupMaxBytes && limits.ArchiveBytes > 0 && limits.ArchiveBytes <= backupMaxBytes
}

type fileTransferOffer struct {
	Version      int    `json:"version"`
	ArchiveBytes int64  `json:"archiveBytes"`
	FileBytes    int64  `json:"fileBytes"`
	Entries      int    `json:"entries"`
	SHA256       string `json:"sha256"`
}

type transferReader struct {
	ctx      context.Context
	reader   io.Reader
	progress func(int64)
}

func (r *transferReader) Read(data []byte) (int, error) {
	if err := r.ctx.Err(); err != nil {
		return 0, err
	}
	n, err := r.reader.Read(data)
	if r.progress != nil && n > 0 {
		r.progress(int64(n))
	}
	return n, err
}

type transferWriter struct {
	ctx        context.Context
	writer     io.Writer
	remaining  int64
	limitError error
}

func (w *transferWriter) Write(data []byte) (int, error) {
	if err := w.ctx.Err(); err != nil {
		return 0, err
	}
	if int64(len(data)) > w.remaining {
		if w.limitError != nil {
			return 0, w.limitError
		}
		return 0, fmt.Errorf("archive exceeds the transfer size limit")
	}
	n, err := w.writer.Write(data)
	w.remaining -= int64(n)
	return n, err
}

// prepareFileTransfer creates a stable offer without keeping file contents in
// memory. The caller retains this private archive until completion/cancellation.
func prepareFileTransfer(ctx context.Context, sources []string, cache string, limits fileTransferLimits, report backupProgress) (offer fileTransferOffer, archive string, err error) {
	if !limits.valid() || !filepath.IsAbs(cache) {
		return offer, "", fmt.Errorf("invalid transfer cache or limits")
	}
	if err := validateMovePath(cache); err != nil {
		return offer, "", err
	}
	if err := os.MkdirAll(cache, 0700); err != nil {
		return offer, "", err
	}
	for _, source := range sources {
		absolute, err := filepath.Abs(source)
		if err != nil {
			return offer, "", err
		}
		relative, err := filepath.Rel(absolute, cache)
		if err == nil && relative != ".." && !strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
			return offer, "", uiError(uiText("error.transfer.storage_outside"), nil)
		}
	}
	available, err := diskFreeBytes(cache)
	if err != nil {
		return offer, "", err
	}
	if available <= diskSpaceReserve {
		return offer, "", errInsufficientDiskSpace
	}
	budget := min(limits.ArchiveBytes, available-diskSpaceReserve)
	var budgetError error
	if budget < limits.ArchiveBytes {
		budgetError = errInsufficientDiskSpace
	}
	file, err := os.CreateTemp(cache, ".transfer-out-")
	if err != nil {
		return offer, "", err
	}
	defer func() {
		file.Close()
		if err != nil {
			os.Remove(file.Name())
		}
	}()
	var copied int64
	err = writeFilesArchive(ctx, &transferWriter{ctx: ctx, writer: file, remaining: budget, limitError: budgetError}, sources, limits, true, func(n int64) {
		copied += n
		if report != nil {
			report(copied, 0, uiText("transfer.phase.preparing"))
		}
	})
	if err != nil {
		return offer, "", err
	}
	info, err := file.Stat()
	if err != nil {
		return offer, "", err
	}
	z, err := inspectFileArchive(file, info.Size(), limits.Entries, uint64(limits.Bytes))
	if err != nil {
		return offer, "", err
	}
	for _, entry := range z.File {
		offer.FileBytes += int64(entry.UncompressedSize64)
	}
	if err = file.Sync(); err != nil {
		return offer, "", err
	}
	if _, err = file.Seek(0, io.SeekStart); err != nil {
		return offer, "", err
	}
	h := sha256.New()
	if _, err = io.Copy(h, &transferReader{ctx: ctx, reader: file}); err != nil {
		return offer, "", err
	}
	offer.Version = 1
	offer.ArchiveBytes = info.Size()
	offer.Entries = len(z.File)
	offer.SHA256 = hex.EncodeToString(h.Sum(nil))
	if err = file.Close(); err != nil {
		return offer, "", err
	}
	return offer, file.Name(), nil
}

func (offer fileTransferOffer) valid(limits fileTransferLimits) bool {
	return limits.valid() && offer.Version == 1 && validSHA256(offer.SHA256) &&
		offer.ArchiveBytes > 0 && offer.ArchiveBytes <= limits.ArchiveBytes &&
		offer.FileBytes >= 0 && offer.FileBytes <= limits.Bytes &&
		offer.Entries >= 1 && offer.Entries <= limits.Entries
}

// receiveFileTransfer is called only after a destination is accepted. It never
// merges into existing files. A complete selection is published as one folder;
// callers choose a new destination for keep-both collision behavior.
func receiveFileTransfer(ctx context.Context, input io.Reader, offer fileTransferOffer, destination string, limits fileTransferLimits, report backupProgress) (err error) {
	if !offer.valid(limits) {
		return fmt.Errorf("invalid file transfer offer")
	}
	if !filepath.IsAbs(destination) {
		return fmt.Errorf("transfer destination must be absolute")
	}
	if err := validateMovePath(destination); err != nil {
		return err
	}
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		return uiError(uiText("error.transfer.new_folder"), nil)
	}
	parent := filepath.Dir(destination)
	if err := requireDiskSpace(parent, offer.ArchiveBytes+offer.FileBytes+diskSpaceReserve); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(parent, ".transfer-in-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	archive, err := os.OpenFile(filepath.Join(stage, "incoming.zip"), os.O_CREATE|os.O_EXCL|os.O_RDWR, 0600)
	if err != nil {
		return err
	}
	defer archive.Close()
	h := sha256.New()
	var received int64
	n, err := io.Copy(io.MultiWriter(archive, h), &transferReader{ctx: ctx, reader: io.LimitReader(input, offer.ArchiveBytes+1), progress: func(n int64) {
		received += n
		if report != nil {
			report(received, offer.ArchiveBytes, uiText("transfer.phase.receiving"))
		}
	}})
	if err != nil {
		return err
	}
	if n != offer.ArchiveBytes || hex.EncodeToString(h.Sum(nil)) != offer.SHA256 {
		return fmt.Errorf("file transfer checksum or length mismatch")
	}
	z, err := inspectFileArchive(archive, n, limits.Entries, uint64(limits.Bytes))
	if err != nil {
		return err
	}
	var total int64
	for _, entry := range z.File {
		total += int64(entry.UncompressedSize64)
	}
	if total != offer.FileBytes || len(z.File) != offer.Entries {
		return fmt.Errorf("file transfer inventory changed")
	}
	contents := filepath.Join(stage, "contents")
	if err := os.Mkdir(contents, 0700); err != nil {
		return err
	}
	root, err := os.OpenRoot(contents)
	if err != nil {
		return err
	}
	defer root.Close()
	var written int64
	for _, entry := range z.File {
		if err := ctx.Err(); err != nil {
			return err
		}
		name := strings.TrimSuffix(entry.Name, "/")
		if entry.FileInfo().IsDir() {
			if err := root.MkdirAll(name, 0700); err != nil {
				return err
			}
			continue
		}
		if err := root.MkdirAll(path.Dir(name), 0700); err != nil {
			return err
		}
		output, err := root.OpenFile(name, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600|(entry.Mode()&0100))
		if err != nil {
			return err
		}
		source, err := entry.Open()
		if err != nil {
			output.Close()
			return err
		}
		count, copyErr := io.Copy(output, &transferReader{ctx: ctx, reader: io.LimitReader(source, int64(entry.UncompressedSize64)+1), progress: func(n int64) {
			written += n
			if report != nil {
				report(written, total, uiText("transfer.phase.placing"))
			}
		}})
		source.Close()
		if copyErr == nil {
			copyErr = output.Sync()
		}
		closeErr := output.Close()
		if copyErr != nil {
			return copyErr
		}
		if closeErr != nil {
			return closeErr
		}
		if count != int64(entry.UncompressedSize64) {
			return fmt.Errorf("file transfer entry size mismatch")
		}
	}
	for index := len(z.File) - 1; index >= 0; index-- {
		entry := z.File[index]
		if !entry.Modified.IsZero() {
			if err := root.Chtimes(strings.TrimSuffix(entry.Name, "/"), entry.Modified, entry.Modified); err != nil {
				return err
			}
		}
	}
	if err := root.Close(); err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	return publishNewDirectory(contents, destination)
}
